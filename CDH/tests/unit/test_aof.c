#include <signal.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <unistd.h>

#include "minitest.h"
#include "miniredis/aof.h"

/* ---------- 환경 ---------- */

typedef struct fake_clock {
    int64_t wall;
    int64_t mono;
} fake_clock;

static int64_t fake_wall(void *ctx) { return ((fake_clock *)ctx)->wall; }
static int64_t fake_mono(void *ctx) { return ((fake_clock *)ctx)->mono; }

static fake_clock g_fc = {1700000000000, 0};
static const mr_clock g_clk = {fake_wall, fake_mono, &g_fc};
static char g_dir[256];
static char g_path[300];

static bool setup(const char *name) {
    const char *tmp = getenv("TMPDIR");
    int n = snprintf(g_dir, sizeof(g_dir), "%s/mr_aof_XXXXXX", tmp != NULL ? tmp : "/tmp");
    if (n < 0 || (size_t)n >= sizeof(g_dir) || mkdtemp(g_dir) == NULL) {
        return false;
    }
    snprintf(g_path, sizeof(g_path), "%s/%s", g_dir, name);
    g_fc.wall = 1700000000000;
    g_fc.mono = 0;
    return true;
}

static void teardown(void) {
    char p[400];
    chmod(g_dir, 0755);
    unlink(g_path);
    snprintf(p, sizeof(p), "%s.rewrite.tmp", g_path);
    unlink(p);
    rmdir(g_dir);
}

static mr_store *new_store(void) {
    mr_store_opts o = {0};
    o.clock = &g_clk;
    mr_store *s = NULL;
    return mr_store_create(&o, &s) == MR_OK ? s : NULL;
}

static mr_status open_aof(mr_store *s, mr_aof_fsync policy, mr_aof **out, mr_aof_load_info *i) {
    mr_aof_opts o = {g_path, policy, &g_clk};
    return mr_aof_open(&o, s, out, i);
}

static long file_size(const char *p) {
    struct stat sb;
    return stat(p, &sb) == 0 ? (long)sb.st_size : -1;
}

static bool write_file(const char *p, const void *data, size_t n, const char *mode) {
    FILE *f = fopen(p, mode);
    if (f == NULL) {
        return false;
    }
    bool ok = fwrite(data, 1, n, f) == n;
    return fclose(f) == 0 && ok;
}

static bool val_is(mr_store *s, const void *k, size_t kl, const void *v, size_t vl) {
    const void *got;
    size_t n;
    return mr_store_get(s, k, kl, &got, &n) == MR_OK && n == vl && (vl == 0 || memcmp(got, v, vl) == 0);
}

#define K(str) (str), (sizeof(str) - 1)

/* 재시작을 흉내 낸다: 새 저장소에 같은 파일을 재생 */
static mr_store *reload(mr_aof_load_info *info, mr_status *st_out) {
    mr_store *s = new_store();
    mr_aof *a = NULL;
    mr_status st = s != NULL ? open_aof(s, MR_AOF_FSYNC_NO, &a, info) : MR_NOMEM;
    if (st_out != NULL) {
        *st_out = st;
    }
    if (st != MR_OK) {
        mr_store_destroy(s);
        return NULL;
    }
    mr_aof_close(a);
    return s;
}

/* ---------- 테스트 ---------- */

