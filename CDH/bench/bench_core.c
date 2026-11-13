/*
 * bench_core: 해시 테이블/저장소 개별 연산의 처리량·지연 (in-process, 싱글 스레드).
 *
 * 측정 방식
 *  - 처리량: 구간 전체를 한 번 재서 ops/s 계산(연산별 시계 호출 오버헤드 없음).
 *    워밍업 1회 후 reps 회 반복하고 중앙값을 보고한다.
 *  - 지연 분포: 별도 패스에서 연산마다 시계를 읽는다. 시계 호출 자체 비용(clock
 *    overhead)을 먼저 재서 같이 출력하므로 p50 같은 작은 값은 그만큼 부풀려 있다.
 *  - 리해싱 최악 지연: 1e6 번 put 을 하나씩 재고, 리해싱 중에 일어난 put 의 합을
 *    "한 번에 몰아서 리해싱했다면 겪었을 정지 시간"의 근사로 비교한다.
 *  - 만료는 가짜 시계로 시간을 앞당겨 결정적으로 만들므로 sleep 이 없다.
 */

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "bench_common.h"
#include "miniredis/clock.h"
#include "miniredis/ht.h"
#include "miniredis/store.h"

#define KEYW 24 /* 키 슬롯 폭(고정). "key:<n>" 이 들어가고 나머지는 0 */

static size_t g_reps = 5;
static bool g_quick = false;

/* ---- 키 생성 ---- */
static char *make_keys(const char *prefix, size_t n) {
    char *buf = (char *)calloc(n, KEYW);
    if (!buf) {
        fprintf(stderr, "alloc failed\n");
        exit(1);
    }
    for (size_t i = 0; i < n; i++) (void)snprintf(buf + i * KEYW, KEYW, "%s%zu", prefix, i);
    return buf;
}

static inline size_t klen_of(const char *k) { return strlen(k); }

/* 삽입 순서와 다른 접근 순서(캐시 지역성 편향 방지): 고정 시드 Fisher-Yates */
static size_t *make_perm(size_t n, uint64_t seed) {
    size_t *p = (size_t *)malloc(n * sizeof *p);
    if (!p) exit(1);
    for (size_t i = 0; i < n; i++) p[i] = i;
    bench_rng r;
    bench_rng_seed(&r, seed);
    for (size_t i = n; i > 1; i--) {
        size_t j = (size_t)(bench_rng_next(&r) % i);
        size_t t = p[i - 1];
        p[i - 1] = p[j];
        p[j] = t;
    }
    return p;
}

/* ---- 가짜 시계(만료 결정화) ---- */
typedef struct fake_time {
    int64_t wall;
} fake_time;
static int64_t fake_wall(void *c) { return ((fake_time *)c)->wall; }
static int64_t fake_mono(void *c) { return ((fake_time *)c)->wall; }

/* ---- 시계 호출 비용 ---- */
static double clock_overhead_ns(void) {
    enum { N = 2000000 };
    uint64_t t0 = bench_now_ns();
    uint64_t acc = 0;
    for (int i = 0; i < N; i++) acc += bench_now_ns();
    uint64_t t1 = bench_now_ns();
    bench_sink += acc;
    return (double)(t1 - t0) / N;
}

/* ================= ht 처리량 ================= */

typedef struct ht_result {
    double put, get_hit, get_miss, del; /* ops/s 중앙값 */
} ht_result;

static mr_ht *new_ht(size_t cap) {
    mr_ht_opts o;
    memset(&o, 0, sizeof o);
    o.initial_capacity = cap;
    mr_ht *h = NULL;
    if (mr_ht_create(&o, &h) != MR_OK) exit(1);
    return h;
}

