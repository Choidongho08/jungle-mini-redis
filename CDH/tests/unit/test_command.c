#include <sys/stat.h>
#include <unistd.h>

#include "minitest.h"
#include "miniredis/command.h"

/* ---------- 환경 ---------- */

typedef struct fake_clock {
    int64_t wall;
    int64_t mono;
} fake_clock;

static int64_t fake_wall(void *ctx) { return ((fake_clock *)ctx)->wall; }
static int64_t fake_mono(void *ctx) { return ((fake_clock *)ctx)->mono; }

#define T0 1700000000000LL

static fake_clock g_fc = {T0, 0};
static const mr_clock g_clk = {fake_wall, fake_mono, &g_fc};
static mr_store *g_store;
static mr_cmd_env g_env;
static mr_buf g_out;

static mr_store *new_store(void) {
    mr_store_opts o;
    memset(&o, 0, sizeof(o));
    o.clock = &g_clk;
    mr_store *s = NULL;
    return mr_store_create(&o, &s) == MR_OK ? s : NULL;
}

static bool setup(void) {
    g_fc.wall = T0;
    g_fc.mono = 0;
    g_store = new_store();
    memset(&g_env, 0, sizeof(g_env));
    g_env.store = g_store;
    g_env.clock = &g_clk;
    mr_buf_init(&g_out);
    return g_store != NULL;
}

static void teardown(void) {
    mr_store_destroy(g_store);
    g_store = NULL;
    mr_buf_free(&g_out);
}

static void print_escaped(const unsigned char *p, size_t n) {
    for (size_t i = 0; i < n; i++) {
        if (p[i] == '\r') {
            fputs("\\r", stderr);
        } else if (p[i] == '\n') {
            fputs("\\n", stderr);
        } else if (p[i] < 32 || p[i] >= 127) {
            fprintf(stderr, "\\x%02x", p[i]);
        } else {
            fputc(p[i], stderr);
        }
    }
}

/* argv 실행 후 응답이 want 와 바이트 단위로 같은지. */
static bool run_argv(size_t argc, const mr_slice *argv, const void *want, size_t wlen) {
    g_out.len = 0;
    mr_status st = mr_cmd_exec(&g_env, argc, argv, &g_out);
    if (st == MR_OK && g_out.len == wlen && memcmp(g_out.data, want, wlen) == 0) {
        return true;
    }
    fprintf(stderr, "    status=%s got='", mr_status_str(st));
    print_escaped(g_out.data, g_out.len);
    fprintf(stderr, "' want='");
    print_escaped(want, wlen);
    fprintf(stderr, "'\n");
    return false;
}

/* 공백으로 나눈 한 줄을 실행. */
static size_t split(const char *line, mr_slice *argv, size_t max) {
    size_t n = 0;
    const char *p = line;
    while (*p != '\0' && n < max) {
        while (*p == ' ') {
            p++;
        }
        if (*p == '\0') {
            break;
        }
        const char *s = p;
        while (*p != '\0' && *p != ' ') {
            p++;
        }
        argv[n].ptr = (const unsigned char *)s;
        argv[n].len = (size_t)(p - s);
        n++;
    }
    return n;
}

static bool run(const char *line, const char *want) {
    mr_slice argv[32];
    size_t argc = split(line, argv, 32);
    bool ok = run_argv(argc, argv, want, strlen(want));
    if (!ok) {
        fprintf(stderr, "    command: %s\n", line);
    }
    return ok;
}

#define EXPECT(line, want) CHECK(run((line), (want)))
#define WRONG_ARGS(name) "-ERR wrong number of arguments for '" name "' command\r\n"
#define NOT_INT "-ERR value is not an integer or out of range\r\n"
#define SYNTAX "-ERR syntax error\r\n"

/* ---------- PING / unknown ---------- */

TEST(test_ping) {
    ASSERT(setup());
    EXPECT("PING", "+PONG\r\n");
    EXPECT("ping hi", "$2\r\nhi\r\n");
    EXPECT("PiNg a b", WRONG_ARGS("ping"));
    teardown();
}

