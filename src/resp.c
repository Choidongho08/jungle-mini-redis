/* RESP2 요청 파서와 응답 직렬화 (design §5.2, §6.1, §6.2)
 *
 * 요청 형식:  *<인자 수>\r\n  $<길이>\r\n<바이트>\r\n  ...
 *
 * TCP 는 바이트 스트림이라 요청 하나가 여러 read 에 나뉘어 오거나, read 한 번에
 * 여러 요청이 붙어 올 수 있다. 그래서 파서는 "지금까지 어디까지 읽었는지"를
 * client 구조체에 저장해 두고, 데이터가 모자라면 NEED_MORE 를 돌려준 뒤 다음 read 에서
 * 이어서 진행한다 (FR-004). */
#include "resp.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "command.h"
#include "util.h"

void client_reset_args(client_t *c); /* server.c */

/* ---------- 응답 ---------- */

void reply_simple(client_t *c, const char *s) {
    buffer_append(&c->reply, "+", 1);
    buffer_append_str(&c->reply, s);
    buffer_append(&c->reply, "\r\n", 2);
}

void reply_error_raw(client_t *c, const char *s, size_t n) {
    buffer_append(&c->reply, "-", 1);
    /* 에러 문자열 안의 줄바꿈은 응답 형식을 깨므로 공백으로 바꾼다 (Redis 와 동일) */
    for (size_t i = 0; i < n; i++) {
        char ch = (s[i] == '\r' || s[i] == '\n') ? ' ' : s[i];
        buffer_append(&c->reply, &ch, 1);
    }
    buffer_append(&c->reply, "\r\n", 2);
}

void reply_error(client_t *c, const char *msg) {
    buffer_t tmp;
    buffer_init(&tmp);
    buffer_append_str(&tmp, "ERR ");
    buffer_append_str(&tmp, msg);
    reply_error_raw(c, tmp.data, tmp.len);
    buffer_free(&tmp);
}

void reply_integer(client_t *c, long long v) {
    char num[32];
    int n = snprintf(num, sizeof(num), ":%lld\r\n", v);
    buffer_append(&c->reply, num, (size_t)n);
}

void reply_bulk(client_t *c, const char *p, size_t len) {
    char hdr[32];
    int n = snprintf(hdr, sizeof(hdr), "$%zu\r\n", len);
    buffer_append(&c->reply, hdr, (size_t)n);
    buffer_append(&c->reply, p, len);
    buffer_append(&c->reply, "\r\n", 2);
}

void reply_null(client_t *c) {
    buffer_append(&c->reply, "$-1\r\n", 5);
}

/* ---------- 요청 파서 ---------- */

typedef enum { PARSE_OK, PARSE_NEED_MORE, PARSE_ERROR } parse_result_t;

/* 프로토콜 에러: 응답을 보내고 연결을 닫도록 표시한다 (FR-005). */
static parse_result_t protocol_error(client_t *c, const char *fmt, ...) __attribute__((format(printf, 2, 3)));

#include <stdarg.h>
static parse_result_t protocol_error(client_t *c, const char *fmt, ...) {
    char msg[128];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof(msg), fmt, ap);
    va_end(ap);

    char full[160];
    int n = snprintf(full, sizeof(full), "Protocol error: %s", msg);
    reply_error(c, full);
    (void)n;
    c->close_after_reply = 1;
    return PARSE_ERROR;
}

/* query[qpos..] 에서 "\r\n" 으로 끝나는 한 줄을 찾는다.
 * 찾으면 '\r' 의 위치를, 아직 줄이 다 안 왔으면 NULL 을 돌려준다. */
static const char *find_line_end(client_t *c) {
    const char *start = c->query.data + c->qpos;
    size_t avail = c->query.len - c->qpos;
    const char *cr = memchr(start, '\r', avail);
    if (cr == NULL) return NULL;
    if ((size_t)(cr - start) + 1 >= avail) return NULL; /* '\n' 이 아직 안 옴 */
    return cr;
}

