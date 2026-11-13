#include "miniredis/store.h"

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "miniredis/glob.h"
#include "miniredis/ht.h"

/* sweep 이 시간 예산을 확인하는 간격(스캔 걸음 수). 매 걸음 시계를 읽는 비용을 줄인다. */
#define SWEEP_CLOCK_CHECK_EVERY 16
/* int64 10진 표현 최대 길이("-9223372036854775808") */
#define INT64_MAX_DIGITS 20

typedef struct store_entry {
    int64_t expire_at; /* epoch ms, 0 = 무기한 */
    size_t vlen;
    unsigned char val[];
} store_entry;

struct mr_store {
    mr_ht *ht;
    const mr_clock *clock;
    mr_store_stats stats; /* keys/volatile_keys/memory_bytes 는 조회 시 채운다 */
    size_t volatile_keys;
    size_t value_bytes;
    uint64_t sweep_cursor;
    bool loading;
};

/* ---------- 엔트리 수명·회계 ---------- */

/* 해시 테이블의 값 소멸자. 덮어쓰기·삭제·clear 어느 경로든 회계가 한 곳에서 맞는다. */
static void entry_free(void *val, void *ud) {
    mr_store *s = ud;
    store_entry *e = val;
    if (e->expire_at != 0) {
        s->volatile_keys--;
    }
    s->value_bytes -= sizeof(*e) + e->vlen;
    free(e);
}

static mr_status entry_new(const void *val, size_t vlen, int64_t expire_at, store_entry **out) {
    if (vlen > SIZE_MAX - sizeof(store_entry)) {
        return MR_OVERFLOW;
    }
    store_entry *e = malloc(sizeof(*e) + vlen);
    if (e == NULL) {
        return MR_NOMEM;
    }
    e->expire_at = expire_at;
    e->vlen = vlen;
    if (vlen > 0) {
        memcpy(e->val, val, vlen);
    }
    *out = e;
    return MR_OK;
}

/* 엔트리를 테이블에 넣는다. 실패하면 엔트리를 해제하고 저장소는 그대로다. */
static mr_status entry_install(mr_store *s, const void *key, size_t klen, store_entry *e) {
    mr_status st = mr_ht_put(s->ht, key, klen, e, NULL);
    if (st != MR_OK) {
        free(e); /* 아직 회계에 반영 전이므로 entry_free 가 아닌 free */
        return st;
    }
    if (e->expire_at != 0) {
        s->volatile_keys++;
    }
    s->value_bytes += sizeof(*e) + e->vlen;
    return MR_OK;
}

static void entry_set_expire(mr_store *s, store_entry *e, int64_t at) {
    if (e->expire_at == 0 && at != 0) {
        s->volatile_keys++;
    } else if (e->expire_at != 0 && at == 0) {
        s->volatile_keys--;
    }
    e->expire_at = at;
}

/* ---------- 만료 ---------- */

static int64_t now_ms(const mr_store *s) { return mr_clock_wall_ms(s->clock); }

static bool is_expired(const mr_store *s, const store_entry *e, int64_t now) {
    return !s->loading && e->expire_at != 0 && now >= e->expire_at;
}

static bool key_ok(const void *key, size_t klen) { return key != NULL || klen == 0; }

/* 살아 있는 엔트리를 찾는다. 만료되었으면 그 자리에서 지운다(lazy 만료). */
static store_entry *lookup(mr_store *s, const void *key, size_t klen, int64_t now) {
    void *v;
    if (mr_ht_get(s->ht, key, klen, &v) != MR_OK) {
        return NULL;
    }
    store_entry *e = v;
    if (is_expired(s, e, now)) {
        (void)mr_ht_delete(s->ht, key, klen);
        s->stats.expired_lazy++;
        return NULL;
    }
    return e;
}

/* ---------- 생성/해제 ---------- */

mr_status mr_store_create(const mr_store_opts *opts, mr_store **out) {
    if (out == NULL) {
        return MR_INVALID_ARG;
    }
    *out = NULL;
    mr_store *s = calloc(1, sizeof(*s));
    if (s == NULL) {
        return MR_NOMEM;
    }
    mr_ht_opts ho = {0};
    ho.val_free = entry_free;
    ho.val_free_ud = s;
    if (opts != NULL) {
        memcpy(ho.seed, opts->hash_seed, sizeof(ho.seed));
    }
    mr_status st = mr_ht_create(&ho, &s->ht);
    if (st != MR_OK) {
        free(s);
        return st;
    }
    s->clock = (opts != NULL && opts->clock != NULL) ? opts->clock : mr_clock_system();
    *out = s;
    return MR_OK;
}

