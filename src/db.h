#ifndef MINI_REDIS_DB_H
#define MINI_REDIS_DB_H

#include <stddef.h>
#include <stdint.h>

/* 문자열 값 (바이너리 안전) */
typedef struct {
    char *ptr;
    size_t len;
} value_t;

/* 키 공간 (design §5.4). Redis 처럼 DB 는 하나만 둔다 (SELECT 범위 밖). */
void db_init(void);
void db_free(void);

/* 키를 찾는다. 없으면 NULL.
 * 모든 명령은 키에 접근할 때 반드시 이 함수를 거친다 → 만료 검사가 한 곳에 모인다 (FR-018). */
value_t *db_lookup(const char *key, size_t klen);

/* 값을 저장한다 (기존 값은 해제). keep_ttl 이 0 이면 기존 만료 시간을 지운다 (FR-011). */
void db_set(const char *key, size_t klen, const char *val, size_t vlen, int keep_ttl);

/* 키의 만료 시각(epoch ms)을 설정한다. 키가 존재하는지는 호출하는 쪽에서 확인한다. */
void db_set_expire(const char *key, size_t klen, int64_t when_ms);

/* 만료 시각(epoch ms). 만료 시간이 없으면 -1 */
int64_t db_get_expire(const char *key, size_t klen);

/* 삭제했으면 1, 없었으면 0 */
int db_delete(const char *key, size_t klen);

/* 모든 키를 지운다 */
void db_flush(void);

size_t db_size(void);

#endif
