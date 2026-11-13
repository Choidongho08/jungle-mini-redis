/*
 * bench_cache: "미니 레디스 캐시 사용 vs 미사용" 비교 (in-process, 싱글 스레드, 단일 클라이언트).
 *
 * 원본 데이터 소스 시뮬레이션: 요청마다 nanosleep 으로 실제로 origin_latency 만큼 잔다.
 * 시뮬레이션 시간을 누적만 하는 방식은 호출 오버헤드·스케줄러 지터를 숨기므로 쓰지 않고
 * 진짜로 재운다(대신 --requests 로 총 실행 시간을 제한). 실제 sleep 은 요청한 값보다
 * 약간 더 자므로 실측 평균 origin 지연도 함께 출력한다.
 *
 * 패턴: cache-aside. GET 요청 -> store 조회 -> miss 면 origin 호출 후 TTL 과 함께 SET.
 * 쓰기 요청(--write-pct) -> origin 에 쓰기(같은 지연) -> 캐시 키 invalidate(del). 주기적으로
 * 접두사 invalidate 도 1회 섞는다.
 *
 * 모든 시나리오는 같은 시드의 같은 요청열(Zipf 키 + 읽기/쓰기 선택)을 재생해 공정하다.
 * 캐시는 비어 있는 채로 시작(콜드 스타트 포함)한다. 네트워크/직렬화 비용은 포함하지 않는다.
 */

#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "bench_common.h"
#include "miniredis/store.h"

#define VALUE_LEN 128

typedef struct cfg {
    long origin_ms;     /* 원본 지연(ms) */
    size_t requests;
    size_t keys;
    double zipf_s;
    long ttl_long_ms;
    long ttl_short_ms;
    int write_pct;      /* 쓰기 비율 시나리오에서의 쓰기 % */
    const char *csv;
} cfg;

typedef struct origin {
    long latency_ms;
    uint64_t calls;
    uint64_t slept_ns; /* 실제로 잔 시간 합(실측 평균 확인용) */
} origin;

/* 원본 읽기/쓰기: 지연만큼 진짜로 잔다. EINTR 이면 남은 시간만큼 이어서 잔다. */
static void origin_sleep(origin *o) {
    struct timespec req = {(time_t)(o->latency_ms / 1000), (long)(o->latency_ms % 1000) * 1000000L};
    uint64_t a = bench_now_ns();
    while (nanosleep(&req, &req) != 0 && errno == EINTR) {
    }
    o->slept_ns += bench_since(a);
    o->calls++;
}

/* 키 id 에서 결정적으로 값 생성(원본의 "행" 흉내). 쓰기 버전은 version 으로 바뀐다. */
static void origin_fill(uint8_t *buf, size_t id, uint32_t version) {
    for (size_t i = 0; i < VALUE_LEN; i++) buf[i] = (uint8_t)('a' + (id * 31 + i * 7 + version) % 26);
}

typedef enum { SC_NOCACHE, SC_CACHE_LONG, SC_CACHE_SHORT, SC_NOCACHE_W, SC_CACHE_W } sc_kind;

typedef struct scenario {
    const char *name;
    sc_kind kind;
    bool use_cache;
    long ttl_ms;
    int write_pct;
} scenario;

typedef struct result {
    const char *name;
    double total_s, throughput, mean_us, p50_us, p95_us, p99_us, max_us;
    double hit_rate; /* 읽기 요청 중 캐시 hit 비율(미사용이면 0) */
    uint64_t reads, writes, origin_calls, expired_lazy;
    double origin_avg_ms;
    double speedup; /* 기준 시나리오 대비 총 시간 배수 */
} result;