TEST(test_unknown_and_empty) {
    ASSERT(setup());
    EXPECT("FOO a b", "-ERR unknown command 'FOO', with args beginning with: 'a' 'b' \r\n");
    EXPECT("FOO", "-ERR unknown command 'FOO', with args beginning with: \r\n");
    /* 사용자 바이트 속 CR/LF 는 공백으로: 응답 프레이밍 보호 */
    mr_slice argv[1] = {{(const unsigned char *)"x\r\ny", 4}};
    CHECK(run_argv(1, argv,
                   "-ERR unknown command 'x  y', with args beginning with: \r\n",
                   strlen("-ERR unknown command 'x  y', with args beginning with: \r\n")));
    CHECK(run_argv(0, NULL, "", 0));
    teardown();
}

/* 9개 밖의 명령은 unknown command 이고 저장소에 영향이 없다. */
TEST(test_removed_commands_are_unknown) {
    ASSERT(setup());
    EXPECT("SET k v", "+OK\r\n");
    EXPECT("ECHO hi", "-ERR unknown command 'ECHO', with args beginning with: 'hi' \r\n");
    EXPECT("DBSIZE", "-ERR unknown command 'DBSIZE', with args beginning with: \r\n");
    EXPECT("KEYS *", "-ERR unknown command 'KEYS', with args beginning with: '*' \r\n");
    EXPECT("INFO", "-ERR unknown command 'INFO', with args beginning with: \r\n");
    EXPECT("PTTL k", "-ERR unknown command 'PTTL', with args beginning with: 'k' \r\n");
    EXPECT("PERSIST k", "-ERR unknown command 'PERSIST', with args beginning with: 'k' \r\n");
    EXPECT("DECR k", "-ERR unknown command 'DECR', with args beginning with: 'k' \r\n");
    EXPECT("INCRBY k 1",
           "-ERR unknown command 'INCRBY', with args beginning with: 'k' '1' \r\n");
    EXPECT("QUIT", "-ERR unknown command 'QUIT', with args beginning with: \r\n");
    EXPECT("GET k", "$1\r\nv\r\n");
    CHECK_EQ(mr_cmd_count(), 9);
    teardown();
}

/* ---------- SET/GET ---------- */

TEST(test_set_get_options) {
    ASSERT(setup());
    EXPECT("SET k v", "+OK\r\n");
    EXPECT("GET k", "$1\r\nv\r\n");
    EXPECT("GET none", "$-1\r\n");
    EXPECT("SET k v2", "+OK\r\n");
    EXPECT("GET k", "$2\r\nv2\r\n");
    EXPECT("SET k v ex 10", "+OK\r\n");
    EXPECT("SET k v EX", SYNTAX);
    EXPECT("SET k v FOO", SYNTAX);
    EXPECT("SET k v EX 10 EX 5", SYNTAX);
    EXPECT("SET k v EX 10 extra", SYNTAX);
    EXPECT("SET k v EX abc", NOT_INT);
    EXPECT("SET k v EX 0", "-ERR invalid expire time in 'set' command\r\n");
    EXPECT("SET k v EX -5", "-ERR invalid expire time in 'set' command\r\n");
    EXPECT("SET k v EX 9223372036854775807",
           "-ERR invalid expire time in 'set' command\r\n");
    /* 제거된 옵션은 문법 오류 */
    EXPECT("SET k v NX", SYNTAX);
    EXPECT("SET k v XX", SYNTAX);
    EXPECT("SET k v PX 100", SYNTAX);
    EXPECT("SET k v EXAT 100", SYNTAX);
    EXPECT("SET k v PXAT 100", SYNTAX);
    EXPECT("SET k", WRONG_ARGS("set"));
    EXPECT("GET", WRONG_ARGS("get"));
    EXPECT("GET a b", WRONG_ARGS("get"));
    teardown();
}

