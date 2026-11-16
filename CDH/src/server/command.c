#include "miniredis/command.h"

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/*
 * 명령 구현. 모든 핸들러는 응답 정확히 하나를 c->out 에 덧붙인다.
 * 핸들러의 반환값은 "응답을 만들 수 있었는가"만 뜻한다(MR_OK 가 아니면 연결 종료).
 * 명령 수준의 실패는 -ERR 응답을 만들고 MR_OK 를 돌려준다.
 */

#define CMD_WRITE 0x1u /* 데이터를 바꿀 수 있음: AOF 고장 시 거부 대상 */

/* 오류 메시지에 사용자 바이트를 옮겨 적을 때의 상한(Redis 와 같은 128) */
#define ERR_ARG_PREVIEW 128

typedef struct cmd_call {
    mr_cmd_env *env;
    size_t argc;
    const mr_slice *argv;
    mr_buf *out;
    const char *name;   /* 테이블의 소문자 이름(오류 메시지용) */
    mr_status aof_err;  /* 마지막 AOF 기록 실패 원인 */
} cmd_call;

typedef mr_status (*cmd_fn)(cmd_call *c);

typedef struct cmd_def {
    const char *name;
    int arity;
    cmd_fn fn;
    unsigned flags;
} cmd_def;

/* ---------- 공용 도우미 ---------- */

static bool slice_eq_ci(const mr_slice *a, const char *lit) {
    size_t n = strlen(lit);
    if (a->len != n) {
        return false;
    }
    for (size_t i = 0; i < n; i++) {
        unsigned char x = a->ptr[i];
        unsigned char y = (unsigned char)lit[i];
        if (x >= 'A' && x <= 'Z') {
            x = (unsigned char)(x - 'A' + 'a');
        }
        if (y >= 'A' && y <= 'Z') {
            y = (unsigned char)(y - 'A' + 'a');
        }
        if (x != y) {
            return false;
        }
    }
    return true;
}

static bool arg_i64(const mr_slice *a, int64_t *out) { return mr_parse_i64(a->ptr, a->len, out); }

static int64_t now_ms(const cmd_call *c) { return mr_clock_wall_ms(c->env->clock); }

static mr_status reply_ok(cmd_call *c) { return mr_resp_simple(c->out, "OK"); }

static mr_status reply_syntax(cmd_call *c) { return mr_resp_error(c->out, "ERR syntax error"); }

static mr_status reply_not_int(cmd_call *c) {
    return mr_resp_error(c->out, "ERR value is not an integer or out of range");
}

static mr_status reply_bad_expire(cmd_call *c) {
    return mr_resp_errorf(c->out, "ERR invalid expire time in '%s' command", c->name);
}

/* 저장소 오류를 사용자 오류로. 응답 작성 실패만 비-MR_OK 로 돌아간다. */
static mr_status reply_store_error(cmd_call *c, mr_status st) {
    switch (st) {
    case MR_NOMEM:
        return mr_resp_error(c->out, "ERR out of memory");
    case MR_NOT_INTEGER:
        return reply_not_int(c);
    case MR_OVERFLOW:
        return mr_resp_error(c->out, "ERR increment or decrement would overflow");
    default:
        return mr_resp_errorf(c->out, "ERR %s", mr_status_str(st));
    }
}

static bool aof_enabled(const cmd_call *c) { return c->env->aof != NULL; }

/* AOF 기록 결과 확인. 실패면 쓰기 차단 플래그를 세우고 true. */
static bool aof_failed(cmd_call *c, mr_status st) {
    if (st == MR_OK) {
        return false;
    }
    c->env->aof_write_failed = true;
    c->aof_err = st;
    return true;
}

static mr_status reply_aof_error(cmd_call *c) {
    return mr_resp_errorf(c->out,
                          "ERR AOF write failed (%s); write commands are disabled until "
                          "the server is restarted",
                          mr_status_str(c->aof_err));
}

/* 사용자 바이트를 오류 메시지용으로 옮긴다(NUL 은 '?', 최대 max 바이트). */
static void preview_append(char *dst, size_t cap, size_t *len, const mr_slice *s, size_t max) {
    size_t n = s->len < max ? s->len : max;
    for (size_t i = 0; i < n && *len + 1 < cap; i++) {
        dst[(*len)++] = s->ptr[i] == '\0' ? '?' : (char)s->ptr[i];
    }
    dst[*len] = '\0';
}

