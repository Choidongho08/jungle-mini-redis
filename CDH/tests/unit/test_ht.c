#include "minitest.h"
#include "miniredis/ht.h"

#define TAG(i) ((void *)(uintptr_t)((size_t)(i) + 1))
#define UNTAG(p) ((size_t)(uintptr_t)(p) - 1)

static uint64_t const_hash(const void *k, size_t n, const uint8_t seed[16]) {
    (void)k;
    (void)n;
    (void)seed;
    return 7;
}

static size_t key_of(char *buf, size_t cap, size_t i) {
    int n = snprintf(buf, cap, "key:%zu", i);
    return (size_t)n;
}

static mr_ht *new_ht(const mr_ht_opts *o) {
    mr_ht *ht = NULL;
    return mr_ht_create(o, &ht) == MR_OK ? ht : NULL;
}

static bool has(mr_ht *ht, size_t i) {
    char k[32];
    void *v = NULL;
    size_t n = key_of(k, sizeof(k), i);
    return mr_ht_get(ht, k, n, &v) == MR_OK && UNTAG(v) == i;
}

static mr_status put_i(mr_ht *ht, size_t i) {
    char k[32];
    size_t n = key_of(k, sizeof(k), i);
    return mr_ht_put(ht, k, n, TAG(i), NULL);
}

static mr_status del_i(mr_ht *ht, size_t i) {
    char k[32];
    size_t n = key_of(k, sizeof(k), i);
    return mr_ht_delete(ht, k, n);
}

static void count_free(void *val, void *ud) {
    (void)val;
    (*(size_t *)ud)++;
}

TEST(test_basic_ops) {
    mr_ht *ht = new_ht(NULL);
    ASSERT(ht != NULL);
    CHECK_EQ(mr_ht_size(ht), 0);
    CHECK_EQ(mr_ht_capacity(ht), 0); /* 첫 삽입까지 버킷을 할당하지 않음 */
    CHECK(mr_ht_get(ht, "a", 1, NULL) == MR_NOT_FOUND);
    CHECK(mr_ht_delete(ht, "a", 1) == MR_NOT_FOUND);

    bool replaced = true;
    CHECK(mr_ht_put(ht, "a", 1, TAG(1), &replaced) == MR_OK);
    CHECK(!replaced);
    CHECK(mr_ht_put(ht, "a", 1, TAG(2), &replaced) == MR_OK);
    CHECK(replaced);
    CHECK_EQ(mr_ht_size(ht), 1);
    void *v = NULL;
    CHECK(mr_ht_get(ht, "a", 1, &v) == MR_OK);
    CHECK_EQ(UNTAG(v), 2);
    CHECK(mr_ht_contains(ht, "a", 1));
    CHECK(!mr_ht_contains(ht, "b", 1));
    CHECK(mr_ht_delete(ht, "a", 1) == MR_OK);
    CHECK_EQ(mr_ht_size(ht), 0);
    CHECK(!mr_ht_contains(ht, "a", 1));

    CHECK(mr_ht_put(ht, NULL, 3, TAG(1), NULL) == MR_INVALID_ARG);
    CHECK(mr_ht_create(NULL, NULL) == MR_INVALID_ARG);
    mr_ht_destroy(ht);
    mr_ht_destroy(NULL);
}

TEST(test_initial_capacity_rounds_to_pow2) {
    mr_ht_opts o = {0};
    o.initial_capacity = 100;
    mr_ht *ht = new_ht(&o);
    ASSERT(ht != NULL);
    CHECK_EQ(mr_ht_capacity(ht), 128);
    mr_ht_destroy(ht);
    o.initial_capacity = SIZE_MAX;
    mr_ht *bad = NULL;
    CHECK(mr_ht_create(&o, &bad) == MR_OVERFLOW);
    CHECK(bad == NULL);
}