TEST(test_roundtrip_all_commands) {
    ASSERT(setup("a.aof"));
    mr_store *s = new_store();
    ASSERT(s != NULL);
    mr_aof *a = NULL;
    mr_aof_load_info info;
    ASSERT(open_aof(s, MR_AOF_FSYNC_ALWAYS, &a, &info) == MR_OK);
    CHECK_EQ(info.records, 0);

    int64_t far = g_fc.wall + 3600000;
    CHECK(mr_aof_log_set(a, K("plain"), K("v1"), 0) == MR_OK);
    CHECK(mr_aof_log_set(a, K("ttl"), K("v2"), far) == MR_OK);
    CHECK(mr_aof_log_set(a, K("gone"), K("x"), 0) == MR_OK);
    CHECK(mr_aof_log_del(a, K("gone")) == MR_OK);
    CHECK(mr_aof_log_set(a, K("p"), K("x"), far) == MR_OK);
    CHECK(mr_aof_log_persist(a, K("p")) == MR_OK);
    CHECK(mr_aof_log_set(a, K("e"), K("x"), 0) == MR_OK);
    CHECK(mr_aof_log_pexpireat(a, K("e"), far) == MR_OK);
    CHECK(mr_aof_log_set(a, K("c:1"), K("x"), 0) == MR_OK);
    CHECK(mr_aof_log_set(a, K("c:2"), K("x"), 0) == MR_OK);
    CHECK(mr_aof_log_set(a, K("d:1"), K("x"), 0) == MR_OK);
    CHECK(mr_aof_log_set(a, K("d:2"), K("x"), 0) == MR_OK);
    CHECK(mr_aof_log_invalidate(a, K("c:*")) == MR_OK);
    CHECK(mr_aof_log_invalidate_prefix(a, K("d:")) == MR_OK);
    CHECK(mr_aof_log_set(a, K("empty"), NULL, 0, 0) == MR_OK);
    CHECK(mr_aof_log_pexpireat(a, K("e"), 0) == MR_INVALID_ARG);
    CHECK(mr_aof_log_set(a, K("k"), K("v"), -1) == MR_INVALID_ARG);
    CHECK(mr_aof_size(a) == (uint64_t)file_size(g_path));
    CHECK(mr_aof_close(a) == MR_OK);
    mr_store_destroy(s);

    mr_store *r = reload(&info, NULL);
    ASSERT(r != NULL);
    CHECK_EQ(info.records, 15);
    CHECK_EQ(info.truncated_bytes, 0);
    CHECK(val_is(r, K("plain"), K("v1")));
    CHECK(val_is(r, K("ttl"), K("v2")));
    CHECK(!mr_store_exists(r, K("gone")));
    int64_t t;
    CHECK(mr_store_pttl(r, K("ttl"), &t) == MR_OK);
    CHECK_EQ(t, 3600000);
    CHECK(mr_store_pttl(r, K("p"), &t) == MR_OK);
    CHECK_EQ(t, -1);
    CHECK(mr_store_pttl(r, K("e"), &t) == MR_OK);
    CHECK_EQ(t, 3600000);
    CHECK(!mr_store_exists(r, K("c:1")) && !mr_store_exists(r, K("c:2")));
    CHECK(!mr_store_exists(r, K("d:1")) && !mr_store_exists(r, K("d:2")));
    CHECK(val_is(r, K("empty"), NULL, 0));
    CHECK_EQ(mr_store_size(r), 5);
    mr_store_destroy(r);

    /* FLUSHALL 재생 */
    s = new_store();
    ASSERT(open_aof(s, MR_AOF_FSYNC_EVERYSEC, &a, NULL) == MR_OK);
    CHECK_EQ(mr_store_size(s), 5);
    CHECK(mr_aof_log_flushall(a) == MR_OK);
    CHECK(mr_aof_log_set(a, K("after"), K("1"), 0) == MR_OK);
    CHECK(mr_aof_tick(a) == MR_OK); /* 1초 전: 아무것도 안 함 */
    g_fc.mono += 1000;
    CHECK(mr_aof_tick(a) == MR_OK); /* 1초 경과: fsync */
    CHECK(mr_aof_sync(a) == MR_OK);
    CHECK(mr_aof_close(a) == MR_OK);
    mr_store_destroy(s);
    r = reload(NULL, NULL);
    ASSERT(r != NULL);
    CHECK_EQ(mr_store_size(r), 1);
    CHECK(val_is(r, K("after"), K("1")));
    mr_store_destroy(r);
    teardown();
}

TEST(test_binary_values_with_nul_and_newlines) {
    ASSERT(setup("b.aof"));
    mr_store *s = new_store();
    mr_aof *a = NULL;
    ASSERT(s != NULL && open_aof(s, MR_AOF_FSYNC_NO, &a, NULL) == MR_OK);
    const char key[] = {'k', '\0', '\r', '\n', '*', '$'};
    const char val[] = {'\r', '\n', '\0', '*', '3', '\r', '\n', '$', '-', '1'};
    CHECK(mr_aof_log_set(a, key, sizeof(key), val, sizeof(val), 0) == MR_OK);
    CHECK(mr_aof_log_set(a, NULL, 0, K("emptykey"), 0) == MR_OK);
    CHECK(mr_aof_close(a) == MR_OK);
    mr_store_destroy(s);
    mr_store *r = reload(NULL, NULL);
    ASSERT(r != NULL);
    CHECK(val_is(r, key, sizeof(key), val, sizeof(val)));
    CHECK(val_is(r, NULL, 0, K("emptykey")));
    mr_store_destroy(r);
    teardown();
}

