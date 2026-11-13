#include "miniredis/resp.h"

#include <inttypes.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* int64 의 10진 표현 최대 자릿수("-9223372036854775808" 은 부호 포함 20). */
#define I64_MAX_CHARS 20

/* ---------- mr_buf ---------- */

void mr_buf_init(mr_buf *b) {
    b->data = NULL;
    b->len = 0;
    b->cap = 0;
}

void mr_buf_free(mr_buf *b) {
    free(b->data);
    mr_buf_init(b);
}

mr_status mr_buf_reserve(mr_buf *b, size_t extra) {
    if (extra > SIZE_MAX - b->len) {
        return MR_OVERFLOW;
    }
    size_t need = b->len + extra;
    if (need <= b->cap) {
        return MR_OK;
    }
    /* 2배씩 키워 append 의 분할 상환 비용을 O(1) 로 유지한다. */
    size_t ncap = b->cap != 0 ? b->cap : 64;
    while (ncap < need) {
        ncap = ncap > SIZE_MAX / 2 ? need : ncap * 2;
    }
    unsigned char *nd = realloc(b->data, ncap);
    if (nd == NULL) {
        return MR_NOMEM;
    }
    b->data = nd;
    b->cap = ncap;
    return MR_OK;
}

mr_status mr_buf_append(mr_buf *b, const void *p, size_t n) {
    if (n == 0) {
        return MR_OK;
    }
    mr_status st = mr_buf_reserve(b, n);
    if (st != MR_OK) {
        return st;
    }
    memcpy(b->data + b->len, p, n);
    b->len += n;
    return MR_OK;
}

/* ---------- 응답 직렬화 ---------- */

static mr_status put_header(mr_buf *b, char type, int64_t v) {
    char tmp[I64_MAX_CHARS + 4];
    int n = snprintf(tmp, sizeof(tmp), "%c%" PRId64 "\r\n", type, v);
    if (n < 0 || (size_t)n >= sizeof(tmp)) {
        return MR_OVERFLOW;
    }
    return mr_buf_append(b, tmp, (size_t)n);
}

/* 한 줄 응답: 본문의 CR/LF 를 공백으로 바꿔 프레이밍이 깨지지 않게 한다. */
static mr_status put_line(mr_buf *b, char type, const char *s, size_t n) {
    mr_status st = mr_buf_reserve(b, n + 3);
    if (st != MR_OK) {
        return st;
    }
    b->data[b->len++] = (unsigned char)type;
    for (size_t i = 0; i < n; i++) {
        unsigned char c = (unsigned char)s[i];
        b->data[b->len++] = (c == '\r' || c == '\n') ? (unsigned char)' ' : c;
    }
    b->data[b->len++] = '\r';
    b->data[b->len++] = '\n';
    return MR_OK;
}

mr_status mr_resp_simple(mr_buf *b, const char *s) { return put_line(b, '+', s, strlen(s)); }

mr_status mr_resp_error(mr_buf *b, const char *msg) { return put_line(b, '-', msg, strlen(msg)); }

mr_status mr_resp_errorf(mr_buf *b, const char *fmt, ...) {
    /* 오류 메시지는 사람이 읽는 짧은 문장이므로 넘치면 잘라도 된다. */
    char tmp[512];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(tmp, sizeof(tmp), fmt, ap);
    va_end(ap);
    if (n < 0) {
        return MR_INVALID_ARG;
    }
    size_t len = (size_t)n < sizeof(tmp) ? (size_t)n : sizeof(tmp) - 1;
    return put_line(b, '-', tmp, len);
}

mr_status mr_resp_int(mr_buf *b, int64_t v) { return put_header(b, ':', v); }

mr_status mr_resp_bulk(mr_buf *b, const void *p, size_t n) {
    if (n > (size_t)INT64_MAX) {
        return MR_OVERFLOW;
    }
    mr_status st = mr_buf_reserve(b, n + I64_MAX_CHARS + 5);
    if (st == MR_OK) {
        st = put_header(b, '$', (int64_t)n);
    }
    if (st == MR_OK) {
        st = mr_buf_append(b, p, n);
    }
    if (st == MR_OK) {
        st = mr_buf_append(b, "\r\n", 2);
    }
    return st;
}

