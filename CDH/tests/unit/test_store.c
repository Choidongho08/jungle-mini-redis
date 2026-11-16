#include "minitest.h"
#include "miniredis/store.h"

/* ---------- 가짜 시계 ---------- */

typedef struct fake_clock {
    int64_t wall;
    int64_t mono;
    int64_t mono_step; /* mono 를 읽을 때마다 증가시킬 양(시간 예산 시험용) */
} fake_clock;

static int64_t fake_wall(void *ctx) { return ((fake_clock *)ctx)->wall; }
static int64_t fake_mono(void *ctx) {
    fake_clock *c = ctx;
    c->mono += c->mono_step;
    return c->mono;
}

typedef struct env {
    fake_clock fc;
    mr_clock clk;
    mr_store *s;
} env;

static bool env_init(env *e) {
    e->fc = (fake_clock){1000000, 0, 0};
    e->clk = (mr_clock){fake_wall, fake_mono, &e->fc};
    mr_store_opts o = {0};
    o.clock = &e->clk;
    e->s = NULL;
    return mr_store_create(&o, &e->s) == MR_OK;
}

#define K(str) (str), (sizeof(str) - 1)

static bool val_is(mr_store *s, const char *k, const char *expect) {
    const void *v;
    size_t n;
    if (mr_store_get(s, k, strlen(k), &v, &n) != MR_OK) {
        return false;
    }
    return n == strlen(expect) && memcmp(v, expect, n) == 0;
}

static mr_status set_str(mr_store *s, const char *k, const char *v, const mr_set_opts *o) {
    return mr_store_set(s, k, strlen(k), v, strlen(v), o);
}

/* ---------- 기본 ---------- */

TEST(test_set_get_del_exists) {
    env e;
    ASSERT(env_init(&e));
    mr_store *s = e.s;
    CHECK(mr_store_get(s, K("a"), NULL, NULL) == MR_NOT_FOUND);
    CHECK(set_str(s, "a", "1", NULL) == MR_OK);
    CHECK(val_is(s, "a", "1"));
    CHECK(set_str(s, "a", "two", NULL) == MR_OK);
    CHECK(val_is(s, "a", "two"));
    CHECK(mr_store_exists(s, K("a")));
    CHECK_EQ(mr_store_size(s), 1);
    CHECK(mr_store_del(s, K("a")) == MR_OK);
    CHECK(mr_store_del(s, K("a")) == MR_NOT_FOUND);
    CHECK(!mr_store_exists(s, K("a")));

    /* 바이너리 값과 빈 값, 빈 키 */
    const char bin[] = {'x', '\0', '\r', '\n', 'y'};
    CHECK(mr_store_set(s, K("bin"), bin, sizeof(bin), NULL) == MR_OK);
    const void *v;
    size_t n;
    CHECK(mr_store_get(s, K("bin"), &v, &n) == MR_OK);
    CHECK(n == sizeof(bin) && memcmp(v, bin, n) == 0);
    CHECK(mr_store_set(s, K("empty"), NULL, 0, NULL) == MR_OK);
    CHECK(mr_store_get(s, K("empty"), &v, &n) == MR_OK);
    CHECK_EQ(n, 0);
    CHECK(mr_store_set(s, NULL, 0, "v", 1, NULL) == MR_OK);
    CHECK(mr_store_exists(s, NULL, 0));

    CHECK(mr_store_set(s, NULL, 3, "v", 1, NULL) == MR_INVALID_ARG);
    CHECK(mr_store_set(s, K("k"), NULL, 3, NULL) == MR_INVALID_ARG);
    CHECK(mr_store_create(NULL, NULL) == MR_INVALID_ARG);
    mr_store_destroy(s);
    mr_store_destroy(NULL);
}

