#include "minitest.h"
#include "miniredis/resp.h"

/* ---------- 도우미 ---------- */

static bool arg_is(const mr_slice *a, const void *s, size_t n) {
    return a->len == n && (n == 0 || memcmp(a->ptr, s, n) == 0);
}

#define ARG_IS(a, lit) arg_is((a), (lit), sizeof(lit) - 1)

/* 고정 시드 xorshift: 실패가 재현 가능하도록 테스트 난수는 항상 같은 순서. */
static uint64_t rng_state;
static uint64_t rng(void) {
    uint64_t x = rng_state;
    x ^= x << 13;
    x ^= x >> 7;
    x ^= x << 17;
    rng_state = x;
    return x;
}
static size_t rng_below(size_t n) { return (size_t)(rng() % n); }

/* 한 번에 전체를 파싱. */
static mr_resp_result parse_all(mr_resp_parser *p, const char *s, size_t *used) {
    return mr_resp_parse(p, (const unsigned char *)s, strlen(s), used);
}

/* ---------- 응답 직렬화 ---------- */

TEST(test_writers) {
    mr_buf b = {NULL, 0, 0};
    CHECK(mr_resp_simple(&b, "OK") == MR_OK);
    CHECK(mr_resp_error(&b, "ERR bad\r\nthing") == MR_OK);
    CHECK(mr_resp_int(&b, -42) == MR_OK);
    CHECK(mr_resp_int(&b, INT64_MIN) == MR_OK);
    CHECK(mr_resp_bulk(&b, "a\0b", 3) == MR_OK);
    CHECK(mr_resp_bulk(&b, NULL, 0) == MR_OK);
    CHECK(mr_resp_null(&b) == MR_OK);
    CHECK(mr_resp_array(&b, 2) == MR_OK);
    CHECK(mr_resp_errorf(&b, "ERR %s %d", "x", 7) == MR_OK);
    static const char want[] = "+OK\r\n-ERR bad  thing\r\n:-42\r\n:-9223372036854775808\r\n"
                               "$3\r\na\0b\r\n$0\r\n\r\n$-1\r\n*2\r\n-ERR x 7\r\n";
    CHECK(b.len == sizeof(want) - 1);
    CHECK(memcmp(b.data, want, sizeof(want) - 1) == 0);
    mr_buf_free(&b);
    CHECK(b.data == NULL && b.len == 0);
}

TEST(test_writer_nomem) {
    mr_buf b = {NULL, 0, 0};
    mt_fail_alloc_after(1);
    CHECK(mr_resp_simple(&b, "OK") == MR_NOMEM);
    mt_alloc_reset();
    mr_buf_free(&b);
}

TEST(test_parse_i64) {
    int64_t v;
    CHECK(mr_parse_i64("0", 1, &v) && v == 0);
    CHECK(mr_parse_i64("-1", 2, &v) && v == -1);
    CHECK(mr_parse_i64("9223372036854775807", 19, &v) && v == INT64_MAX);
    CHECK(mr_parse_i64("-9223372036854775808", 20, &v) && v == INT64_MIN);
    CHECK(!mr_parse_i64("9223372036854775808", 19, &v));
    CHECK(!mr_parse_i64("-0", 2, &v));
    CHECK(!mr_parse_i64("01", 2, &v));
    CHECK(!mr_parse_i64("+1", 2, &v));
    CHECK(!mr_parse_i64(" 1", 2, &v));
    CHECK(!mr_parse_i64("", 0, &v));
    CHECK(!mr_parse_i64("1a", 2, &v));
}

/* ---------- 파서: 정상 경로 ---------- */

TEST(test_parse_multibulk) {
    mr_resp_parser p;
    mr_resp_parser_init(&p, NULL);
    size_t used;
    const char *in = "*3\r\n$3\r\nSET\r\n$1\r\nk\r\n$5\r\nhello\r\nTAIL";
    ASSERT(parse_all(&p, in, &used) == MR_RESP_COMMAND);
    CHECK_EQ(used, strlen(in) - 4);
    ASSERT_EQ(mr_resp_argc(&p), 3);
    const mr_slice *a = mr_resp_argv(&p);
    CHECK(ARG_IS(&a[0], "SET"));
    CHECK(ARG_IS(&a[1], "k"));
    CHECK(ARG_IS(&a[2], "hello"));
    mr_resp_parser_free(&p);
}

