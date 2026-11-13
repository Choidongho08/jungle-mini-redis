/* 명령 테이블과 디스패치 (design §6.3, §6.4) */
#include "command.h"

#include <limits.h>
#include <stdio.h>
#include <string.h>

#include "buffer.h"
#include "db.h"
#include "resp.h"
#include "util.h"

static int ascii_lower(int ch);

typedef void (*command_fn)(client_t *c);

typedef struct {
    const char *name; /* 소문자 */
    command_fn fn;
    /* Redis 의 arity 규칙 (명령 이름 포함):
     *   양수 N  → 인자가 정확히 N 개
     *   음수 -N → 인자가 최소 N 개 */
    int arity;
} command_t;

/* ---------- 명령 구현 ---------- */

static void cmd_ping(client_t *c) {
    if (c->argc > 2) {
        reply_error(c, "wrong number of arguments for 'ping' command");
        return;
    }
    if (c->argc == 2)
        reply_bulk(c, c->argv[1].ptr, c->argv[1].len);
    else
        reply_simple(c, "PONG");
}

static int arg_equals_nocase(const bytes_t *arg, const char *lower);

/* SET key value */
static void cmd_set(client_t *c) {
    if (c->argc != 3) {
        reply_error(c, "syntax error");
        return;
    }
    db_set(c->argv[1].ptr, c->argv[1].len, c->argv[2].ptr, c->argv[2].len, 0);
    reply_simple(c, "OK");
}

static void cmd_get(client_t *c) {
    value_t *v = db_lookup(c->argv[1].ptr, c->argv[1].len);
    if (v == NULL)
        reply_null(c);
    else
        reply_bulk(c, v->ptr, v->len);
}

/* DEL key [key ...] → 실제로 지운 개수 */
static void cmd_del(client_t *c) {
    long long n = 0;
    for (int i = 1; i < c->argc; i++) {
        /* lookup 을 먼저 거쳐야 만료된 키를 "지웠다"고 세지 않는다 (T8) */
        if (db_lookup(c->argv[i].ptr, c->argv[i].len) != NULL)
            n += db_delete(c->argv[i].ptr, c->argv[i].len);
    }
    reply_integer(c, n);
}

/* EXISTS key [key ...] → 존재하는 키 개수 (같은 키를 여러 번 주면 그만큼 셈) */
static void cmd_exists(client_t *c) {
    long long n = 0;
    for (int i = 1; i < c->argc; i++)
        if (db_lookup(c->argv[i].ptr, c->argv[i].len) != NULL) n++;
    reply_integer(c, n);
}

/* INCR key
 * 단일 스레드라 "읽기 → +1 → 쓰기" 사이에 다른 명령이 끼어들 수 없다.
 * 그래서 락 없이도 여러 클라이언트의 INCR 이 하나도 빠지지 않는다 (US-004). */
static void cmd_incr(client_t *c) {
    long long cur = 0;
    value_t *v = db_lookup(c->argv[1].ptr, c->argv[1].len);
    if (v != NULL && !util_str_to_ll(v->ptr, v->len, &cur)) {
        reply_error(c, "value is not an integer or out of range");
        return;
    }
    if (cur == LLONG_MAX) {
        reply_error(c, "increment or decrement would overflow");
        return;
    }
    cur++;

    char num[32];
    int n = snprintf(num, sizeof(num), "%lld", cur);
    /* 값은 10진 문자열로 저장한다 (Redis 의 int 인코딩 최적화는 범위 밖, FAQ Q3).
     * 기존 만료 시간은 유지한다 (FR-019). */
    db_set(c->argv[1].ptr, c->argv[1].len, num, (size_t)n, 1);
    reply_integer(c, cur);
}

/* FLUSHALL [ASYNC|SYNC]. 비동기 삭제는 없으므로 두 옵션 모두 동기로 처리한다. */
static void cmd_flushall(client_t *c) {
    if (c->argc > 2 || (c->argc == 2 && !arg_equals_nocase(&c->argv[1], "async") &&
                        !arg_equals_nocase(&c->argv[1], "sync"))) {
        reply_error(c, "syntax error");
        return;
    }
    db_flush();
    reply_simple(c, "OK");
}

/* ---------- 테이블 ---------- */

static const command_t command_table[] = {
    { "ping", cmd_ping, -1 },
    { "set", cmd_set, -3 },
    { "get", cmd_get, 2 },
    { "del", cmd_del, -2 },
    { "exists", cmd_exists, -2 },
    { "incr", cmd_incr, 2 },
    { "flushall", cmd_flushall, -1 },
};

#define COMMAND_COUNT (sizeof(command_table) / sizeof(command_table[0]))

static int ascii_lower(int ch) {
    return (ch >= 'A' && ch <= 'Z') ? ch + ('a' - 'A') : ch;
}

/* 인자가 소문자 문자열 lower 와 대소문자 무시하고 같은지 */
static int arg_equals_nocase(const bytes_t *arg, const char *lower) {
    size_t len = strlen(lower);
    if (arg->len != len) return 0;
    for (size_t i = 0; i < len; i++)
        if (ascii_lower((unsigned char)arg->ptr[i]) != lower[i]) return 0;
    return 1;
}

/* 명령 이름은 대소문자를 구분하지 않는다 (FR-007). */
static const command_t *lookup(const bytes_t *name) {
    for (size_t i = 0; i < COMMAND_COUNT; i++)
        if (arg_equals_nocase(name, command_table[i].name)) return &command_table[i];
    return NULL;
}

/* Redis 와 같은 형식:
 *   ERR unknown command 'foo', with args beginning with: 'a' 'b'
 * 인자 목록은 합계 128자에서 자른다. */
static void reply_unknown_command(client_t *c) {
    buffer_t args, msg;
    buffer_init(&args);
    buffer_init(&msg);

    for (int i = 1; i < c->argc && args.len < 128; i++) {
        size_t room = 128 - args.len;
        /* Redis 는 C 문자열 포맷(%.*s)을 써서 NUL 에서 끊기므로 똑같이 맞춘다 */
        size_t n = strnlen(c->argv[i].ptr, c->argv[i].len);
        if (n > room) n = room;
        buffer_append(&args, "'", 1);
        buffer_append(&args, c->argv[i].ptr, n);
        buffer_append(&args, "' ", 2);
    }

    size_t name_len = strnlen(c->argv[0].ptr, c->argv[0].len);
    if (name_len > 128) name_len = 128;

    buffer_append_str(&msg, "ERR unknown command '");
    buffer_append(&msg, c->argv[0].ptr, name_len);
    buffer_append_str(&msg, "', with args beginning with: ");
    buffer_append(&msg, args.data, args.len);

    reply_error_raw(c, msg.data, msg.len);
    buffer_free(&args);
    buffer_free(&msg);
}

void command_execute(client_t *c) {
    const command_t *cmd = lookup(&c->argv[0]);
    if (cmd == NULL) {
        reply_unknown_command(c);
        return;
    }

    if ((cmd->arity > 0 && c->argc != cmd->arity) || (cmd->arity < 0 && c->argc < -cmd->arity)) {
        char msg[96];
        snprintf(msg, sizeof(msg), "wrong number of arguments for '%s' command", cmd->name);
        reply_error(c, msg);
        return;
    }

    cmd->fn(c);
}