static void text_append(char *dst, size_t cap, size_t *len, const char *s) {
    mr_slice sl = {(const unsigned char *)s, strlen(s)};
    preview_append(dst, cap, len, &sl, sl.len);
}

/* Redis 7 형식: unknown command 'foo', with args beginning with: 'a' 'b' */
static mr_status reply_unknown(mr_buf *out, size_t argc, const mr_slice *argv) {
    char msg[1024];
    size_t len = 0;
    msg[0] = '\0';
    text_append(msg, sizeof(msg), &len, "ERR unknown command '");
    preview_append(msg, sizeof(msg), &len, &argv[0], ERR_ARG_PREVIEW);
    text_append(msg, sizeof(msg), &len, "', with args beginning with: ");
    for (size_t i = 1; i < argc && len < sizeof(msg) - ERR_ARG_PREVIEW - 4; i++) {
        text_append(msg, sizeof(msg), &len, "'");
        preview_append(msg, sizeof(msg), &len, &argv[i], ERR_ARG_PREVIEW);
        text_append(msg, sizeof(msg), &len, "' ");
    }
    return mr_resp_error(out, msg);
}

/* ---------- 연결 ---------- */

static mr_status cmd_ping(cmd_call *c) {
    if (c->argc > 2) {
        return mr_resp_errorf(c->out, "ERR wrong number of arguments for '%s' command", c->name);
    }
    if (c->argc == 2) {
        return mr_resp_bulk(c->out, c->argv[1].ptr, c->argv[1].len);
    }
    return mr_resp_simple(c->out, "PONG");
}

/* ---------- SET / GET / DEL / EXISTS ---------- */

typedef enum set_parse {
    SET_PARSE_OK,
    SET_PARSE_SYNTAX,
    SET_PARSE_NOT_INT,
    SET_PARSE_BAD_EXPIRE
} set_parse;

/*
 * SET 옵션 해석: EX seconds 하나만 받는다. 상대 TTL 도 여기서 절대 시각으로 바꿔
 * 저장소에 PXAT 로 넘긴다: 저장소와 AOF 가 "같은 절대 시각"을 쓰게 해 재생 결과가
 * 정확히 일치하게 하려는 것.
 */
static set_parse parse_set_options(const cmd_call *c, int64_t now, mr_set_opts *o) {
    o->cond = MR_SET_ALWAYS;
    o->expire_kind = MR_EXPIRE_NONE;
    o->expire = 0;
    for (size_t i = 3; i < c->argc; i++) {
        if (!slice_eq_ci(&c->argv[i], "EX") || o->expire_kind != MR_EXPIRE_NONE ||
            i + 1 >= c->argc) {
            return SET_PARSE_SYNTAX;
        }
        int64_t v;
        if (!arg_i64(&c->argv[i + 1], &v)) {
            return SET_PARSE_NOT_INT;
        }
        if (v <= 0 || v > INT64_MAX / 1000 || now > INT64_MAX - v * 1000) {
            return SET_PARSE_BAD_EXPIRE;
        }
        o->expire = now + v * 1000;
        o->expire_kind = MR_EXPIRE_PXAT;
        i++;
    }
    return SET_PARSE_OK;
}

static mr_status cmd_set(cmd_call *c) {
    mr_set_opts o;
    switch (parse_set_options(c, now_ms(c), &o)) {
    case SET_PARSE_SYNTAX:
        return reply_syntax(c);
    case SET_PARSE_NOT_INT:
        return reply_not_int(c);
    case SET_PARSE_BAD_EXPIRE:
        return reply_bad_expire(c);
    case SET_PARSE_OK:
        break;
    }
    const mr_slice *k = &c->argv[1];
    const mr_slice *v = &c->argv[2];
    mr_status st = mr_store_set(c->env->store, k->ptr, k->len, v->ptr, v->len, &o);
    if (st != MR_OK) {
        return reply_store_error(c, st);
    }
    /* 이미 지난 절대 시각이어도 그대로 기록한다: 재생기는 로딩 끝에 만료 키를 버린다. */
    if (aof_enabled(c) &&
        aof_failed(c, mr_aof_log_set(c->env->aof, k->ptr, k->len, v->ptr, v->len, o.expire))) {
        return reply_aof_error(c);
    }
    return reply_ok(c);
}

