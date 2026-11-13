#ifndef MINIREDIS_BENCH_COMMON_H
#define MINIREDIS_BENCH_COMMON_H

/*
 * 벤치마크 공용 유틸(헤더 온리, 싱글 스레드).
 *  - 단조 시계(CLOCK_MONOTONIC) 나노초 타이머
 *  - 정렬 기반 통계(평균/p50/p95/p99/max)
 *  - 고정 시드 xorshift64* 난수와 Zipf 샘플러(실행마다 같은 요청열 → 시나리오 간 공정 비교)
 *  - 최적화 제거 방지용 volatile sink
 * 모든 함수는 static 이라 두 벤치 바이너리가 각자 필요한 것만 쓴다.
 */

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* 왜 volatile: 결과를 읽지 않는 루프를 컴파일러가 통째로 지우면 처리량이 무한대로 나온다. */
static volatile uint64_t bench_sink;

static uint64_t bench_backward; /* 시계가 거꾸로 간 횟수(가상화 환경 진단용) */

static inline uint64_t bench_now_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}


/* 경과 시간(ns). VM 에서 코어 이동 시 단조 시계가 수 us 역행하는 것이 관찰되어(실측),
 * 부호 없는 뺄셈이 2^64 근처로 터지지 않게 0 으로 자르고 횟수를 센다. */
static inline uint64_t bench_since(uint64_t a) {
    uint64_t b = bench_now_ns();
    if (b < a) {
        bench_backward++;
        return 0;
    }
    return b - a;
}
/* ---- 결정적 난수 ---- */
typedef struct bench_rng {
    uint64_t s;
} bench_rng;

static inline void bench_rng_seed(bench_rng *r, uint64_t seed) {
    r->s = seed ? seed : 0x9E3779B97F4A7C15ull; /* xorshift 는 상태 0 이 고정점이라 피한다 */
}

static inline uint64_t bench_rng_next(bench_rng *r) {
    uint64_t x = r->s;
    x ^= x >> 12;
    x ^= x << 25;
    x ^= x >> 27;
    r->s = x;
    return x * 0x2545F4914F6CDD1Dull;
}

/* [0,1) 균등 실수: 상위 53비트 사용 */
static inline double bench_rng_unit(bench_rng *r) {
    return (double)(bench_rng_next(r) >> 11) * (1.0 / 9007199254740992.0);
}

/* ---- Zipf: 순위 k(0-base) 의 확률 ∝ 1/(k+1)^s. 누적분포 + 이분 탐색 ---- */
typedef struct bench_zipf {
    size_t n;
    double *cdf;
} bench_zipf;

static inline int bench_zipf_init(bench_zipf *z, size_t n, double s) {
    z->n = n;
    z->cdf = (double *)malloc(n * sizeof(double));
    if (!z->cdf) return -1;
    double sum = 0.0;
    for (size_t k = 0; k < n; k++) {
        sum += 1.0 / pow((double)(k + 1), s);
        z->cdf[k] = sum;
    }
    for (size_t k = 0; k < n; k++) z->cdf[k] /= sum;
    return 0;
}

static inline size_t bench_zipf_next(const bench_zipf *z, bench_rng *r) {
    double u = bench_rng_unit(r);
    size_t lo = 0, hi = z->n - 1;
    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2;
        if (z->cdf[mid] < u) lo = mid + 1;
        else hi = mid;
    }
    return lo;
}

static inline void bench_zipf_free(bench_zipf *z) {
    free(z->cdf);
    z->cdf = NULL;
}

/* ---- 통계 ---- */
typedef struct bench_stats {
    size_t n;
    double mean, p50, p95, p99, p999, max, min;
} bench_stats;

static int bench_cmp_u64(const void *a, const void *b) {
    uint64_t x = *(const uint64_t *)a, y = *(const uint64_t *)b;
    return (x > y) - (x < y);
}

static int bench_cmp_dbl(const void *a, const void *b) {
    double x = *(const double *)a, y = *(const double *)b;
    return (x > y) - (x < y);
}

/* nearest-rank 백분위(정렬된 배열). */
static inline uint64_t bench_pct_sorted(const uint64_t *v, size_t n, double p) {
    size_t rank = (size_t)ceil(p / 100.0 * (double)n);
    if (rank < 1) rank = 1;
    if (rank > n) rank = n;
    return v[rank - 1];
}

/* v 를 제자리 정렬한다(호출 후 순서 바뀜). 단위는 입력과 동일(보통 ns). */
static inline bench_stats bench_stats_compute(uint64_t *v, size_t n) {
    bench_stats st;
    memset(&st, 0, sizeof st);
    if (n == 0) return st;
    qsort(v, n, sizeof v[0], bench_cmp_u64);
    long double sum = 0;
    for (size_t i = 0; i < n; i++) sum += (long double)v[i];
    st.n = n;
    st.mean = (double)(sum / (long double)n);
    st.min = (double)v[0];
    st.p50 = (double)bench_pct_sorted(v, n, 50.0);
    st.p95 = (double)bench_pct_sorted(v, n, 95.0);
    st.p99 = (double)bench_pct_sorted(v, n, 99.0);
    st.p999 = (double)bench_pct_sorted(v, n, 99.9);
    st.max = (double)v[n - 1];
    return st;
}

/* 반복 측정값의 중앙값(짝수면 두 중앙값 평균). v 는 제자리 정렬됨. */
static inline double bench_median(double *v, size_t n) {
    if (n == 0) return 0.0;
    qsort(v, n, sizeof v[0], bench_cmp_dbl);
    return (n % 2) ? v[n / 2] : (v[n / 2 - 1] + v[n / 2]) / 2.0;
}

/* ---- 출력 ---- */
static inline void bench_rule(int width) {
    for (int i = 0; i < width; i++) putchar('-');
    putchar('\n');
}

/* 명령행 정수 파싱(실패 시 def). */
static inline long bench_arg_long(const char *s, long def) {
    char *end = NULL;
    long v = strtol(s, &end, 10);
    return (end && *end == '\0' && end != s) ? v : def;
}

#endif /* MINIREDIS_BENCH_COMMON_H */
