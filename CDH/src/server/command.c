#include "miniredis/command.h"

#include <inttypes.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "miniredis/glob.h"

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
    unsigned result;    /* MR_CMD_RESULT_* */
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
                          "BGREWRITEAOF succeeds",
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

/* ---------- 연결/기타 ---------- */

static mr_status cmd_ping(cmd_call *c) {
    if (c->argc > 2) {
        return mr_resp_errorf(c->out, "ERR wrong number of arguments for '%s' command", c->name);
    }
    if (c->argc == 2) {
        return mr_resp_bulk(c->out, c->argv[1].ptr, c->argv[1].len);
    }
    return mr_resp_simple(c->out, "PONG");
}

static mr_status cmd_echo(cmd_call *c) {
    return mr_resp_bulk(c->out, c->argv[1].ptr, c->argv[1].len);
}

static mr_status cmd_quit(cmd_call *c) {
    c->result |= MR_CMD_RESULT_CLOSE;
    return reply_ok(c);
}

static mr_status cmd_select(cmd_call *c) {
    int64_t db;
    if (!arg_i64(&c->argv[1], &db)) {
        return reply_not_int(c);
    }
    /* 데이터베이스는 하나뿐이다. 0 을 허용해 SELECT 0 을 보내는 클라이언트와 호환. */
    if (db != 0) {
        return mr_resp_error(c->out, "ERR DB index is out of range");
    }
    return reply_ok(c);
}

/* ---------- SET / GET / DEL / EXISTS ---------- */

typedef enum set_parse {
    SET_PARSE_OK,
    SET_PARSE_SYNTAX,
    SET_PARSE_NOT_INT,
    SET_PARSE_BAD_EXPIRE
} set_parse;

/* EX/PX/EXAT/PXAT 값을 절대 epoch ms 로. 0 이하·오버플로는 false. */
static bool expire_option_to_at(const mr_slice *opt, int64_t v, int64_t now, int64_t *at) {
    bool seconds = slice_eq_ci(opt, "EX") || slice_eq_ci(opt, "EXAT");
    bool absolute = slice_eq_ci(opt, "EXAT") || slice_eq_ci(opt, "PXAT");
    if (v <= 0 || (seconds && v > INT64_MAX / 1000)) {
        return false;
    }
    int64_t ms = seconds ? v * 1000 : v;
    if (absolute) {
        *at = ms;
        return true;
    }
    if (now > INT64_MAX - ms) {
        return false;
    }
    *at = now + ms;
    return true;
}

static bool is_expire_option(const mr_slice *a) {
    return slice_eq_ci(a, "EX") || slice_eq_ci(a, "PX") || slice_eq_ci(a, "EXAT") ||
           slice_eq_ci(a, "PXAT");
}

/*
 * SET 옵션 해석. 상대 TTL 도 여기서 절대 시각으로 바꿔 저장소에 PXAT 로 넘긴다:
 * 저장소와 AOF 가 "같은 절대 시각"을 쓰게 해 재생 결과가 정확히 일치하게 하려는 것.
 */
