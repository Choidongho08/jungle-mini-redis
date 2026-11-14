/* util_str_to_ll 경계값 테스트 (Redis string2ll 규칙) */
#include <limits.h>
#include <stdio.h>
#include <string.h>

#include "util.h"

static int failures = 0;
static int total = 0;

static void expect_ok(const char *s, long long want) {
    long long got = 0;
    total++;
    if (!util_str_to_ll(s, strlen(s), &got) || got != want) {
        failures++;
        fprintf(stderr, "FAIL: \"%s\" -> expected %lld\n", s, want);
    }
}

static void expect_fail_len(const char *s, size_t len) {
    long long got = 0;
    total++;
    if (util_str_to_ll(s, len, &got)) {
        failures++;
        fprintf(stderr, "FAIL: \"%.*s\" should be rejected (got %lld)\n", (int)len, s, got);
    }
}

static void expect_fail(const char *s) { expect_fail_len(s, strlen(s)); }

int main(void) {
    expect_ok("0", 0);
    expect_ok("1", 1);
    expect_ok("-1", -1);
    expect_ok("10", 10);
    expect_ok("1234567890", 1234567890LL);
    expect_ok("9223372036854775807", LLONG_MAX);
    expect_ok("-9223372036854775808", LLONG_MIN);

    expect_fail("");
    expect_fail("-");
    expect_fail("-0");
    expect_fail("01");
    expect_fail("+1");
    expect_fail(" 1");
    expect_fail("1 ");
    expect_fail("1a");
    expect_fail("abc");
    expect_fail("1.5");
    expect_fail("9223372036854775808");
    expect_fail("-9223372036854775809");
    expect_fail("99999999999999999999999");
    /* 길이 기반: NUL 이 섞여 있어도 len 전체를 본다 */
    expect_fail_len("1\0002", 3);

    /* mstime 은 양수이고 단조 감소하지 않아야 한다 (짧은 구간) */
    total++;
    int64_t a = mstime(), b = mstime();
    if (a <= 0 || b < a) {
        failures++;
        fprintf(stderr, "FAIL: mstime %lld %lld\n", (long long)a, (long long)b);
    }

    printf("test_util: %d/%d passed\n", total - failures, total);
    return failures ? 1 : 0;
}