TEST(test_special_keys) {
    mr_ht *ht = new_ht(NULL);
    ASSERT(ht != NULL);
    const char nul1[] = {'a', '\0', 'b'};
    const char nul2[] = {'a', '\0', 'c'};
    const char *utf8 = "\xED\x82\xA4:\xEA\xB0\x92"; /* "키:값" */
    enum { BIG = 8192 };
    char *big = malloc(BIG);
    ASSERT(big != NULL);
    memset(big, 'x', BIG);
    big[BIG - 1] = 'y';

    CHECK(mr_ht_put(ht, NULL, 0, TAG(0), NULL) == MR_OK); /* 빈 키 */
    CHECK(mr_ht_put(ht, "", 0, TAG(9), NULL) == MR_OK);   /* 같은 빈 키 → 교체 */
    CHECK(mr_ht_put(ht, nul1, sizeof(nul1), TAG(1), NULL) == MR_OK);
    CHECK(mr_ht_put(ht, nul2, sizeof(nul2), TAG(2), NULL) == MR_OK);
    CHECK(mr_ht_put(ht, "a", 1, TAG(3), NULL) == MR_OK); /* "a\0b" 의 접두 */
    CHECK(mr_ht_put(ht, utf8, strlen(utf8), TAG(4), NULL) == MR_OK);
    CHECK(mr_ht_put(ht, big, BIG, TAG(5), NULL) == MR_OK);
    CHECK_EQ(mr_ht_size(ht), 6);

    void *v;
    CHECK(mr_ht_get(ht, NULL, 0, &v) == MR_OK && UNTAG(v) == 9);
    CHECK(mr_ht_get(ht, nul1, sizeof(nul1), &v) == MR_OK && UNTAG(v) == 1);
    CHECK(mr_ht_get(ht, nul2, sizeof(nul2), &v) == MR_OK && UNTAG(v) == 2);
    CHECK(mr_ht_get(ht, "a", 1, &v) == MR_OK && UNTAG(v) == 3);
    CHECK(mr_ht_get(ht, utf8, strlen(utf8), &v) == MR_OK && UNTAG(v) == 4);
    CHECK(mr_ht_get(ht, big, BIG, &v) == MR_OK && UNTAG(v) == 5);
    big[0] = 'z';
    CHECK(mr_ht_get(ht, big, BIG, &v) == MR_NOT_FOUND); /* 키는 복사되어 저장됨 */
    CHECK(mr_ht_get(ht, big, BIG - 1, &v) == MR_NOT_FOUND);
    free(big);
    mr_ht_destroy(ht);
}

TEST(test_full_collision_chain_integrity) {
    mr_ht_opts o = {0};
    o.hash = const_hash;
    mr_ht *ht = new_ht(&o);
    ASSERT(ht != NULL);
    enum { N = 300 };
    for (size_t i = 0; i < N; i++) {
        ASSERT(put_i(ht, i) == MR_OK);
    }
    CHECK_EQ(mr_ht_size(ht), N);
    for (size_t i = 0; i < N; i++) {
        CHECK(has(ht, i));
    }
    /* 체인은 머리 삽입이라 N-1 이 앞, 0 이 끝: 앞/중간/끝 삭제 */
    CHECK(del_i(ht, N - 1) == MR_OK);
    CHECK(del_i(ht, N / 2) == MR_OK);
    CHECK(del_i(ht, 0) == MR_OK);
    CHECK(del_i(ht, 0) == MR_NOT_FOUND);
    CHECK_EQ(mr_ht_size(ht), N - 3);
    for (size_t i = 0; i < N; i++) {
        bool expect = i != 0 && i != N / 2 && i != N - 1;
        CHECK(has(ht, i) == expect);
    }
    mr_ht_destroy(ht);
}

/* 리해싱이 시작될 때까지 삽입하고 삽입한 개수를 돌려준다. */
static size_t fill_until_rehashing(mr_ht *ht, size_t min) {
    size_t i = 0;
    while (i < min || !mr_ht_is_rehashing(ht)) {
        if (put_i(ht, i) != MR_OK) {
            return 0;
        }
        i++;
    }
    return i;
}