static void ht_throughput(size_t n, const char *keys, const char *miss, const size_t *perm,
                          ht_result *out) {
    double *vp = (double *)malloc(g_reps * sizeof(double));
    double *vg = (double *)malloc(g_reps * sizeof(double));
    double *vm = (double *)malloc(g_reps * sizeof(double));
    double *vd = (double *)malloc(g_reps * sizeof(double));
    if (!vp || !vg || !vm || !vd) exit(1);

    /* rep == 0 은 워밍업(페이지 폴트·캐시·분기예측 안정화)이라 버린다. */
    for (size_t rep = 0; rep <= g_reps; rep++) {
        mr_ht *h = new_ht(0);

        uint64_t t0 = bench_now_ns();
        for (size_t i = 0; i < n; i++) {
            const char *k = keys + i * KEYW;
            if (mr_ht_put(h, k, klen_of(k), (void *)k, NULL) != MR_OK) exit(1);
        }
        uint64_t t1 = bench_now_ns();

        uint64_t acc = 0;
        for (size_t i = 0; i < n; i++) {
            const char *k = keys + perm[i] * KEYW;
            void *v = NULL;
            if (mr_ht_get(h, k, klen_of(k), &v) != MR_OK) exit(1);
            acc += (uint64_t)(uintptr_t)v;
        }
        uint64_t t2 = bench_now_ns();

        for (size_t i = 0; i < n; i++) {
            const char *k = miss + perm[i] * KEYW;
            void *v = NULL;
            acc += (uint64_t)(mr_ht_get(h, k, klen_of(k), &v) == MR_NOT_FOUND);
        }
        uint64_t t3 = bench_now_ns();

        for (size_t i = 0; i < n; i++) {
            const char *k = keys + perm[i] * KEYW;
            acc += (uint64_t)(mr_ht_delete(h, k, klen_of(k)) == MR_OK);
        }
        uint64_t t4 = bench_now_ns();

        bench_sink += acc;
        mr_ht_destroy(h);

        if (rep == 0) continue;
        size_t r = rep - 1;
        vp[r] = (double)n / ((double)(t1 - t0) / 1e9);
        vg[r] = (double)n / ((double)(t2 - t1) / 1e9);
        vm[r] = (double)n / ((double)(t3 - t2) / 1e9);
        vd[r] = (double)n / ((double)(t4 - t3) / 1e9);
    }
    out->put = bench_median(vp, g_reps);
    out->get_hit = bench_median(vg, g_reps);
    out->get_miss = bench_median(vm, g_reps);
    out->del = bench_median(vd, g_reps);
    free(vp);
    free(vg);
    free(vm);
    free(vd);
}

/* ================= ht 지연 분포(연산별 시계) ================= */

static void print_lat_row(const char *name, size_t n, bench_stats *s) {
    printf("  %-10s n=%-8zu mean=%8.0f  p50=%7.0f  p95=%7.0f  p99=%8.0f  p99.9=%9.0f  max=%10.0f ns\n",
           name, n, s->mean, s->p50, s->p95, s->p99, s->p999, s->max);
}

static void ht_latency(size_t n, const char *keys, const char *miss, const size_t *perm) {
    uint64_t *lat = (uint64_t *)malloc(n * sizeof *lat);
    if (!lat) exit(1);
    mr_ht *h = new_ht(0);

    for (size_t i = 0; i < n; i++) {
        const char *k = keys + i * KEYW;
        uint64_t a = bench_now_ns();
        (void)mr_ht_put(h, k, klen_of(k), (void *)k, NULL);
        lat[i] = bench_since(a);
    }
    bench_stats s = bench_stats_compute(lat, n);
    print_lat_row("put", n, &s);

    uint64_t acc = 0;
    for (size_t i = 0; i < n; i++) {
        const char *k = keys + perm[i] * KEYW;
        void *v = NULL;
        uint64_t a = bench_now_ns();
        (void)mr_ht_get(h, k, klen_of(k), &v);
        lat[i] = bench_since(a);
        acc += (uint64_t)(uintptr_t)v;
    }
    s = bench_stats_compute(lat, n);
    print_lat_row("get(hit)", n, &s);

    for (size_t i = 0; i < n; i++) {
        const char *k = miss + perm[i] * KEYW;
        void *v = NULL;
        uint64_t a = bench_now_ns();
        (void)mr_ht_get(h, k, klen_of(k), &v);
        lat[i] = bench_since(a);
    }
    s = bench_stats_compute(lat, n);
    print_lat_row("get(miss)", n, &s);

    for (size_t i = 0; i < n; i++) {
        const char *k = keys + perm[i] * KEYW;
        uint64_t a = bench_now_ns();
        acc += (uint64_t)(mr_ht_delete(h, k, klen_of(k)) == MR_OK);
        lat[i] = bench_since(a);
    }
    s = bench_stats_compute(lat, n);
    print_lat_row("delete", n, &s);

    bench_sink += acc;
    mr_ht_destroy(h);
    free(lat);
}