TEST(test_parse_binary_and_empty_bulk) {
    mr_resp_parser p;
    mr_resp_parser_init(&p, NULL);
    static const char in[] = "*3\r\n$4\r\na\0\r\n\r\n$0\r\n\r\n$2\r\n\xed\x95\r\n";
    size_t used;
    ASSERT(mr_resp_parse(&p, (const unsigned char *)in, sizeof(in) - 1, &used) ==
           MR_RESP_COMMAND);
    CHECK_EQ(used, sizeof(in) - 1);
    const mr_slice *a = mr_resp_argv(&p);
    CHECK(arg_is(&a[0], "a\0\r\n", 4));
    CHECK(a[1].len == 0);
    CHECK(arg_is(&a[2], "\xed\x95", 2));
    mr_resp_parser_free(&p);
}

TEST(test_parse_byte_by_byte) {
    /* 버퍼가 한 바이트씩 자라는 상황: 완성 직전까지 NEED_MORE, 완성 시 정확히 한 번 COMMAND. */
    const char *in = "*2\r\n$4\r\nECHO\r\n$11\r\nhello world\r\n";
    size_t n = strlen(in);
    mr_resp_parser p;
    mr_resp_parser_init(&p, NULL);
    size_t used = 0;
    for (size_t len = 1; len < n; len++) {
        CHECK(mr_resp_parse(&p, (const unsigned char *)in, len, &used) == MR_RESP_NEED_MORE);
    }
    ASSERT(mr_resp_parse(&p, (const unsigned char *)in, n, &used) == MR_RESP_COMMAND);
    CHECK_EQ(used, n);
    CHECK(ARG_IS(&mr_resp_argv(&p)[1], "hello world"));
    mr_resp_parser_free(&p);
}

TEST(test_parse_pipeline) {
    const char *in = "*1\r\n$4\r\nPING\r\nPING\r\n*2\r\n$3\r\nGET\r\n$1\r\nx\r\n";
    size_t n = strlen(in);
    mr_resp_parser p;
    mr_resp_parser_init(&p, NULL);
    size_t off = 0;
    size_t cmds = 0;
    while (off < n) {
        size_t used;
        mr_resp_result r = mr_resp_parse(&p, (const unsigned char *)in + off, n - off, &used);
        ASSERT(r == MR_RESP_COMMAND);
        off += used;
        cmds++;
    }
    CHECK_EQ(cmds, 3);
    CHECK(ARG_IS(&mr_resp_argv(&p)[1], "x"));
    mr_resp_parser_free(&p);
}

TEST(test_parse_inline) {
    mr_resp_parser p;
    mr_resp_parser_init(&p, NULL);
    size_t used;
    ASSERT(parse_all(&p, "  SET\tkey  value \r\nREST", &used) == MR_RESP_COMMAND);
    CHECK_EQ(used, strlen("  SET\tkey  value \r\n"));
    ASSERT_EQ(mr_resp_argc(&p), 3);
    CHECK(ARG_IS(&mr_resp_argv(&p)[0], "SET"));
    CHECK(ARG_IS(&mr_resp_argv(&p)[2], "value"));
    /* 빈 줄은 인자 0 개 명령으로 소비 */
    ASSERT(parse_all(&p, "\r\n", &used) == MR_RESP_COMMAND);
    CHECK_EQ(used, 2);
    CHECK_EQ(mr_resp_argc(&p), 0);
    /* LF 만 있어도 된다(telnet/nc) */
    ASSERT(parse_all(&p, "PING\n", &used) == MR_RESP_COMMAND);
    CHECK_EQ(mr_resp_argc(&p), 1);
    CHECK(parse_all(&p, "PIN", &used) == MR_RESP_NEED_MORE);
    mr_resp_parser_free(&p);
}