TEST(test_nx_xx) {
    env e;
    ASSERT(env_init(&e));
    mr_set_opts nx = {MR_SET_NX, MR_EXPIRE_NONE, 0};
    mr_set_opts xx = {MR_SET_XX, MR_EXPIRE_NONE, 0};
    CHECK(set_str(e.s, "k", "1", &xx) == MR_COND_FAILED);
    CHECK(!mr_store_exists(e.s, K("k")));
    CHECK(set_str(e.s, "k", "1", &nx) == MR_OK);
    CHECK(set_str(e.s, "k", "2", &nx) == MR_COND_FAILED);
    CHECK(val_is(e.s, "k", "1"));
    CHECK(set_str(e.s, "k", "3", &xx) == MR_OK);
    CHECK(val_is(e.s, "k", "3"));
    mr_set_opts bad = {(mr_set_cond)99, MR_EXPIRE_NONE, 0};
    CHECK(set_str(e.s, "k", "4", &bad) == MR_INVALID_ARG);
    mr_store_destroy(e.s);
}

/* ---------- TTL ---------- */

TEST(test_px_expiry_boundary_and_lazy) {
    env e;
    ASSERT(env_init(&e));
    mr_set_opts px = {MR_SET_ALWAYS, MR_EXPIRE_PX, 100};
    CHECK(set_str(e.s, "k", "v", &px) == MR_OK);
    int64_t ttl;
    CHECK(mr_store_pttl(e.s, K("k"), &ttl) == MR_OK);
    CHECK_EQ(ttl, 100);
    e.fc.wall += 99;
    CHECK(val_is(e.s, "k", "v"));
    CHECK(mr_store_pttl(e.s, K("k"), &ttl) == MR_OK);
    CHECK_EQ(ttl, 1);
    e.fc.wall += 1; /* 정확히 만료 시각: 보이지 않아야 함 */
    CHECK_EQ(mr_store_size(e.s), 1); /* 아직 회수 전 */
    CHECK(mr_store_get(e.s, K("k"), NULL, NULL) == MR_NOT_FOUND);
    CHECK_EQ(mr_store_size(e.s), 0); /* lazy 만료로 회수됨 */
    mr_store_stats st;
    mr_store_stats_get(e.s, &st);
    CHECK_EQ(st.expired_lazy, 1);
    CHECK_EQ(st.volatile_keys, 0);
    CHECK(mr_store_pttl(e.s, K("k"), &ttl) == MR_NOT_FOUND);

    /* 만료 직후 재설정: NX 가 성공해야 하고 TTL 은 없음 */
    mr_set_opts nx = {MR_SET_NX, MR_EXPIRE_NONE, 0};
    CHECK(set_str(e.s, "k", "new", &px) == MR_OK);
    e.fc.wall += 100;
    CHECK(set_str(e.s, "k", "again", &nx) == MR_OK);
    CHECK(val_is(e.s, "k", "again"));
    CHECK(mr_store_pttl(e.s, K("k"), &ttl) == MR_OK);
    CHECK_EQ(ttl, -1);
    mr_store_destroy(e.s);
}

TEST(test_set_ttl_edge_values) {
    env e;
    ASSERT(env_init(&e));
    mr_set_opts o = {MR_SET_ALWAYS, MR_EXPIRE_PX, 0};
    CHECK(set_str(e.s, "k", "v", &o) == MR_INVALID_ARG);
    o.expire = -5;
    CHECK(set_str(e.s, "k", "v", &o) == MR_INVALID_ARG);
    o.expire = INT64_MAX;
    CHECK(set_str(e.s, "k", "v", &o) == MR_OVERFLOW);
    CHECK(!mr_store_exists(e.s, K("k")));

    o.expire_kind = MR_EXPIRE_PXAT;
    o.expire = INT64_MAX; /* 사실상 무기한이지만 TTL 있는 키 */
    CHECK(set_str(e.s, "k", "v", &o) == MR_OK);
    int64_t ttl;
    CHECK(mr_store_pttl(e.s, K("k"), &ttl) == MR_OK);
    CHECK_EQ(ttl, INT64_MAX - e.fc.wall);
    o.expire = 0;
    CHECK(set_str(e.s, "k", "v", &o) == MR_INVALID_ARG);

    /* 과거 절대 시각: 기존 키까지 지워진다 */
    o.expire = e.fc.wall - 1;
    CHECK(set_str(e.s, "k", "v", &o) == MR_OK);
    CHECK(!mr_store_exists(e.s, K("k")));
    o.expire = e.fc.wall; /* 정확히 지금 = 이미 만료 */
    CHECK(set_str(e.s, "k2", "v", &o) == MR_OK);
    CHECK(!mr_store_exists(e.s, K("k2")));

    /* 일반 SET 은 TTL 을 지운다 */
    o.expire = e.fc.wall + 1000;
    CHECK(set_str(e.s, "k", "v", &o) == MR_OK);
    CHECK(set_str(e.s, "k", "v2", NULL) == MR_OK);
    CHECK(mr_store_pttl(e.s, K("k"), &ttl) == MR_OK);
    CHECK_EQ(ttl, -1);
    mr_store_stats st;
    mr_store_stats_get(e.s, &st);
    CHECK_EQ(st.volatile_keys, 0);
    mr_store_destroy(e.s);
}