TEST(test_ops_during_incremental_rehash) {
    mr_ht *ht = new_ht(NULL);
    ASSERT(ht != NULL);
    size_t n = fill_until_rehashing(ht, 4000);
    ASSERT(n > 0);
    ASSERT(mr_ht_is_rehashing(ht));
    size_t cap_during = mr_ht_capacity(ht);

    /* 한 번에 다 옮기지 않았음: 몇 번의 연산 뒤에도 여전히 리해싱 중 */
    CHECK(has(ht, 0));
    CHECK(has(ht, n - 1));
    CHECK(mr_ht_is_rehashing(ht));

    /* 리해싱 중 삭제·교체·삽입 */
    for (size_t i = 0; i < n; i += 3) {
        CHECK(del_i(ht, i) == MR_OK);
    }
    for (size_t i = n; i < n + 50; i++) {
        CHECK(put_i(ht, i) == MR_OK);
    }
    for (size_t i = 0; i < n + 50; i++) {
        bool expect = i >= n || i % 3 != 0;
        CHECK(has(ht, i) == expect);
    }
    CHECK(cap_during > 0);
    /* 충분히 많은 연산 후에는 리해싱이 끝나 있어야 함 */
    for (size_t k = 0; k < 4 * n && mr_ht_is_rehashing(ht); k++) {
        (void)mr_ht_contains(ht, "x", 1);
    }
    CHECK(!mr_ht_is_rehashing(ht));
    for (size_t i = 0; i < n + 50; i++) {
        bool expect = i >= n || i % 3 != 0;
        CHECK(has(ht, i) == expect);
    }
    mr_ht_destroy(ht);
}

TEST(test_shrink) {
    mr_ht *ht = new_ht(NULL);
    ASSERT(ht != NULL);
    enum { N = 10000, KEEP = 10 };
    for (size_t i = 0; i < N; i++) {
        ASSERT(put_i(ht, i) == MR_OK);
    }
    size_t big = mr_ht_capacity(ht);
    CHECK(big >= N);
    for (size_t i = KEEP; i < N; i++) {
        CHECK(del_i(ht, i) == MR_OK);
    }
    for (size_t k = 0; k < 1000 && mr_ht_is_rehashing(ht); k++) {
        (void)mr_ht_contains(ht, "x", 1);
    }
    CHECK(!mr_ht_is_rehashing(ht));
    CHECK(mr_ht_capacity(ht) <= 64);
    CHECK_EQ(mr_ht_size(ht), KEEP);
    for (size_t i = 0; i < KEEP; i++) {
        CHECK(has(ht, i));
    }
    /* 전부 지우면 최소 크기로 */
    for (size_t i = 0; i < KEEP; i++) {
        CHECK(del_i(ht, i) == MR_OK);
    }
    for (size_t k = 0; k < 100 && mr_ht_is_rehashing(ht); k++) {
        (void)mr_ht_contains(ht, "x", 1);
    }
    CHECK(mr_ht_capacity(ht) <= 8);
    mr_ht_destroy(ht);
}

TEST(test_large_volume) {
    mr_ht *ht = new_ht(NULL);
    ASSERT(ht != NULL);
    enum { N = 150000 };
    for (size_t i = 0; i < N; i++) {
        ASSERT(put_i(ht, i) == MR_OK);
    }
    CHECK_EQ(mr_ht_size(ht), N);
    size_t misses = 0;
    for (size_t i = 0; i < N; i++) {
        misses += has(ht, i) ? 0 : 1;
    }
    CHECK_EQ(misses, 0);
    CHECK(!has(ht, N));
    size_t mem = mr_ht_memory_usage(ht);
    CHECK(mem > N * sizeof(void *));
    for (size_t i = 0; i < N; i += 2) {
        CHECK(del_i(ht, i) == MR_OK);
    }
    CHECK_EQ(mr_ht_size(ht), N / 2);
    misses = 0;
    for (size_t i = 0; i < N; i++) {
        misses += has(ht, i) == (i % 2 == 1) ? 0 : 1;
    }
    CHECK_EQ(misses, 0);
    CHECK(mr_ht_memory_usage(ht) < mem);
    mr_ht_destroy(ht);
}

TEST(test_value_destructor_counts) {
    size_t freed = 0;
    mr_ht_opts o = {0};
    o.val_free = count_free;
    o.val_free_ud = &freed;
    mr_ht *ht = new_ht(&o);
    ASSERT(ht != NULL);
    for (size_t i = 0; i < 100; i++) {
        ASSERT(put_i(ht, i) == MR_OK);
    }
    CHECK_EQ(freed, 0);
    for (size_t i = 0; i < 10; i++) {
        ASSERT(put_i(ht, i) == MR_OK); /* 같은 값 포인터로 교체: 해제하지 않음 */
    }
    CHECK_EQ(freed, 0);
    char k[32];
    size_t kn = key_of(k, sizeof(k), 5);
    ASSERT(mr_ht_put(ht, k, kn, TAG(999), NULL) == MR_OK); /* 다른 값으로 교체 */
    CHECK_EQ(freed, 1);
    CHECK(del_i(ht, 6) == MR_OK);
    CHECK_EQ(freed, 2);
    CHECK(del_i(ht, 6) == MR_NOT_FOUND);
    CHECK_EQ(freed, 2);
    mr_ht_clear(ht);
    CHECK_EQ(freed, 101);
    CHECK_EQ(mr_ht_size(ht), 0);
    CHECK(put_i(ht, 1) == MR_OK); /* clear 후 재사용 */
    mr_ht_destroy(ht);
    CHECK_EQ(freed, 102);
}