mr_status mr_resp_bulk_cstr(mr_buf *b, const char *s) { return mr_resp_bulk(b, s, strlen(s)); }

mr_status mr_resp_null(mr_buf *b) { return mr_buf_append(b, "$-1\r\n", 5); }

mr_status mr_resp_array(mr_buf *b, size_t n) {
    if (n > (size_t)INT64_MAX) {
        return MR_OVERFLOW;
    }
    return put_header(b, '*', (int64_t)n);
}

/* ---------- 정수 해석 ---------- */

bool mr_parse_i64(const void *ptr, size_t len, int64_t *out) {
    const unsigned char *p = ptr;
    if (len == 0 || len > I64_MAX_CHARS) {
        return false;
    }
    if (len == 1 && p[0] == '0') {
        *out = 0;
        return true;
    }
    size_t i = 0;
    bool neg = false;
    if (p[0] == '-') {
        neg = true;
        i = 1;
    }
    /* 선행 0 과 "-0" 을 막아 한 값에 표현이 하나만 있게 한다(Redis string2ll). */
    if (i >= len || p[i] < '1' || p[i] > '9') {
        return false;
    }
    uint64_t v = 0;
    for (; i < len; i++) {
        if (p[i] < '0' || p[i] > '9') {
            return false;
        }
        unsigned d = (unsigned)(p[i] - '0');
        if (v > (UINT64_MAX - d) / 10) {
            return false;
        }
        v = v * 10 + d;
    }
    if (neg) {
        if (v > (uint64_t)INT64_MAX + 1) {
            return false;
        }
        *out = -(int64_t)(v - 1) - 1;
    } else {
        if (v > (uint64_t)INT64_MAX) {
            return false;
        }
        *out = (int64_t)v;
    }
    return true;
}

/* ---------- 요청 파서 ---------- */

enum {
    ST_START = 0,   /* 명령 첫 바이트 대기 */
    ST_BULK_HEADER, /* "$<len>\r\n" 대기 */
    ST_BULK_DATA,   /* bulk 본문 + "\r\n" 대기 */
    ST_ERROR        /* 고착된 프로토콜 오류 */
};

/* 헤더 줄 탐색 결과 */
enum { LINE_MORE = 0, LINE_FOUND, LINE_TOO_LONG };

void mr_resp_parser_init(mr_resp_parser *p, const mr_resp_limits *lim) {
    memset(p, 0, sizeof(*p));
    if (lim != NULL) {
        p->lim = *lim;
    }
    if (p->lim.max_bulk_len == 0) {
        p->lim.max_bulk_len = MR_RESP_DEFAULT_MAX_BULK;
    }
    if (p->lim.max_args == 0) {
        p->lim.max_args = MR_RESP_DEFAULT_MAX_ARGS;
    }
    if (p->lim.max_inline_len == 0) {
        p->lim.max_inline_len = MR_RESP_DEFAULT_MAX_INLINE;
    }
    p->state = ST_START;
}

void mr_resp_parser_free(mr_resp_parser *p) {
    free(p->spans);
    free(p->argv);
    p->spans = NULL;
    p->argv = NULL;
    p->cap = 0;
    p->argc = 0;
}

size_t mr_resp_argc(const mr_resp_parser *p) { return p->argc; }
const mr_slice *mr_resp_argv(const mr_resp_parser *p) { return p->argv; }
const char *mr_resp_error_msg(const mr_resp_parser *p) { return p->err; }

static mr_resp_result fail(mr_resp_parser *p, const char *msg) {
    p->state = ST_ERROR;
    p->err = msg;
    return MR_RESP_ERROR;
}

