/*
 * 기능 테스트: 실제 TCP 서버(포트 0)와 loopback 클라이언트 소켓을 "같은 스레드"에서
 * 번갈아 구동한다. 클라이언트 소켓은 non-blocking 이고, 기다려야 할 때마다
 * mr_server_run_once() 로 서버 틱을 돌린다. 스레드가 없으므로 결과가 결정적이다.
 *
 * 데드락 방지: alarm 대신 모든 대기 루프가 실제 단조 시계 기준 데드라인을 확인한다.
 * 누수 방지: 각 테스트 앞뒤로 /proc/self/fd 개수를 비교한다.
 */
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <signal.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "minitest.h"
#include "miniredis/server.h"

/* ---------- 시계 ---------- */

typedef struct fake_clock {
    int64_t wall;
    int64_t mono;
} fake_clock;

static int64_t fake_wall(void *ctx) { return ((fake_clock *)ctx)->wall; }
static int64_t fake_mono(void *ctx) { return ((fake_clock *)ctx)->mono; }

#define T0 1700000000000LL
#define TEST_DEADLINE_MS 30000

static fake_clock g_fc = {T0, 0};
static const mr_clock g_clk = {fake_wall, fake_mono, &g_fc};

static int64_t real_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static int64_t g_deadline;

static bool timed_out(void) {
    if (real_ms() <= g_deadline) {
        return false;
    }
    fprintf(stderr, "    test deadline exceeded\n");
    return true;
}

/* ---------- 서버 ---------- */

static mr_server *g_srv;

static void base_cfg(mr_server_config *c) {
    mr_server_config_default(c);
    c->port = 0;
    c->clock = &g_clk;
    c->log_level = MR_LOG_SILENT;
}

static int count_fds(void) {
    DIR *d = opendir("/proc/self/fd");
    if (d == NULL) {
        return -1;
    }
    int n = 0;
    while (readdir(d) != NULL) {
        n++;
    }
    closedir(d);
    return n;
}

static int g_fds_before;

/* 가짜 시계를 건드리지 않고 서버를 띄운다(AOF 재시작 시 시각 유지용). */
static bool start_keep_clock(const mr_server_config *cfg) {
    signal(SIGPIPE, SIG_IGN);
    g_fds_before = count_fds();
    g_deadline = real_ms() + TEST_DEADLINE_MS;
    char err[256] = "";
    mr_status st = mr_server_create(cfg, &g_srv, err, sizeof(err));
    if (st != MR_OK) {
        fprintf(stderr, "    server create failed: %s\n", err);
        return false;
    }
    return true;
}

static bool start(const mr_server_config *cfg) {
    g_fc.wall = T0;
    g_fc.mono = 0;
    return start_keep_clock(cfg);
}

static void tick(int timeout_ms) { (void)mr_server_run_once(g_srv, timeout_ms); }

static void ticks(int n) {
    for (int i = 0; i < n; i++) {
        tick(1);
    }
}

/* ---------- 클라이언트 ---------- */

typedef struct client {
    int fd;
    mr_buf in;
} client;

static bool cli_open_rcvbuf(client *c, int rcvbuf) {
    mr_buf_init(&c->in);
    c->fd = socket(AF_INET, SOCK_STREAM, 0);
    if (c->fd < 0) {
        return false;
    }
    if (rcvbuf > 0) {
        (void)setsockopt(c->fd, SOL_SOCKET, SO_RCVBUF, &rcvbuf, sizeof(rcvbuf));
    }
    int fl = fcntl(c->fd, F_GETFL, 0);
    (void)fcntl(c->fd, F_SETFL, fl | O_NONBLOCK);
    struct sockaddr_in sa;
    memset(&sa, 0, sizeof(sa));
    sa.sin_family = AF_INET;
    sa.sin_port = htons(mr_server_port(g_srv));
    sa.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (connect(c->fd, (struct sockaddr *)&sa, sizeof(sa)) != 0 && errno != EINPROGRESS) {
        close(c->fd);
        c->fd = -1;
        return false;
    }
    return true;
}

static bool cli_open(client *c) { return cli_open_rcvbuf(c, 0); }

static void cli_close(client *c) {
    if (c->fd >= 0) {
        close(c->fd);
        c->fd = -1;
    }
    mr_buf_free(&c->in);
}

static bool cli_send(client *c, const void *p, size_t n) {
    const unsigned char *b = p;
    while (n > 0) {
        if (timed_out()) {
            return false;
        }
        ssize_t w = send(c->fd, b, n, MSG_NOSIGNAL);
        if (w > 0) {
            b += w;
            n -= (size_t)w;
        } else if (w < 0 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR ||
                             errno == ENOTCONN)) {
            tick(1); /* 서버가 읽어 가야 소켓 버퍼가 빈다 */
        } else {
            return false;
        }
    }
    return true;
}

static bool cli_send_str(client *c, const char *s) { return cli_send(c, s, strlen(s)); }

