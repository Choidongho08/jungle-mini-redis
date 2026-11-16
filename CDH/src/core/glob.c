#include "miniredis/glob.h"

#include <stdint.h>

/* '[' 로 시작하는 집합 토큰을 평가한다. *next 에 토큰 다음 위치를 돌려준다. */
static bool class_match(const unsigned char *p, size_t plen, size_t pi, unsigned char c,
                        size_t *next) {
    size_t i = pi + 1;
    bool negate = false;
    bool matched = false;

    if (i < plen && p[i] == '^') {
        negate = true;
        i++;
    }
    while (i < plen) {
        if (p[i] == '\\' && i + 1 < plen) {
            matched = matched || p[i + 1] == c;
            i += 2;
        } else if (p[i] == ']') {
            i++;
            break;
        } else if (i + 2 < plen && p[i + 1] == '-') {
            unsigned char lo = p[i];
            unsigned char hi = p[i + 2];
            if (lo > hi) {
                unsigned char t = lo;
                lo = hi;
                hi = t;
            }
            matched = matched || (c >= lo && c <= hi);
            i += 3;
        } else {
            matched = matched || p[i] == c;
            i++;
        }
    }
    *next = i;
    return negate ? !matched : matched;
}

/* '*' 가 아닌 1바이트짜리 토큰(?, [..], \x, 리터럴)이 c 와 맞는지. */
static bool token_match(const unsigned char *p, size_t plen, size_t pi, unsigned char c,
                        size_t *next) {
    switch (p[pi]) {
    case '?':
        *next = pi + 1;
        return true;
    case '[':
        return class_match(p, plen, pi, c, next);
    case '\\':
        if (pi + 1 < plen) {
            *next = pi + 2;
            return p[pi + 1] == c;
        }
        *next = pi + 1;
        return c == '\\';
    default:
        *next = pi + 1;
        return p[pi] == c;
    }
}

bool mr_glob_match(const void *pattern, size_t plen, const void *str, size_t slen) {
    const unsigned char *p = pattern;
    const unsigned char *s = str;
    size_t pi = 0;
    size_t si = 0;
    size_t star_pi = SIZE_MAX; /* 마지막 '*' 다음 패턴 위치 */
    size_t star_si = 0;        /* 그 '*' 가 흡수를 시작한 문자열 위치 */

    while (si < slen) {
        size_t next;
        if (pi < plen && p[pi] == '*') {
            while (pi < plen && p[pi] == '*') {
                pi++;
            }
            if (pi == plen) {
                return true;
            }
            star_pi = pi;
            star_si = si;
            continue;
        }
        if (pi < plen && token_match(p, plen, pi, s[si], &next)) {
            pi = next;
            si++;
            continue;
        }
        if (star_pi == SIZE_MAX) {
            return false;
        }
        /* 마지막 '*' 가 한 바이트 더 흡수하도록 하고 그 뒤부터 다시 맞춘다. */
        star_si++;
        si = star_si;
        pi = star_pi;
    }
    while (pi < plen && p[pi] == '*') {
        pi++;
    }
    return pi == plen;
}