static void push_arg(client_t *c, const char *p, size_t len) {
    if (c->argc == c->argv_cap) {
        /* 인자 수 선언만 크게 보내는 공격에 대비해 미리 다 잡지 않고 필요할 때 늘린다. */
        c->argv_cap = c->argv_cap ? c->argv_cap * 2 : 8;
        c->argv = xrealloc(c->argv, sizeof(bytes_t) * (size_t)c->argv_cap);
    }
    /* argv 는 query 버퍼를 가리키지 않고 복사본을 가진다.
     * query 는 처리 후 앞부분을 잘라내므로(consume) 포인터가 무효가 되기 때문. */
    char *copy = xmalloc(len + 1);
    memcpy(copy, p, len);
    copy[len] = '\0'; /* 편의용. 길이는 항상 len 을 기준으로 쓴다. */
    c->argv[c->argc].ptr = copy;
    c->argv[c->argc].len = len;
    c->argc++;
}

/* 요청 하나를 파싱한다. PARSE_OK 이면 c->argc/argv 에 완성된 요청이 있다
 * (argc == 0 이면 빈 요청이라 무시). */
static parse_result_t parse_request(client_t *c) {
    long long ll;

    if (c->multibulk_left == 0) {
        /* 새 요청의 시작: "*<n>\r\n" */
        if (c->qpos >= c->query.len) return PARSE_NEED_MORE;

        if (c->query.data[c->qpos] != '*') {
            /* Redis 는 이 경우 inline 명령으로 처리하지만 MVP 범위 밖 (design §6.2) */
            return protocol_error(c, "expected '*', got '%c'", c->query.data[c->qpos]);
        }

        const char *cr = find_line_end(c);
        if (cr == NULL) {
            if (c->query.len - c->qpos > PROTO_INLINE_MAX_SIZE)
                return protocol_error(c, "too big mbulk count string");
            return PARSE_NEED_MORE;
        }

        const char *num = c->query.data + c->qpos + 1;
        if (!util_str_to_ll(num, (size_t)(cr - num), &ll) || ll > PROTO_MAX_MULTIBULK)
            return protocol_error(c, "invalid multibulk length");

        c->qpos = (size_t)(cr - c->query.data) + 2;

        if (ll <= 0) return PARSE_OK; /* "*0", "*-1": 빈 요청, 무시 */
        c->multibulk_left = (long)ll;
        c->bulk_len = -1;
    }

    while (c->multibulk_left > 0) {
        if (c->bulk_len == -1) {
            /* 인자 길이 줄: "$<len>\r\n" */
            if (c->qpos >= c->query.len) return PARSE_NEED_MORE;

            if (c->query.data[c->qpos] != '$')
                return protocol_error(c, "expected '$', got '%c'", c->query.data[c->qpos]);

            const char *cr = find_line_end(c);
            if (cr == NULL) {
                if (c->query.len - c->qpos > PROTO_INLINE_MAX_SIZE)
                    return protocol_error(c, "too big bulk count string");
                return PARSE_NEED_MORE;
            }

            const char *num = c->query.data + c->qpos + 1;
            if (!util_str_to_ll(num, (size_t)(cr - num), &ll) || ll < 0 || ll > PROTO_MAX_BULK_LEN)
                return protocol_error(c, "invalid bulk length");

            c->qpos = (size_t)(cr - c->query.data) + 2;
            c->bulk_len = (long)ll;
        }

        /* 인자 본문 + "\r\n" 이 다 왔는지 확인 */
        size_t need = (size_t)c->bulk_len + 2;
        if (c->query.len - c->qpos < need) {
            /* 큰 값은 한 번에 받을 공간을 미리 확보해 realloc 반복을 줄인다 */
            buffer_reserve(&c->query, need - (c->query.len - c->qpos));
            return PARSE_NEED_MORE;
        }

        push_arg(c, c->query.data + c->qpos, (size_t)c->bulk_len);
        c->qpos += need; /* Redis 처럼 끝의 "\r\n" 은 확인하지 않고 건너뛴다 */
        c->bulk_len = -1;
        c->multibulk_left--;
    }
    return PARSE_OK;
}

/* 새 데이터가 들어올 때마다 server.c 가 호출한다.
 * 완성된 요청을 순서대로 모두 실행하고, 처리한 바이트는 한 번에 잘라낸다. */
void client_process_input(client_t *c) {
    while (!c->close_after_reply) {
        parse_result_t r = parse_request(c);
        if (r == PARSE_NEED_MORE || r == PARSE_ERROR) break;

        if (c->argc > 0) command_execute(c);
        client_reset_args(c);
    }

    if (c->close_after_reply) {
        /* 에러 이후의 입력은 의미가 없으므로 버린다 */
        buffer_clear(&c->query);
        c->qpos = 0;
        return;
    }
    buffer_consume(&c->query, c->qpos);
    c->qpos = 0;
}