typedef struct seen_ctx {
    unsigned char *seen;
    size_t n;
    size_t calls;
} seen_ctx;

static mr_ht_scan_action mark_seen(const void *key, size_t klen, void *val, void *ud) {
    (void)key;
    (void)klen;
    seen_ctx *c = ud;
    size_t i = UNTAG(val);
    if (i < c->n && c->seen[i] < 255) {
        c->seen[i]++;
    }
    c->calls++;
    return MR_HT_SCAN_KEEP;
}

TEST(test_scan_returns_all_across_growth) {
    mr_ht *ht = new_ht(NULL);
    ASSERT(ht != NULL);
    size_t n = fill_until_rehashing(ht, 1000);
    ASSERT(n > 0 && mr_ht_is_rehashing(ht));
    seen_ctx c = {calloc(n, 1), n, 0};
    ASSERT(c.seen != NULL);
    uint64_t cur = 0;
    size_t extra = n;
    size_t steps = 0;
    do {
        cur = mr_ht_scan(ht, cur, mark_seen, &c);
        /* 스캔 사이에 대량 삽입 → 리해싱 진행 + 추가 확장 */
        for (int j = 0; j < 40 && extra < n + 20000; j++) {
            ASSERT(put_i(ht, extra++) == MR_OK);
        }
        steps++;
    } while (cur != 0 && steps < 10000000);
    size_t missing = 0;
    for (size_t i = 0; i < n; i++) {
        missing += c.seen[i] == 0 ? 1 : 0;
    }
    CHECK_EQ(missing, 0);
    free(c.seen);
    mr_ht_destroy(ht);
}

TEST(test_scan_returns_all_across_shrink) {
    mr_ht *ht = new_ht(NULL);
    ASSERT(ht != NULL);
    enum { N = 20000, KEEP = 500 };
    for (size_t i = 0; i < N; i++) {
        ASSERT(put_i(ht, i) == MR_OK);
    }
    seen_ctx c = {calloc(N, 1), N, 0};
    ASSERT(c.seen != NULL);
    uint64_t cur = 0;
    size_t next_del = KEEP;
    bool saw_rehash = false;
    do {
        cur = mr_ht_scan(ht, cur, mark_seen, &c);
        /* 스캔 사이에 KEEP 이후 키를 지워 축소 리해싱을 일으킨다 */
        for (int j = 0; j < 200 && next_del < N; j++) {
            CHECK(del_i(ht, next_del++) == MR_OK);
        }
        saw_rehash = saw_rehash || mr_ht_is_rehashing(ht);
    } while (cur != 0);
    CHECK(saw_rehash);
    size_t missing = 0;
    for (size_t i = 0; i < KEEP; i++) {
        missing += c.seen[i] == 0 ? 1 : 0;
    }
    CHECK_EQ(missing, 0);
    free(c.seen);
    mr_ht_destroy(ht);
}

TEST(test_scan_without_mutation_is_exact) {
    mr_ht *ht = new_ht(NULL);
    ASSERT(ht != NULL);
    enum { N = 5000 };
    for (size_t i = 0; i < N; i++) {
        ASSERT(put_i(ht, i) == MR_OK);
    }
    seen_ctx c = {calloc(N, 1), N, 0};
    ASSERT(c.seen != NULL);
    uint64_t cur = 0;
    do {
        cur = mr_ht_scan(ht, cur, mark_seen, &c);
    } while (cur != 0);
    size_t bad = 0;
    for (size_t i = 0; i < N; i++) {
        bad += c.seen[i] == 1 ? 0 : 1;
    }
    CHECK_EQ(bad, 0);
    CHECK_EQ(c.calls, N);
    CHECK_EQ(mr_ht_scan(ht, 0, NULL, NULL), 0);
    free(c.seen);
    mr_ht_destroy(ht);

    mr_ht *empty = new_ht(NULL);
    ASSERT(empty != NULL);
    seen_ctx e = {NULL, 0, 0};
    CHECK_EQ(mr_ht_scan(empty, 0, mark_seen, &e), 0);
    CHECK_EQ(e.calls, 0);
    mr_ht_destroy(empty);
}