static mr_status cmd_get(cmd_call *c) {
    const void *val;
    size_t vlen;
    mr_status st = mr_store_get(c->env->store, c->argv[1].ptr, c->argv[1].len, &val, &vlen);
    if (st == MR_NOT_FOUND) {
        return mr_resp_null(c->out);
    }
    if (st != MR_OK) {
        return reply_store_error(c, st);
    }
    return mr_resp_bulk(c->out, val, vlen);
}

static mr_status cmd_del(cmd_call *c) {
    int64_t deleted = 0;
    for (size_t i = 1; i < c->argc; i++) {
        const mr_slice *k = &c->argv[i];
        if (mr_store_del(c->env->store, k->ptr, k->len) != MR_OK) {
            continue;
        }
        deleted++;
        if (aof_enabled(c) && aof_failed(c, mr_aof_log_del(c->env->aof, k->ptr, k->len))) {
            return reply_aof_error(c);
        }
    }
    return mr_resp_int(c->out, deleted);
}

static mr_status cmd_exists(cmd_call *c) {
    /* Redis 처럼 같은 키를 여러 번 주면 여러 번 센다. */
    int64_t n = 0;
    for (size_t i = 1; i < c->argc; i++) {
        if (mr_store_exists(c->env->store, c->argv[i].ptr, c->argv[i].len)) {
            n++;
        }
    }
    return mr_resp_int(c->out, n);
}

/* ---------- EXPIRE / TTL ---------- */

/*
 * EXPIRE key seconds. 상대 TTL 을 절대 시각으로 바꿔 적용·기록한다(AOF 에는 PEXPIREAT).
 * seconds 가 0 이하이면 Redis 처럼 키를 즉시 지우고 DEL 로 기록한다.
 */
static mr_status cmd_expire(cmd_call *c) {
    int64_t v;
    if (!arg_i64(&c->argv[2], &v)) {
        return reply_not_int(c);
    }
    int64_t now = now_ms(c);
    int64_t at;
    if (v > INT64_MAX / 1000 || v < INT64_MIN / 1000) {
        return reply_bad_expire(c);
    }
    int64_t ms = v * 1000;
    if (ms <= 0) {
        at = now;
    } else if (now > INT64_MAX - ms) {
        return reply_bad_expire(c);
    } else {
        at = now + ms;
    }
    const mr_slice *k = &c->argv[1];
    if (!mr_store_exists(c->env->store, k->ptr, k->len)) {
        return mr_resp_int(c->out, 0);
    }
    mr_status st;
    if (at <= now) {
        (void)mr_store_del(c->env->store, k->ptr, k->len);
        st = aof_enabled(c) ? mr_aof_log_del(c->env->aof, k->ptr, k->len) : MR_OK;
    } else {
        st = mr_store_pexpireat(c->env->store, k->ptr, k->len, at);
        if (st == MR_NOT_FOUND) {
            return mr_resp_int(c->out, 0);
        }
        if (st != MR_OK) {
            return reply_store_error(c, st);
        }
        st = aof_enabled(c) ? mr_aof_log_pexpireat(c->env->aof, k->ptr, k->len, at) : MR_OK;
    }
    if (aof_failed(c, st)) {
        return reply_aof_error(c);
    }
    return mr_resp_int(c->out, 1);
}

static mr_status cmd_ttl(cmd_call *c) {
    int64_t v;
    const mr_slice *k = &c->argv[1];
    mr_status st = mr_store_ttl(c->env->store, k->ptr, k->len, &v);
    if (st == MR_NOT_FOUND) {
        return mr_resp_int(c->out, -2);
    }
    if (st != MR_OK) {
        return reply_store_error(c, st);
    }
    return mr_resp_int(c->out, v);
}

/* ---------- INCR ---------- */

/*
 * 키의 절대 만료 시각(무기한 0). 저장소는 남은 ms 만 알려 주므로 전후로 시계를 읽어
 * 같은 ms 안에서 잰 값일 때 "now + 남은 ms" 가 정확한 절대 시각이 된다.
 * 시계가 계속 움직이면 몇 번 재시도하고, 그래도 안 되면 1ms 이내 근사를 쓴다.
 */
static mr_status key_expire_at(cmd_call *c, const mr_slice *k, int64_t *at) {
    for (int attempt = 0; attempt < 4; attempt++) {
        int64_t before = now_ms(c);
        int64_t left;
        mr_status st = mr_store_pttl(c->env->store, k->ptr, k->len, &left);
        int64_t after = now_ms(c);
        if (st != MR_OK) {
            return st;
        }
        *at = left < 0 ? 0 : before + left;
        if (before == after || left < 0) {
            break;
        }
    }
    return MR_OK;
}