TEST(test_expired_records_are_dropped) {
    ASSERT(setup("c.aof"));
    mr_store *s = new_store();
    mr_aof *a = NULL;
    ASSERT(s != NULL && open_aof(s, MR_AOF_FSYNC_NO, &a, NULL) == MR_OK);
    int64_t soon = g_fc.wall + 10;
    CHECK(mr_aof_log_set(a, K("short"), K("x"), soon) == MR_OK);
    CHECK(mr_aof_log_set(a, K("long"), K("x"), soon + 100000) == MR_OK);
    /* 원래 시점에 TTL 이 있었지만 만료 전에 PERSIST 된 키 */
    CHECK(mr_aof_log_set(a, K("saved"), K("x"), soon) == MR_OK);
    CHECK(mr_aof_log_persist(a, K("saved")) == MR_OK);
    /* 과거 시각 PEXPIREAT 는 재생 후 정리 */
    CHECK(mr_aof_log_set(a, K("old"), K("x"), 0) == MR_OK);
    CHECK(mr_aof_log_pexpireat(a, K("old"), soon) == MR_OK);
    CHECK(mr_aof_close(a) == MR_OK);
    mr_store_destroy(s);

    g_fc.wall += 1000; /* 재시작 시점에는 soon 이 지남 */
    mr_aof_load_info info;
    mr_store *r = reload(&info, NULL);
    ASSERT(r != NULL);
    CHECK(!mr_store_exists(r, K("short")));
    CHECK(!mr_store_exists(r, K("old")));
    CHECK(mr_store_exists(r, K("long")));
    CHECK(mr_store_exists(r, K("saved")));
    CHECK_EQ(info.expired_dropped, 2);
    CHECK_EQ(mr_store_size(r), 2);
    mr_store_destroy(r);
    teardown();
}

TEST(test_truncated_tail_is_cut_at_every_offset) {
    ASSERT(setup("d.aof"));
    static const char good[] = "*3\r\n$3\r\nSET\r\n$1\r\na\r\n$1\r\n1\r\n";
    static const char last[] = "*5\r\n$3\r\nSET\r\n$1\r\nb\r\n$5\r\nhello\r\n$4\r\nPXAT\r\n"
                               "$13\r\n9999999999999\r\n";
    size_t glen = sizeof(good) - 1;
    size_t llen = sizeof(last) - 1;
    for (size_t cut = 1; cut < llen; cut++) {
        ASSERT(write_file(g_path, good, glen, "wb"));
        ASSERT(write_file(g_path, last, cut, "ab"));
        mr_aof_load_info info;
        mr_status st;
        mr_store *r = reload(&info, &st);
        CHECK(st == MR_OK);
        if (r == NULL) {
            fprintf(stderr, "    cut=%zu status=%s\n", cut, mr_status_str(st));
            break;
        }
        CHECK_EQ(info.records, 1);
        CHECK_EQ(info.truncated_bytes, cut);
        CHECK(val_is(r, K("a"), K("1")));
        CHECK(!mr_store_exists(r, K("b")));
        CHECK_EQ(file_size(g_path), glen);
        mr_store_destroy(r);
    }
    /* 잘라낸 뒤 이어 쓰기와 재시작이 정상 */
    mr_store *s = new_store();
    mr_aof *a = NULL;
    ASSERT(s != NULL && open_aof(s, MR_AOF_FSYNC_NO, &a, NULL) == MR_OK);
    CHECK(mr_aof_log_set(a, K("c"), K("3"), 0) == MR_OK);
    mr_aof_close(a);
    mr_store_destroy(s);
    mr_store *r = reload(NULL, NULL);
    ASSERT(r != NULL);
    CHECK(val_is(r, K("a"), K("1")) && val_is(r, K("c"), K("3")));
    mr_store_destroy(r);
    teardown();
}

static void expect_corrupt(const char *content) {
    ASSERT(write_file(g_path, content, strlen(content), "wb"));
    mr_status st;
    mr_store *r = reload(NULL, &st);
    CHECK(r == NULL);
    CHECK(st == MR_CORRUPT);
    if (st != MR_CORRUPT) {
        fprintf(stderr, "    content=%s status=%s\n", content, mr_status_str(st));
    }
    mr_store_destroy(r);
}