/* ================= 리해싱 최악 지연 ================= */

static void rehash_study(size_t n, const char *keys) {
    uint64_t *lat = (uint64_t *)malloc(n * sizeof *lat);
    if (!lat) exit(1);
    uint64_t *sorted = (uint64_t *)malloc(n * sizeof *sorted);
    if (!sorted) exit(1);

    double base_mean_ns = 0.0; /* 기준선 pass 의 평균 put 비용(창 안 "정상 비용" 제거용) */
    for (int step = 0; step < 2; step++) {
        /* 기준선(용량 선할당, 리해싱 없음)을 먼저 재서 그 평균을 보정에 쓴다. */
        int pass = step == 0 ? 1 : 0;
        /* pass 0: 기본(0 에서 시작, 여러 번 확장). pass 1: 기준선. */
        size_t cap = pass == 0 ? 0 : n; /* 로드 팩터 1.0 확장 정책상 n 이면 확장 없음 */
        mr_ht *h = new_ht(cap);

        uint64_t total = 0, reh_sum = 0, win_sum = 0, max_win = 0;
        size_t reh_ops = 0, windows = 0, max_at = 0, win_ops = 0, max_win_ops = 0;
        uint64_t max_lat = 0;
        bool in_win = false;

        for (size_t i = 0; i < n; i++) {
            const char *k = keys + i * KEYW;
            bool rehashing_before = mr_ht_is_rehashing(h);
            uint64_t a = bench_now_ns();
            (void)mr_ht_put(h, k, klen_of(k), (void *)k, NULL);
            uint64_t d = bench_since(a);
            lat[i] = d;
            total += d;
            if (d > max_lat) {
                max_lat = d;
                max_at = i;
            }
            /* put 이 끝난 뒤에도 리해싱 중이거나 시작 전부터 중이었으면 "창" 안의 연산 */
            bool rehashing_now = rehashing_before || mr_ht_is_rehashing(h);
            if (rehashing_now) {
                reh_sum += d;
                reh_ops++;
                win_sum += d;
                win_ops++;
                if (!in_win) {
                    in_win = true;
                    windows++;
                }
            } else if (in_win) {
                in_win = false;
                if (win_sum > max_win) {
                    max_win = win_sum;
                    max_win_ops = win_ops;
                }
                win_sum = 0;
                win_ops = 0;
            }
        }
        if (in_win && win_sum > max_win) {
            max_win = win_sum;
            max_win_ops = win_ops;
        }

        memcpy(sorted, lat, n * sizeof *sorted);
        bench_stats s = bench_stats_compute(sorted, n);
        printf("  [%s]\n", pass == 0 ? "기본(initial_capacity=0, 점진적 리해싱 발생)"
                                      : "기준선(용량 선할당, 리해싱 없음)");
        printf("    총 삽입 시간        : %.1f ms (평균 %.0f ns/put)\n", (double)total / 1e6, s.mean);
        printf("    put 지연            : p50=%.0f p99=%.0f p99.9=%.0f ns, max=%.1f us (i=%zu 번째 put)\n",
               s.p50, s.p99, s.p999, s.max / 1e3, max_at);
        if (pass == 1) base_mean_ns = s.mean;
        if (pass == 0) {
            double adj = (double)max_win - (double)max_win_ops * base_mean_ns;
            printf("    가장 긴 창의 put 수 : %zu 회 -> 기준선 평균(%.0f ns)로 보정한 순수 리해싱 추가분 ≈ %.1f ms\n",
                   max_win_ops, base_mean_ns, adj / 1e6);
            printf("    리해싱 창          : %zu 개, 창 안 put %zu 회(전체의 %.1f%%), 창 안 put 합 %.1f ms\n",
                   windows, reh_ops, 100.0 * (double)reh_ops / (double)n, (double)reh_sum / 1e6);
            printf("    가장 긴 창의 put 합 : %.2f ms  <- 한 번에 몰아 리해싱했다면 단일 요청이 겪을 정지의 근사\n",
                   (double)max_win / 1e6);
            printf("    단일 put 최대 지연  : %.1f us  (가장 긴 창 합의 %.2f%%)\n", (double)max_lat / 1e3,
                   max_win ? 100.0 * (double)max_lat / (double)max_win : 0.0);
        }
        bench_sink += total;
        mr_ht_destroy(h);
    }
    free(lat);
    free(sorted);
}

