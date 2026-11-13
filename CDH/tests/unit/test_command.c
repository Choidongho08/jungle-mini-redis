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
static mr_cmd_server_info g_info;
static mr_buf g_out;
static unsigned g_flags;

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
    memset(&g_info, 0, sizeof(g_info));
    g_info.maxclients = 10000;
    g_info.appendfsync = "everysec";
    g_env.store = g_store;
    g_env.clock = &g_clk;
    g_env.info = &g_info;
    g_env.aof_last_rewrite_ok = true;
    g_env.aof_last_rewrite_ms = -1;
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
    mr_status st = mr_cmd_exec(&g_env, argc, argv, &g_out, &g_flags);
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

/* 응답을 실행만 하고 g_out 에 남긴다. */
static void exec_line(const char *line) {
    mr_slice argv[32];
    size_t argc = split(line, argv, 32);
    g_out.len = 0;
    (void)mr_cmd_exec(&g_env, argc, argv, &g_out, &g_flags);
}

static bool out_contains(const char *needle) {
    size_t n = strlen(needle);
    for (size_t i = 0; i + n <= g_out.len; i++) {
        if (memcmp(g_out.data + i, needle, n) == 0) {
            return true;
        }
    }
    return false;
}

#define EXPECT(line, want) CHECK(run((line), (want)))
#define WRONG_ARGS(name) "-ERR wrong number of arguments for '" name "' command\r\n"
#define NOT_INT "-ERR value is not an integer or out of range\r\n"
#define SYNTAX "-ERR syntax error\r\n"

/* ---------- 연결/기타 ---------- */