/* 인자 배열을 n 개 이상으로. 선언된 개수가 아니라 실제 도착한 개수에 맞춰 부른다. */
static bool ensure_cap(mr_resp_parser *p, size_t n) {
    if (n <= p->cap) {
        return true;
    }
    size_t ncap = p->cap != 0 ? p->cap : 8;
    while (ncap < n) {
        ncap *= 2;
    }
    mr_resp_span *ns = realloc(p->spans, ncap * sizeof(*ns));
    if (ns == NULL) {
        return false;
    }
    p->spans = ns;
    mr_slice *na = realloc(p->argv, ncap * sizeof(*na));
    if (na == NULL) {
        return false;
    }
    p->argv = na;
    p->cap = ncap;
    return true;
}

static void push_span(mr_resp_parser *p, size_t off, size_t len) {
    p->spans[p->argc].off = off;
    p->spans[p->argc].len = len;
    p->argc++;
}

/* pos 에서 시작하는 "\r\n" 으로 끝나는 헤더 줄을 찾는다. *cr 은 '\r' 의 위치. */
static int find_crlf(mr_resp_parser *p, const unsigned char *buf, size_t len, size_t *cr) {
    size_t from = p->scan > p->pos ? p->scan : p->pos;
    while (from < len) {
        const unsigned char *hit = memchr(buf + from, '\r', len - from);
        if (hit == NULL) {
            break;
        }
        size_t i = (size_t)(hit - buf);
        if (i + 1 >= len) {
            /* '\r' 뒤 바이트가 아직 안 옴: 다음엔 이 '\r' 부터 다시 본다. */
            from = i;
            p->scan = i;
            return i - p->pos > p->lim.max_inline_len ? LINE_TOO_LONG : LINE_MORE;
        }
        if (buf[i + 1] == '\n') {
            *cr = i;
            p->scan = 0;
            return i - p->pos > p->lim.max_inline_len ? LINE_TOO_LONG : LINE_FOUND;
        }
        from = i + 1;
    }
    p->scan = len;
    return len - p->pos > p->lim.max_inline_len ? LINE_TOO_LONG : LINE_MORE;
}

/* 완성된 명령의 argv 를 현재 버퍼 주소로 만들고 상태를 처음으로 되돌린다. */
static mr_resp_result finish(mr_resp_parser *p, const unsigned char *buf, size_t end,
                             size_t *consumed) {
    for (size_t i = 0; i < p->argc; i++) {
        p->argv[i].ptr = buf + p->spans[i].off;
        p->argv[i].len = p->spans[i].len;
    }
    *consumed = end;
    p->state = ST_START;
    p->pos = 0;
    p->scan = 0;
    return MR_RESP_COMMAND;
}

static bool is_inline_space(unsigned char c) { return c == ' ' || c == '\t' || c == '\r'; }

static mr_resp_result parse_inline(mr_resp_parser *p, const unsigned char *buf, size_t len,
                                   size_t *consumed) {
    size_t from = p->scan;
    const unsigned char *nl = from < len ? memchr(buf + from, '\n', len - from) : NULL;
    if (nl == NULL) {
        p->scan = len;
        if (len > p->lim.max_inline_len) {
            return fail(p, "Protocol error: too big inline request");
        }
        return MR_RESP_NEED_MORE;
    }
    size_t end = (size_t)(nl - buf);
    if (end > p->lim.max_inline_len) {
        return fail(p, "Protocol error: too big inline request");
    }
    size_t i = 0;
    while (i < end) {
        while (i < end && is_inline_space(buf[i])) {
            i++;
        }
        if (i == end) {
            break;
        }
        size_t start = i;
        while (i < end && !is_inline_space(buf[i])) {
            i++;
        }
        if (p->argc >= p->lim.max_args) {
            return fail(p, "Protocol error: too many arguments in inline request");
        }
        if (!ensure_cap(p, p->argc + 1)) {
            return MR_RESP_NOMEM;
        }
        push_span(p, start, i - start);
    }
    return finish(p, buf, end + 1, consumed);
}