TEST(test_middle_corruption_is_reported) {
    ASSERT(setup("e.aof"));
#define REC_A "*3\r\n$3\r\nSET\r\n$1\r\na\r\n$1\r\n1\r\n"
    long before;
    expect_corrupt(REC_A "garbage\r\n" REC_A);
    expect_corrupt(REC_A "*3\r\n$3\r\nSET\r\n$1\r\nab\r\n$1\r\n1\r\n" REC_A); /* 길이 불일치 */
    expect_corrupt(REC_A "*2\r\n$4\r\nNOPE\r\n$1\r\na\r\n" REC_A);           /* 모르는 명령 */
    expect_corrupt(REC_A "*2\r\n$3\r\nSET\r\n$1\r\na\r\n" REC_A);            /* 인자 수 */
    expect_corrupt("*5\r\n$3\r\nSET\r\n$1\r\na\r\n$1\r\n1\r\n$4\r\nPXAT\r\n$2\r\n-5\r\n");
    expect_corrupt("*5\r\n$3\r\nSET\r\n$1\r\na\r\n$1\r\n1\r\n$2\r\nEX\r\n$2\r\n10\r\n");
    expect_corrupt("*0\r\n");
    expect_corrupt("*99\r\n");
    expect_corrupt("*1\r\n$999999999999\r\n");
    expect_corrupt("*1\r\n$-1\r\n");
    expect_corrupt("*1\r\n$8\r\nFLUSHALL\n\n");
    /* 손상 시 파일은 건드리지 않는다 */
    before = file_size(g_path);
    expect_corrupt("*1\r\n$8\r\nFLUSHALL\n\n");
    CHECK_EQ(file_size(g_path), before);
#undef REC_A
    teardown();
}

TEST(test_rewrite_compacts_and_survives_restart) {
    ASSERT(setup("f.aof"));
    mr_store *s = new_store();
    mr_aof *a = NULL;
    ASSERT(s != NULL && open_aof(s, MR_AOF_FSYNC_EVERYSEC, &a, NULL) == MR_OK);
    char v[32];
    /* 같은 키를 여러 번 덮어써 로그를 부풀린다(서버처럼 store 와 AOF 를 함께 갱신) */
    for (int i = 0; i < 2000; i++) {
        int n = snprintf(v, sizeof(v), "%d", i);
        ASSERT(mr_store_set(s, K("counter"), v, (size_t)n, NULL) == MR_OK);
        ASSERT(mr_aof_log_set(a, K("counter"), v, (size_t)n, 0) == MR_OK);
    }
    mr_set_opts px = {MR_SET_ALWAYS, MR_EXPIRE_PX, 5000};
    ASSERT(mr_store_set(s, K("ttl"), K("t"), &px) == MR_OK);
    ASSERT(mr_aof_log_set(a, K("ttl"), K("t"), g_fc.wall + 5000) == MR_OK);
    mr_set_opts px1 = {MR_SET_ALWAYS, MR_EXPIRE_PX, 1};
    ASSERT(mr_store_set(s, K("dead"), K("d"), &px1) == MR_OK);
    ASSERT(mr_aof_log_set(a, K("dead"), K("d"), g_fc.wall + 1) == MR_OK);
    g_fc.wall += 1; /* dead 만료 */

    long big = file_size(g_path);
    CHECK(mr_aof_rewrite(a, s) == MR_OK);
    long small = file_size(g_path);
    CHECK(small > 0 && small < big / 10);
    CHECK(mr_aof_size(a) == (uint64_t)small);
    /* rewrite 이후 이어 쓰기도 새 파일에 들어가야 함 */
    ASSERT(mr_aof_log_set(a, K("post"), K("p"), 0) == MR_OK);
    CHECK(mr_aof_close(a) == MR_OK);
    mr_store_destroy(s);

    char tmp[400];
    snprintf(tmp, sizeof(tmp), "%s.rewrite.tmp", g_path);
    CHECK(access(tmp, F_OK) != 0); /* 임시 파일이 남지 않음 */

    mr_aof_load_info info;
    mr_store *r = reload(&info, NULL);
    ASSERT(r != NULL);
    CHECK_EQ(info.records, 3);
    CHECK(val_is(r, K("counter"), K("1999")));
    CHECK(val_is(r, K("post"), K("p")));
    CHECK(!mr_store_exists(r, K("dead")));
    int64_t t;
    CHECK(mr_store_pttl(r, K("ttl"), &t) == MR_OK);
    CHECK_EQ(t, 4999);
    mr_store_destroy(r);
    teardown();
}