TEST(test_parse_empty_multibulk) {
    mr_resp_parser p;
    mr_resp_parser_init(&p, NULL);
    size_t used;
    ASSERT(parse_all(&p, "*0\r\n", &used) == MR_RESP_COMMAND);
    CHECK_EQ(used, 4);
    CHECK_EQ(mr_resp_argc(&p), 0);
    ASSERT(parse_all(&p, "*-1\r\n", &used) == MR_RESP_COMMAND);
    CHECK_EQ(used, 5);
    CHECK_EQ(mr_resp_argc(&p), 0);
    mr_resp_parser_free(&p);
}

TEST(test_parse_no_prealloc_for_declared_count) {
    /* "*1000000" 만 보내도 선언된 개수만큼 할당하지 않아야 한다(메모리 DoS 방지). */
    mr_resp_parser p;
    mr_resp_parser_init(&p, NULL);
    size_t used;
    CHECK(parse_all(&p, "*1000000\r\n$1\r\na\r\n", &used) == MR_RESP_NEED_MORE);
    CHECK(p.cap <= 8);
    mr_resp_parser_free(&p);
}

TEST(test_parse_buffer_moves_between_calls) {
    /* 오프셋 기반이므로 호출 사이에 버퍼가 다른 주소로 옮겨져도 결과가 같아야 한다. */
    const char *in = "*2\r\n$3\r\nGET\r\n$5\r\nabcde\r\n";
    size_t n = strlen(in);
    mr_resp_parser p;
    mr_resp_parser_init(&p, NULL);
    char a[64];
    char b[64];
    size_t used;
    memcpy(a, in, 10);
    CHECK(mr_resp_parse(&p, (const unsigned char *)a, 10, &used) == MR_RESP_NEED_MORE);
    memset(a, 'X', sizeof(a));
    memcpy(b, in, n);
    ASSERT(mr_resp_parse(&p, (const unsigned char *)b, n, &used) == MR_RESP_COMMAND);
    CHECK(ARG_IS(&mr_resp_argv(&p)[1], "abcde"));
    CHECK(mr_resp_argv(&p)[1].ptr == (const unsigned char *)b + n - 7);
    mr_resp_parser_free(&p);
}

/* ---------- 파서: 오류 경로 ---------- */

static const char *parse_error_of(const char *in, const mr_resp_limits *lim) {
    mr_resp_parser p;
    mr_resp_parser_init(&p, lim);
    size_t used;
    mr_resp_result r = parse_all(&p, in, &used);
    const char *msg = r == MR_RESP_ERROR ? mr_resp_error_msg(&p) : NULL;
    mr_resp_parser_free(&p);
    return msg;
}

static bool err_contains(const char *msg, const char *needle) {
    return msg != NULL && strstr(msg, needle) != NULL && strncmp(msg, "Protocol error", 14) == 0;
}

TEST(test_parse_errors) {
    CHECK(err_contains(parse_error_of("*abc\r\n", NULL), "invalid multibulk length"));
    CHECK(err_contains(parse_error_of("*1\r\nGET\r\n", NULL), "expected '$'"));
    CHECK(err_contains(parse_error_of("*1\r\n$-1\r\n", NULL), "invalid bulk length"));
    CHECK(err_contains(parse_error_of("*1\r\n$x\r\n", NULL), "invalid bulk length"));
    CHECK(err_contains(parse_error_of("*1\r\n$3\r\nabcde\r\n", NULL), "expected CRLF"));
    CHECK(err_contains(parse_error_of("*01\r\n", NULL), "invalid multibulk length"));
    mr_resp_limits lim = {10, 4, 16};
    CHECK(err_contains(parse_error_of("*1\r\n$11\r\n", &lim), "invalid bulk length"));
    CHECK(err_contains(parse_error_of("*5\r\n", &lim), "invalid multibulk length"));
    CHECK(err_contains(parse_error_of("aaaaaaaaaaaaaaaaaaaaaa", &lim), "too big inline"));
    CHECK(err_contains(parse_error_of("a b c d e\r\n", &lim), "too many arguments"));
    CHECK(err_contains(parse_error_of("*1111111111111111111111", &lim), "too big mbulk"));
    CHECK(err_contains(parse_error_of("*1\r\n$11111111111111111111", &lim), "too big bulk"));
    /* 상한 이내는 정상 */
    CHECK(parse_error_of("*1\r\n$10\r\n0123456789\r\n", &lim) == NULL);
}