static result run_scenario(const cfg *c, const scenario *sc, const bench_zipf *z) {
    bench_rng r_key, r_op;
    bench_rng_seed(&r_key, 20240607); /* 모든 시나리오 동일 시드 */
    bench_rng_seed(&r_op, 987654321);

    mr_store *store = NULL;
    if (sc->use_cache && mr_store_create(NULL, &store) != MR_OK) exit(1);

    origin og = {c->origin_ms, 0, 0};
    uint64_t *lat = (uint64_t *)malloc(c->requests * sizeof *lat);
    uint32_t *version = (uint32_t *)calloc(c->keys, sizeof *version); /* 원본의 현재 버전 */
    if (!lat || !version) exit(1);

    uint64_t reads = 0, writes = 0, hits = 0, acc = 0;
    uint8_t vbuf[VALUE_LEN];
    char key[32];

    uint64_t t_start = bench_now_ns();
    for (size_t i = 0; i < c->requests; i++) {
        size_t id = bench_zipf_next(z, &r_key);
        bool is_write = (int)(bench_rng_next(&r_op) % 100) < sc->write_pct;
        int kl = snprintf(key, sizeof key, "item:%zu", id);
        size_t klen = (size_t)kl;

        uint64_t a = bench_now_ns();
        if (is_write) {
            writes++;
            origin_sleep(&og); /* 원본에 쓰기 */
            version[id]++;
            if (sc->use_cache) {
                (void)mr_store_del(store, key, klen); /* 쓰기 후 무효화 */
                if (i % 500 == 499) { /* 가끔 범주 무효화(접두사) 도 호출: 예 item:1* */
                    size_t del = 0;
                    (void)mr_store_invalidate_prefix(store, "item:1", 6, &del);
                    acc += del;
                }
            }
        } else {
            reads++;
            const void *cv = NULL;
            size_t cl = 0;
            if (sc->use_cache && mr_store_get(store, key, klen, &cv, &cl) == MR_OK) {
                hits++;
                acc += cl + ((const uint8_t *)cv)[0];
            } else {
                origin_sleep(&og); /* cache miss: 원본 조회 */
                origin_fill(vbuf, id, version[id]);
                acc += vbuf[0];
                if (sc->use_cache) {
                    mr_set_opts so;
                    memset(&so, 0, sizeof so);
                    so.expire_kind = MR_EXPIRE_PX;
                    so.expire = sc->ttl_ms;
                    if (mr_store_set(store, key, klen, vbuf, VALUE_LEN, &so) != MR_OK) exit(1);
                }
            }
        }
        lat[i] = bench_since(a);
        /* 이벤트 루프가 하듯 주기적으로 능동 만료를 돌린다(비용은 요청 지연에 포함되지 않게 밖에서) */
        if (sc->use_cache && i % 100 == 99) (void)mr_store_sweep(store, 50, 0);
    }
    uint64_t t_end = bench_now_ns();
    bench_sink += acc;

    result res;
    memset(&res, 0, sizeof res);
    res.name = sc->name;
    res.total_s = (double)(t_end - t_start) / 1e9;
    res.throughput = (double)c->requests / res.total_s;
    bench_stats st = bench_stats_compute(lat, c->requests);
    res.mean_us = st.mean / 1e3;
    res.p50_us = st.p50 / 1e3;
    res.p95_us = st.p95 / 1e3;
    res.p99_us = st.p99 / 1e3;
    res.max_us = st.max / 1e3;
    res.reads = reads;
    res.writes = writes;
    res.hit_rate = (sc->use_cache && reads) ? (double)hits / (double)reads : 0.0;
    res.origin_calls = og.calls;
    res.origin_avg_ms = og.calls ? (double)og.slept_ns / (double)og.calls / 1e6 : 0.0;
    if (store) {
        mr_store_stats ss;
        mr_store_stats_get(store, &ss);
        res.expired_lazy = ss.expired_lazy + ss.expired_active;
        mr_store_destroy(store);
    }
    free(lat);
    free(version);
    return res;
}