/* INCR 은 AOF 에 "결과값 SET + 기존 만료 시각"으로 기록한다(재생이 결정적이게). */
static mr_status log_incr_result(cmd_call *c, const mr_slice *k, int64_t v) {
    char num[24];
    int n = snprintf(num, sizeof(num), "%" PRId64, v);
    int64_t at = 0;
    mr_status st = key_expire_at(c, k, &at);
    if (st == MR_NOT_FOUND) {
        /* 기록 직전에 만료 시각에 닿아 사라졌다: 결과도 "없음"이다. */
        return mr_aof_log_del(c->env->aof, k->ptr, k->len);
    }
    if (st != MR_OK) {
        return st;
    }
    return mr_aof_log_set(c->env->aof, k->ptr, k->len, num, (size_t)n, at);
}

static mr_status cmd_incr(cmd_call *c) {
    const mr_slice *k = &c->argv[1];
    int64_t v;
    mr_status st = mr_store_incrby(c->env->store, k->ptr, k->len, 1, &v);
    if (st != MR_OK) {
        return reply_store_error(c, st);
    }
    if (aof_enabled(c) && aof_failed(c, log_incr_result(c, k, v))) {
        return reply_aof_error(c);
    }
    return mr_resp_int(c->out, v);
}

/* ---------- FLUSHALL ---------- */

static mr_status cmd_flushall(cmd_call *c) {
    /* ASYNC|SYNC 는 호환을 위해 받기만 한다(싱글 스레드라 항상 동기). */
    if (c->argc > 2 ||
        (c->argc == 2 && !slice_eq_ci(&c->argv[1], "ASYNC") && !slice_eq_ci(&c->argv[1], "SYNC"))) {
        return reply_syntax(c);
    }
    mr_store_flushall(c->env->store);
    if (aof_enabled(c) && aof_failed(c, mr_aof_log_flushall(c->env->aof))) {
        return reply_aof_error(c);
    }
    return reply_ok(c);
}

/* ---------- 명령 테이블 ---------- */

static const cmd_def COMMANDS[] = {
    {"ping", -1, cmd_ping, 0},
    {"set", -3, cmd_set, CMD_WRITE},
    {"get", 2, cmd_get, 0},
    {"del", -2, cmd_del, CMD_WRITE},
    {"exists", -2, cmd_exists, 0},
    {"expire", 3, cmd_expire, CMD_WRITE},
    {"ttl", 2, cmd_ttl, 0},
    {"incr", 2, cmd_incr, CMD_WRITE},
    {"flushall", -1, cmd_flushall, CMD_WRITE},
};

#define COMMAND_COUNT (sizeof(COMMANDS) / sizeof(COMMANDS[0]))

/* 명령이 9개라 선형 탐색으로 충분하다(길이 비교로 대부분 즉시 탈락). */
static const cmd_def *lookup(const void *name, size_t len) {
    mr_slice s = {name, len};
    for (size_t i = 0; i < COMMAND_COUNT; i++) {
        if (slice_eq_ci(&s, COMMANDS[i].name)) {
            return &COMMANDS[i];
        }
    }
    return NULL;
}

static bool arity_ok(const cmd_def *d, size_t argc) {
    if (d->arity >= 0) {
        return argc == (size_t)d->arity;
    }
    return argc >= (size_t)(-d->arity);
}

size_t mr_cmd_count(void) { return COMMAND_COUNT; }

bool mr_cmd_is_write(const void *name, size_t len) {
    const cmd_def *d = lookup(name, len);
    return d != NULL && (d->flags & CMD_WRITE) != 0;
}

mr_status mr_cmd_exec(mr_cmd_env *env, size_t argc, const mr_slice *argv, mr_buf *out) {
    if (argc == 0) {
        return MR_OK;
    }
    const cmd_def *d = lookup(argv[0].ptr, argv[0].len);
    if (d == NULL) {
        return reply_unknown(out, argc, argv);
    }
    if (!arity_ok(d, argc)) {
        return mr_resp_errorf(out, "ERR wrong number of arguments for '%s' command", d->name);
    }
    if ((d->flags & CMD_WRITE) != 0 && env->aof_write_failed) {
        return mr_resp_error(out, "ERR AOF write error earlier; write commands are disabled "
                                  "until the server is restarted");
    }
    cmd_call c = {env, argc, argv, out, d->name, MR_OK};
    return d->fn(&c);
}