TEST(test_expire_ttl_persist) {
    env e;
    ASSERT(env_init(&e));
    mr_store *s = e.s;
    int64_t t;
    CHECK(mr_store_expire(s, K("none"), 10) == MR_NOT_FOUND);
    CHECK(mr_store_ttl(s, K("none"), &t) == MR_NOT_FOUND);
    CHECK(mr_store_persist(s, K("none")) == MR_NOT_FOUND);

    CHECK(set_str(s, "k", "v", NULL) == MR_OK);
    CHECK(mr_store_ttl(s, K("k"), &t) == MR_OK);
    CHECK_EQ(t, -1);
    CHECK(mr_store_persist(s, K("k")) == MR_COND_FAILED);

    CHECK(mr_store_expire(s, K("k"), 10) == MR_OK);
    CHECK(mr_store_ttl(s, K("k"), &t) == MR_OK);
    CHECK_EQ(t, 10);
    e.fc.wall += 8500; /* 1500ms 남음 -> 반올림 2초 */
    CHECK(mr_store_ttl(s, K("k"), &t) == MR_OK);
    CHECK_EQ(t, 2);
    CHECK(mr_store_pttl(s, K("k"), &t) == MR_OK);
    CHECK_EQ(t, 1500);
    CHECK(mr_store_persist(s, K("k")) == MR_OK);
    e.fc.wall += 100000;
    CHECK(mr_store_exists(s, K("k")));

    CHECK(mr_store_pexpire(s, K("k"), 50) == MR_OK);
    CHECK(mr_store_pttl(s, K("k"), &t) == MR_OK);
    CHECK_EQ(t, 50);
    CHECK(mr_store_pexpireat(s, K("k"), e.fc.wall + 7) == MR_OK);
    CHECK(mr_store_pttl(s, K("k"), &t) == MR_OK);
    CHECK_EQ(t, 7);
    CHECK(mr_store_pexpireat(s, K("k"), 0) == MR_INVALID_ARG);

    /* 0·음수 TTL 은 즉시 삭제 */
    CHECK(mr_store_expire(s, K("k"), 0) == MR_OK);
    CHECK(!mr_store_exists(s, K("k")));
    CHECK(set_str(s, "k", "v", NULL) == MR_OK);
    CHECK(mr_store_pexpire(s, K("k"), -1) == MR_OK);
    CHECK(!mr_store_exists(s, K("k")));
    CHECK(set_str(s, "k", "v", NULL) == MR_OK);
    CHECK(mr_store_pexpireat(s, K("k"), e.fc.wall - 1) == MR_OK);
    CHECK(!mr_store_exists(s, K("k")));

    /* 오버플로 */
    CHECK(set_str(s, "k", "v", NULL) == MR_OK);
    CHECK(mr_store_expire(s, K("k"), INT64_MAX) == MR_OVERFLOW);
    CHECK(mr_store_expire(s, K("k"), INT64_MIN) == MR_OVERFLOW);
    CHECK(mr_store_pexpire(s, K("k"), INT64_MAX) == MR_OVERFLOW);
    CHECK(mr_store_pexpireat(s, K("k"), INT64_MAX) == MR_OK);
    CHECK(mr_store_exists(s, K("k")));
    mr_store_destroy(s);
}

/* ---------- INCR ---------- */