TEST(test_parse_error_is_sticky) {
    mr_resp_parser p;
    mr_resp_parser_init(&p, NULL);
    size_t used;
    CHECK(parse_all(&p, "*x\r\n", &used) == MR_RESP_ERROR);
    CHECK(parse_all(&p, "PING\r\n", &used) == MR_RESP_ERROR);
    mr_resp_parser_free(&p);
}

TEST(test_parse_nomem) {
    mr_resp_parser p;
    mr_resp_parser_init(&p, NULL);
    size_t used;
    mt_fail_alloc_after(1);
    CHECK(parse_all(&p, "*1\r\n$1\r\na\r\n", &used) == MR_RESP_NOMEM);
    mt_alloc_reset();
    mr_resp_parser_free(&p);
    mr_resp_parser_init(&p, NULL);
    mt_fail_alloc_after(2); /* spans 성공, argv 실패 */
    CHECK(parse_all(&p, "a b\n", &used) == MR_RESP_NOMEM);
    mt_alloc_reset();
    mr_resp_parser_free(&p);
}

/* ---------- 퍼징(고정 시드) ---------- */

#define FUZZ_MAX_ARGS 8
#define FUZZ_MAX_ARG_LEN 300

typedef struct fuzz_cmd {
    size_t argc;
    size_t lens[FUZZ_MAX_ARGS];
    unsigned char args[FUZZ_MAX_ARGS][FUZZ_MAX_ARG_LEN];
} fuzz_cmd;

static void gen_cmd(fuzz_cmd *c, bool inline_safe) {
    c->argc = 1 + rng_below(FUZZ_MAX_ARGS);
    for (size_t i = 0; i < c->argc; i++) {
        c->lens[i] = inline_safe ? 1 + rng_below(20) : rng_below(FUZZ_MAX_ARG_LEN);
        for (size_t j = 0; j < c->lens[i]; j++) {
            /* inline 은 공백/개행/'*' 시작이 없는 출력 가능 문자만 */
            c->args[i][j] = inline_safe ? (unsigned char)('a' + rng_below(26))
                                        : (unsigned char)rng_below(256);
        }
    }
}

static void encode_cmd(mr_buf *out, const fuzz_cmd *c, bool as_inline) {
    char hdr[32];
    if (as_inline) {
        for (size_t i = 0; i < c->argc; i++) {
            (void)mr_buf_append(out, i == 0 ? "" : " ", i == 0 ? 0 : 1);
            (void)mr_buf_append(out, c->args[i], c->lens[i]);
        }
        (void)mr_buf_append(out, "\r\n", 2);
        return;
    }
    int n = snprintf(hdr, sizeof(hdr), "*%zu\r\n", c->argc);
    (void)mr_buf_append(out, hdr, (size_t)n);
    for (size_t i = 0; i < c->argc; i++) {
        (void)mr_resp_bulk(out, c->args[i], c->lens[i]);
    }
}

static bool cmd_matches(const mr_resp_parser *p, const fuzz_cmd *c) {
    if (mr_resp_argc(p) != c->argc) {
        return false;
    }
    for (size_t i = 0; i < c->argc; i++) {
        if (!arg_is(&mr_resp_argv(p)[i], c->args[i], c->lens[i])) {
            return false;
        }
    }
    return true;
}

/*
 * 연결 버퍼 흉내: 무작위 크기 조각으로 받아 붙이고, 완성된 명령은 앞에서 걷어내
 * 당긴다(서버의 compact 와 같은 방식). 파싱 결과가 원본 명령열과 같아야 한다.
 */