int main(int argc, char **argv) {
    cfg c = {5, 3000, 1000, 1.1, 60000, 300, 10, NULL};
    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        bool more = i + 1 < argc;
        if (!strcmp(a, "--origin-latency-ms") && more) c.origin_ms = bench_arg_long(argv[++i], 5);
        else if (!strcmp(a, "--requests") && more) c.requests = (size_t)bench_arg_long(argv[++i], 3000);
        else if (!strcmp(a, "--keys") && more) c.keys = (size_t)bench_arg_long(argv[++i], 1000);
        else if (!strcmp(a, "--zipf-s") && more) c.zipf_s = strtod(argv[++i], NULL);
        else if (!strcmp(a, "--ttl-long-ms") && more) c.ttl_long_ms = bench_arg_long(argv[++i], 60000);
        else if (!strcmp(a, "--ttl-short-ms") && more) c.ttl_short_ms = bench_arg_long(argv[++i], 300);
        else if (!strcmp(a, "--write-pct") && more) c.write_pct = (int)bench_arg_long(argv[++i], 10);
        else if (!strcmp(a, "--csv") && more) c.csv = argv[++i];
        else {
            fprintf(stderr,
                    "usage: bench_cache [--origin-latency-ms N=5] [--requests N=3000] [--keys N=1000]\n"
                    "                   [--zipf-s S=1.1] [--ttl-long-ms N=60000] [--ttl-short-ms N=300]\n"
                    "                   [--write-pct P=10] [--csv PATH]\n");
            return 2;
        }
    }
    if (c.origin_ms < 0 || c.requests == 0 || c.keys == 0 || c.zipf_s <= 0.0 || c.write_pct < 0 ||
        c.write_pct > 100) {
        fprintf(stderr, "invalid arguments\n");
        return 2;
    }

    bench_zipf z;
    if (bench_zipf_init(&z, c.keys, c.zipf_s) != 0) return 1;

    scenario sc[] = {
        {"A 캐시 미사용(읽기만)", SC_NOCACHE, false, 0, 0},
        {"B 캐시 사용, TTL 김", SC_CACHE_LONG, true, c.ttl_long_ms, 0},
        {"C 캐시 사용, TTL 짧음", SC_CACHE_SHORT, true, c.ttl_short_ms, 0},
        {"D 캐시 미사용(쓰기 혼합)", SC_NOCACHE_W, false, 0, c.write_pct},
        {"E 캐시 사용+invalidate(쓰기 혼합)", SC_CACHE_W, true, c.ttl_long_ms, c.write_pct},
    };
    enum { NSC = 5 };
    result res[NSC];

    printf("== bench_cache: cache-aside, 단일 클라이언트 순차 요청, 실제 nanosleep 원본 지연\n");
    printf("설정: origin=%ldms requests=%zu keys=%zu zipf s=%.2f ttl_long=%ldms ttl_short=%ldms "
           "write_pct=%d (D/E)\n",
           c.origin_ms, c.requests, c.keys, c.zipf_s, c.ttl_long_ms, c.ttl_short_ms, c.write_pct);
    printf("예상 최대 소요(미사용 2개 시나리오 기준): ~%.0f s\n\n",
           (double)c.requests * (double)c.origin_ms / 1000.0 * 2.0);
    fflush(stdout);

    for (int i = 0; i < NSC; i++) {
        res[i] = run_scenario(&c, &sc[i], &z);
        printf("  [%d/%d] %-34s %7.2f s\n", i + 1, NSC, sc[i].name, res[i].total_s);
        fflush(stdout);
    }
    /* 속도 향상: B,C 는 A 대비, E 는 D 대비. A,D 는 1.0 */
    res[0].speedup = 1.0;
    res[1].speedup = res[0].total_s / res[1].total_s;
    res[2].speedup = res[0].total_s / res[2].total_s;
    res[3].speedup = 1.0;
    res[4].speedup = res[3].total_s / res[4].total_s;

    printf("\n%-34s %8s %10s %9s %9s %9s %9s %9s %7s %8s %8s\n", "scenario", "total_s", "req/s",
           "mean_us", "p50_us", "p95_us", "p99_us", "max_us", "hit%", "origin#", "speedup");
    bench_rule(140);
    for (int i = 0; i < NSC; i++) {
        const result *r = &res[i];
        printf("%-34s %8.2f %10.0f %9.1f %9.1f %9.1f %9.1f %9.1f %6.1f%% %8llu %7.2fx\n", r->name,
               r->total_s, r->throughput, r->mean_us, r->p50_us, r->p95_us, r->p99_us, r->max_us,
               r->hit_rate * 100.0, (unsigned long long)r->origin_calls, r->speedup);
    }
    bench_rule(140);
    printf("speedup: B,C 는 A 대비, E 는 D 대비(같은 요청열). 쓰기 혼합 시나리오의 읽기/쓰기 수: %llu/%llu\n",
           (unsigned long long)res[3].reads, (unsigned long long)res[3].writes);
    printf("실측 평균 원본 지연(요청 %ldms): A=%.3fms B=%.3fms C=%.3fms D=%.3fms E=%.3fms\n", c.origin_ms,
           res[0].origin_avg_ms, res[1].origin_avg_ms, res[2].origin_avg_ms, res[3].origin_avg_ms,
           res[4].origin_avg_ms);
    printf("만료로 회수된 키 수(lazy+active): B=%llu C=%llu E=%llu\n",
           (unsigned long long)res[1].expired_lazy, (unsigned long long)res[2].expired_lazy,
           (unsigned long long)res[4].expired_lazy);
    printf("(sink=%llu, 시계 역행 감지 %llu 회)\n", (unsigned long long)bench_sink,
           (unsigned long long)bench_backward);

    if (c.csv) {
        FILE *f = fopen(c.csv, "w");
        if (!f) {
            fprintf(stderr, "cannot open %s: %s\n", c.csv, strerror(errno));
            bench_zipf_free(&z);
            return 1;
        }
        fprintf(f, "scenario,total_s,req_per_s,mean_us,p50_us,p95_us,p99_us,max_us,hit_rate,origin_calls,"
                   "reads,writes,speedup\n");
        for (int i = 0; i < NSC; i++) {
            const result *r = &res[i];
            fprintf(f, "\"%s\",%.4f,%.1f,%.2f,%.2f,%.2f,%.2f,%.2f,%.4f,%llu,%llu,%llu,%.3f\n", r->name,
                    r->total_s, r->throughput, r->mean_us, r->p50_us, r->p95_us, r->p99_us, r->max_us,
                    r->hit_rate, (unsigned long long)r->origin_calls, (unsigned long long)r->reads,
                    (unsigned long long)r->writes, r->speedup);
        }
        fclose(f);
        printf("CSV 저장: %s\n", c.csv);
    }
    bench_zipf_free(&z);
    return 0;
}