TEST(test_set_binary_and_utf8) {
    ASSERT(setup());
    mr_slice set[3] = {{(const unsigned char *)"SET", 3},
                       {(const unsigned char *)"a\0b", 3},
                       {(const unsigned char *)"v\r\n\0", 4}};
    CHECK(run_argv(3, set, "+OK\r\n", 5));
    mr_slice get[2] = {{(const unsigned char *)"GET", 3}, {(const unsigned char *)"a\0b", 3}};
    CHECK(run_argv(2, get, "$4\r\nv\r\n\0\r\n", 10));
    EXPECT("SET 한글키 값", "+OK\r\n");
    EXPECT("GET 한글키", "$3\r\n값\r\n");
    teardown();
}

TEST(test_set_ex) {
    ASSERT(setup());
    EXPECT("SET t v EX 10", "+OK\r\n");
    EXPECT("TTL t", ":10\r\n");
    g_fc.wall += 1500;
    EXPECT("TTL t", ":9\r\n");
    g_fc.wall += 8499;
    EXPECT("GET t", "$1\r\nv\r\n");
    g_fc.wall += 1;
    EXPECT("GET t", "$-1\r\n");
    EXPECT("TTL t", ":-2\r\n");
    /* 일반 SET 은 TTL 제거 */
    EXPECT("SET a v EX 100", "+OK\r\n");
    EXPECT("SET a v2", "+OK\r\n");
    EXPECT("TTL a", ":-1\r\n");
    teardown();
}

/* ---------- EXPIRE / TTL ---------- */

TEST(test_expire_ttl) {
    ASSERT(setup());
    EXPECT("EXPIRE none 10", ":0\r\n");
    EXPECT("TTL none", ":-2\r\n");
    EXPECT("SET k v", "+OK\r\n");
    EXPECT("TTL k", ":-1\r\n");
    EXPECT("EXPIRE k 100", ":1\r\n");
    EXPECT("TTL k", ":100\r\n");
    EXPECT("EXPIRE k 5", ":1\r\n"); /* 덮어쓰기 */
    EXPECT("TTL k", ":5\r\n");
    EXPECT("EXPIRE k x", NOT_INT);
    EXPECT("EXPIRE k 9223372036854775807", "-ERR invalid expire time in 'expire' command\r\n");
    EXPECT("EXPIRE k", WRONG_ARGS("expire"));
    EXPECT("TTL", WRONG_ARGS("ttl"));
    EXPECT("TTL a b", WRONG_ARGS("ttl"));
    /* 0 이하는 즉시 삭제 */
    EXPECT("EXPIRE k 0", ":1\r\n");
    EXPECT("GET k", "$-1\r\n");
    EXPECT("SET k v", "+OK\r\n");
    EXPECT("EXPIRE k -1", ":1\r\n");
    EXPECT("EXISTS k", ":0\r\n");
    /* 만료 경계: 정확히 그 시각부터 없음 */
    EXPECT("SET e v EX 1", "+OK\r\n");
    g_fc.wall += 999;
    EXPECT("GET e", "$1\r\nv\r\n");
    g_fc.wall += 1;
    EXPECT("GET e", "$-1\r\n");
    EXPECT("EXPIRE e 10", ":0\r\n"); /* 만료된 키는 없는 키 */
    teardown();
}

/* ---------- INCR ---------- */

TEST(test_incr) {
    ASSERT(setup());
    EXPECT("INCR c", ":1\r\n");
    EXPECT("INCR c", ":2\r\n");
    EXPECT("GET c", "$1\r\n2\r\n");
    EXPECT("SET n -5", "+OK\r\n");
    EXPECT("INCR n", ":-4\r\n");
    EXPECT("SET s abc", "+OK\r\n");
    EXPECT("INCR s", NOT_INT);
    EXPECT("SET s 007", "+OK\r\n");
    EXPECT("INCR s", NOT_INT);
    EXPECT("SET s 1.5", "+OK\r\n");
    EXPECT("INCR s", NOT_INT);
    EXPECT("SET m 9223372036854775807", "+OK\r\n");
    EXPECT("INCR m", "-ERR increment or decrement would overflow\r\n");
    EXPECT("GET m", "$19\r\n9223372036854775807\r\n");
    /* INCR 은 TTL 을 유지 */
    EXPECT("SET t 5 EX 100", "+OK\r\n");
    EXPECT("INCR t", ":6\r\n");
    EXPECT("TTL t", ":100\r\n");
    EXPECT("INCR", WRONG_ARGS("incr"));
    EXPECT("INCR a b", WRONG_ARGS("incr"));
    teardown();
}