/* ================= store ================= */

typedef struct store_result {
    double set_new, set_over, get_hit, get_miss, set_px;
} store_result;

static mr_store *new_store(const mr_clock *clk) {
    mr_store_opts o;
    memset(&o, 0, sizeof o);
    o.clock = clk;
    mr_store *s = NULL;
    if (mr_store_create(&o, &s) != MR_OK) exit(1);
    return s;
}

static void store_value_bench(size_t n, size_t vsz, const char *keys, const char *miss,
                              const size_t *perm, store_result *out) {
    char *val = (char *)malloc(vsz);
    if (!val) exit(1);
    memset(val, 'v', vsz);
    double *a[5];
    for (int i = 0; i < 5; i++) {
        a[i] = (double *)malloc(g_reps * sizeof(double));
        if (!a[i]) exit(1);
    }
    mr_set_opts px;
    memset(&px, 0, sizeof px);
    px.expire_kind = MR_EXPIRE_PX;
    px.expire = 3600 * 1000;

    for (size_t rep = 0; rep <= g_reps; rep++) {
        mr_store *s = new_store(NULL);
        uint64_t acc = 0;

        uint64_t t0 = bench_now_ns();
        for (size_t i = 0; i < n; i++) {
            const char *k = keys + i * KEYW;
            if (mr_store_set(s, k, klen_of(k), val, vsz, NULL) != MR_OK) exit(1);
        }
        uint64_t t1 = bench_now_ns();
        for (size_t i = 0; i < n; i++) { /* 같은 키 덮어쓰기(새 버퍼 할당 + 기존 해제) */
            const char *k = keys + perm[i] * KEYW;
            if (mr_store_set(s, k, klen_of(k), val, vsz, NULL) != MR_OK) exit(1);
        }
        uint64_t t2 = bench_now_ns();
        for (size_t i = 0; i < n; i++) {
            const char *k = keys + perm[i] * KEYW;
            const void *v = NULL;
            size_t vl = 0;
            if (mr_store_get(s, k, klen_of(k), &v, &vl) != MR_OK) exit(1);
            acc += vl + (uint64_t)((const unsigned char *)v)[0];
        }
        uint64_t t3 = bench_now_ns();
        for (size_t i = 0; i < n; i++) {
            const char *k = miss + perm[i] * KEYW;
            const void *v = NULL;
            size_t vl = 0;
            acc += (uint64_t)(mr_store_get(s, k, klen_of(k), &v, &vl) == MR_NOT_FOUND);
        }
        uint64_t t4 = bench_now_ns();
        for (size_t i = 0; i < n; i++) { /* SET ... PX (TTL 부착 쓰기) */
            const char *k = keys + perm[i] * KEYW;
            if (mr_store_set(s, k, klen_of(k), val, vsz, &px) != MR_OK) exit(1);
        }
        uint64_t t5 = bench_now_ns();

        bench_sink += acc;
        mr_store_destroy(s);
        if (rep == 0) continue;
        size_t r = rep - 1;
        a[0][r] = (double)n / ((double)(t1 - t0) / 1e9);
        a[1][r] = (double)n / ((double)(t2 - t1) / 1e9);
        a[2][r] = (double)n / ((double)(t3 - t2) / 1e9);
        a[3][r] = (double)n / ((double)(t4 - t3) / 1e9);
        a[4][r] = (double)n / ((double)(t5 - t4) / 1e9);
    }
    out->set_new = bench_median(a[0], g_reps);
    out->set_over = bench_median(a[1], g_reps);
    out->get_hit = bench_median(a[2], g_reps);
    out->get_miss = bench_median(a[3], g_reps);
    out->set_px = bench_median(a[4], g_reps);
    for (int i = 0; i < 5; i++) free(a[i]);
    free(val);
}