static bool cli_send_argv(client *c, size_t argc, const char *const *argv, const size_t *lens) {
    mr_buf b;
    mr_buf_init(&b);
    char hdr[32];
    int n = snprintf(hdr, sizeof(hdr), "*%zu\r\n", argc);
    bool ok = mr_buf_append(&b, hdr, (size_t)n) == MR_OK;
    for (size_t i = 0; i < argc && ok; i++) {
        ok = mr_resp_bulk(&b, argv[i], lens != NULL ? lens[i] : strlen(argv[i])) == MR_OK;
    }
    ok = ok && cli_send(c, b.data, b.len);
    mr_buf_free(&b);
    return ok;
}

/* 줄 끝("\r\n")의 '\r' 위치. 없으면 SIZE_MAX. */
static size_t line_end(const unsigned char *p, size_t n) {
    for (size_t i = 0; i + 1 < n; i++) {
        if (p[i] == '\r' && p[i + 1] == '\n') {
            return i;
        }
    }
    return SIZE_MAX;
}

/* 첫 응답 하나의 길이. 1 완성, 0 미완, -1 형식 오류. */
static int reply_span(const unsigned char *p, size_t n, size_t *len) {
    size_t e = line_end(p, n);
    if (n == 0 || e == SIZE_MAX) {
        return 0;
    }
    long long v = strtoll((const char *)p + 1, NULL, 10);
    switch (p[0]) {
    case '+':
    case '-':
    case ':':
        *len = e + 2;
        return 1;
    case '$':
        if (v < 0) {
            *len = e + 2;
            return 1;
        }
        if (n < e + 2 + (size_t)v + 2) {
            return 0;
        }
        *len = e + 2 + (size_t)v + 2;
        return 1;
    case '*': {
        size_t off = e + 2;
        for (long long i = 0; i < v; i++) {
            size_t sub;
            int r = reply_span(p + off, n - off, &sub);
            if (r != 1) {
                return r;
            }
            off += sub;
        }
        *len = off;
        return 1;
    }
    default:
        return -1;
    }
}

/* 응답 하나를 out 으로 꺼낸다. EOF·형식 오류·타임아웃이면 false. */
static bool cli_read_reply(client *c, mr_buf *out) {
    for (;;) {
        size_t len;
        int r = reply_span(c->in.data, c->in.len, &len);
        if (r < 0) {
            return false;
        }
        if (r == 1) {
            out->len = 0;
            (void)mr_buf_append(out, c->in.data, len);
            memmove(c->in.data, c->in.data + len, c->in.len - len);
            c->in.len -= len;
            return true;
        }
        if (timed_out() || mr_buf_reserve(&c->in, 65536) != MR_OK) {
            return false;
        }
        ssize_t n = recv(c->fd, c->in.data + c->in.len, c->in.cap - c->in.len, 0);
        if (n > 0) {
            c->in.len += (size_t)n;
        } else if (n == 0) {
            return false;
        } else if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) {
            tick(1);
        } else {
            return false;
        }
    }
}

/* 서버가 연결을 닫을 때까지(남은 바이트는 버림) 기다린다. */
static bool cli_wait_eof(client *c) {
    char tmp[65536];
    for (;;) {
        if (timed_out()) {
            return false;
        }
        ssize_t n = recv(c->fd, tmp, sizeof(tmp), 0);
        if (n == 0 || (n < 0 && errno == ECONNRESET)) {
            return true;
        }
        if (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR) {
            return false;
        }
        tick(1);
    }
}