TEST(test_incr_family) {
    env e;
    ASSERT(env_init(&e));
    mr_store *s = e.s;
    int64_t v;
    CHECK(mr_store_incr(s, K("n"), &v) == MR_OK);
    CHECK_EQ(v, 1);
    CHECK(mr_store_incrby(s, K("n"), 41, &v) == MR_OK);
    CHECK_EQ(v, 42);
    CHECK(val_is(s, "n", "42"));
    CHECK(mr_store_decrby(s, K("n"), 50, &v) == MR_OK);
    CHECK_EQ(v, -8);
    CHECK(mr_store_decr(s, K("n"), &v) == MR_OK);
    CHECK_EQ(v, -9);
    CHECK(val_is(s, "n", "-9"));
    CHECK(mr_store_incr(s, K("n"), NULL) == MR_OK);

    static const char *bad[] = {"",   " 1", "1 ",  "+1",   "01", "-0", "abc", "1a",
                                "-",  "1.5", "--1", "0x10", "99999999999999999999",
                                "9223372036854775808", "-9223372036854775809"};
    for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
        CHECK(set_str(s, "b", bad[i], NULL) == MR_OK);
        CHECK(mr_store_incr(s, K("b"), &v) == MR_NOT_INTEGER);
        CHECK(val_is(s, "b", bad[i])); /* 실패 시 변화 없음 */
    }
    const char nul[] = {'1', '\0'};
    CHECK(mr_store_set(s, K("b"), nul, sizeof(nul), NULL) == MR_OK);
    CHECK(mr_store_incr(s, K("b"), &v) == MR_NOT_INTEGER);

    CHECK(set_str(s, "max", "9223372036854775807", NULL) == MR_OK);
    CHECK(mr_store_incr(s, K("max"), &v) == MR_OVERFLOW);
    CHECK(val_is(s, "max", "9223372036854775807"));
    CHECK(set_str(s, "min", "-9223372036854775808", NULL) == MR_OK);
    CHECK(mr_store_decr(s, K("min"), &v) == MR_OVERFLOW);
    CHECK(mr_store_incrby(s, K("min"), INT64_MAX, &v) == MR_OK);
    CHECK_EQ(v, -1);
    CHECK(mr_store_decrby(s, K("zero"), INT64_MIN, &v) == MR_OVERFLOW);
    CHECK(mr_store_incrby(s, K("zero"), INT64_MIN, &v) == MR_OK);
    CHECK_EQ(v, INT64_MIN);
    CHECK(val_is(s, "zero", "-9223372036854775808"));
    CHECK(set_str(s, "z", "0", NULL) == MR_OK);
    CHECK(mr_store_incrby(s, K("z"), 0, &v) == MR_OK);
    CHECK_EQ(v, 0);

    /* INCR 은 TTL 을 유지 */
    mr_set_opts px = {MR_SET_ALWAYS, MR_EXPIRE_PX, 1000};
    CHECK(set_str(s, "t", "5", &px) == MR_OK);
    CHECK(mr_store_incr(s, K("t"), &v) == MR_OK);
    int64_t ttl;
    CHECK(mr_store_pttl(s, K("t"), &ttl) == MR_OK);
    CHECK_EQ(ttl, 1000);
    /* 만료된 카운터는 0 부터 다시 */
    e.fc.wall += 1000;
    CHECK(mr_store_incr(s, K("t"), &v) == MR_OK);
    CHECK_EQ(v, 1);
    CHECK(mr_store_pttl(s, K("t"), &ttl) == MR_OK);
    CHECK_EQ(ttl, -1);
    mr_store_destroy(s);
}

/* ---------- 순회 / 무효화 ---------- */

typedef struct collect {
    size_t n;
    char names[64][16];
} collect;

static bool collect_cb(const void *k, size_t kl, const void *v, size_t vl, int64_t at, void *ud) {
    (void)v;
    (void)vl;
    (void)at;
    collect *c = ud;
    if (c->n < 64 && kl < 16) {
        memcpy(c->names[c->n], k, kl);
        c->names[c->n][kl] = '\0';
        c->n++;
    }
    return true;
}

static bool collected(const collect *c, const char *name) {
    for (size_t i = 0; i < c->n; i++) {
        if (strcmp(c->names[i], name) == 0) {
            return true;
        }
    }
    return false;
}