static bool fuzz_round(fuzz_cmd *cmds, size_t ncmds) {
    mr_buf stream = {NULL, 0, 0};
    for (size_t i = 0; i < ncmds; i++) {
        bool inl = rng_below(4) == 0;
        gen_cmd(&cmds[i], inl);
        encode_cmd(&stream, &cmds[i], inl);
    }
    mr_resp_parser p;
    mr_resp_parser_init(&p, NULL);
    mr_buf conn = {NULL, 0, 0};
    size_t fed = 0;
    size_t got = 0;
    bool ok = true;
    while (ok && got < ncmds) {
        if (fed < stream.len) {
            size_t chunk = 1 + rng_below(rng_below(2) ? 7 : 700);
            chunk = chunk > stream.len - fed ? stream.len - fed : chunk;
            (void)mr_buf_append(&conn, stream.data + fed, chunk);
            fed += chunk;
        }
        for (;;) {
            size_t used;
            mr_resp_result r = mr_resp_parse(&p, conn.data, conn.len, &used);
            if (r == MR_RESP_NEED_MORE) {
                /* 다 넣었는데 남은 명령이 있다며 더 달라면 실패 */
                ok = fed < stream.len || got == ncmds;
                break;
            }
            if (r != MR_RESP_COMMAND || !cmd_matches(&p, &cmds[got])) {
                ok = false;
                break;
            }
            got++;
            memmove(conn.data, conn.data + used, conn.len - used);
            conn.len -= used;
        }
    }
    ok = ok && got == ncmds && conn.len == 0;
    mr_resp_parser_free(&p);
    mr_buf_free(&conn);
    mr_buf_free(&stream);
    return ok;
}

TEST(test_fuzz_split_streams) {
    static fuzz_cmd cmds[24];
    rng_state = 0x5eed1234abcdULL;
    for (int round = 0; round < 300; round++) {
        size_t n = 1 + rng_below(24);
        if (!fuzz_round(cmds, n)) {
            CHECK(!"fuzz round mismatch");
            fprintf(stderr, "    round %d failed\n", round);
            return;
        }
    }
    CHECK(true);
}

/* 무작위 바이트·변이 입력: 어떤 결과든 좋지만 터지거나(ASan) 무한 루프가 없어야 한다. */
static void parse_garbage(const unsigned char *buf, size_t len) {
    mr_resp_limits lim = {1024, 64, 512};
    mr_resp_parser p;
    mr_resp_parser_init(&p, &lim);
    size_t off = 0;
    for (int guard = 0; guard < 10000 && off < len; guard++) {
        size_t used;
        mr_resp_result r = mr_resp_parse(&p, buf + off, len - off, &used);
        if (r != MR_RESP_COMMAND) {
            break;
        }
        for (size_t i = 0; i < mr_resp_argc(&p); i++) {
            const mr_slice *a = &mr_resp_argv(&p)[i];
            /* 인자는 반드시 입력 범위 안을 가리켜야 한다 */
            CHECK(a->ptr >= buf + off && a->ptr + a->len <= buf + len);
        }
        CHECK(used > 0);
        off += used;
    }
    mr_resp_parser_free(&p);
}

TEST(test_fuzz_garbage_and_mutations) {
    rng_state = 0xfeedfacecafeULL;
    unsigned char buf[2048];
    static const char alphabet[] = "*$\r\n0123456789-abc ";
    for (int round = 0; round < 3000; round++) {
        size_t len = rng_below(sizeof(buf));
        bool structured = rng_below(2) == 0;
        for (size_t i = 0; i < len; i++) {
            buf[i] = structured ? (unsigned char)alphabet[rng_below(sizeof(alphabet) - 1)]
                                : (unsigned char)rng_below(256);
        }
        parse_garbage(buf, len);
    }
    /* 올바른 스트림에 몇 바이트만 뒤집기 */
    fuzz_cmd c;
    for (int round = 0; round < 2000; round++) {
        mr_buf s = {NULL, 0, 0};
        gen_cmd(&c, false);
        encode_cmd(&s, &c, false);
        gen_cmd(&c, false);
        encode_cmd(&s, &c, false);
        for (int k = 0; k < 3 && s.len > 0; k++) {
            s.data[rng_below(s.len)] = (unsigned char)rng_below(256);
        }
        parse_garbage(s.data, s.len);
        mr_buf_free(&s);
    }
}

MT_MAIN(test_writers, test_writer_nomem, test_parse_i64, test_parse_multibulk,
        test_parse_binary_and_empty_bulk, test_parse_byte_by_byte, test_parse_pipeline,
        test_parse_inline, test_parse_empty_multibulk, test_parse_no_prealloc_for_declared_count,
        test_parse_buffer_moves_between_calls, test_parse_errors, test_parse_error_is_sticky,
        test_parse_nomem, test_fuzz_split_streams, test_fuzz_garbage_and_mutations)
