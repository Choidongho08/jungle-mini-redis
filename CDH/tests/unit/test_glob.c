#include <time.h>

#include "minitest.h"
#include "miniredis/glob.h"

static bool m(const char *p, const char *s) { return mr_glob_match(p, strlen(p), s, strlen(s)); }

static double elapsed_ms(const struct timespec *a) {
    struct timespec b;
    clock_gettime(CLOCK_MONOTONIC, &b);
    return (double)(b.tv_sec - a->tv_sec) * 1e3 + (double)(b.tv_nsec - a->tv_nsec) / 1e6;
}

TEST(test_literal_and_empty) {
    CHECK(m("", ""));
    CHECK(!m("", "a"));
    CHECK(!m("a", ""));
    CHECK(m("abc", "abc"));
    CHECK(!m("abc", "abd"));
    CHECK(!m("abc", "abcd"));
    CHECK(mr_glob_match(NULL, 0, NULL, 0));
}

TEST(test_star) {
    CHECK(m("*", ""));
    CHECK(m("*", "anything"));
    CHECK(m("a*", "a"));
    CHECK(m("a*", "abc"));
    CHECK(!m("a*", "ba"));
    CHECK(m("*c", "abc"));
    CHECK(m("a*c", "ac"));
    CHECK(m("a*c", "abbbc"));
    CHECK(!m("a*c", "abcb"));
    CHECK(m("a**c", "abc"));
    CHECK(m("user:*:name", "user:42:name"));
    CHECK(m("*a*b*", "xxaxxbxx"));
    CHECK(!m("*a*b*", "xxbxxaxx"));
}

TEST(test_question) {
    CHECK(m("?", "a"));
    CHECK(!m("?", ""));
    CHECK(!m("?", "ab"));
    CHECK(m("h?llo", "hello"));
    CHECK(m("h?llo", "hallo"));
    CHECK(!m("h?llo", "hllo"));
    CHECK(m("*?", "a"));
    CHECK(!m("*??", "a"));
}

TEST(test_class) {
    CHECK(m("h[ae]llo", "hello"));
    CHECK(m("h[ae]llo", "hallo"));
    CHECK(!m("h[ae]llo", "hillo"));
    CHECK(m("h[^e]llo", "hallo"));
    CHECK(!m("h[^e]llo", "hello"));
    CHECK(m("h[a-b]llo", "hbllo"));
    CHECK(!m("h[a-b]llo", "hcllo"));
    CHECK(m("[z-a]", "m")); /* 역순 범위는 뒤집어 해석 */
    CHECK(m("[0-9][0-9]", "42"));
    CHECK(!m("[]", "a"));
    CHECK(m("[\\]]", "]"));
    CHECK(m("[a\\-z]", "-"));
    CHECK(!m("[a\\-z]", "b"));
    /* 닫히지 않은 '[' 는 패턴 끝에서 집합 종료 */
    CHECK(m("[abc", "b"));
    CHECK(!m("[abc", "d"));
}

TEST(test_escape) {
    CHECK(m("\\*", "*"));
    CHECK(!m("\\*", "a"));
    CHECK(m("\\?", "?"));
    CHECK(m("a\\[b", "a[b"));
    CHECK(m("\\\\", "\\"));
    CHECK(m("abc\\", "abc\\")); /* 끝의 단독 '\' 는 리터럴 */
}

TEST(test_binary_safe) {
    const char pat[] = {'a', '*', '\0', 'z'};
    const char s1[] = {'a', 'x', '\0', 'z'};
    const char s2[] = {'a', 'x', 'y', 'z'};
    CHECK(mr_glob_match(pat, sizeof(pat), s1, sizeof(s1)));
    CHECK(!mr_glob_match(pat, sizeof(pat), s2, sizeof(s2)));
    CHECK(m("\xED\x95\x9C*", "\xED\x95\x9C\xEA\xB8\x80")); /* UTF-8 "한글" */
    CHECK(m("[\x80-\xff]", "\xC3"));                         /* 상위 바이트는 부호 없이 비교 */
}

TEST(test_pathological_patterns_are_fast) {
    enum { N = 20000 };
    char *s = malloc(N + 1);
    ASSERT(s != NULL);
    memset(s, 'a', N);
    s[N] = '\0';
    struct timespec t0;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    /* 재귀 구현이면 지수 시간이 걸리는 패턴들 */
    CHECK(!m("a*a*a*a*a*a*a*a*a*a*a*a*a*a*a*a*b", s));
    CHECK(!m("*a*a*a*a*a*a*a*a*a*a*a*a*a*a*a*a*a*a*a*a*?b", s));
    CHECK(m("*a*a*a*a*a*a*a*a*a*a*a*a*a*a*a*a*a*a*a*a*", s));
    CHECK(!m("****************************************b", s));
    double ms = elapsed_ms(&t0);
    CHECK(ms < 1000.0);
    free(s);
}

MT_MAIN(test_literal_and_empty, test_star, test_question, test_class, test_escape,
        test_binary_safe, test_pathological_patterns_are_fast)