/* incr / ttl: 값 크기와 무관하므로 별도 1회 */
static void store_incr_ttl(size_t n, const char *keys, const size_t *perm, double *incr_ops,
                           double *ttl_ops, double *pttl_ops) {
    double *vi = (double *)malloc(g_reps * sizeof(double));
    double *vt = (double *)malloc(g_reps * sizeof(double));
    double *vp = (double *)malloc(g_reps * sizeof(double));
    if (!vi || !vt || !vp) exit(1);
    mr_set_opts px;
    memset(&px, 0, sizeof px);
    px.expire_kind = MR_EXPIRE_PX;
    px.expire = 3600 * 1000;

    for (size_t rep = 0; rep <= g_reps; rep++) {
        mr_store *s = new_store(NULL);
        for (size_t i = 0; i < n; i++) {
            const char *k = keys + i * KEYW;
            if (mr_store_set(s, k, klen_of(k), "0", 1, &px) != MR_OK) exit(1);
        }
        uint64_t acc = 0;
        uint64_t t0 = bench_now_ns();
        for (size_t i = 0; i < n; i++) {
            const char *k = keys + perm[i] * KEYW;
            int64_t out = 0;
            if (mr_store_incr(s, k, klen_of(k), &out) != MR_OK) exit(1);
            acc += (uint64_t)out;
        }
        uint64_t t1 = bench_now_ns();
        for (size_t i = 0; i < n; i++) {
            const char *k = keys + perm[i] * KEYW;
            int64_t out = 0;
            if (mr_store_ttl(s, k, klen_of(k), &out) != MR_OK) exit(1);
            acc += (uint64_t)out;
        }
        uint64_t t2 = bench_now_ns();
        for (size_t i = 0; i < n; i++) {
            const char *k = keys + perm[i] * KEYW;
            int64_t out = 0;
            if (mr_store_pttl(s, k, klen_of(k), &out) != MR_OK) exit(1);
            acc += (uint64_t)out;
        }
        uint64_t t3 = bench_now_ns();
        bench_sink += acc;
        mr_store_destroy(s);
        if (rep == 0) continue;
        vi[rep - 1] = (double)n / ((double)(t1 - t0) / 1e9);
        vt[rep - 1] = (double)n / ((double)(t2 - t1) / 1e9);
        vp[rep - 1] = (double)n / ((double)(t3 - t2) / 1e9);
    }
    *incr_ops = bench_median(vi, g_reps);
    *ttl_ops = bench_median(vt, g_reps);
    *pttl_ops = bench_median(vp, g_reps);
    free(vi);
    free(vt);
    free(vp);
}