static bool stop_cb(const void *k, size_t kl, const void *v, size_t vl, int64_t at, void *ud) {
    (void)k;
    (void)kl;
    (void)v;
    (void)vl;
    (void)at;
    (*(size_t *)ud)++;
    return false;
}

TEST(test_keys_and_scan) {
    env e;
    ASSERT(env_init(&e));
    mr_store *s = e.s;
    const char *names[] = {"user:1", "user:2", "user:10", "session:a", "cfg"};
    for (size_t i = 0; i < 5; i++) {
        ASSERT(set_str(s, names[i], "v", NULL) == MR_OK);
    }
    mr_set_opts px = {MR_SET_ALWAYS, MR_EXPIRE_PX, 10};
    ASSERT(set_str(s, "user:dead", "v", &px) == MR_OK);
    e.fc.wall += 10;

    collect c = {0};
    CHECK(mr_store_keys(s, K("user:?"), collect_cb, &c) == MR_OK);
    CHECK_EQ(c.n, 2);
    CHECK(collected(&c, "user:1") && collected(&c, "user:2"));
    c.n = 0;
    CHECK(mr_store_keys(s, NULL, 0, collect_cb, &c) == MR_OK);
    CHECK_EQ(c.n, 5); /* 만료 키는 제외 */
    CHECK(!collected(&c, "user:dead"));

    size_t calls = 0;
    CHECK(mr_store_keys(s, NULL, 0, stop_cb, &calls) == MR_OK);
    CHECK_EQ(calls, 1);

    c.n = 0;
    uint64_t cur = 0;
    size_t rounds = 0;
    do {
        CHECK(mr_store_scan(s, cur, K("user:*"), 1, collect_cb, &c, &cur) == MR_OK);
        rounds++;
    } while (cur != 0 && rounds < 1000);
    CHECK_EQ(cur, 0);
    CHECK_EQ(c.n, 3);
    CHECK(collected(&c, "user:10"));
    CHECK(mr_store_scan(s, 0, NULL, 0, 1, NULL, NULL, &cur) == MR_INVALID_ARG);
    mr_store_destroy(s);
}

TEST(test_invalidate_and_flushall) {
    env e;
    ASSERT(env_init(&e));
    mr_store *s = e.s;
    char k[32];
    for (int i = 0; i < 500; i++) {
        int n = snprintf(k, sizeof(k), "cache:%d", i);
        ASSERT(mr_store_set(s, k, (size_t)n, "v", 1, NULL) == MR_OK);
        n = snprintf(k, sizeof(k), "keep:%d", i);
        ASSERT(mr_store_set(s, k, (size_t)n, "v", 1, NULL) == MR_OK);
    }
    ASSERT(set_str(s, "cache:x[1]", "v", NULL) == MR_OK);
    size_t del = 0;
    CHECK(mr_store_invalidate(s, K("cache:1*"), &del) == MR_OK);
    CHECK_EQ(del, 111); /* 1, 10-19, 100-199 */
    CHECK(mr_store_invalidate(s, K("cache:x\\[1\\]"), &del) == MR_OK);
    CHECK_EQ(del, 1);
    CHECK(mr_store_invalidate_prefix(s, K("cache:"), &del) == MR_OK);
    CHECK_EQ(del, 389);
    CHECK(mr_store_invalidate_prefix(s, K("cache:"), &del) == MR_OK);
    CHECK_EQ(del, 0);
    CHECK_EQ(mr_store_size(s), 500);
    CHECK(mr_store_exists(s, K("keep:499")));
    CHECK(mr_store_invalidate(s, NULL, 3, &del) == MR_INVALID_ARG);

    mr_set_opts px = {MR_SET_ALWAYS, MR_EXPIRE_PX, 5};
    ASSERT(set_str(s, "t", "v", &px) == MR_OK);
    mr_store_flushall(s);
    CHECK_EQ(mr_store_size(s), 0);
    mr_store_stats st;
    mr_store_stats_get(s, &st);
    CHECK_EQ(st.volatile_keys, 0);
    CHECK(set_str(s, "after", "v", NULL) == MR_OK);
    CHECK(mr_store_exists(s, K("after")));
    /* 빈 접두사 = 전부 */
    CHECK(mr_store_invalidate_prefix(s, NULL, 0, &del) == MR_OK);
    CHECK_EQ(del, 1);
    mr_store_destroy(s);
}

