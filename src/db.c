/* 키 공간 (design §5.4, TD-004)
 *
 *   db.main    : key → value_t*
 *   db.expires : key → int64_t* (만료 시각, epoch ms)
 *
 * Redis 처럼 만료 정보를 별도 dict 에 둔다. TTL 이 있는 키만 따로 모여 있으므로
 * 나중에 active expiration(주기적 샘플링)을 붙이기 쉽다.
 *
 * 만료는 lazy 방식: 키에 접근할 때(db_lookup) 만료 시각이 지났으면 그때 지운다. */
#include "db.h"

#include <stdlib.h>
#include <string.h>

#include "dict.h"
#include "util.h"

static struct {
    dict_t *main;
    dict_t *expires;
    int64_t now; /* 현재 명령의 기준 시각 (db_begin_command) */
} db;

/* Redis 의 command time snapshot 과 같은 개념.
 * 명령 실행 도중 시간이 흘러도 같은 명령 안에서는 만료 판단이 바뀌지 않게 한다.
 * 예: EXISTS k k 가 첫 번째 k 는 살아 있고 두 번째 k 는 만료된 것으로 세는 일을 막는다. */
void db_begin_command(void) {
    db.now = mstime();
}

int64_t db_now(void) {
    return db.now ? db.now : mstime();
}

static void value_free(void *p) {
    value_t *v = p;
    free(v->ptr);
    free(v);
}

static value_t *value_new(const char *p, size_t len) {
    value_t *v = xmalloc(sizeof(*v));
    v->ptr = xmalloc(len ? len : 1);
    memcpy(v->ptr, p, len);
    v->len = len;
    return v;
}

void db_init(void) {
    db.main = dict_create(value_free);
    db.expires = dict_create(free);
}

void db_free(void) {
    dict_free(db.main);
    dict_free(db.expires);
    db.main = db.expires = NULL;
}

int64_t db_get_expire(const char *key, size_t klen) {
    int64_t *when = dict_find(db.expires, key, klen);
    return when ? *when : -1;
}

value_t *db_lookup(const char *key, size_t klen) {
    int64_t when = db_get_expire(key, klen);
    if (when != -1 && db_now() >= when) {
        /* 만료됨: 지금 지우고 없는 키로 취급한다 (lazy expiration, FR-018) */
        dict_delete(db.main, key, klen);
        dict_delete(db.expires, key, klen);
        return NULL;
    }
    return dict_find(db.main, key, klen);
}

void db_set(const char *key, size_t klen, const char *val, size_t vlen, int keep_ttl) {
    dict_set(db.main, key, klen, value_new(val, vlen));
    /* SET 은 기존 TTL 을 지운다 (FR-011). INCR 은 keep_ttl=1 로 유지한다 (FR-019). */
    if (!keep_ttl) dict_delete(db.expires, key, klen);
}

void db_set_expire(const char *key, size_t klen, int64_t when_ms) {
    int64_t *when = xmalloc(sizeof(*when));
    *when = when_ms;
    dict_set(db.expires, key, klen, when);
}

int db_delete(const char *key, size_t klen) {
    dict_delete(db.expires, key, klen);
    return dict_delete(db.main, key, klen);
}

void db_flush(void) {
    dict_clear(db.main);
    dict_clear(db.expires);
}

size_t db_size(void) {
    return dict_size(db.main);
}