/* lazy 만료: 이미 만료된 키를 get 하면 "삭제 + miss" 가 된다. 살아있는 hit / 없는 키 miss 와 비교. */
static void store_lazy_expiry(size_t n, const char *keys, const char *miss, const size_t *perm) {
    double *ve = (double *)malloc(g_reps * sizeof(double));
    double *vh = (double *)malloc(g_reps * sizeof(double));
    double *vm = (double *)malloc(g_reps * sizeof(double));
    if (!ve || !vh || !vm) exit(1);
    char val[16];
    memset(val, 'v', sizeof val);

    for (size_t rep = 0; rep <= g_reps; rep++) {
        fake_time ft = {1000000};
        mr_clock clk = {fake_wall, fake_mono, &ft};
        mr_store *s = new_store(&clk);
        mr_set_opts px;
        memset(&px, 0, sizeof px);
        px.expire_kind = MR_EXPIRE_PX;
        px.expire = 1000;
        for (size_t i = 0; i < n; i++) {
            const char *k = keys + i * KEYW;
            (void)mr_store_set(s, k, klen_of(k), val, sizeof val, &px);
        }
        uint64_t acc = 0;
        /* (a) 만료 전 hit */
        uint64_t t0 = bench_now_ns();
        for (size_t i = 0; i < n; i++) {
            const char *k = keys + perm[i] * KEYW;
            const void *v = NULL;
            size_t vl = 0;
            acc += (uint64_t)(mr_store_get(s, k, klen_of(k), &v, &vl) == MR_OK);
        }
        uint64_t t1 = bench_now_ns();
        /* (b) 존재하지 않는 키 miss */
        for (size_t i = 0; i < n; i++) {
            const char *k = miss + perm[i] * KEYW;
            const void *v = NULL;
            size_t vl = 0;
            acc += (uint64_t)(mr_store_get(s, k, klen_of(k), &v, &vl) == MR_NOT_FOUND);
        }
        uint64_t t2 = bench_now_ns();
        /* (c) 시간 경과 후 만료 키 접근 = lazy 삭제 */
        ft.wall += 5000;
        for (size_t i = 0; i < n; i++) {
            const char *k = keys + perm[i] * KEYW;
            const void *v = NULL;
            size_t vl = 0;
            acc += (uint64_t)(mr_store_get(s, k, klen_of(k), &v, &vl) == MR_NOT_FOUND);
        }
        uint64_t t3 = bench_now_ns();
        bench_sink += acc + mr_store_size(s);
        mr_store_destroy(s);
        if (rep == 0) continue;
        vh[rep - 1] = (double)n / ((double)(t1 - t0) / 1e9);
        vm[rep - 1] = (double)n / ((double)(t2 - t1) / 1e9);
        ve[rep - 1] = (double)n / ((double)(t3 - t2) / 1e9);
    }
    printf("  get hit(TTL 살아있음)      : %12.0f ops/s\n", bench_median(vh, g_reps));
    printf("  get miss(없는 키)          : %12.0f ops/s\n", bench_median(vm, g_reps));
    printf("  get 만료 키(lazy 삭제+miss): %12.0f ops/s\n", bench_median(ve, g_reps));
    free(ve);
    free(vh);
    free(vm);
}