TEST(test_io_error_paths) {
    ASSERT(setup("g.aof"));
    mr_store *s = new_store();
    ASSERT(s != NULL);
    mr_aof *a = NULL;

    /* 존재하지 않는 디렉터리 */
    mr_aof_opts bad = {"/nonexistent-dir-for-mr-test/x.aof", MR_AOF_FSYNC_NO, &g_clk};
    CHECK(mr_aof_open(&bad, s, &a, NULL) == MR_IO_ERROR);
    CHECK(a == NULL);
    /* 디렉터리를 파일로 열기 */
    mr_aof_opts dir = {g_dir, MR_AOF_FSYNC_NO, &g_clk};
    CHECK(mr_aof_open(&dir, s, &a, NULL) == MR_IO_ERROR);
    /* 잘못된 인자 */
    mr_aof_opts nul = {NULL, MR_AOF_FSYNC_NO, &g_clk};
    CHECK(mr_aof_open(&nul, s, &a, NULL) == MR_INVALID_ARG);
    mr_aof_opts pol = {g_path, (mr_aof_fsync)42, &g_clk};
    CHECK(mr_aof_open(&pol, s, &a, NULL) == MR_INVALID_ARG);
    CHECK(mr_aof_close(NULL) == MR_OK);

    if (geteuid() == 0) {
        fprintf(stderr, "    (root: 권한 기반 실패 경로는 건너뜀)\n");
    } else {
        /* 읽기 전용 파일 */
        ASSERT(write_file(g_path, "", 0, "wb"));
        ASSERT(chmod(g_path, 0444) == 0);
        CHECK(open_aof(s, MR_AOF_FSYNC_NO, &a, NULL) == MR_IO_ERROR);
        ASSERT(chmod(g_path, 0644) == 0);

        /* 읽기 전용 디렉터리에서 rewrite: 실패하고 기존 파일로 계속 동작 */
        ASSERT(open_aof(s, MR_AOF_FSYNC_NO, &a, NULL) == MR_OK);
        ASSERT(mr_store_set(s, K("k"), K("v"), NULL) == MR_OK);
        CHECK(mr_aof_log_set(a, K("k"), K("v"), 0) == MR_OK);
        ASSERT(chmod(g_dir, 0555) == 0);
        CHECK(mr_aof_rewrite(a, s) == MR_IO_ERROR);
        ASSERT(chmod(g_dir, 0755) == 0);
        CHECK(mr_aof_log_set(a, K("k2"), K("v2"), 0) == MR_OK);
        CHECK(mr_aof_close(a) == MR_OK);
        mr_store *r = reload(NULL, NULL);
        ASSERT(r != NULL);
        CHECK(val_is(r, K("k"), K("v")) && val_is(r, K("k2"), K("v2")));
        mr_store_destroy(r);
    }
    mr_store_destroy(s);
    teardown();
}

/* 파일 크기 제한(RLIMIT_FSIZE)으로 실제 디스크 쓰기 실패·부분 쓰기를 만든다. */
TEST(test_write_failure_rolls_back_partial_record) {
    ASSERT(setup("h.aof"));
    mr_store *s = new_store();
    mr_aof *a = NULL;
    ASSERT(s != NULL && open_aof(s, MR_AOF_FSYNC_ALWAYS, &a, NULL) == MR_OK);
    ASSERT(mr_aof_log_set(a, K("a"), K("1"), 0) == MR_OK);
    uint64_t before = mr_aof_size(a);

    struct rlimit old;
    ASSERT(getrlimit(RLIMIT_FSIZE, &old) == 0);
    void (*prev)(int) = signal(SIGXFSZ, SIG_IGN);
    struct rlimit lim = old;
    lim.rlim_cur = (rlim_t)before + 64;
    ASSERT(setrlimit(RLIMIT_FSIZE, &lim) == 0);

    char big[4096];
    memset(big, 'z', sizeof(big));
    mr_status st = mr_aof_log_set(a, K("big"), big, sizeof(big), 0);
    long after_fail = file_size(g_path);
    mr_status st_small = mr_aof_log_set(a, K("b"), K("2"), 0); /* 한도 안: 성공해야 함 */

    ASSERT(setrlimit(RLIMIT_FSIZE, &old) == 0);
    signal(SIGXFSZ, prev);

    CHECK(st == MR_IO_ERROR);
    CHECK_EQ(after_fail, (long)before); /* 반쪽 레코드가 지워짐 */
    CHECK(st_small == MR_OK);
    CHECK(mr_aof_close(a) == MR_OK);
    mr_store_destroy(s);

    mr_aof_load_info info;
    mr_status rst;
    mr_store *r = reload(&info, &rst);
    CHECK(rst == MR_OK);
    ASSERT(r != NULL);
    CHECK_EQ(info.truncated_bytes, 0);
    CHECK(val_is(r, K("a"), K("1")) && val_is(r, K("b"), K("2")));
    CHECK(!mr_store_exists(r, K("big")));
    mr_store_destroy(r);
    teardown();
}