/* ---------- DEL / EXISTS / FLUSHALL ---------- */

TEST(test_del_exists_flushall) {
    ASSERT(setup());
    EXPECT("SET a 1", "+OK\r\n");
    EXPECT("SET b 2", "+OK\r\n");
    EXPECT("EXISTS a a b z", ":3\r\n");
    EXPECT("EXISTS z", ":0\r\n");
    EXPECT("DEL a b c", ":2\r\n");
    EXPECT("DEL a", ":0\r\n");
    EXPECT("EXISTS a b", ":0\r\n");
    EXPECT("SET a 1", "+OK\r\n");
    EXPECT("SET b 2 EX 100", "+OK\r\n");
    EXPECT("FLUSHALL", "+OK\r\n");
    EXPECT("EXISTS a b", ":0\r\n");
    EXPECT("TTL b", ":-2\r\n");
    EXPECT("FLUSHALL ASYNC", "+OK\r\n");
    EXPECT("FLUSHALL now", SYNTAX);
    EXPECT("DEL", WRONG_ARGS("del"));
    EXPECT("EXISTS", WRONG_ARGS("exists"));
    teardown();
}

TEST(test_write_flag) {
    ASSERT(setup());
    CHECK(mr_cmd_is_write("set", 3) && mr_cmd_is_write("FLUSHALL", 8));
    CHECK(mr_cmd_is_write("del", 3) && mr_cmd_is_write("expire", 6) && mr_cmd_is_write("incr", 4));
    CHECK(!mr_cmd_is_write("get", 3) && !mr_cmd_is_write("ttl", 3));
    CHECK(!mr_cmd_is_write("exists", 6) && !mr_cmd_is_write("ping", 4));
    CHECK(!mr_cmd_is_write("nope", 4) && !mr_cmd_is_write("persist", 7));
    teardown();
}

/* ---------- 메모리 부족 ---------- */

TEST(test_reply_nomem) {
    ASSERT(setup());
    mr_slice argv[2] = {{(const unsigned char *)"PING", 4}, {(const unsigned char *)"hi", 2}};
    mt_fail_alloc_after(1);
    CHECK(mr_cmd_exec(&g_env, 2, argv, &g_out) == MR_NOMEM);
    mt_alloc_reset();
    teardown();
}

/* ---------- AOF 연동 ---------- */

static char g_dir[256];
static char g_path[300];

static bool tmp_setup(void) {
    const char *tmp = getenv("TMPDIR");
    int n = snprintf(g_dir, sizeof(g_dir), "%s/mr_cmd_XXXXXX", tmp != NULL ? tmp : "/tmp");
    if (n < 0 || (size_t)n >= sizeof(g_dir) || mkdtemp(g_dir) == NULL) {
        return false;
    }
    snprintf(g_path, sizeof(g_path), "%s/a.aof", g_dir);
    return true;
}

static void tmp_teardown(void) {
    char p[400];
    unlink(g_path);
    snprintf(p, sizeof(p), "%s.rewrite.tmp", g_path);
    unlink(p);
    rmdir(g_dir);
}

static mr_aof *open_aof(mr_store *s) {
    mr_aof_opts o = {g_path, MR_AOF_FSYNC_NO, &g_clk};
    mr_aof *a = NULL;
    return mr_aof_open(&o, s, &a, NULL) == MR_OK ? a : NULL;
}

