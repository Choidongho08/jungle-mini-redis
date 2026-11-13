/* 명령 테이블과 디스패치 (design §6.3, §6.4) */
#include "command.h"

#include <stdio.h>
#include <string.h>

#include "buffer.h"
#include "resp.h"

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

/* ---------- 테이블 ---------- */

static const command_t command_table[] = {
    { "ping", cmd_ping, -1 },
};

#define COMMAND_COUNT (sizeof(command_table) / sizeof(command_table[0]))

static int ascii_lower(int ch) {
    return (ch >= 'A' && ch <= 'Z') ? ch + ('a' - 'A') : ch;
}

/* 명령 이름은 대소문자를 구분하지 않는다 (FR-007). */
static const command_t *lookup(const bytes_t *name) {
    for (size_t i = 0; i < COMMAND_COUNT; i++) {
        const char *n = command_table[i].name;
        size_t len = strlen(n);
        if (len != name->len) continue;
        size_t j = 0;
        while (j < len && ascii_lower((unsigned char)name->ptr[j]) == n[j]) j++;
        if (j == len) return &command_table[i];
    }
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