TEST(test_stats) {
    env e;
    ASSERT(env_init(&e));
    mr_store *s = e.s;
    mr_store_stats st;
    mr_store_stats_get(s, &st);
    size_t mem0 = st.memory_bytes;
    CHECK(set_str(s, "a", "1", NULL) == MR_OK);
    mr_set_opts px = {MR_SET_ALWAYS, MR_EXPIRE_PX, 10};
    CHECK(set_str(s, "b", "2", &px) == MR_OK);
    CHECK(val_is(s, "a", "1"));
    CHECK(!val_is(s, "zz", ""));
    CHECK(mr_store_del(s, K("a")) == MR_OK);
    mr_store_stats_get(s, &st);
    CHECK_EQ(st.sets, 2);
    CHECK_EQ(st.hits, 1);
    CHECK_EQ(st.misses, 1);
    CHECK_EQ(st.deletes, 1);
    CHECK_EQ(st.keys, 1);
    CHECK_EQ(st.volatile_keys, 1);
    CHECK(st.memory_bytes > mem0);
    e.fc.wall += 10;
    CHECK(mr_store_sweep(s, 100, 0) == 1);
    mr_store_stats_get(s, &st);
    CHECK_EQ(st.expired_active, 1);
    CHECK_EQ(st.keys, 0);
    CHECK_EQ(st.volatile_keys, 0);
    mr_store_destroy(s);
}

/* ---------- 능동 만료 ---------- */

TEST(test_sweep_samples_and_progress) {
    env e;
    ASSERT(env_init(&e));
    mr_store *s = e.s;
    enum { N = 2000 };
    char k[32];
    mr_set_opts px = {MR_SET_ALWAYS, MR_EXPIRE_PX, 100};
    for (int i = 0; i < N; i++) {
        int n = snprintf(k, sizeof(k), "v:%d", i);
        ASSERT(mr_store_set(s, k, (size_t)n, "x", 1, &px) == MR_OK);
        n = snprintf(k, sizeof(k), "p:%d", i);
        ASSERT(mr_store_set(s, k, (size_t)n, "x", 1, NULL) == MR_OK);
    }
    CHECK_EQ(mr_store_sweep(s, 1000, 0), 0); /* 아직 만료 전 */
    e.fc.wall += 100;
    size_t first = mr_store_sweep(s, 50, 0);
    CHECK(first > 0);
    CHECK(first < N); /* 표본 한도 때문에 한 번에 다 지우지 않음 */
    size_t total = first;
    for (int r = 0; r < 100000 && total < N; r++) {
        total += mr_store_sweep(s, 50, 0);
    }
    CHECK_EQ(total, N);
    CHECK_EQ(mr_store_size(s), N);
    mr_store_stats st;
    mr_store_stats_get(s, &st);
    CHECK_EQ(st.volatile_keys, 0);
    CHECK_EQ(st.expired_active, N);
    CHECK_EQ(mr_store_sweep(s, SIZE_MAX, 0), 0); /* TTL 키 없음: 즉시 0 */
    CHECK_EQ(mr_store_sweep(s, 0, 0), 0);
    mr_store_destroy(s);
}

TEST(test_sweep_time_budget) {
    env e;
    ASSERT(env_init(&e));
    mr_store *s = e.s;
    enum { N = 20000 };
    char k[32];
    mr_set_opts px = {MR_SET_ALWAYS, MR_EXPIRE_PX, 1};
    for (int i = 0; i < N; i++) {
        int n = snprintf(k, sizeof(k), "v:%d", i);
        ASSERT(mr_store_set(s, k, (size_t)n, "x", 1, &px) == MR_OK);
    }
    e.fc.wall += 1;
    e.fc.mono_step = 1; /* 단조 시계를 읽을 때마다 1ms 흐름 */
    size_t got = mr_store_sweep(s, SIZE_MAX, 3);
    CHECK(got > 0);
    CHECK(got < N);
    e.fc.mono_step = 0;
    size_t rest = mr_store_sweep(s, SIZE_MAX, 0);
    CHECK_EQ(got + rest, N);
    mr_store_destroy(s);
}