void mr_store_destroy(mr_store *s) {
    if (s == NULL) {
        return;
    }
    mr_ht_destroy(s->ht);
    free(s);
}

/* ---------- SET / GET / DEL ---------- */

static mr_status resolve_expire(const mr_set_opts *o, int64_t now, int64_t *at) {
    switch (o->expire_kind) {
    case MR_EXPIRE_NONE:
        *at = 0;
        return MR_OK;
    case MR_EXPIRE_PX:
        if (o->expire <= 0) {
            return MR_INVALID_ARG;
        }
        if (now > INT64_MAX - o->expire) {
            return MR_OVERFLOW;
        }
        *at = now + o->expire;
        return MR_OK;
    case MR_EXPIRE_PXAT:
        if (o->expire <= 0) {
            return MR_INVALID_ARG;
        }
        *at = o->expire;
        return MR_OK;
    }
    return MR_INVALID_ARG;
}

mr_status mr_store_set(mr_store *s, const void *key, size_t klen, const void *val, size_t vlen,
                       const mr_set_opts *opts) {
    static const mr_set_opts plain = {MR_SET_ALWAYS, MR_EXPIRE_NONE, 0};
    if (s == NULL || !key_ok(key, klen) || (val == NULL && vlen > 0)) {
        return MR_INVALID_ARG;
    }
    if (opts == NULL) {
        opts = &plain;
    }
    if (opts->cond != MR_SET_ALWAYS && opts->cond != MR_SET_NX && opts->cond != MR_SET_XX) {
        return MR_INVALID_ARG;
    }
    int64_t now = now_ms(s);
    int64_t at;
    mr_status st = resolve_expire(opts, now, &at);
    if (st != MR_OK) {
        return st;
    }

    store_entry *cur = lookup(s, key, klen, now);
    if ((opts->cond == MR_SET_NX && cur != NULL) || (opts->cond == MR_SET_XX && cur == NULL)) {
        return MR_COND_FAILED;
    }
    if (!s->loading && at != 0 && at <= now) {
        /* 이미 지난 절대 시각: 써 봤자 즉시 만료이므로 기존 키만 지운다. */
        if (cur != NULL) {
            (void)mr_ht_delete(s->ht, key, klen);
        }
        s->stats.sets++;
        return MR_OK;
    }
    store_entry *e;
    st = entry_new(val, vlen, at, &e);
    if (st == MR_OK) {
        st = entry_install(s, key, klen, e);
    }
    if (st == MR_OK) {
        s->stats.sets++;
    }
    return st;
}

mr_status mr_store_get(mr_store *s, const void *key, size_t klen, const void **val,
                       size_t *vlen) {
    if (s == NULL || !key_ok(key, klen)) {
        return MR_INVALID_ARG;
    }
    store_entry *e = lookup(s, key, klen, now_ms(s));
    if (e == NULL) {
        s->stats.misses++;
        return MR_NOT_FOUND;
    }
    s->stats.hits++;
    if (val != NULL) {
        *val = e->val;
    }
    if (vlen != NULL) {
        *vlen = e->vlen;
    }
    return MR_OK;
}

mr_status mr_store_del(mr_store *s, const void *key, size_t klen) {
    if (s == NULL || !key_ok(key, klen)) {
        return MR_INVALID_ARG;
    }
    if (lookup(s, key, klen, now_ms(s)) == NULL) {
        return MR_NOT_FOUND;
    }
    (void)mr_ht_delete(s->ht, key, klen);
    s->stats.deletes++;
    return MR_OK;
}

bool mr_store_exists(mr_store *s, const void *key, size_t klen) {
    if (s == NULL || !key_ok(key, klen)) {
        return false;
    }
    return lookup(s, key, klen, now_ms(s)) != NULL;
}

/* ---------- TTL ---------- */

/* at 이 now 이하이면 삭제, 아니면 TTL 설정. at 은 0 이 아니어야 한다. */
static mr_status apply_expire_at(mr_store *s, const void *key, size_t klen, int64_t at,
                                 int64_t now) {
    store_entry *e = lookup(s, key, klen, now);
    if (e == NULL) {
        return MR_NOT_FOUND;
    }
    if (!s->loading && at <= now) {
        (void)mr_ht_delete(s->ht, key, klen);
        s->stats.deletes++;
        return MR_OK;
    }
    entry_set_expire(s, e, at);
    return MR_OK;
}