TEST(test_large_value_roundtrip) {
    ASSERT(setup("i.aof"));
    enum { SZ = 3 * 1024 * 1024 + 17 };
    unsigned char *big = malloc(SZ);
    ASSERT(big != NULL);
    for (size_t i = 0; i < SZ; i++) {
        big[i] = (unsigned char)(i * 131 + 3);
    }
    mr_store *s = new_store();
    mr_aof *a = NULL;
    ASSERT(s != NULL && open_aof(s, MR_AOF_FSYNC_NO, &a, NULL) == MR_OK);
    CHECK(mr_aof_log_set(a, K("big"), big, SZ, 0) == MR_OK);
    CHECK(mr_aof_log_set(a, K("small"), K("s"), 0) == MR_OK);
    mr_aof_close(a);
    mr_store_destroy(s);
    mr_store *r = reload(NULL, NULL);
    ASSERT(r != NULL);
    CHECK(val_is(r, K("big"), big, SZ));
    CHECK(val_is(r, K("small"), K("s")));
    mr_store_destroy(r);
    free(big);
    teardown();
}

TEST(test_alloc_failure_injection) {
    ASSERT(setup("j.aof"));
    {
        mr_store *s = new_store();
        mr_aof *a = NULL;
        ASSERT(s != NULL && open_aof(s, MR_AOF_FSYNC_NO, &a, NULL) == MR_OK);
        char k[32];
        for (int i = 0; i < 50; i++) {
            int n = snprintf(k, sizeof(k), "key%d", i);
            ASSERT(mr_aof_log_set(a, k, (size_t)n, K("value"), 0) == MR_OK);
        }
        mr_aof_close(a);
        mr_store_destroy(s);
    }
    long size = file_size(g_path);
    unsigned long n;
    for (n = 1; n < 5000; n++) {
        unsigned long before = mt_alloc_failures_total();
        mr_store *s = new_store();
        ASSERT(s != NULL);
        mr_aof *a = NULL;
        mt_fail_alloc_after(n);
        mr_status st = open_aof(s, MR_AOF_FSYNC_NO, &a, NULL);
        if (st == MR_OK) {
            st = mr_aof_log_set(a, K("x"), K("y"), 0);
            CHECK(st == MR_OK || st == MR_NOMEM);
            if (st == MR_OK) {
                mt_alloc_reset();
                CHECK(mr_aof_log_del(a, K("x")) == MR_OK); /* 파일 상태를 원래대로 */
            }
            st = mr_aof_rewrite(a, s);
            CHECK(st == MR_OK || st == MR_NOMEM);
        } else {
            CHECK(st == MR_NOMEM);
        }
        mt_alloc_reset();
        mr_aof_close(a);
        mr_store_destroy(s);
        mr_store *r = reload(NULL, NULL);
        CHECK(r != NULL); /* 어떤 실패 뒤에도 파일은 손상되지 않음 */
        if (r != NULL) {
            CHECK_EQ(mr_store_size(r), 50);
        }
        mr_store_destroy(r);
        if (mt_alloc_failures_total() == before) {
            break;
        }
    }
    CHECK(n > 3);
    CHECK(size > 0);
    teardown();
}

MT_MAIN(test_roundtrip_all_commands, test_binary_values_with_nul_and_newlines,
        test_expired_records_are_dropped, test_truncated_tail_is_cut_at_every_offset,
        test_middle_corruption_is_reported, test_rewrite_compacts_and_survives_restart,
        test_io_error_paths, test_write_failure_rolls_back_partial_record,
        test_large_value_roundtrip, test_alloc_failure_injection)