TEST(test_ping_echo_select_quit) {
    ASSERT(setup());
    EXPECT("PING", "+PONG\r\n");
    EXPECT("ping hi", "$2\r\nhi\r\n");
    EXPECT("PiNg a b", WRONG_ARGS("ping"));
    EXPECT("ECHO hello", "$5\r\nhello\r\n");
    EXPECT("ECHO", WRONG_ARGS("echo"));
    EXPECT("SELECT 0", "+OK\r\n");
    EXPECT("SELECT 1", "-ERR DB index is out of range\r\n");
    EXPECT("SELECT x", NOT_INT);
    CHECK(g_flags == 0);
    EXPECT("QUIT", "+OK\r\n");
    CHECK((g_flags & MR_CMD_RESULT_CLOSE) != 0);
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

/* ---------- SET/GET ---------- */

TEST(test_set_get_options) {
    ASSERT(setup());
    EXPECT("SET k v", "+OK\r\n");
    EXPECT("GET k", "$1\r\nv\r\n");
    EXPECT("GET none", "$-1\r\n");
    EXPECT("SET k v2 NX", "$-1\r\n");
    EXPECT("SET n v XX", "$-1\r\n");
    EXPECT("SET n v NX", "+OK\r\n");
    EXPECT("SET k v3 xx", "+OK\r\n");
    EXPECT("GET k", "$2\r\nv3\r\n");
    EXPECT("SET k v NX XX", SYNTAX);
    EXPECT("SET k v EX", SYNTAX);
    EXPECT("SET k v FOO", SYNTAX);
    EXPECT("SET k v EX 10 PX 100", SYNTAX);
    EXPECT("SET k v EX abc", NOT_INT);
    EXPECT("SET k v EX 0", "-ERR invalid expire time in 'set' command\r\n");
    EXPECT("SET k v PX -5", "-ERR invalid expire time in 'set' command\r\n");
    EXPECT("SET k v EX 9223372036854775807",
           "-ERR invalid expire time in 'set' command\r\n");
    EXPECT("SET k", WRONG_ARGS("set"));
    EXPECT("GET", WRONG_ARGS("get"));
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

TEST(test_set_expire_options) {
    ASSERT(setup());
    EXPECT("SET t v EX 10", "+OK\r\n");
    EXPECT("TTL t", ":10\r\n");
    EXPECT("PTTL t", ":10000\r\n");
    g_fc.wall += 1500;
    EXPECT("TTL t", ":9\r\n");
    EXPECT("PTTL t", ":8500\r\n");
    EXPECT("SET p v PX 100", "+OK\r\n");
    g_fc.wall += 100;
    EXPECT("GET p", "$-1\r\n");
    char line[96];
    snprintf(line, sizeof(line), "SET a v EXAT %lld", (long long)(g_fc.wall / 1000 + 100));
    EXPECT(line, "+OK\r\n");
    /* wall 이 T0+1600 이라 초 단위 절대 시각은 99.4 초 뒤: 반올림하면 99 */
    EXPECT("TTL a", ":99\r\n");
    snprintf(line, sizeof(line), "SET b v PXAT %lld", (long long)(g_fc.wall + 5000));
    EXPECT(line, "+OK\r\n");
    EXPECT("PTTL b", ":5000\r\n");
    /* 지난 절대 시각: OK 이지만 키는 없다 */
    EXPECT("SET b v EXAT 1", "+OK\r\n");
    EXPECT("GET b", "$-1\r\n");
    /* 일반 SET 은 TTL 제거 */
    EXPECT("SET a v2", "+OK\r\n");
    EXPECT("TTL a", ":-1\r\n");
    teardown();
}

/* ---------- TTL 계열 ---------- */

TEST(test_expire_family) {
    ASSERT(setup());
    EXPECT("EXPIRE none 10", ":0\r\n");
    EXPECT("TTL none", ":-2\r\n");
    EXPECT("SET k v", "+OK\r\n");
    EXPECT("TTL k", ":-1\r\n");
    EXPECT("EXPIRE k 100", ":1\r\n");
    EXPECT("TTL k", ":100\r\n");
    EXPECT("PERSIST k", ":1\r\n");
    EXPECT("TTL k", ":-1\r\n");
    EXPECT("PERSIST k", ":0\r\n");
    EXPECT("PERSIST none", ":0\r\n");
    EXPECT("PEXPIRE k 1500", ":1\r\n");
    EXPECT("PTTL k", ":1500\r\n");
    char line[96];
    snprintf(line, sizeof(line), "PEXPIREAT k %lld", (long long)(g_fc.wall + 2000));
    EXPECT(line, ":1\r\n");
    EXPECT("PTTL k", ":2000\r\n");
    snprintf(line, sizeof(line), "EXPIREAT k %lld", (long long)(g_fc.wall / 1000 + 7));
    EXPECT(line, ":1\r\n");
    EXPECT("TTL k", ":7\r\n");
    EXPECT("EXPIRE k x", NOT_INT);
    EXPECT("EXPIRE k 9223372036854775807", "-ERR invalid expire time in 'expire' command\r\n");
    EXPECT("EXPIRE k", WRONG_ARGS("expire"));
    /* 0 이하·과거 시각은 즉시 삭제 */
    EXPECT("EXPIRE k 0", ":1\r\n");
    EXPECT("GET k", "$-1\r\n");
    EXPECT("SET k v", "+OK\r\n");
    EXPECT("EXPIREAT k 1", ":1\r\n");
    EXPECT("EXISTS k", ":0\r\n");
    /* 만료 경계: 정확히 그 시각부터 없음 */
    EXPECT("SET e v PX 50", "+OK\r\n");
    g_fc.wall += 49;
    EXPECT("GET e", "$1\r\nv\r\n");
    g_fc.wall += 1;
    EXPECT("GET e", "$-1\r\n");
    teardown();
}

/* ---------- INCR 계열 ---------- */

TEST(test_incr_family) {
    ASSERT(setup());
    EXPECT("INCR c", ":1\r\n");
    EXPECT("INCRBY c 10", ":11\r\n");
    EXPECT("DECR c", ":10\r\n");
    EXPECT("DECRBY c 3", ":7\r\n");
    EXPECT("INCRBY c -7", ":0\r\n");
    EXPECT("INCRBY c x", NOT_INT);
    EXPECT("SET s abc", "+OK\r\n");
    EXPECT("INCR s", NOT_INT);
    EXPECT("SET s 007", "+OK\r\n");
    EXPECT("INCR s", NOT_INT);
    EXPECT("SET m 9223372036854775807", "+OK\r\n");
    EXPECT("INCR m", "-ERR increment or decrement would overflow\r\n");
    EXPECT("GET m", "$19\r\n9223372036854775807\r\n");
    EXPECT("SET m -9223372036854775808", "+OK\r\n");
    EXPECT("DECR m", "-ERR increment or decrement would overflow\r\n");
    EXPECT("DECRBY c -9223372036854775808", "-ERR decrement would overflow\r\n");
    /* INCR 은 TTL 을 유지 */
    EXPECT("SET t 5 EX 100", "+OK\r\n");
    EXPECT("INCR t", ":6\r\n");
    EXPECT("TTL t", ":100\r\n");
    EXPECT("INCR", WRONG_ARGS("incr"));
    teardown();
}

/* ---------- DEL/EXISTS/KEYS/SCAN/DBSIZE ---------- */

TEST(test_del_exists_dbsize_flushall) {
    ASSERT(setup());
    EXPECT("SET a 1", "+OK\r\n");
    EXPECT("SET b 2", "+OK\r\n");
    EXPECT("EXISTS a a b z", ":3\r\n");
    EXPECT("DBSIZE", ":2\r\n");
    EXPECT("DEL a b c", ":2\r\n");
    EXPECT("DEL a", ":0\r\n");
    EXPECT("DBSIZE", ":0\r\n");
    EXPECT("SET a 1", "+OK\r\n");
    EXPECT("FLUSHALL", "+OK\r\n");
    EXPECT("DBSIZE", ":0\r\n");
    EXPECT("FLUSHALL ASYNC", "+OK\r\n");
    EXPECT("FLUSHALL now", SYNTAX);
    EXPECT("DEL", WRONG_ARGS("del"));
    EXPECT("DBSIZE x", WRONG_ARGS("dbsize"));
    teardown();
}

TEST(test_keys_pattern) {
    ASSERT(setup());
    EXPECT("SET user:1 a", "+OK\r\n");
    EXPECT("SET user:2 b", "+OK\r\n");
    EXPECT("SET other c", "+OK\r\n");
    exec_line("KEYS user:*");
    CHECK(g_out.len > 4 && memcmp(g_out.data, "*2\r\n", 4) == 0);
    CHECK(out_contains("$6\r\nuser:1\r\n"));
    CHECK(out_contains("$6\r\nuser:2\r\n"));
    CHECK(!out_contains("other"));
    EXPECT("KEYS nomatch*", "*0\r\n");
    EXPECT("KEYS user:[2]", "*1\r\n$6\r\nuser:2\r\n");
    teardown();
}

/* "*2\r\n$n\r\n<cursor>\r\n*k\r\n($len\r\n<key>\r\n)*" 해석: 커서와 키 목록 */
static bool parse_scan_reply(uint64_t *cursor, bool *seen, size_t nseen) {
    const char *p = (const char *)g_out.data;
    const char *end = p + g_out.len;
    if (g_out.len < 4 || memcmp(p, "*2\r\n$", 5) != 0) {
        return false;
    }
    p = strchr(p + 5, '\n') + 1;
    *cursor = strtoull(p, NULL, 10);
    p = strchr(p, '\n') + 1;
    if (*p != '*') {
        return false;
    }
    long k = strtol(p + 1, NULL, 10);
    p = strchr(p, '\n') + 1;
    for (long i = 0; i < k && p < end; i++) {
        p = strchr(p, '\n') + 1; /* $len 줄 */
        int id = atoi(p + 4);    /* "key:<id>" */
        if (id >= 0 && (size_t)id < nseen) {
            seen[id] = true;
        }
        p = strchr(p, '\n') + 1;
    }
    return true;
}

TEST(test_scan_full_iteration) {
    ASSERT(setup());
    char line[64];
    for (int i = 0; i < 200; i++) {
        snprintf(line, sizeof(line), "SET key:%d v", i);
        exec_line(line);
    }
    EXPECT("SET zzz v", "+OK\r\n");
    bool seen[200] = {false};
    uint64_t cursor = 0;
    int rounds = 0;
    do {
        snprintf(line, sizeof(line), "SCAN %llu MATCH key:* COUNT 7", (unsigned long long)cursor);
        exec_line(line);
        ASSERT(parse_scan_reply(&cursor, seen, 200));
        rounds++;
    } while (cursor != 0 && rounds < 10000);
    CHECK(rounds > 1);
    size_t missing = 0;
    for (int i = 0; i < 200; i++) {
        missing += seen[i] ? 0 : 1;
    }
    CHECK_EQ(missing, 0);
    EXPECT("SCAN x", "-ERR invalid cursor\r\n");
    EXPECT("SCAN 0 COUNT 0", SYNTAX);
    EXPECT("SCAN 0 COUNT x", NOT_INT);
    EXPECT("SCAN 0 MATCH", SYNTAX);
    EXPECT("SCAN 0 TYPE string", SYNTAX);
    EXPECT("SCAN", WRONG_ARGS("scan"));
    teardown();
}

/* ---------- INVALIDATE ---------- */

TEST(test_invalidate) {
    ASSERT(setup());
    EXPECT("SET cache:a 1", "+OK\r\n");
    EXPECT("SET cache:b 2", "+OK\r\n");
    EXPECT("SET x:1 3", "+OK\r\n");
    EXPECT("SET keep 4", "+OK\r\n");
    EXPECT("INVALIDATE cache:*", ":2\r\n");
    EXPECT("INVALIDATE PREFIX x:", ":1\r\n");
    EXPECT("INVALIDATE prefix x:", ":0\r\n");
    EXPECT("INVALIDATE PREFIX", ":0\r\n"); /* 인자 하나면 glob "PREFIX" */
    EXPECT("INVALIDATE a b c", SYNTAX);
    EXPECT("INVALIDATE", WRONG_ARGS("invalidate"));
    EXPECT("DBSIZE", ":1\r\n");
    teardown();
}

/* ---------- INFO/COMMAND/CONFIG ---------- */

TEST(test_info_command_config) {
    ASSERT(setup());
    EXPECT("SET k v", "+OK\r\n");
    EXPECT("GET k", "$1\r\nv\r\n");
    EXPECT("GET nope", "$-1\r\n");
    exec_line("INFO");
    CHECK(g_out.len > 0 && g_out.data[0] == '$');
    CHECK(out_contains("# Server\r\n"));
    CHECK(out_contains("keyspace_hits:1\r\n"));
    CHECK(out_contains("keyspace_misses:1\r\n"));
    CHECK(out_contains("aof_enabled:0\r\n"));
    CHECK(out_contains("aof_rewrite_mode:synchronous\r\n"));
    CHECK(out_contains("db0:keys=1,expires=0"));
    CHECK(out_contains("total_commands_processed:"));
    exec_line("INFO stats");
    CHECK(out_contains("# Stats") && !out_contains("# Server"));
    CHECK(g_info.total_commands >= 5);

    EXPECT("COMMAND", "*0\r\n");
    char want[32];
    snprintf(want, sizeof(want), ":%zu\r\n", mr_cmd_count());
    EXPECT("COMMAND COUNT", want);
    EXPECT("command docs", "*0\r\n");
    exec_line("COMMAND LIST");
    CHECK(out_contains("$10\r\ninvalidate\r\n"));
    EXPECT("COMMAND FOO", "-ERR unknown subcommand 'FOO'. Try COMMAND HELP.\r\n");

    EXPECT("CONFIG GET maxclients", "*2\r\n$10\r\nmaxclients\r\n$5\r\n10000\r\n");
    EXPECT("CONFIG GET MAXCLIENTS", "*2\r\n$10\r\nmaxclients\r\n$5\r\n10000\r\n");
    EXPECT("CONFIG GET save", "*2\r\n$4\r\nsave\r\n$0\r\n\r\n");
    EXPECT("CONFIG GET appendonly", "*2\r\n$10\r\nappendonly\r\n$2\r\nno\r\n");
    EXPECT("CONFIG GET nosuch", "*0\r\n");
    exec_line("CONFIG GET *");
    CHECK(g_out.len > 3 && memcmp(g_out.data, "*16\r\n", 5) == 0);
    EXPECT("CONFIG GET", "-ERR wrong number of arguments for 'config|get' command\r\n");
    EXPECT("CONFIG", WRONG_ARGS("config"));
    EXPECT("CONFIG SET a b", "-ERR CONFIG SET/RESETSTAT/REWRITE are not supported\r\n");
    EXPECT("CONFIG X", "-ERR unknown subcommand 'X'. Try CONFIG HELP.\r\n");

    CHECK(mr_cmd_is_write("set", 3) && mr_cmd_is_write("FLUSHALL", 8));
    CHECK(!mr_cmd_is_write("get", 3) && !mr_cmd_is_write("nope", 4));
    teardown();
}

TEST(test_info_without_server_info) {
    ASSERT(setup());
    g_env.info = NULL;
    exec_line("INFO");
    CHECK(out_contains("connected_clients:0"));
    EXPECT("CONFIG GET port", "*2\r\n$4\r\nport\r\n$1\r\n0\r\n");
    teardown();
}

/* ---------- 메모리 부족 ---------- */

TEST(test_reply_nomem) {
    ASSERT(setup());
    mr_slice argv[2] = {{(const unsigned char *)"ECHO", 4}, {(const unsigned char *)"hi", 2}};
    mt_fail_alloc_after(1);
    CHECK(mr_cmd_exec(&g_env, 2, argv, &g_out, NULL) == MR_NOMEM);
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
    EXPECT("SET cnt 5 PX 60000", "+OK\r\n");
    EXPECT("INCRBY cnt 10", ":15\r\n");
    EXPECT("SET gone v", "+OK\r\n");
    EXPECT("DEL gone", ":1\r\n");
    EXPECT("SET p v EX 50", "+OK\r\n");
    EXPECT("PERSIST p", ":1\r\n");
    EXPECT("SET e v", "+OK\r\n");
    EXPECT("PEXPIRE e 3000", ":1\r\n");
    EXPECT("SET z v", "+OK\r\n");
    EXPECT("EXPIRE z 0", ":1\r\n");
    EXPECT("SET c:1 v", "+OK\r\n");
    EXPECT("SET c:2 v", "+OK\r\n");
    EXPECT("SET d:1 v", "+OK\r\n");
    EXPECT("INVALIDATE c:*", ":2\r\n");
    EXPECT("INVALIDATE PREFIX d:", ":1\r\n");
    g_fc.wall += 1000;
    ASSERT(reopen_from_aof());
    EXPECT("GET plain", "$1\r\nv\r\n");
    EXPECT("TTL ttl", ":99\r\n");
    EXPECT("GET cnt", "$2\r\n15\r\n");
    EXPECT("PTTL cnt", ":59000\r\n");
    EXPECT("EXISTS gone z c:1 c:2 d:1", ":0\r\n");
    EXPECT("TTL p", ":-1\r\n");
    EXPECT("PTTL e", ":2000\r\n");
    EXPECT("DBSIZE", ":5\r\n");
    /* rewrite 후에도 동일 */
    EXPECT("BGREWRITEAOF", "+Append only file rewritten synchronously\r\n");
    CHECK_EQ(g_env.aof_rewrites, 1);
    ASSERT(reopen_from_aof());
    EXPECT("DBSIZE", ":5\r\n");
    EXPECT("PTTL cnt", ":59000\r\n");
    EXPECT("FLUSHALL", "+OK\r\n");
    ASSERT(reopen_from_aof());
    EXPECT("DBSIZE", ":0\r\n");
    mr_aof_close(g_env.aof);
    tmp_teardown();
    teardown();
}

TEST(test_aof_failure_blocks_writes) {
    ASSERT(setup());
    EXPECT("BGREWRITEAOF", "-ERR AOF is not enabled (start the server with --aof PATH)\r\n");
    ASSERT(tmp_setup());
    g_env.aof = open_aof(g_store);
    ASSERT(g_env.aof != NULL);
    EXPECT("SET k v", "+OK\r\n");
    /* 기록 실패를 흉내: 플래그가 서면 쓰기만 막히고 읽기는 된다 */
    g_env.aof_write_failed = true;
    EXPECT("SET k v2", "-ERR AOF write error earlier; write commands are disabled until "
                       "BGREWRITEAOF succeeds\r\n");
    EXPECT("GET k", "$1\r\nv\r\n");
    exec_line("INFO persistence");
    CHECK(out_contains("aof_last_write_status:err"));
    EXPECT("BGREWRITEAOF", "+Append only file rewritten synchronously\r\n");
    CHECK(!g_env.aof_write_failed);
    EXPECT("SET k v3", "+OK\r\n");
    mr_aof_close(g_env.aof);
    tmp_teardown();
    teardown();
}

MT_MAIN(test_ping_echo_select_quit, test_unknown_and_empty, test_set_get_options,
        test_set_binary_and_utf8, test_set_expire_options, test_expire_family,
        test_incr_family, test_del_exists_dbsize_flushall, test_keys_pattern,
        test_scan_full_iteration, test_invalidate, test_info_command_config,
        test_info_without_server_info, test_reply_nomem, test_aof_roundtrip_all_writes,
        test_aof_failure_blocks_writes)