mr_status mr_store_pexpireat(mr_store *s, const void *key, size_t klen, int64_t at_ms) {
    if (s == NULL || !key_ok(key, klen) || at_ms <= 0) {
        return MR_INVALID_ARG;
    }
    return apply_expire_at(s, key, klen, at_ms, now_ms(s));
}

mr_status mr_store_pexpire(mr_store *s, const void *key, size_t klen, int64_t ms) {
    if (s == NULL || !key_ok(key, klen)) {
        return MR_INVALID_ARG;
    }
    int64_t now = now_ms(s);
    if (ms <= 0) {
        /* 0 이하 TTL 은 "이미 만료" — 현재 시각을 넘겨 즉시 삭제 경로를 탄다. */
        return apply_expire_at(s, key, klen, now > 0 ? now : 1, now);
    }
    if (now > INT64_MAX - ms) {
        return MR_OVERFLOW;
    }
    return apply_expire_at(s, key, klen, now + ms, now);
}

mr_status mr_store_expire(mr_store *s, const void *key, size_t klen, int64_t seconds) {
    if (seconds > INT64_MAX / 1000 || seconds < INT64_MIN / 1000) {
        return MR_OVERFLOW;
    }
    return mr_store_pexpire(s, key, klen, seconds * 1000);
}

mr_status mr_store_pttl(mr_store *s, const void *key, size_t klen, int64_t *out_ms) {
    if (s == NULL || !key_ok(key, klen) || out_ms == NULL) {
        return MR_INVALID_ARG;
    }
    int64_t now = now_ms(s);
    store_entry *e = lookup(s, key, klen, now);
    if (e == NULL) {
        return MR_NOT_FOUND;
    }
    *out_ms = e->expire_at == 0 ? -1 : e->expire_at - now;
    return MR_OK;
}

mr_status mr_store_ttl(mr_store *s, const void *key, size_t klen, int64_t *out_sec) {
    int64_t ms;
    mr_status st = mr_store_pttl(s, key, klen, out_sec != NULL ? &ms : NULL);
    if (st == MR_OK) {
        /* Redis 와 같이 반올림: 1500ms 남으면 2초 */
        *out_sec = ms < 0 ? -1 : (ms + 500) / 1000;
    }
    return st;
}

mr_status mr_store_persist(mr_store *s, const void *key, size_t klen) {
    if (s == NULL || !key_ok(key, klen)) {
        return MR_INVALID_ARG;
    }
    store_entry *e = lookup(s, key, klen, now_ms(s));
    if (e == NULL) {
        return MR_NOT_FOUND;
    }
    if (e->expire_at == 0) {
        return MR_COND_FAILED;
    }
    entry_set_expire(s, e, 0);
    return MR_OK;
}

/* ---------- INCR ---------- */

/* Redis string2ll 과 같은 규칙의 엄격한 파서. 성공 시 true. */
static bool parse_i64(const unsigned char *p, size_t len, int64_t *out) {
    if (len == 0 || len > INT64_MAX_DIGITS) {
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
    /* 선행 0 과 "-0" 을 막아 한 값에 표현이 하나만 있게 한다. */
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
        /* -(INT64_MIN) 은 표현 불가하므로 1 을 빼고 부호를 바꾼 뒤 다시 1 을 뺀다. */
        *out = v == 0 ? 0 : -(int64_t)(v - 1) - 1;
    } else {
        if (v > (uint64_t)INT64_MAX) {
            return false;
        }
        *out = (int64_t)v;
    }
    return true;
}

mr_status mr_store_incrby(mr_store *s, const void *key, size_t klen, int64_t delta, int64_t *out) {
    if (s == NULL || !key_ok(key, klen)) {
        return MR_INVALID_ARG;
    }
    store_entry *e = lookup(s, key, klen, now_ms(s));
    int64_t cur = 0;
    int64_t at = 0;
    if (e != NULL) {
        if (!parse_i64(e->val, e->vlen, &cur)) {
            return MR_NOT_INTEGER;
        }
        at = e->expire_at;
    }
    if ((delta > 0 && cur > INT64_MAX - delta) || (delta < 0 && cur < INT64_MIN - delta)) {
        return MR_OVERFLOW;
    }
    int64_t nv = cur + delta;
    char buf[INT64_MAX_DIGITS + 2];
    int n = snprintf(buf, sizeof(buf), "%" PRId64, nv);
    if (n < 0 || (size_t)n >= sizeof(buf)) {
        return MR_OVERFLOW;
    }
    store_entry *ne;
    mr_status st = entry_new(buf, (size_t)n, at, &ne);
    if (st == MR_OK) {
        st = entry_install(s, key, klen, ne);
    }
    if (st != MR_OK) {
        return st;
    }
    s->stats.sets++;
    if (out != NULL) {
        *out = nv;
    }
    return MR_OK;
}