static mr_ht_scan_action delete_odd(const void *key, size_t klen, void *val, void *ud) {
    (void)key;
    (void)klen;
    (void)ud;
    return UNTAG(val) % 2 == 1 ? MR_HT_SCAN_DELETE : MR_HT_SCAN_KEEP;
}

TEST(test_scan_delete_action) {
    size_t freed = 0;
    mr_ht_opts o = {0};
    o.val_free = count_free;
    o.val_free_ud = &freed;
    mr_ht *ht = new_ht(&o);
    ASSERT(ht != NULL);
    enum { N = 3000 };
    for (size_t i = 0; i < N; i++) {
        ASSERT(put_i(ht, i) == MR_OK);
    }
    uint64_t cur = 0;
    do {
        cur = mr_ht_scan(ht, cur, delete_odd, NULL);
    } while (cur != 0);
    CHECK_EQ(freed, N / 2);
    CHECK_EQ(mr_ht_size(ht), N / 2);
    for (size_t i = 0; i < N; i++) {
        CHECK(has(ht, i) == (i % 2 == 0));
    }
    mr_ht_destroy(ht);
}

static void free_val(void *val, void *ud) {
    (void)ud;
    free(val);
}

/* N 번째 할당을 실패시키며 생성+삽입을 반복: 크래시·누수(ASan) 없이, 실패한 put 은
 * 흔적이 없고 성공한 put 은 모두 남아 있어야 한다. */
TEST(test_alloc_failure_injection) {
    enum { KEYS = 64 };
    unsigned long n;
    for (n = 1; n < 5000; n++) {
        unsigned long before = mt_alloc_failures_total();
        mr_ht_opts o = {0};
        o.val_free = free_val;
        mt_fail_alloc_after(n);
        mr_ht *ht = NULL;
        mr_status st = mr_ht_create(&o, &ht);
        bool ok[KEYS] = {false};
        if (st == MR_OK) {
            for (size_t i = 0; i < KEYS; i++) {
                size_t *v = malloc(sizeof(*v));
                if (v == NULL) {
                    continue;
                }
                *v = i;
                char k[32];
                size_t kn = key_of(k, sizeof(k), i);
                st = mr_ht_put(ht, k, kn, v, NULL);
                CHECK(st == MR_OK || st == MR_NOMEM);
                if (st == MR_OK) {
                    ok[i] = true;
                } else {
                    free(v); /* 실패 시 소유권은 호출자 */
                }
            }
        } else {
            CHECK(st == MR_NOMEM);
            CHECK(ht == NULL);
        }
        mt_alloc_reset();
        if (ht != NULL) {
            size_t cnt = 0;
            for (size_t i = 0; i < KEYS; i++) {
                char k[32];
                size_t kn = key_of(k, sizeof(k), i);
                void *v = NULL;
                bool found = mr_ht_get(ht, k, kn, &v) == MR_OK;
                CHECK(found == ok[i]);
                if (found) {
                    CHECK_EQ(*(size_t *)v, i);
                }
                cnt += ok[i] ? 1 : 0;
            }
            CHECK_EQ(mr_ht_size(ht), cnt);
            mr_ht_destroy(ht);
        }
        if (mt_alloc_failures_total() == before) {
            break; /* 더 이상 실패시킬 할당 지점이 없음: 모든 지점을 시험함 */
        }
    }
    CHECK(n > KEYS); /* 적어도 키 수만큼의 할당 지점을 지나야 함 */
}

MT_MAIN(test_basic_ops, test_initial_capacity_rounds_to_pow2, test_special_keys,
        test_full_collision_chain_integrity, test_ops_during_incremental_rehash, test_shrink,
        test_large_volume, test_value_destructor_counts, test_scan_returns_all_across_growth,
        test_scan_returns_all_across_shrink, test_scan_without_mutation_is_exact,
        test_scan_delete_action, test_alloc_failure_injection)
