#include "util.h"

#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

static void oom(size_t size) {
    fprintf(stderr, "mini-redis: out of memory allocating %zu bytes\n", size);
    abort();
}

void *xmalloc(size_t size) {
    void *p = malloc(size ? size : 1);
    if (p == NULL) oom(size);
    return p;
}

void *xcalloc(size_t n, size_t size) {
    void *p = calloc(n ? n : 1, size ? size : 1);
    if (p == NULL) oom(n * size);
    return p;
}

void *xrealloc(void *ptr, size_t size) {
    void *p = realloc(ptr, size ? size : 1);
    if (p == NULL) oom(size);
    return p;
}

int64_t mstime(void) {
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

int util_str_to_ll(const char *s, size_t len, long long *out) {
    const char *p = s;
    size_t plen = 0;
    int negative = 0;
    unsigned long long v;

    if (len == 0) return 0;

    /* "0" 은 유일하게 0 으로 시작할 수 있는 표현이다. */
    if (len == 1 && p[0] == '0') {
        *out = 0;
        return 1;
    }

    if (p[0] == '-') {
        negative = 1;
        p++;
        plen++;
        if (plen == len) return 0; /* "-" 만 있음 */
    }

    /* 첫 숫자는 1~9 여야 한다. 그래서 "01", "-0", "+1", " 1" 이 모두 거절된다. */
    if (p[0] >= '1' && p[0] <= '9') {
        v = (unsigned long long)(p[0] - '0');
        p++;
        plen++;
    } else {
        return 0;
    }

    while (plen < len && p[0] >= '0' && p[0] <= '9') {
        unsigned long long d = (unsigned long long)(p[0] - '0');
        if (v > ULLONG_MAX / 10) return 0;
        v *= 10;
        if (v > ULLONG_MAX - d) return 0;
        v += d;
        p++;
        plen++;
    }

    /* 숫자가 아닌 문자가 남아 있으면 실패 (뒤쪽 공백 포함) */
    if (plen < len) return 0;

    if (negative) {
        /* LLONG_MIN 의 절댓값은 LLONG_MAX + 1 */
        if (v > (unsigned long long)LLONG_MAX + 1ULL) return 0;
        if (v == (unsigned long long)LLONG_MAX + 1ULL) {
            *out = LLONG_MIN;
        } else {
            *out = -(long long)v;
        }
    } else {
        if (v > (unsigned long long)LLONG_MAX) return 0;
        *out = (long long)v;
    }
    return 1;
}