mr_status mr_store_incr(mr_store *s, const void *key, size_t klen, int64_t *out) {
    return mr_store_incrby(s, key, klen, 1, out);
}

mr_status mr_store_decr(mr_store *s, const void *key, size_t klen, int64_t *out) {
    return mr_store_incrby(s, key, klen, -1, out);
}

mr_status mr_store_decrby(mr_store *s, const void *key, size_t klen, int64_t delta, int64_t *out) {
    if (delta == INT64_MIN) {
        return MR_OVERFLOW;
    }
    return mr_store_incrby(s, key, klen, -delta, out);
}

/* ---------- 순회: KEYS / SCAN ---------- */

typedef struct visit_ctx {
    const mr_store *s;
    const void *pattern;
    size_t plen;
    mr_store_visit_fn fn;
    void *ud;
    int64_t now;
    size_t visited;
    bool stop;
} visit_ctx;

/* 읽기 전용 순회. 만료 키는 건너뛰기만 한다: 여기서 지우면 테이블 축소가 일어나
 * 같은 키를 두 번 돌려줄 수 있기 때문이다. */
static mr_ht_scan_action visit_cb(const void *key, size_t klen, void *val, void *ud) {
    visit_ctx *c = ud;
    const store_entry *e = val;
    c->visited++;
    if (c->stop || is_expired(c->s, e, c->now)) {
        return MR_HT_SCAN_KEEP;
    }
    if (c->pattern != NULL && !mr_glob_match(c->pattern, c->plen, key, klen)) {
        return MR_HT_SCAN_KEEP;
    }
    if (!c->fn(key, klen, e->val, e->vlen, e->expire_at, c->ud)) {
        c->stop = true;
    }
    return MR_HT_SCAN_KEEP;
}

mr_status mr_store_keys(mr_store *s, const void *pattern, size_t plen, mr_store_visit_fn fn,
                        void *ud) {
    if (s == NULL || fn == NULL || (pattern == NULL && plen > 0)) {
        return MR_INVALID_ARG;
    }
    visit_ctx c = {s, pattern, plen, fn, ud, now_ms(s), 0, false};
    uint64_t cursor = 0;
    do {
        cursor = mr_ht_scan(s->ht, cursor, visit_cb, &c);
    } while (cursor != 0 && !c.stop);
    return MR_OK;
}

mr_status mr_store_scan(mr_store *s, uint64_t cursor, const void *pattern, size_t plen,
                        size_t count, mr_store_visit_fn fn, void *ud, uint64_t *next_cursor) {
    if (s == NULL || fn == NULL || next_cursor == NULL || (pattern == NULL && plen > 0)) {
        return MR_INVALID_ARG;
    }
    if (count == 0) {
        count = 1;
    }
    /* 빈 버킷이 많아도 한 호출이 무한정 길어지지 않도록 걸음 수를 제한한다(Redis 방식). */
    size_t max_steps = count > SIZE_MAX / 10 ? SIZE_MAX : count * 10;
    visit_ctx c = {s, pattern, plen, fn, ud, now_ms(s), 0, false};
    do {
        cursor = mr_ht_scan(s->ht, cursor, visit_cb, &c);
    } while (cursor != 0 && !c.stop && c.visited < count && --max_steps > 0);
    *next_cursor = c.stop ? 0 : cursor;
    return MR_OK;
}

/* ---------- 무효화 / 전체 삭제 ---------- */

typedef struct inval_ctx {
    mr_store *s;
    const unsigned char *pat;
    size_t plen;
    bool prefix;
    int64_t now;
    size_t deleted;
} inval_ctx;