/* 현재 env 를 AOF 를 닫고 새 저장소로 재생한 상태로 바꾼다. */
static bool reopen_from_aof(void) {
    if (mr_aof_close(g_env.aof) != MR_OK) {
        return false;
    }
    mr_store_destroy(g_store);
    g_store = new_store();
    g_env.store = g_store;
    g_env.aof = g_store != NULL ? open_aof(g_store) : NULL;
    return g_env.aof != NULL;
}

TEST(test_aof_roundtrip_all_writes) {
    ASSERT(setup());
    ASSERT(tmp_setup());
    g_env.aof = open_aof(g_store);
    ASSERT(g_env.aof != NULL);
    EXPECT("SET plain v", "+OK\r\n");
    EXPECT("SET ttl v EX 100", "+OK\r\n");
    EXPECT("SET cnt 5 EX 60", "+OK\r\n");
    EXPECT("INCR cnt", ":6\r\n");
    EXPECT("INCR fresh", ":1\r\n");
    EXPECT("SET gone v", "+OK\r\n");
    EXPECT("DEL gone", ":1\r\n");
    EXPECT("SET e v", "+OK\r\n");
    EXPECT("EXPIRE e 3", ":1\r\n");
    EXPECT("SET z v", "+OK\r\n");
    EXPECT("EXPIRE z 0", ":1\r\n");
    EXPECT("SET late v EX 1", "+OK\r\n");
    g_fc.wall += 1000;
    ASSERT(reopen_from_aof());
    EXPECT("GET plain", "$1\r\nv\r\n");
    EXPECT("TTL ttl", ":99\r\n");
    EXPECT("GET cnt", "$1\r\n6\r\n");
    EXPECT("TTL cnt", ":59\r\n");
    EXPECT("GET fresh", "$1\r\n1\r\n");
    EXPECT("TTL fresh", ":-1\r\n");
    EXPECT("TTL e", ":2\r\n");
    EXPECT("EXISTS gone z late", ":0\r\n");
    EXPECT("FLUSHALL", "+OK\r\n");
    ASSERT(reopen_from_aof());
    EXPECT("EXISTS plain ttl cnt fresh e", ":0\r\n");
    mr_aof_close(g_env.aof);
    tmp_teardown();
    teardown();
}

/* AOF 기록 실패 뒤에는 모든 쓰기가 막히고, 이를 해제하는 명령은 없다(서버 재시작뿐). */
TEST(test_aof_failure_blocks_writes) {
    ASSERT(setup());
    ASSERT(tmp_setup());
    g_env.aof = open_aof(g_store);
    ASSERT(g_env.aof != NULL);
    EXPECT("SET k v", "+OK\r\n");
    /* 기록 실패를 흉내: 플래그가 서면 쓰기만 막히고 읽기는 된다 */
    g_env.aof_write_failed = true;
    const char *blocked = "-ERR AOF write error earlier; write commands are disabled until "
                          "the server is restarted\r\n";
    EXPECT("SET k v2", blocked);
    EXPECT("DEL k", blocked);
    EXPECT("EXPIRE k 10", blocked);
    EXPECT("INCR n", blocked);
    EXPECT("FLUSHALL", blocked);
    EXPECT("GET k", "$1\r\nv\r\n");
    EXPECT("EXISTS k", ":1\r\n");
    EXPECT("TTL k", ":-1\r\n");
    EXPECT("PING", "+PONG\r\n");
    /* BGREWRITEAOF 는 더 이상 없고, 플래그는 그대로다 */
    EXPECT("BGREWRITEAOF", "-ERR unknown command 'BGREWRITEAOF', with args beginning with: \r\n");
    CHECK(g_env.aof_write_failed);
    EXPECT("SET k v3", blocked);
    mr_aof_close(g_env.aof);
    tmp_teardown();
    teardown();
}

MT_MAIN(test_ping, test_unknown_and_empty, test_removed_commands_are_unknown,
        test_set_get_options, test_set_binary_and_utf8, test_set_ex, test_expire_ttl,
        test_incr, test_del_exists_flushall, test_write_flag, test_reply_nomem,
        test_aof_roundtrip_all_writes, test_aof_failure_blocks_writes)