/* "*<n>\r\n" 해석. n <= 0 은 빈 명령으로 소비한다(Redis 동일). */
static mr_resp_result parse_multibulk_header(mr_resp_parser *p, const unsigned char *buf,
                                             size_t len, size_t *consumed) {
    size_t cr;
    int r = find_crlf(p, buf, len, &cr);
    if (r == LINE_TOO_LONG) {
        return fail(p, "Protocol error: too big mbulk count string");
    }
    if (r == LINE_MORE) {
        return MR_RESP_NEED_MORE;
    }
    int64_t n;
    if (!mr_parse_i64(buf + 1, cr - 1, &n) || (n > 0 && (uint64_t)n > p->lim.max_args)) {
        return fail(p, "Protocol error: invalid multibulk length");
    }
    p->pos = cr + 2;
    if (n <= 0) {
        return finish(p, buf, p->pos, consumed);
    }
    p->need = (size_t)n;
    p->state = ST_BULK_HEADER;
    return MR_RESP_NEED_MORE; /* 호출자 루프가 이어서 진행 */
}

static mr_resp_result parse_bulk_header(mr_resp_parser *p, const unsigned char *buf, size_t len) {
    if (p->pos >= len) {
        return MR_RESP_NEED_MORE;
    }
    if (buf[p->pos] != '$') {
        return fail(p, "Protocol error: expected '$'");
    }
    size_t cr;
    int r = find_crlf(p, buf, len, &cr);
    if (r == LINE_TOO_LONG) {
        return fail(p, "Protocol error: too big bulk count string");
    }
    if (r == LINE_MORE) {
        return MR_RESP_NEED_MORE;
    }
    int64_t n;
    if (!mr_parse_i64(buf + p->pos + 1, cr - p->pos - 1, &n) || n < 0 ||
        (uint64_t)n > p->lim.max_bulk_len) {
        return fail(p, "Protocol error: invalid bulk length");
    }
    p->bulk_len = (size_t)n;
    p->pos = cr + 2;
    p->state = ST_BULK_DATA;
    return MR_RESP_NEED_MORE;
}

static mr_resp_result parse_bulk_data(mr_resp_parser *p, const unsigned char *buf, size_t len,
                                      size_t *consumed) {
    /* bulk_len <= max_bulk_len 이므로 덧셈은 넘치지 않는다. */
    size_t end = p->pos + p->bulk_len;
    if (len < end + 2) {
        return MR_RESP_NEED_MORE;
    }
    if (buf[end] != '\r' || buf[end + 1] != '\n') {
        return fail(p, "Protocol error: expected CRLF after bulk data");
    }
    if (!ensure_cap(p, p->argc + 1)) {
        return MR_RESP_NOMEM;
    }
    push_span(p, p->pos, p->bulk_len);
    p->pos = end + 2;
    if (p->argc == p->need) {
        return finish(p, buf, p->pos, consumed);
    }
    p->state = ST_BULK_HEADER;
    return MR_RESP_NEED_MORE;
}

mr_resp_result mr_resp_parse(mr_resp_parser *p, const unsigned char *buf, size_t len,
                             size_t *consumed) {
    *consumed = 0;
    if (p->state == ST_ERROR) {
        return MR_RESP_ERROR;
    }
    if (p->state == ST_START) {
        p->argc = 0;
        if (len == 0) {
            return MR_RESP_NEED_MORE;
        }
        if (buf[0] != '*') {
            return parse_inline(p, buf, len, consumed);
        }
        mr_resp_result r = parse_multibulk_header(p, buf, len, consumed);
        if (r != MR_RESP_NEED_MORE || p->state == ST_START) {
            return r;
        }
    }
    /* 한 번의 호출에서 가능한 만큼 인자를 연달아 해석한다. */
    for (;;) {
        size_t before = p->pos;
        int state_before = p->state;
        mr_resp_result r = p->state == ST_BULK_HEADER ? parse_bulk_header(p, buf, len)
                                                      : parse_bulk_data(p, buf, len, consumed);
        if (r != MR_RESP_NEED_MORE) {
            return r;
        }
        if (p->pos == before && p->state == state_before) {
            return MR_RESP_NEED_MORE; /* 진전 없음: 바이트가 더 필요 */
        }
    }
}