static mr_ht_scan_action inval_cb(const void *key, size_t klen, void *val, void *ud) {
    inval_ctx *c = ud;
    if (is_expired(c->s, val, c->now)) {
        c->s->stats.expired_lazy++;
        return MR_HT_SCAN_DELETE;
    }
    bool match = c->prefix ? (klen >= c->plen && (c->plen == 0 || memcmp(key, c->pat, c->plen) == 0))
                           : mr_glob_match(c->pat, c->plen, key, klen);
    if (!match) {
        return MR_HT_SCAN_KEEP;
    }
    c->deleted++;
    c->s->stats.deletes++;
    return MR_HT_SCAN_DELETE;
}

static mr_status invalidate(mr_store *s, const void *pat, size_t plen, bool prefix,
                            size_t *deleted) {
    if (s == NULL || (pat == NULL && plen > 0)) {
        return MR_INVALID_ARG;
    }
    /* 삭제 중 축소로 일부 키를 두 번 볼 수는 있어도(이미 지워져 무해) 계속 존재한
     * 키를 놓치지는 않는다는 scan 보장에 기대어 한 바퀴만 돈다. */
    inval_ctx c = {s, pat, plen, prefix, now_ms(s), 0};
    uint64_t cursor = 0;
    do {
        cursor = mr_ht_scan(s->ht, cursor, inval_cb, &c);
    } while (cursor != 0);
    if (deleted != NULL) {
        *deleted = c.deleted;
    }
    return MR_OK;
}

mr_status mr_store_invalidate(mr_store *s, const void *pattern, size_t plen, size_t *deleted) {
    return invalidate(s, pattern, plen, false, deleted);
}

mr_status mr_store_invalidate_prefix(mr_store *s, const void *prefix, size_t plen,
                                     size_t *deleted) {
    return invalidate(s, prefix, plen, true, deleted);
}

void mr_store_flushall(mr_store *s) {
    if (s == NULL) {
        return;
    }
    mr_ht_clear(s->ht);
    s->sweep_cursor = 0;
}

/* ---------- 통계 ---------- */

size_t mr_store_size(const mr_store *s) { return mr_ht_size(s->ht); }

void mr_store_stats_get(const mr_store *s, mr_store_stats *out) {
    *out = s->stats;
    out->keys = mr_ht_size(s->ht);
    out->volatile_keys = s->volatile_keys;
    out->memory_bytes = sizeof(*s) + mr_ht_memory_usage(s->ht) + s->value_bytes;
}

/* ---------- 능동 만료 ---------- */

typedef struct sweep_ctx {
    mr_store *s;
    int64_t now;
    size_t samples;
    size_t expired;
} sweep_ctx;

static mr_ht_scan_action sweep_cb(const void *key, size_t klen, void *val, void *ud) {
    (void)key;
    (void)klen;
    sweep_ctx *c = ud;
    c->samples++;
    if (is_expired(c->s, val, c->now)) {
        c->expired++;
        return MR_HT_SCAN_DELETE;
    }
    return MR_HT_SCAN_KEEP;
}

size_t mr_store_sweep(mr_store *s, size_t max_samples, int64_t time_budget_ms) {
    if (s == NULL || s->loading || s->volatile_keys == 0 || max_samples == 0) {
        return 0;
    }
    sweep_ctx c = {s, now_ms(s), 0, 0};
    int64_t start = time_budget_ms > 0 ? mr_clock_mono_ms(s->clock) : 0;
    uint64_t cursor = s->sweep_cursor;
    unsigned steps = 0;
    do {
        cursor = mr_ht_scan(s->ht, cursor, sweep_cb, &c);
        if (time_budget_ms > 0 && ++steps % SWEEP_CLOCK_CHECK_EVERY == 0 &&
            mr_clock_mono_ms(s->clock) - start >= time_budget_ms) {
            break;
        }
        /* 커서가 0 으로 돌아오면 한 바퀴를 끝낸 것: 같은 키를 다시 훑지 않도록 멈춘다. */
    } while (cursor != 0 && c.samples < max_samples && s->volatile_keys > 0);
    s->sweep_cursor = cursor;
    s->stats.expired_active += c.expired;
    return c.expired;
}

size_t mr_store_set_loading(mr_store *s, bool loading) {
    if (s == NULL || s->loading == loading) {
        return 0;
    }
    s->loading = loading;
    if (loading) {
        return 0;
    }
    sweep_ctx c = {s, now_ms(s), 0, 0};
    uint64_t cursor = 0;
    do {
        cursor = mr_ht_scan(s->ht, cursor, sweep_cb, &c);
    } while (cursor != 0);
    s->stats.expired_active += c.expired;
    return c.expired;
}
