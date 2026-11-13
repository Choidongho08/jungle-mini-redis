/* 키 공간: key → value 해시 테이블 (design §5.4) */
#include "db.h"

#include <stdlib.h>
#include <string.h>

#include "dict.h"
#include "util.h"

static struct {
    dict_t *main; /* key → value_t* */
} db;

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
}

void db_free(void) {
    dict_free(db.main);
    db.main = NULL;
}

value_t *db_lookup(const char *key, size_t klen) {
    return dict_find(db.main, key, klen);
}

void db_set(const char *key, size_t klen, const char *val, size_t vlen, int keep_ttl) {
    (void)keep_ttl; /* 만료 정보는 T8 에서 추가 */
    dict_set(db.main, key, klen, value_new(val, vlen));
}

int db_delete(const char *key, size_t klen) {
    return dict_delete(db.main, key, klen);
}

void db_flush(void) {
    dict_clear(db.main);
}

size_t db_size(void) {
    return dict_size(db.main);
}