/* sweep: (1) 전부 만료 -> 호출 단위 지연과 제거 속도, (2) 전부 살아있음 -> 순수 훑기 비용 */
static void store_sweep(size_t n, const char *keys) {
    static const size_t samples_list[] = {20, 200, 2000};
    char val[16];
    memset(val, 'v', sizeof val);
    printf("  %-8s | %-30s | %-30s\n", "samples", "전부 만료: 호출당 지연 p50/max(us)",
           "전부 만료: 제거 속도 / 총 시간");
    for (size_t si = 0; si < sizeof samples_list / sizeof samples_list[0]; si++) {
        size_t smp = samples_list[si];
        fake_time ft = {1000000};
        mr_clock clk = {fake_wall, fake_mono, &ft};
        mr_store *s = new_store(&clk);
        mr_set_opts px;
        memset(&px, 0, sizeof px);
        px.expire_kind = MR_EXPIRE_PX;
        px.expire = 1000;
        for (size_t i = 0; i < n; i++) {
            const char *k = keys + i * KEYW;
            (void)mr_store_set(s, k, klen_of(k), val, sizeof val, &px);
        }
        ft.wall += 5000;
        size_t cap_calls = n + 1000;
        uint64_t *lat = (uint64_t *)malloc(cap_calls * sizeof *lat);
        if (!lat) exit(1);
        size_t calls = 0, removed = 0;
        uint64_t ts = bench_now_ns();
        while (mr_store_size(s) > 0 && calls < cap_calls) {
            uint64_t a = bench_now_ns();
            removed += mr_store_sweep(s, smp, 0);
            lat[calls++] = bench_since(a);
        }
        uint64_t te = bench_now_ns();
        bench_stats st = bench_stats_compute(lat, calls);
        printf("  %-8zu | calls=%-6zu p50=%7.1f max=%8.1f | %10.0f keys/s, %.1f ms (removed=%zu)\n",
               smp, calls, st.p50 / 1e3, st.max / 1e3,
               (double)removed / ((double)(te - ts) / 1e9), (double)(te - ts) / 1e6, removed);
        bench_sink += removed;
        free(lat);
        mr_store_destroy(s);
    }

    /* 살아있는 TTL 키만 있을 때: 만료가 없어도 한 바퀴 훑는 비용 */
    {
        fake_time ft = {1000000};
        mr_clock clk = {fake_wall, fake_mono, &ft};
        mr_store *s = new_store(&clk);
        mr_set_opts px;
        memset(&px, 0, sizeof px);
        px.expire_kind = MR_EXPIRE_PX;
        px.expire = 3600 * 1000;
        for (size_t i = 0; i < n; i++) {
            const char *k = keys + i * KEYW;
            (void)mr_store_set(s, k, klen_of(k), val, sizeof val, &px);
        }
        size_t smp = 200, calls = (n / smp) * 2; /* 대략 두 바퀴 */
        uint64_t *lat = (uint64_t *)malloc(calls * sizeof *lat);
        if (!lat) exit(1);
        uint64_t ts = bench_now_ns();
        for (size_t c = 0; c < calls; c++) {
            uint64_t a = bench_now_ns();
            bench_sink += mr_store_sweep(s, smp, 0);
            lat[c] = bench_since(a);
        }
        uint64_t te = bench_now_ns();
        bench_stats st = bench_stats_compute(lat, calls);
        printf("  만료 없음(TTL 키 %zu개 전부 생존), samples=%zu: 호출당 p50=%.1f us p99=%.1f us max=%.1f us,"
               " 훑기 %.0f keys/s\n",
               n, smp, st.p50 / 1e3, st.p99 / 1e3, st.max / 1e3,
               (double)(calls * smp) / ((double)(te - ts) / 1e9));
        free(lat);
        mr_store_destroy(s);
    }
    /* TTL 키가 하나도 없으면 sweep 은 즉시 반환(설계상) */
    {
        mr_store *s = new_store(NULL);
        for (size_t i = 0; i < n; i++) {
            const char *k = keys + i * KEYW;
            (void)mr_store_set(s, k, klen_of(k), val, sizeof val, NULL);
        }
        enum { C = 1000000 };
        uint64_t t0 = bench_now_ns();
        for (int c = 0; c < C; c++) bench_sink += mr_store_sweep(s, 200, 0);
        uint64_t t1 = bench_now_ns();
        printf("  TTL 키 없음(키 %zu개): sweep 호출 %.1f ns/call (즉시 반환)\n", n,
               (double)(t1 - t0) / C);
        mr_store_destroy(s);
    }
}

