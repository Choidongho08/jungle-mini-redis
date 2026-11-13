#ifndef MINITEST_H
#define MINITEST_H

/*
 * 최소 단위 테스트 프레임워크(헤더 하나, 테스트 바이너리당 한 번만 include).
 *
 *   TEST(name) { CHECK(x == 1); ASSERT(p != NULL); ... }
 *   MT_MAIN(name1, name2, ...)
 *
 * CHECK 는 실패를 기록하고 계속, ASSERT 는 실패 시 현재 테스트 함수를 끝낸다.
 * 실패 시 파일:줄과 식을 출력하고, 하나라도 실패하면 프로세스는 1 로 종료한다.
 *
 * 할당 실패 주입: 테스트 바이너리는 -Wl,--wrap=malloc,--wrap=calloc,--wrap=realloc
 * 으로 링크된다. mt_fail_alloc_after(n) 은 지금부터 n 번째(1부터) 할당 하나를
 * NULL 로 만든다. 라이브러리 코드는 수정 없이 실제 malloc 경로로 시험된다.
 */

#include <inttypes.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef void (*mt_fn)(void);

typedef struct mt_case {
    const char *name;
    mt_fn fn;
} mt_case;

static int mt_cur_failed;
static unsigned long mt_checks;

static inline bool mt_check(bool ok, const char *expr, const char *file, int line) {
    mt_checks++;
    if (!ok) {
        mt_cur_failed = 1;
        fprintf(stderr, "    %s:%d: check failed: %s\n", file, line, expr);
    }
    return ok;
}

static inline bool mt_check_i64(int64_t a, int64_t b, const char *ea, const char *eb, const char *file,
                         int line) {
    mt_checks++;
    if (a != b) {
        mt_cur_failed = 1;
        fprintf(stderr, "    %s:%d: %s == %s failed: %" PRId64 " != %" PRId64 "\n", file, line,
                ea, eb, a, b);
        return false;
    }
    return true;
}

#define TEST(name) static void name(void)
#define CHECK(cond) ((void)mt_check((cond) ? true : false, #cond, __FILE__, __LINE__))
#define ASSERT(cond)                                                                           \
    do {                                                                                       \
        if (!mt_check((cond) ? true : false, #cond, __FILE__, __LINE__)) {                     \
            return;                                                                            \
        }                                                                                      \
    } while (0)
#define CHECK_EQ(a, b)                                                                         \
    ((void)mt_check_i64((int64_t)(a), (int64_t)(b), #a, #b, __FILE__, __LINE__))
#define ASSERT_EQ(a, b)                                                                        \
    do {                                                                                       \
        if (!mt_check_i64((int64_t)(a), (int64_t)(b), #a, #b, __FILE__, __LINE__)) {           \
            return;                                                                            \
        }                                                                                      \
    } while (0)

/* ---------- 할당 실패 주입 ---------- */

void *__real_malloc(size_t n);
void *__real_calloc(size_t n, size_t sz);
void *__real_realloc(void *p, size_t n);
void *__wrap_malloc(size_t n);
void *__wrap_calloc(size_t n, size_t sz);
void *__wrap_realloc(void *p, size_t n);

static unsigned long mt_alloc_countdown; /* 0 = 비활성 */
static unsigned long mt_alloc_failures;

static inline void mt_fail_alloc_after(unsigned long n) { mt_alloc_countdown = n; }
static inline void mt_alloc_reset(void) { mt_alloc_countdown = 0; }
/* 주입된 실패가 실제로 발생했는지(=시험할 할당 지점이 남았는지) 확인용 */
static inline unsigned long mt_alloc_failures_total(void) { return mt_alloc_failures; }

static inline bool mt_alloc_should_fail(void) {
    if (mt_alloc_countdown == 0) {
        return false;
    }
    if (--mt_alloc_countdown == 0) {
        mt_alloc_failures++;
        return true;
    }
    return false;
}

void *__wrap_malloc(size_t n) { return mt_alloc_should_fail() ? NULL : __real_malloc(n); }
void *__wrap_calloc(size_t n, size_t sz) {
    return mt_alloc_should_fail() ? NULL : __real_calloc(n, sz);
}
void *__wrap_realloc(void *p, size_t n) {
    return mt_alloc_should_fail() ? NULL : __real_realloc(p, n);
}

/* ---------- 실행기 ---------- */

static inline int mt_run(const char *suite, const mt_case *cases, size_t n) {
    size_t failed = 0;
    for (size_t i = 0; i < n; i++) {
        mt_cur_failed = 0;
        mt_alloc_reset();
        cases[i].fn();
        mt_alloc_reset();
        if (mt_cur_failed) {
            failed++;
            fprintf(stderr, "  [FAIL] %s\n", cases[i].name);
        } else {
            printf("  [ OK ] %s\n", cases[i].name);
        }
    }
    printf("%s: %zu tests, %zu passed, %zu failed, %lu checks\n", suite, n, n - failed, failed,
           mt_checks);
    fflush(stdout);
    return failed == 0 ? 0 : 1;
}


/* 가변 인자 매크로로 테스트 목록을 만들기 위해 이름 배열과 함수 배열을 따로 둔다. */
#define MT_MAIN(...)                                                                           \
    int main(void) {                                                                           \
        static const mt_fn fns[] = {__VA_ARGS__};                                              \
        static const char names[] = #__VA_ARGS__;                                              \
        enum { N = sizeof(fns) / sizeof(fns[0]) };                                             \
        mt_case cases[N];                                                                      \
        char buf[sizeof(names)];                                                               \
        memcpy(buf, names, sizeof(names));                                                     \
        char *save = buf;                                                                      \
        for (size_t i = 0; i < (size_t)N; i++) {                                               \
            while (*save == ' ' || *save == ',' || *save == '\n') {                            \
                save++;                                                                        \
            }                                                                                  \
            cases[i].name = save;                                                              \
            while (*save != '\0' && *save != ',' && *save != ' ') {                            \
                save++;                                                                        \
            }                                                                                  \
            if (*save != '\0') {                                                               \
                *save++ = '\0';                                                                \
            }                                                                                  \
            cases[i].fn = fns[i];                                                              \
        }                                                                                      \
        return mt_run(__FILE__, cases, (size_t)N);                                             \
    }

#endif /* MINITEST_H */