TEST(test_loading_mode_preserves_past_ttl) {
    env e;
    ASSERT(env_init(&e));
    mr_store *s = e.s;
    CHECK_EQ(mr_store_set_loading(s, true), 0);
    mr_set_opts at = {MR_SET_ALWAYS, MR_EXPIRE_PXAT, e.fc.wall - 10};
    CHECK(set_str(s, "resurrect", "v", &at) == MR_OK);
    CHECK(set_str(s, "gone", "v", &at) == MR_OK);
    CHECK(mr_store_exists(s, K("gone"))); /* 로딩 중에는 만료 검사 안 함 */
    CHECK(mr_store_persist(s, K("resurrect")) == MR_OK);
    CHECK_EQ(mr_store_sweep(s, 100, 0), 0);
    CHECK_EQ(mr_store_set_loading(s, false), 1);
    CHECK(mr_store_exists(s, K("resurrect")));
    CHECK(!mr_store_exists(s, K("gone")));
    mr_store_destroy(s);
}

TEST(test_large_value) {
    env e;
    ASSERT(env_init(&e));
    enum { SZ = 8 * 1024 * 1024 };
    unsigned char *big = malloc(SZ);
    ASSERT(big != NULL);
    for (size_t i = 0; i < SZ; i++) {
        big[i] = (unsigned char)(i * 31 + 7);
    }
    CHECK(mr_store_set(e.s, K("big"), big, SZ, NULL) == MR_OK);
    const void *v;
    size_t n;
    CHECK(mr_store_get(e.s, K("big"), &v, &n) == MR_OK);
    CHECK(n == SZ && memcmp(v, big, SZ) == 0);
    mr_store_stats st;
    mr_store_stats_get(e.s, &st);
    CHECK(st.memory_bytes > SZ);
    CHECK(mr_store_del(e.s, K("big")) == MR_OK);
    mr_store_stats_get(e.s, &st);
    CHECK(st.memory_bytes < SZ);
    free(big);
    mr_store_destroy(e.s);
}

TEST(test_alloc_failure_injection) {
    unsigned long n;
    for (n = 1; n < 5000; n++) {
        unsigned long before = mt_alloc_failures_total();
        fake_clock fc = {1000, 0, 0};
        mr_clock clk = {fake_wall, fake_mono, &fc};
        mr_store_opts o = {0};
        o.clock = &clk;
        mt_fail_alloc_after(n);
        mr_store *s = NULL;
        mr_status st = mr_store_create(&o, &s);
        if (st == MR_OK) {
            char k[32];
            for (int i = 0; i < 40; i++) {
                int kn = snprintf(k, sizeof(k), "k%d", i);
                st = mr_store_set(s, k, (size_t)kn, "value", 5, NULL);
                CHECK(st == MR_OK || st == MR_NOMEM);
                if (st == MR_NOMEM) {
                    mr_store_stats sst;
                    mr_store_stats_get(s, &sst);
                    CHECK(!mr_store_exists(s, k, (size_t)kn));
                }
            }
            st = mr_store_incr(s, K("k0"), NULL);
            CHECK(st == MR_OK || st == MR_NOMEM || st == MR_NOT_INTEGER);
        } else {
            CHECK(st == MR_NOMEM);
        }
        mt_alloc_reset();
        mr_store_destroy(s);
        if (mt_alloc_failures_total() == before) {
            break;
        }
    }
    CHECK(n > 40);
}

MT_MAIN(test_set_get_del_exists, test_nx_xx, test_px_expiry_boundary_and_lazy,
        test_set_ttl_edge_values, test_expire_ttl_persist, test_incr_family, test_keys_and_scan,
        test_invalidate_and_flushall, test_stats, test_sweep_samples_and_progress,
        test_sweep_time_budget, test_loading_mode_preserves_past_ttl, test_large_value,
        test_alloc_failure_injection)