static set_parse parse_set_options(const cmd_call *c, int64_t now, mr_set_opts *o) {
    o->cond = MR_SET_ALWAYS;
    o->expire_kind = MR_EXPIRE_NONE;
    o->expire = 0;
    for (size_t i = 3; i < c->argc; i++) {
        const mr_slice *a = &c->argv[i];
        if (slice_eq_ci(a, "NX") || slice_eq_ci(a, "XX")) {
            if (o->cond != MR_SET_ALWAYS) {
                return SET_PARSE_SYNTAX;
            }
            o->cond = slice_eq_ci(a, "NX") ? MR_SET_NX : MR_SET_XX;
        } else if (is_expire_option(a)) {
            if (o->expire_kind != MR_EXPIRE_NONE || i + 1 >= c->argc) {
                return SET_PARSE_SYNTAX;
            }
            int64_t v;
            if (!arg_i64(&c->argv[i + 1], &v)) {
                return SET_PARSE_NOT_INT;
            }
            if (!expire_option_to_at(a, v, now, &o->expire)) {
                return SET_PARSE_BAD_EXPIRE;
            }
            o->expire_kind = MR_EXPIRE_PXAT;
            i++;
        } else {
            return SET_PARSE_SYNTAX;
        }
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
    if (st == MR_COND_FAILED) {
        return mr_resp_null(c->out);
    }
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

/* ---------- TTL ---------- */

/* 값 v(unit 단위)를 절대 시각으로. 상대 TTL 이 0 이하이면 now(=즉시 삭제)로 둔다. */
static bool expire_arg_to_at(int64_t v, int64_t unit, bool absolute, int64_t now, int64_t *at) {
    if (v > INT64_MAX / unit || v < INT64_MIN / unit) {
        return false;
    }
    int64_t ms = v * unit;
    if (absolute) {
        *at = ms;
        return true;
    }
    if (ms <= 0) {
        *at = now;
        return true;
    }
    if (now > INT64_MAX - ms) {
        return false;
    }
    *at = now + ms;
    return true;
}

/*
 * EXPIRE 계열 공통. 상대 TTL 도 절대 시각으로 바꿔 PEXPIREAT 로 적용·기록한다.
 * 결과 시각이 지금 이하이면 Redis 처럼 키를 지우고 DEL 로 기록한다.
 */
static mr_status expire_generic(cmd_call *c, int64_t unit, bool absolute) {
    int64_t v;
    if (!arg_i64(&c->argv[2], &v)) {
        return reply_not_int(c);
    }
    int64_t now = now_ms(c);
    int64_t at;
    if (!expire_arg_to_at(v, unit, absolute, now, &at)) {
        return reply_bad_expire(c);
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

static mr_status cmd_expire(cmd_call *c) { return expire_generic(c, 1000, false); }
static mr_status cmd_pexpire(cmd_call *c) { return expire_generic(c, 1, false); }
static mr_status cmd_expireat(cmd_call *c) { return expire_generic(c, 1000, true); }
static mr_status cmd_pexpireat(cmd_call *c) { return expire_generic(c, 1, true); }

static mr_status ttl_generic(cmd_call *c, bool ms) {
    int64_t v;
    const mr_slice *k = &c->argv[1];
    mr_status st = ms ? mr_store_pttl(c->env->store, k->ptr, k->len, &v)
                      : mr_store_ttl(c->env->store, k->ptr, k->len, &v);
    if (st == MR_NOT_FOUND) {
        return mr_resp_int(c->out, -2);
    }
    if (st != MR_OK) {
        return reply_store_error(c, st);
    }
    return mr_resp_int(c->out, v);
}

static mr_status cmd_ttl(cmd_call *c) { return ttl_generic(c, false); }
static mr_status cmd_pttl(cmd_call *c) { return ttl_generic(c, true); }

static mr_status cmd_persist(cmd_call *c) {
    const mr_slice *k = &c->argv[1];
    mr_status st = mr_store_persist(c->env->store, k->ptr, k->len);
    if (st != MR_OK) {
        return mr_resp_int(c->out, 0);
    }
    if (aof_enabled(c) && aof_failed(c, mr_aof_log_persist(c->env->aof, k->ptr, k->len))) {
        return reply_aof_error(c);
    }
    return mr_resp_int(c->out, 1);
}

/* ---------- INCR 계열 ---------- */

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

static mr_status incr_generic(cmd_call *c, int64_t delta) {
    const mr_slice *k = &c->argv[1];
    int64_t v;
    mr_status st = mr_store_incrby(c->env->store, k->ptr, k->len, delta, &v);
    if (st != MR_OK) {
        return reply_store_error(c, st);
    }
    if (aof_enabled(c) && aof_failed(c, log_incr_result(c, k, v))) {
        return reply_aof_error(c);
    }
    return mr_resp_int(c->out, v);
}

static mr_status cmd_incr(cmd_call *c) { return incr_generic(c, 1); }
static mr_status cmd_decr(cmd_call *c) { return incr_generic(c, -1); }

static mr_status cmd_incrby(cmd_call *c) {
    int64_t d;
    if (!arg_i64(&c->argv[2], &d)) {
        return reply_not_int(c);
    }
    return incr_generic(c, d);
}

static mr_status cmd_decrby(cmd_call *c) {
    int64_t d;
    if (!arg_i64(&c->argv[2], &d)) {
        return reply_not_int(c);
    }
    if (d == INT64_MIN) {
        /* -INT64_MIN 은 표현할 수 없다(Redis 동일 메시지). */
        return mr_resp_error(c->out, "ERR decrement would overflow");
    }
    return incr_generic(c, -d);
}

/* ---------- KEYS / SCAN / DBSIZE ---------- */

typedef struct collect_ctx {
    mr_buf items;
    size_t n;
    mr_status st;
} collect_ctx;

static bool collect_visit(const void *key, size_t klen, const void *val, size_t vlen,
                          int64_t expire_at_ms, void *ud) {
    (void)val;
    (void)vlen;
    (void)expire_at_ms;
    collect_ctx *x = ud;
    x->st = mr_resp_bulk(&x->items, key, klen);
    if (x->st == MR_OK) {
        x->n++;
    }
    return x->st == MR_OK;
}

/* 개수를 먼저 알아야 배열 헤더를 쓸 수 있어 원소를 임시 버퍼에 모은 뒤 붙인다. */
static mr_status emit_collected(cmd_call *c, collect_ctx *x) {
    mr_status st = mr_resp_array(c->out, x->n);
    if (st == MR_OK) {
        st = mr_buf_append(c->out, x->items.data, x->items.len);
    }
    return st;
}

static mr_status cmd_keys(cmd_call *c) {
    collect_ctx x = {{NULL, 0, 0}, 0, MR_OK};
    const mr_slice *p = &c->argv[1];
    mr_status st = mr_store_keys(c->env->store, p->ptr, p->len, collect_visit, &x);
    if (st == MR_OK) {
        st = x.st;
    }
    if (st == MR_OK) {
        st = emit_collected(c, &x);
    }
    mr_buf_free(&x.items);
    return st;
}

static bool parse_cursor(const mr_slice *a, uint64_t *out) {
    if (a->len == 0 || a->len > 20) {
        return false;
    }
    uint64_t v = 0;
    for (size_t i = 0; i < a->len; i++) {
        if (a->ptr[i] < '0' || a->ptr[i] > '9') {
            return false;
        }
        unsigned d = (unsigned)(a->ptr[i] - '0');
        if (v > (UINT64_MAX - d) / 10) {
            return false;
        }
        v = v * 10 + d;
    }
    *out = v;
    return true;
}

typedef struct scan_args {
    uint64_t cursor;
    const mr_slice *match;
    size_t count;
} scan_args;

/* 응답을 이미 만들었으면(오류) *replied=true. */
static mr_status parse_scan_args(cmd_call *c, scan_args *a, bool *replied) {
    *replied = true;
    if (!parse_cursor(&c->argv[1], &a->cursor)) {
        return mr_resp_error(c->out, "ERR invalid cursor");
    }
    a->match = NULL;
    a->count = 10;
    for (size_t i = 2; i < c->argc; i += 2) {
        if (i + 1 >= c->argc) {
            return reply_syntax(c);
        }
        if (slice_eq_ci(&c->argv[i], "MATCH")) {
            a->match = &c->argv[i + 1];
        } else if (slice_eq_ci(&c->argv[i], "COUNT")) {
            int64_t n;
            if (!arg_i64(&c->argv[i + 1], &n)) {
                return reply_not_int(c);
            }
            if (n < 1) {
                return reply_syntax(c);
            }
            a->count = (uint64_t)n > SIZE_MAX ? SIZE_MAX : (size_t)n;
        } else {
            return reply_syntax(c);
        }
    }
    *replied = false;
    return MR_OK;
}

static mr_status cmd_scan(cmd_call *c) {
    scan_args a;
    bool replied;
    mr_status st = parse_scan_args(c, &a, &replied);
    if (replied) {
        return st;
    }
    collect_ctx x = {{NULL, 0, 0}, 0, MR_OK};
    uint64_t next = 0;
    st = mr_store_scan(c->env->store, a.cursor, a.match != NULL ? a.match->ptr : NULL,
                       a.match != NULL ? a.match->len : 0, a.count, collect_visit, &x, &next);
    if (st == MR_OK) {
        st = x.st;
    }
    char cur[24];
    int n = snprintf(cur, sizeof(cur), "%" PRIu64, next);
    if (st == MR_OK) {
        st = mr_resp_array(c->out, 2);
    }
    if (st == MR_OK) {
        st = mr_resp_bulk(c->out, cur, (size_t)n);
    }
    if (st == MR_OK) {
        st = emit_collected(c, &x);
    }
    mr_buf_free(&x.items);
    return st;
}

static mr_status cmd_dbsize(cmd_call *c) {
    return mr_resp_int(c->out, (int64_t)mr_store_size(c->env->store));
}

/* ---------- FLUSHALL / INVALIDATE / BGREWRITEAOF ---------- */

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

/*
 * INVALIDATE <glob>           glob 에 맞는 키 전부 삭제
 * INVALIDATE PREFIX <prefix>  접두사로 시작하는 키 전부 삭제(glob 메타문자 이스케이프 불필요)
 * 응답: 지운 키 수. 하나도 안 지웠으면 AOF 에 기록하지 않는다(재생 결과가 같으므로).
 */
static mr_status cmd_invalidate(cmd_call *c) {
    bool prefix;
    const mr_slice *arg;
    if (c->argc == 2) {
        prefix = false;
        arg = &c->argv[1];
    } else if (c->argc == 3 && slice_eq_ci(&c->argv[1], "PREFIX")) {
        prefix = true;
        arg = &c->argv[2];
    } else {
        return reply_syntax(c);
    }
    size_t deleted = 0;
    mr_status st = prefix
                       ? mr_store_invalidate_prefix(c->env->store, arg->ptr, arg->len, &deleted)
                       : mr_store_invalidate(c->env->store, arg->ptr, arg->len, &deleted);
    if (st != MR_OK) {
        return reply_store_error(c, st);
    }
    if (deleted > 0 && aof_enabled(c)) {
        st = prefix ? mr_aof_log_invalidate_prefix(c->env->aof, arg->ptr, arg->len)
                    : mr_aof_log_invalidate(c->env->aof, arg->ptr, arg->len);
        if (aof_failed(c, st)) {
            return reply_aof_error(c);
        }
    }
    return mr_resp_int(c->out, (int64_t)deleted);
}

/*
 * 이름은 Redis 호환을 위해 BGREWRITEAOF 지만 백그라운드가 아니다: fork/스레드를 쓰지
 * 않는 싱글 스레드 설계라 압축을 그 자리에서 동기로 수행하며, 끝날 때까지 서버 전체가
 * 멈춘다(INFO 의 aof_rewrite_mode:synchronous). 키 수에 비례하는 시간이 든다.
 */
static mr_status cmd_bgrewriteaof(cmd_call *c) {
    if (!aof_enabled(c)) {
        return mr_resp_error(c->out, "ERR AOF is not enabled (start the server with --aof PATH)");
    }
    int64_t t0 = mr_clock_mono_ms(c->env->clock);
    mr_status st = mr_aof_rewrite(c->env->aof, c->env->store);
    c->env->aof_last_rewrite_ms = mr_clock_mono_ms(c->env->clock) - t0;
    c->env->aof_last_rewrite_ok = st == MR_OK;
    if (st != MR_OK) {
        return mr_resp_errorf(c->out, "ERR AOF rewrite failed: %s", mr_status_str(st));
    }
    /* 새 파일이 현재 메모리 전체를 담으므로 메모리-파일 불일치가 해소되었다. */
    c->env->aof_write_failed = false;
    c->env->aof_rewrites++;
    return mr_resp_simple(c->out, "Append only file rewritten synchronously");
}

/* ---------- INFO ---------- */

static mr_status appendf(mr_buf *b, const char *fmt, ...) __attribute__((format(printf, 2, 3)));

static mr_status appendf(mr_buf *b, const char *fmt, ...) {
    char tmp[256];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(tmp, sizeof(tmp), fmt, ap);
    va_end(ap);
    if (n < 0) {
        return MR_INVALID_ARG;
    }
    size_t len = (size_t)n < sizeof(tmp) ? (size_t)n : sizeof(tmp) - 1;
    return mr_buf_append(b, tmp, len);
}

static mr_status info_server(const cmd_call *c, mr_buf *b) {
    const mr_cmd_server_info *in = c->env->info;
    int64_t up = 0;
    if (in != NULL) {
        up = (mr_clock_mono_ms(c->env->clock) - in->start_mono_ms) / 1000;
    }
    return appendf(b,
                   "# Server\r\nmini_redis_version:%s\r\nredis_mode:standalone\r\n"
                   "multiplexing_api:poll\r\nthreads:1\r\nprocess_id:%ld\r\ntcp_port:%d\r\n"
                   "uptime_in_seconds:%" PRId64 "\r\nuptime_in_days:%" PRId64 "\r\n",
                   MR_SERVER_VERSION, (long)getpid(), in != NULL ? in->port : 0, up,
                   up / 86400);
}

static mr_status info_clients(const cmd_call *c, mr_buf *b) {
    const mr_cmd_server_info *in = c->env->info;
    return appendf(b, "# Clients\r\nconnected_clients:%" PRIu64 "\r\nmaxclients:%" PRIu64 "\r\n",
                   in != NULL ? in->connected_clients : 0, in != NULL ? in->maxclients : 0);
}

static mr_status info_memory(const cmd_call *c, mr_buf *b) {
    mr_store_stats s;
    mr_store_stats_get(c->env->store, &s);
    return appendf(b, "# Memory\r\nused_memory_dataset:%zu\r\n", s.memory_bytes);
}

static mr_status info_persistence(const cmd_call *c, mr_buf *b) {
    const mr_cmd_env *e = c->env;
    const char *fs = e->info != NULL && e->info->appendfsync != NULL ? e->info->appendfsync : "-";
    mr_status st = appendf(b,
                           "# Persistence\r\nloading:0\r\naof_enabled:%d\r\naof_appendfsync:%s\r\n"
                           "aof_current_size:%" PRIu64 "\r\naof_last_write_status:%s\r\n",
                           e->aof != NULL ? 1 : 0, fs, e->aof != NULL ? mr_aof_size(e->aof) : 0,
                           e->aof_write_failed ? "err" : "ok");
    if (st == MR_OK) {
        st = appendf(b,
                     "aof_rewrites:%" PRIu64 "\r\naof_last_bgrewrite_status:%s\r\n"
                     "aof_last_rewrite_time_ms:%" PRId64 "\r\naof_rewrite_mode:synchronous\r\n",
                     e->aof_rewrites, e->aof_last_rewrite_ok ? "ok" : "err",
                     e->aof_last_rewrite_ms);
    }
    return st;
}

static mr_status info_stats(const cmd_call *c, mr_buf *b) {
    const mr_cmd_server_info *in = c->env->info;
    mr_cmd_server_info zero = {0};
    if (in == NULL) {
        in = &zero;
    }
    mr_store_stats s;
    mr_store_stats_get(c->env->store, &s);
    mr_status st = appendf(b,
                           "# Stats\r\ntotal_connections_received:%" PRIu64
                           "\r\ntotal_commands_processed:%" PRIu64
                           "\r\nrejected_connections:%" PRIu64 "\r\n",
                           in->total_connections, in->total_commands, in->rejected_connections);
    if (st == MR_OK) {
        st = appendf(b,
                     "total_net_input_bytes:%" PRIu64 "\r\ntotal_net_output_bytes:%" PRIu64
                     "\r\n",
                     in->total_net_input_bytes, in->total_net_output_bytes);
    }
    if (st == MR_OK) {
        st = appendf(b,
                     "keyspace_hits:%" PRIu64 "\r\nkeyspace_misses:%" PRIu64
                     "\r\nexpired_keys:%" PRIu64 "\r\nexpired_keys_lazy:%" PRIu64
                     "\r\nexpired_keys_active:%" PRIu64 "\r\n",
                     s.hits, s.misses, s.expired_lazy + s.expired_active, s.expired_lazy,
                     s.expired_active);
    }
    return st;
}

static mr_status info_keyspace(const cmd_call *c, mr_buf *b) {
    mr_store_stats s;
    mr_store_stats_get(c->env->store, &s);
    mr_status st = appendf(b, "# Keyspace\r\n");
    if (st == MR_OK && s.keys > 0) {
        st = appendf(b, "db0:keys=%zu,expires=%zu,avg_ttl=0\r\n", s.keys, s.volatile_keys);
    }
    return st;
}

typedef mr_status (*info_fn)(const cmd_call *c, mr_buf *b);

typedef struct info_section {
    const char *name;
    info_fn fn;
} info_section;

static const info_section INFO_SECTIONS[] = {
    {"server", info_server},           {"clients", info_clients}, {"memory", info_memory},
    {"persistence", info_persistence}, {"stats", info_stats},     {"keyspace", info_keyspace},
};

#define INFO_SECTION_COUNT (sizeof(INFO_SECTIONS) / sizeof(INFO_SECTIONS[0]))

static bool info_wanted(const cmd_call *c, const char *name) {
    if (c->argc == 1) {
        return true;
    }
    for (size_t i = 1; i < c->argc; i++) {
        const mr_slice *a = &c->argv[i];
        if (slice_eq_ci(a, name) || slice_eq_ci(a, "all") || slice_eq_ci(a, "default") ||
            slice_eq_ci(a, "everything")) {
            return true;
        }
    }
    return false;
}

static mr_status cmd_info(cmd_call *c) {
    mr_buf b = {NULL, 0, 0};
    mr_status st = MR_OK;
    bool first = true;
    for (size_t i = 0; i < INFO_SECTION_COUNT && st == MR_OK; i++) {
        if (!info_wanted(c, INFO_SECTIONS[i].name)) {
            continue;
        }
        if (!first) {
            st = mr_buf_append(&b, "\r\n", 2);
        }
        first = false;
        if (st == MR_OK) {
            st = INFO_SECTIONS[i].fn(c, &b);
        }
    }
    if (st == MR_OK) {
        st = mr_resp_bulk(c->out, b.data, b.len);
    }
    mr_buf_free(&b);
    return st;
}

/* ---------- COMMAND / CONFIG ---------- */

/* 테이블은 핸들러 뒤에 정의되므로 접근 함수를 먼저 선언한다. */
static size_t table_count(void);
static const char *table_name(size_t i);

static mr_status cmd_command(cmd_call *c) {
    /* redis-cli 는 접속 시 COMMAND DOCS 를 보낸다: 빈 배열이면 힌트 없이 동작한다. */
    if (c->argc == 1) {
        return mr_resp_array(c->out, 0);
    }
    const mr_slice *sub = &c->argv[1];
    if (slice_eq_ci(sub, "COUNT")) {
        return mr_resp_int(c->out, (int64_t)table_count());
    }
    if (slice_eq_ci(sub, "LIST")) {
        mr_status st = mr_resp_array(c->out, table_count());
        for (size_t i = 0; i < table_count() && st == MR_OK; i++) {
            st = mr_resp_bulk_cstr(c->out, table_name(i));
        }
        return st;
    }
    if (slice_eq_ci(sub, "DOCS") || slice_eq_ci(sub, "INFO")) {
        return mr_resp_array(c->out, 0);
    }
    char msg[256];
    size_t len = 0;
    text_append(msg, sizeof(msg), &len, "ERR unknown subcommand '");
    preview_append(msg, sizeof(msg), &len, sub, ERR_ARG_PREVIEW);
    text_append(msg, sizeof(msg), &len, "'. Try COMMAND HELP.");
    return mr_resp_error(c->out, msg);
}

/* CONFIG GET 이 노출하는 읽기 전용 설정. 값은 서버 정보에서 만든다. */
static void config_value(const cmd_call *c, size_t idx, char *out, size_t cap) {
    const mr_cmd_server_info *in = c->env->info;
    switch (idx) {
    case 0:
        snprintf(out, cap, "%s", in != NULL && in->bind_addr != NULL ? in->bind_addr : "");
        break;
    case 1:
        snprintf(out, cap, "%d", in != NULL ? in->port : 0);
        break;
    case 2:
        snprintf(out, cap, "%" PRIu64, in != NULL ? in->maxclients : 0);
        break;
    case 3:
        snprintf(out, cap, "%" PRId64, in != NULL ? in->idle_timeout_sec : 0);
        break;
    case 4:
        snprintf(out, cap, "%s", c->env->aof != NULL ? "yes" : "no");
        break;
    case 5:
        snprintf(out, cap, "%s", in != NULL && in->appendfsync != NULL ? in->appendfsync : "no");
        break;
    case 6: /* RDB 스냅샷은 없다: redis-benchmark 등이 묻는 값 */
        out[0] = '\0';
        break;
    default:
        snprintf(out, cap, "1"); /* databases */
        break;
    }
}

static const char *const CONFIG_NAMES[] = {"bind",       "port",        "maxclients", "timeout",
                                           "appendonly", "appendfsync", "save",       "databases"};
#define CONFIG_NAME_COUNT (sizeof(CONFIG_NAMES) / sizeof(CONFIG_NAMES[0]))

/* 패턴과 이름 모두 소문자로 비교한다(CONFIG GET 은 대소문자 무시). */
static bool config_match(const mr_slice *pat, const char *name) {
    char low[ERR_ARG_PREVIEW];
    if (pat->len > sizeof(low)) {
        return false; /* 이렇게 긴 패턴에 맞는 설정 이름은 없다 */
    }
    for (size_t i = 0; i < pat->len; i++) {
        unsigned char ch = pat->ptr[i];
        low[i] = (char)(ch >= 'A' && ch <= 'Z' ? ch - 'A' + 'a' : ch);
    }
    return mr_glob_match(low, pat->len, name, strlen(name));
}

static mr_status cmd_config_get(cmd_call *c) {
    bool hit[CONFIG_NAME_COUNT] = {false};
    size_t n = 0;
    for (size_t i = 2; i < c->argc; i++) {
        for (size_t j = 0; j < CONFIG_NAME_COUNT; j++) {
            if (!hit[j] && config_match(&c->argv[i], CONFIG_NAMES[j])) {
                hit[j] = true;
                n++;
            }
        }
    }
    mr_status st = mr_resp_array(c->out, n * 2);
    for (size_t j = 0; j < CONFIG_NAME_COUNT && st == MR_OK; j++) {
        if (!hit[j]) {
            continue;
        }
        char val[128];
        config_value(c, j, val, sizeof(val));
        st = mr_resp_bulk_cstr(c->out, CONFIG_NAMES[j]);
        if (st == MR_OK) {
            st = mr_resp_bulk_cstr(c->out, val);
        }
    }
    return st;
}

static mr_status cmd_config(cmd_call *c) {
    const mr_slice *sub = &c->argv[1];
    if (slice_eq_ci(sub, "GET")) {
        if (c->argc < 3) {
            return mr_resp_error(c->out, "ERR wrong number of arguments for 'config|get' command");
        }
        return cmd_config_get(c);
    }
    if (slice_eq_ci(sub, "SET") || slice_eq_ci(sub, "RESETSTAT") || slice_eq_ci(sub, "REWRITE")) {
        return mr_resp_error(c->out, "ERR CONFIG SET/RESETSTAT/REWRITE are not supported");
    }
    char msg[256];
    size_t len = 0;
    text_append(msg, sizeof(msg), &len, "ERR unknown subcommand '");
    preview_append(msg, sizeof(msg), &len, sub, ERR_ARG_PREVIEW);
    text_append(msg, sizeof(msg), &len, "'. Try CONFIG HELP.");
    return mr_resp_error(c->out, msg);
}

/* ---------- 명령 테이블 ---------- */

static const cmd_def COMMANDS[] = {
    {"ping", -1, cmd_ping, 0},
    {"echo", 2, cmd_echo, 0},
    {"quit", -1, cmd_quit, 0},
    {"select", 2, cmd_select, 0},
    {"set", -3, cmd_set, CMD_WRITE},
    {"get", 2, cmd_get, 0},
    {"del", -2, cmd_del, CMD_WRITE},
    {"exists", -2, cmd_exists, 0},
    {"expire", 3, cmd_expire, CMD_WRITE},
    {"pexpire", 3, cmd_pexpire, CMD_WRITE},
    {"expireat", 3, cmd_expireat, CMD_WRITE},
    {"pexpireat", 3, cmd_pexpireat, CMD_WRITE},
    {"ttl", 2, cmd_ttl, 0},
    {"pttl", 2, cmd_pttl, 0},
    {"persist", 2, cmd_persist, CMD_WRITE},
    {"incr", 2, cmd_incr, CMD_WRITE},
    {"decr", 2, cmd_decr, CMD_WRITE},
    {"incrby", 3, cmd_incrby, CMD_WRITE},
    {"decrby", 3, cmd_decrby, CMD_WRITE},
    {"keys", 2, cmd_keys, 0},
    {"scan", -2, cmd_scan, 0},
    {"dbsize", 1, cmd_dbsize, 0},
    {"flushall", -1, cmd_flushall, CMD_WRITE},
    {"invalidate", -2, cmd_invalidate, CMD_WRITE},
    {"bgrewriteaof", 1, cmd_bgrewriteaof, 0},
    {"info", -1, cmd_info, 0},
    {"command", -1, cmd_command, 0},
    {"config", -2, cmd_config, 0},
};

#define COMMAND_COUNT (sizeof(COMMANDS) / sizeof(COMMANDS[0]))

static size_t table_count(void) { return COMMAND_COUNT; }
static const char *table_name(size_t i) { return COMMANDS[i].name; }

/* 명령이 30개 남짓이라 선형 탐색으로 충분하다(길이 비교로 대부분 즉시 탈락). */
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

mr_status mr_cmd_exec(mr_cmd_env *env, size_t argc, const mr_slice *argv, mr_buf *out,
                      unsigned *flags) {
    if (flags != NULL) {
        *flags = 0;
    }
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
                                  "until BGREWRITEAOF succeeds");
    }
    if (env->info != NULL) {
        env->info->total_commands++;
    }
    cmd_call c = {env, argc, argv, out, d->name, 0, MR_OK};
    mr_status st = d->fn(&c);
    if (flags != NULL) {
        *flags = c.result;
    }
    return st;
}