int main(int argc, char **argv) {
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--quick") == 0) g_quick = true;
        else if (strcmp(argv[i], "--reps") == 0 && i + 1 < argc) {
            long r = bench_arg_long(argv[++i], 5);
            g_reps = r < 1 ? 1 : (size_t)r;
        } else {
            fprintf(stderr, "usage: bench_core [--reps N] [--quick]\n");
            return 2;
        }
    }
    size_t sizes_full[] = {10000, 100000, 1000000};
    size_t sizes_quick[] = {10000, 100000};
    size_t *sizes = g_quick ? sizes_quick : sizes_full;
    size_t nsizes = g_quick ? 2 : 3;

    printf("== bench_core (in-process, 싱글 스레드, reps=%zu + 워밍업 1회, 처리량은 중앙값)\n", g_reps);
    double co = clock_overhead_ns();
    printf("clock_gettime(CLOCK_MONOTONIC) 호출 비용: %.1f ns (지연 분포 표의 p50 등에 포함됨)\n\n", co);

    size_t maxn = sizes[nsizes - 1];
    char *keys = make_keys("key:", maxn);
    char *miss = make_keys("miss:", maxn);

    printf("[1] 해시 테이블 처리량 (ops/s, 값은 포인터만 저장)\n");
    printf("  %-9s %14s %14s %14s %14s\n", "keys", "put(fresh)", "get(hit)", "get(miss)", "delete");
    for (size_t si = 0; si < nsizes; si++) {
        size_t n = sizes[si];
        size_t *perm = make_perm(n, 12345);
        ht_result r;
        ht_throughput(n, keys, miss, perm, &r);
        printf("  %-9zu %14.0f %14.0f %14.0f %14.0f\n", n, r.put, r.get_hit, r.get_miss, r.del);
        fflush(stdout);
        free(perm);
    }

    printf("\n[2] 해시 테이블 지연 분포 (연산별 시계 측정, 1회 패스, ns)\n");
    for (size_t si = 0; si < nsizes; si++) {
        size_t n = sizes[si];
        size_t *perm = make_perm(n, 777);
        ht_latency(n, keys, miss, perm);
        free(perm);
    }

    printf("\n[3] 리해싱 최악 지연: %zu 키 연속 삽입 중 put 1회당 지연\n", maxn);
    rehash_study(maxn, keys);

    size_t sn = 100000;
    size_t *sperm = make_perm(sn, 999);
    printf("\n[4] 저장소(store) 처리량, 키 %zu개 (ops/s, 값 크기 비교)\n", sn);
    printf("  %-8s %13s %13s %13s %13s %13s\n", "value", "set(new)", "set(overwr)", "get(hit)",
           "get(miss)", "set PX");
    size_t vsizes[] = {16, 1024};
    for (size_t vi = 0; vi < 2; vi++) {
        store_result r;
        store_value_bench(sn, vsizes[vi], keys, miss, sperm, &r);
        printf("  %-6zuB  %13.0f %13.0f %13.0f %13.0f %13.0f\n", vsizes[vi], r.set_new, r.set_over,
               r.get_hit, r.get_miss, r.set_px);
        fflush(stdout);
    }
    double inc, ttl, pttl;
    store_incr_ttl(sn, keys, sperm, &inc, &ttl, &pttl);
    printf("  INCR %.0f ops/s | TTL %.0f ops/s | PTTL %.0f ops/s (키 %zu개, 값 \"0\"+TTL)\n", inc, ttl,
           pttl, sn);

    printf("\n[5] lazy 만료 비용 (가짜 시계로 시간 경과, 키 %zu개, 16B 값)\n", sn);
    store_lazy_expiry(sn, keys, miss, sperm);

    printf("\n[6] sweep 비용 (키 %zu개, 16B 값, 가짜 시계)\n", sn);
    store_sweep(sn, keys);

    printf("\n(sink=%llu: 최적화 제거 방지용; 시계 역행 감지 %llu 회(0 으로 보정))\n",
           (unsigned long long)bench_sink, (unsigned long long)bench_backward);
    free(sperm);
    free(keys);
    free(miss);
    return 0;
}