static void print_escaped(const unsigned char *p, size_t n) {
    size_t lim = n > 200 ? 200 : n;
    for (size_t i = 0; i < lim; i++) {
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
    if (n > lim) {
        fputs("...", stderr);
    }
}

static bool reply_is(const mr_buf *got, const char *want) {
    size_t n = strlen(want);
    if (got->len == n && memcmp(got->data, want, n) == 0) {
        return true;
    }
    fprintf(stderr, "    got '");
    print_escaped(got->data, got->len);
    fprintf(stderr, "' want '");
    print_escaped((const unsigned char *)want, n);
    fprintf(stderr, "'\n");
    return false;
}

static bool expect(client *c, const char *want) {
    mr_buf r;
    mr_buf_init(&r);
    bool ok = cli_read_reply(c, &r) && reply_is(&r, want);
    if (!ok && r.len == 0) {
        fprintf(stderr, "    no reply (want '%s')\n", want);
    }
    mr_buf_free(&r);
    return ok;
}

/* 명령 하나 보내고 응답 비교. */
static bool roundtrip(client *c, size_t argc, const char *const *argv, const char *want) {
    if (!cli_send_argv(c, argc, argv, NULL)) {
        return false;
    }
    bool ok = expect(c, want);
    if (!ok) {
        fprintf(stderr, "    command: %s%s\n", argv[0], argc > 1 ? " ..." : "");
    }
    return ok;
}

#define CMD(c, want, ...)                                                                      \
    do {                                                                                       \
        const char *const argv_[] = {__VA_ARGS__};                                             \
        CHECK(roundtrip((c), sizeof(argv_) / sizeof(argv_[0]), argv_, (want)));                 \
    } while (0)

/* INFO 의 "name:<정수>" 값. 없으면 -1. */
static long long info_field(client *c, const char *name) {
    const char *const argv[] = {"INFO"};
    mr_buf r;
    mr_buf_init(&r);
    long long v = -1;
    if (cli_send_argv(c, 1, argv, NULL) && cli_read_reply(c, &r) &&
        mr_buf_append(&r, "", 1) == MR_OK) {
        char key[64];
        snprintf(key, sizeof(key), "\n%s:", name);
        const char *p = strstr((const char *)r.data, key);
        if (p != NULL) {
            v = strtoll(p + strlen(key), NULL, 10);
        }
    }
    mr_buf_free(&r);
    return v;
}

static void finish(void) {
    CHECK(mr_server_destroy(g_srv) == MR_OK);
    g_srv = NULL;
    /* 서버·클라이언트 소켓이 모두 닫혀야 한다(누수 없음). */
    CHECK_EQ(count_fds(), g_fds_before);
}

/* ---------- 명령 정상·오류 경로 ---------- */

TEST(test_all_commands_over_tcp) {
    mr_server_config cfg;
    base_cfg(&cfg);
    ASSERT(start(&cfg));
    client c;
    ASSERT(cli_open(&c));
    CMD(&c, "+PONG\r\n", "PING");
    CMD(&c, "$2\r\nhi\r\n", "ping", "hi");
    CMD(&c, "$3\r\nabc\r\n", "ECHO", "abc");
    CMD(&c, "+OK\r\n", "SELECT", "0");
    CMD(&c, "-ERR DB index is out of range\r\n", "SELECT", "3");
    CMD(&c, "+OK\r\n", "SET", "k", "v");
    CMD(&c, "$1\r\nv\r\n", "GET", "k");
    CMD(&c, "$-1\r\n", "GET", "missing");
    CMD(&c, "$-1\r\n", "SET", "k", "x", "NX");
    CMD(&c, "-ERR syntax error\r\n", "SET", "k", "x", "NX", "XX");
    CMD(&c, "-ERR wrong number of arguments for 'get' command\r\n", "GET");
    CMD(&c, "-ERR unknown command 'NOPE', with args beginning with: 'a' \r\n", "NOPE", "a");
    CMD(&c, ":1\r\n", "EXISTS", "k");
    CMD(&c, ":1\r\n", "EXPIRE", "k", "100");
    CMD(&c, ":100\r\n", "TTL", "k");
    CMD(&c, ":100000\r\n", "PTTL", "k");
    CMD(&c, ":1\r\n", "PERSIST", "k");
    CMD(&c, ":-1\r\n", "TTL", "k");
    CMD(&c, ":-2\r\n", "TTL", "missing");
    CMD(&c, ":1\r\n", "PEXPIRE", "k", "5000");
    CMD(&c, ":1\r\n", "EXPIREAT", "k", "1700000100");
    CMD(&c, ":100\r\n", "TTL", "k");
    CMD(&c, ":1\r\n", "PEXPIREAT", "k", "1700000200000");
    CMD(&c, ":200\r\n", "TTL", "k");
    CMD(&c, ":1\r\n", "INCR", "n");
    CMD(&c, ":11\r\n", "INCRBY", "n", "10");
    CMD(&c, ":10\r\n", "DECR", "n");
    CMD(&c, ":5\r\n", "DECRBY", "n", "5");
    CMD(&c, "-ERR value is not an integer or out of range\r\n", "INCR", "k");
    CMD(&c, "+OK\r\n", "SET", "max", "9223372036854775807");
    CMD(&c, "-ERR increment or decrement would overflow\r\n", "INCR", "max");
    CMD(&c, ":2\r\n", "DEL", "max", "n", "nope");
    CMD(&c, "*1\r\n$1\r\nk\r\n", "KEYS", "*");
    CMD(&c, "*2\r\n$1\r\n0\r\n*1\r\n$1\r\nk\r\n", "SCAN", "0");
    CMD(&c, ":1\r\n", "DBSIZE");
    CMD(&c, ":1\r\n", "INVALIDATE", "k*");
    CMD(&c, ":0\r\n", "INVALIDATE", "PREFIX", "k");
    CMD(&c, "*0\r\n", "COMMAND");
    CMD(&c, "*0\r\n", "COMMAND", "DOCS");
    CMD(&c, "*0\r\n", "CONFIG", "GET", "nosuch");
    CMD(&c, "*2\r\n$10\r\nappendonly\r\n$2\r\nno\r\n", "CONFIG", "GET", "appendonly");
    CMD(&c, "-ERR AOF is not enabled (start the server with --aof PATH)\r\n", "BGREWRITEAOF");
    CMD(&c, "+OK\r\n", "FLUSHALL");
    CHECK_EQ(info_field(&c, "connected_clients"), 1);
    CHECK(info_field(&c, "total_commands_processed") > 40);
    CHECK_EQ(info_field(&c, "tcp_port"), mr_server_port(g_srv));
    CMD(&c, "+OK\r\n", "QUIT");
    CHECK(cli_wait_eof(&c));
    cli_close(&c);
    ticks(2);
    CHECK_EQ(mr_server_client_count(g_srv), 0);
    finish();
}

/* ---------- 스트리밍·파이프라이닝·inline ---------- */

TEST(test_split_one_byte_at_a_time) {
    mr_server_config cfg;
    base_cfg(&cfg);
    ASSERT(start(&cfg));
    client c;
    ASSERT(cli_open(&c));
    const char *req = "*3\r\n$3\r\nSET\r\n$5\r\nsplit\r\n$11\r\nhello world\r\n"
                      "*2\r\n$3\r\nGET\r\n$5\r\nsplit\r\n";
    for (size_t i = 0; req[i] != '\0'; i++) {
        CHECK(cli_send(&c, req + i, 1));
        tick(0); /* 바이트마다 서버가 부분 입력을 처리하게 한다 */
    }
    CHECK(expect(&c, "+OK\r\n"));
    CHECK(expect(&c, "$11\r\nhello world\r\n"));
    cli_close(&c);
    finish();
}

TEST(test_pipeline_100) {
    mr_server_config cfg;
    base_cfg(&cfg);
    ASSERT(start(&cfg));
    client c;
    ASSERT(cli_open(&c));
    mr_buf req;
    mr_buf_init(&req);
    char line[64];
    for (int i = 0; i < 50; i++) {
        int n = snprintf(line, sizeof(line), "*3\r\n$3\r\nSET\r\n$4\r\nk%03d\r\n$1\r\n%d\r\n", i,
                         i % 10);
        (void)mr_buf_append(&req, line, (size_t)n);
    }
    for (int i = 0; i < 50; i++) {
        int n = snprintf(line, sizeof(line), "GET k%03d\r\n", i); /* inline 섞기 */
        (void)mr_buf_append(&req, line, (size_t)n);
    }
    ASSERT(cli_send(&c, req.data, req.len)); /* 한 번에 전송 */
    mr_buf_free(&req);
    for (int i = 0; i < 50; i++) {
        CHECK(expect(&c, "+OK\r\n"));
    }
    for (int i = 0; i < 50; i++) {
        snprintf(line, sizeof(line), "$1\r\n%d\r\n", i % 10);
        CHECK(expect(&c, line));
    }
    cli_close(&c);
    finish();
}

TEST(test_inline_commands) {
    mr_server_config cfg;
    base_cfg(&cfg);
    ASSERT(start(&cfg));
    client c;
    ASSERT(cli_open(&c));
    ASSERT(cli_send_str(&c, "PING\r\nSET a  b\nGET a\r\n\r\nECHO x\r\n"));
    CHECK(expect(&c, "+PONG\r\n"));
    CHECK(expect(&c, "+OK\r\n"));
    CHECK(expect(&c, "$1\r\nb\r\n"));
    CHECK(expect(&c, "$1\r\nx\r\n")); /* 빈 줄은 무시 */
    cli_close(&c);
    finish();
}

/* ---------- 큰 값·바이너리·한글 ---------- */

TEST(test_large_binary_utf8_values) {
    mr_server_config cfg;
    base_cfg(&cfg);
    ASSERT(start(&cfg));
    client c;
    ASSERT(cli_open(&c));
    size_t big = (size_t)4 * 1024 * 1024 + 123;
    unsigned char *val = malloc(big);
    ASSERT(val != NULL);
    uint32_t x = 12345;
    for (size_t i = 0; i < big; i++) {
        x = x * 1103515245u + 12345u;
        val[i] = (unsigned char)(x >> 16); /* NUL·CR·LF 모두 포함 */
    }
    const char *argv[3] = {"SET", "big\0key", (const char *)val};
    size_t lens[3] = {3, 7, big};
    CHECK(cli_send_argv(&c, 3, argv, lens));
    CHECK(expect(&c, "+OK\r\n"));
    const char *gargv[2] = {"GET", "big\0key"};
    size_t glens[2] = {3, 7};
    CHECK(cli_send_argv(&c, 2, gargv, glens));
    mr_buf r;
    mr_buf_init(&r);
    CHECK(cli_read_reply(&c, &r));
    char hdr[32];
    int hn = snprintf(hdr, sizeof(hdr), "$%zu\r\n", big);
    CHECK(r.len == (size_t)hn + big + 2);
    CHECK(r.len >= (size_t)hn + big && memcmp(r.data, hdr, (size_t)hn) == 0 &&
          memcmp(r.data + hn, val, big) == 0);
    mr_buf_free(&r);
    free(val);
    CMD(&c, "+OK\r\n", "SET", "사용자:한글", "값😀");
    CMD(&c, "$7\r\n값😀\r\n", "GET", "사용자:한글");
    CMD(&c, "*1\r\n$16\r\n사용자:한글\r\n", "KEYS", "사용자*");
    cli_close(&c);
    finish();
}

/* ---------- TTL(가짜 시계) ---------- */

TEST(test_ttl_with_fake_clock) {
    mr_server_config cfg;
    base_cfg(&cfg);
    ASSERT(start(&cfg));
    client c;
    ASSERT(cli_open(&c));
    CMD(&c, "+OK\r\n", "SET", "s", "v", "EX", "10");
    g_fc.wall += 9999;
    CMD(&c, "$1\r\nv\r\n", "GET", "s");
    g_fc.wall += 1;
    CMD(&c, "$-1\r\n", "GET", "s");
    CMD(&c, "+OK\r\n", "SET", "p", "v", "PX", "500");
    CMD(&c, ":1\r\n", "PERSIST", "p");
    g_fc.wall += 100000;
    CMD(&c, "$1\r\nv\r\n", "GET", "p");
    /* 능동 만료: 아무도 접근하지 않아도 cron 의 sweep 이 지운다 */
    char key[16];
    for (int i = 0; i < 50; i++) {
        snprintf(key, sizeof(key), "vol%d", i);
        CMD(&c, "+OK\r\n", "SET", key, "v", "PX", "100");
    }
    CMD(&c, ":51\r\n", "DBSIZE"); /* p + vol0..49 (s 는 GET 때 lazy 삭제됨) */
    g_fc.wall += 1000;
    for (int i = 0; i < 5; i++) {
        g_fc.mono += 200; /* cron 주기(100ms) 경과 */
        tick(0);
    }
    CMD(&c, ":1\r\n", "DBSIZE");
    CHECK(info_field(&c, "expired_keys_active") >= 50);
    cli_close(&c);
    finish();
}

TEST(test_scan_consistency) {
    mr_server_config cfg;
    base_cfg(&cfg);
    ASSERT(start(&cfg));
    client c;
    ASSERT(cli_open(&c));
    enum { N = 500 };
    char key[16];
    for (int i = 0; i < N; i++) {
        snprintf(key, sizeof(key), "s:%d", i);
        const char *argv[3] = {"SET", key, "v"};
        CHECK(cli_send_argv(&c, 3, argv, NULL));
    }
    for (int i = 0; i < N; i++) {
        CHECK(expect(&c, "+OK\r\n"));
    }
    static bool seen[N];
    memset(seen, 0, sizeof(seen));
    char cur[24] = "0";
    mr_buf r;
    mr_buf_init(&r);
    int rounds = 0;
    do {
        const char *argv[4] = {"SCAN", cur, "COUNT", "37"};
        ASSERT(cli_send_argv(&c, 4, argv, NULL) && cli_read_reply(&c, &r));
        (void)mr_buf_append(&r, "", 1);
        /* *2\r\n$n\r\n<cursor>\r\n*k\r\n ... */
        char *p = strchr((char *)r.data + 4, '\n') + 1;
        size_t cl = (size_t)(strchr(p, '\r') - p);
        memcpy(cur, p, cl);
        cur[cl] = '\0';
        p = strchr(p, '\n') + 1;
        long k = strtol(p + 1, NULL, 10);
        p = strchr(p, '\n') + 1;
        for (long i = 0; i < k; i++) {
            p = strchr(p, '\n') + 1;
            int id = atoi(p + 2);
            if (id >= 0 && id < N) {
                seen[id] = true;
            }
            p = strchr(p, '\n') + 1;
        }
        rounds++;
    } while (strcmp(cur, "0") != 0 && rounds < 100000);
    mr_buf_free(&r);
    int missing = 0;
    for (int i = 0; i < N; i++) {
        missing += seen[i] ? 0 : 1;
    }
    CHECK_EQ(missing, 0);
    CHECK(rounds > 5);
    CMD(&c, ":500\r\n", "DBSIZE");
    CMD(&c, ":111\r\n", "INVALIDATE", "s:1*"); /* 1, 10-19, 100-199 */
    CMD(&c, ":389\r\n", "DBSIZE");
    cli_close(&c);
    finish();
}

/* ---------- 동시성(싱글 스레드 원자성) ---------- */

TEST(test_100_clients_incr) {
    mr_server_config cfg;
    base_cfg(&cfg);
    ASSERT(start(&cfg));
    enum { NC = 100, PER = 10 };
    static client cs[NC];
    for (int i = 0; i < NC; i++) {
        ASSERT(cli_open(&cs[i]));
    }
    /* 모든 클라이언트가 먼저 보내고 나서 응답을 읽는다: 서버 안에서 요청이 섞인다. */
    for (int k = 0; k < PER; k++) {
        for (int i = 0; i < NC; i++) {
            CHECK(cli_send_str(&cs[i], "*2\r\n$4\r\nINCR\r\n$7\r\ncounter\r\n"));
        }
    }
    static bool got[NC * PER + 1];
    memset(got, 0, sizeof(got));
    mr_buf r;
    mr_buf_init(&r);
    int bad = 0;
    for (int i = 0; i < NC; i++) {
        long long prev = 0;
        for (int k = 0; k < PER; k++) {
            if (!cli_read_reply(&cs[i], &r) || r.data[0] != ':') {
                bad++;
                continue;
            }
            long long v = strtoll((const char *)r.data + 1, NULL, 10);
            /* 각 값은 정확히 한 번만 나와야 하고, 한 연결 안에서는 증가해야 한다 */
            if (v < 1 || v > NC * PER || got[v] || v <= prev) {
                bad++;
            } else {
                got[v] = true;
            }
            prev = v;
        }
    }
    mr_buf_free(&r);
    CHECK_EQ(bad, 0);
    CMD(&cs[0], "$4\r\n1000\r\n", "GET", "counter");
    CHECK_EQ(info_field(&cs[0], "connected_clients"), NC);
    for (int i = 0; i < NC; i++) {
        cli_close(&cs[i]);
    }
    finish();
}

/* ---------- 연결 관리 ---------- */

TEST(test_maxclients) {
    mr_server_config cfg;
    base_cfg(&cfg);
    cfg.maxclients = 2;
    ASSERT(start(&cfg));
    client a;
    client b;
    client x;
    ASSERT(cli_open(&a));
    ASSERT(cli_open(&b));
    CMD(&a, "+PONG\r\n", "PING");
    CMD(&b, "+PONG\r\n", "PING");
    ASSERT(cli_open(&x));
    CHECK(expect(&x, "-ERR max number of clients reached\r\n"));
    CHECK(cli_wait_eof(&x));
    cli_close(&x);
    CMD(&a, "+PONG\r\n", "PING");
    CHECK_EQ(info_field(&a, "rejected_connections"), 1);
    /* 자리가 나면 다시 받는다 */
    cli_close(&b);
    ticks(3);
    ASSERT(cli_open(&x));
    CMD(&x, "+PONG\r\n", "PING");
    cli_close(&x);
    cli_close(&a);
    finish();
}

TEST(test_protocol_error_closes_connection) {
    mr_server_config cfg;
    base_cfg(&cfg);
    cfg.proto.max_bulk_len = 1024;
    ASSERT(start(&cfg));
    client bad;
    client good;
    ASSERT(cli_open(&good));
    CMD(&good, "+OK\r\n", "SET", "k", "v");

    ASSERT(cli_open(&bad));
    ASSERT(cli_send_str(&bad, "*1\r\n$abc\r\nPING\r\n"));
    CHECK(expect(&bad, "-ERR Protocol error: invalid bulk length\r\n"));
    CHECK(cli_wait_eof(&bad));
    cli_close(&bad);

    /* 상한 초과 bulk: 본문을 받기 전에 거절 */
    ASSERT(cli_open(&bad));
    ASSERT(cli_send_str(&bad, "*2\r\n$3\r\nGET\r\n$5000\r\n"));
    CHECK(expect(&bad, "-ERR Protocol error: invalid bulk length\r\n"));
    CHECK(cli_wait_eof(&bad));
    cli_close(&bad);

    ASSERT(cli_open(&bad));
    ASSERT(cli_send_str(&bad, "*2\r\n$3\r\nGET\r\n:1\r\n"));
    CHECK(expect(&bad, "-ERR Protocol error: expected '$'\r\n"));
    CHECK(cli_wait_eof(&bad));
    cli_close(&bad);

    /* 서버와 다른 연결은 멀쩡하다 */
    CMD(&good, "$1\r\nv\r\n", "GET", "k");
    cli_close(&good);
    finish();
}

TEST(test_abrupt_disconnects) {
    mr_server_config cfg;
    base_cfg(&cfg);
    ASSERT(start(&cfg));
    client keep;
    ASSERT(cli_open(&keep));
    CMD(&keep, "+OK\r\n", "SET", "big", "xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx");
    for (int round = 0; round < 3; round++) {
        client c;
        ASSERT(cli_open(&c));
        CMD(&c, "+PONG\r\n", "PING");
        if (round == 0) {
            /* 명령 중간에서 끊기 */
            CHECK(cli_send_str(&c, "*3\r\n$3\r\nSET\r\n$1"));
        } else if (round == 1) {
            /* 응답을 읽지 않고 끊기(서버 쓰기가 EPIPE/ECONNRESET) */
            for (int i = 0; i < 200; i++) {
                CHECK(cli_send_str(&c, "GET big\r\n"));
            }
        }
        tick(0);
        /* RST 로 끊기: SO_LINGER 0 */
        struct linger lg = {1, 0};
        (void)setsockopt(c.fd, SOL_SOCKET, SO_LINGER, &lg, sizeof(lg));
        cli_close(&c);
        ticks(5);
    }
    CHECK_EQ(mr_server_client_count(g_srv), 1);
    CMD(&keep, "+PONG\r\n", "PING");

    /* half-close: 요청 후 쓰기 방향만 닫아도 응답을 받고 연결이 정리된다 */
    client h;
    ASSERT(cli_open(&h));
    ASSERT(cli_send_str(&h, "PING\r\n"));
    shutdown(h.fd, SHUT_WR);
    CHECK(expect(&h, "+PONG\r\n"));
    CHECK(cli_wait_eof(&h));
    cli_close(&h);
    cli_close(&keep);
    finish();
}

TEST(test_output_buffer_limit) {
    mr_server_config cfg;
    base_cfg(&cfg);
    cfg.max_output_bytes = 256 * 1024;
    ASSERT(start(&cfg));
    client w;
    ASSERT(cli_open(&w));
    size_t big = 1024 * 1024;
    char *val = malloc(big);
    ASSERT(val != NULL);
    memset(val, 'z', big);
    const char *argv[3] = {"SET", "big", val};
    size_t lens[3] = {3, 3, big};
    CHECK(cli_send_argv(&w, 3, argv, lens));
    CHECK(expect(&w, "+OK\r\n"));
    free(val);

    client slow;
    ASSERT(cli_open_rcvbuf(&slow, 4096)); /* 커널 버퍼를 작게 해 출력이 서버에 쌓이게 */
    CMD(&slow, "+PONG\r\n", "PING");
    for (int i = 0; i < 40; i++) {
        CHECK(cli_send_str(&slow, "GET big\r\n"));
    }
    for (int i = 0; i < 50 && mr_server_client_count(g_srv) > 1; i++) {
        tick(1);
    }
    CHECK_EQ(mr_server_client_count(g_srv), 1); /* 느린 클라이언트만 끊김 */
    CHECK(cli_wait_eof(&slow));
    cli_close(&slow);
    CMD(&w, "+PONG\r\n", "PING");
    cli_close(&w);
    finish();
}

TEST(test_idle_timeout) {
    mr_server_config cfg;
    base_cfg(&cfg);
    cfg.idle_timeout_ms = 5000;
    ASSERT(start(&cfg));
    client c;
    client fresh;
    ASSERT(cli_open(&c));
    CMD(&c, "+PONG\r\n", "PING");
    g_fc.mono += 4000;
    ASSERT(cli_open(&fresh));
    CMD(&fresh, "+PONG\r\n", "PING");
    g_fc.mono += 2000; /* c 는 6초, fresh 는 2초 유휴 */
    ticks(2);
    CHECK(cli_wait_eof(&c));
    CMD(&fresh, "+PONG\r\n", "PING");
    cli_close(&c);
    cli_close(&fresh);
    finish();
}

TEST(test_quit_with_pipelined_tail) {
    mr_server_config cfg;
    base_cfg(&cfg);
    ASSERT(start(&cfg));
    client c;
    ASSERT(cli_open(&c));
    ASSERT(cli_send_str(&c, "SET a 1\r\nQUIT\r\nSET a 2\r\n"));
    CHECK(expect(&c, "+OK\r\n"));
    CHECK(expect(&c, "+OK\r\n"));
    CHECK(cli_wait_eof(&c));
    cli_close(&c);
    ASSERT(cli_open(&c));
    CMD(&c, "$1\r\n1\r\n", "GET", "a"); /* QUIT 뒤 명령은 실행되지 않음 */
    cli_close(&c);
    finish();
}

/* ---------- 수명 주기 ---------- */

TEST(test_run_stop_and_port_in_use) {
    mr_server_config cfg;
    base_cfg(&cfg);
    ASSERT(start(&cfg));
    /* 같은 포트로 두 번째 서버: 명확한 오류 */
    mr_server_config c2;
    base_cfg(&c2);
    c2.port = mr_server_port(g_srv);
    mr_server *dup = NULL;
    char err[256] = "";
    CHECK(mr_server_create(&c2, &dup, err, sizeof(err)) == MR_IO_ERROR);
    CHECK(dup == NULL);
    CHECK(strstr(err, "Address already in use") != NULL);
    /* 외부 정지 플래그(시그널 핸들러가 세우는 것)와 request_stop */
    mr_server_request_stop(g_srv);
    CHECK(mr_server_run(g_srv) == MR_OK);
    finish();

    volatile sig_atomic_t flag = 1;
    base_cfg(&cfg);
    cfg.stop_flag = &flag;
    ASSERT(start(&cfg));
    CHECK(mr_server_run(g_srv) == MR_OK);
    finish();

    /* 잘못된 설정 */
    base_cfg(&cfg);
    cfg.maxclients = 0;
    CHECK(mr_server_create(&cfg, &dup, err, sizeof(err)) == MR_INVALID_ARG);
    CHECK(dup == NULL);
    CHECK(mr_server_destroy(NULL) == MR_OK);
}

/* ---------- AOF ---------- */

static char g_dir[256];
static char g_aof[300];

static bool aof_dir_setup(void) {
    const char *tmp = getenv("TMPDIR");
    int n = snprintf(g_dir, sizeof(g_dir), "%s/mr_srv_XXXXXX", tmp != NULL ? tmp : "/tmp");
    if (n < 0 || (size_t)n >= sizeof(g_dir) || mkdtemp(g_dir) == NULL) {
        return false;
    }
    snprintf(g_aof, sizeof(g_aof), "%s/appendonly.aof", g_dir);
    return true;
}

static void aof_dir_teardown(void) {
    char p[400];
    unlink(g_aof);
    snprintf(p, sizeof(p), "%s.rewrite.tmp", g_aof);
    unlink(p);
    rmdir(g_dir);
}

static long file_size(const char *p) {
    struct stat sb;
    return stat(p, &sb) == 0 ? (long)sb.st_size : -1;
}

/* 같은 AOF 로 서버를 다시 띄운다(가짜 시계는 유지). */
static bool restart_with_aof(mr_server_config *cfg) {
    finish();
    return start_keep_clock(cfg);
}

static void check_restored(client *c) {
    CMD(c, "$5\r\nhello\r\n", "GET", "plain");
    CMD(c, ":99\r\n", "TTL", "ttl");
    CMD(c, "$1\r\n3\r\n", "GET", "n");
    CMD(c, ":49\r\n", "TTL", "n");
    CMD(c, ":-1\r\n", "TTL", "persisted");
    CMD(c, ":0\r\n", "EXISTS", "deleted", "inv:1", "inv:2", "expired");
    CMD(c, ":4\r\n", "DBSIZE");
}

TEST(test_aof_restart_and_rewrite) {
    ASSERT(aof_dir_setup());
    mr_server_config cfg;
    base_cfg(&cfg);
    cfg.aof_path = g_aof;
    cfg.appendfsync = MR_AOF_FSYNC_ALWAYS;
    ASSERT(start(&cfg));
    client c;
    ASSERT(cli_open(&c));
    CMD(&c, "+OK\r\n", "SET", "plain", "hello");
    CMD(&c, "+OK\r\n", "SET", "ttl", "v", "EX", "100");
    CMD(&c, ":1\r\n", "INCR", "n");
    CMD(&c, ":1\r\n", "EXPIRE", "n", "50");
    CMD(&c, ":2\r\n", "INCR", "n");
    CMD(&c, ":3\r\n", "INCR", "n");
    CMD(&c, "+OK\r\n", "SET", "persisted", "v", "PX", "10");
    CMD(&c, ":1\r\n", "PERSIST", "persisted");
    CMD(&c, "+OK\r\n", "SET", "deleted", "v");
    CMD(&c, ":1\r\n", "DEL", "deleted");
    CMD(&c, "+OK\r\n", "SET", "inv:1", "v");
    CMD(&c, "+OK\r\n", "SET", "inv:2", "v");
    CMD(&c, ":2\r\n", "INVALIDATE", "PREFIX", "inv:");
    CMD(&c, "+OK\r\n", "SET", "expired", "v", "PX", "500");
    for (int i = 0; i < 20; i++) {
        CMD(&c, "+OK\r\n", "SET", "plain", "hello"); /* rewrite 로 줄어들 중복 기록 */
    }
    CHECK_EQ(info_field(&c, "aof_enabled"), 1);
    cli_close(&c);

    g_fc.wall += 1000; /* 재시작 사이에 1초 경과: expired 는 사라지고 TTL 은 줄어든다 */
    ASSERT(restart_with_aof(&cfg));
    ASSERT(cli_open(&c));
    check_restored(&c);
    long before = file_size(g_aof);
    CMD(&c, "+Append only file rewritten synchronously\r\n", "BGREWRITEAOF");
    CHECK(file_size(g_aof) < before);
    CHECK_EQ(info_field(&c, "aof_rewrites"), 1);
    cli_close(&c);

    ASSERT(restart_with_aof(&cfg));
    ASSERT(cli_open(&c));
    check_restored(&c);
    CMD(&c, "+OK\r\n", "FLUSHALL");
    cli_close(&c);
    ASSERT(restart_with_aof(&cfg));
    ASSERT(cli_open(&c));
    CMD(&c, ":0\r\n", "DBSIZE");
    cli_close(&c);
    finish();
    aof_dir_teardown();
}

TEST(test_aof_corrupt_refuses_start) {
    ASSERT(aof_dir_setup());
    FILE *f = fopen(g_aof, "wb");
    ASSERT(f != NULL);
    fputs("*1\r\n$7\r\nBOGUSCM\r\n", f);
    fclose(f);
    mr_server_config cfg;
    base_cfg(&cfg);
    cfg.aof_path = g_aof;
    mr_server *s = NULL;
    char err[256] = "";
    int before = count_fds();
    CHECK(mr_server_create(&cfg, &s, err, sizeof(err)) == MR_CORRUPT);
    CHECK(s == NULL);
    CHECK(strstr(err, "open AOF") != NULL);
    CHECK_EQ(count_fds(), before);
    aof_dir_teardown();
}

MT_MAIN(test_all_commands_over_tcp, test_split_one_byte_at_a_time, test_pipeline_100,
        test_inline_commands, test_large_binary_utf8_values, test_ttl_with_fake_clock,
        test_scan_consistency, test_100_clients_incr, test_maxclients,
        test_protocol_error_closes_connection, test_abrupt_disconnects, test_output_buffer_limit,
        test_idle_timeout, test_quit_with_pipelined_tail, test_run_stop_and_port_in_use,
        test_aof_restart_and_rewrite, test_aof_corrupt_refuses_start)
