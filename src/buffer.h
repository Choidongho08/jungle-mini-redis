#ifndef MINI_REDIS_BUFFER_H
#define MINI_REDIS_BUFFER_H

#include <stddef.h>

/* 길이 기반 동적 바이트 버퍼.
 * NUL 종료에 의존하지 않으므로 값에 '\0', "\r\n" 이 섞여 있어도 안전하다 (NFR-005). */
typedef struct {
    char *data;
    size_t len; /* 사용 중인 바이트 수 */
    size_t cap; /* 할당된 바이트 수 */
} buffer_t;

/* 길이가 있는 바이트열 (RESP 인자 등) */
typedef struct {
    char *ptr;
    size_t len;
} bytes_t;

void buffer_init(buffer_t *b);
void buffer_free(buffer_t *b);

/* 뒤쪽에 최소 extra 바이트의 여유 공간을 확보한다. */
void buffer_reserve(buffer_t *b, size_t extra);

void buffer_append(buffer_t *b, const void *p, size_t n);
void buffer_append_str(buffer_t *b, const char *s);

/* 앞에서 n 바이트를 버리고 나머지를 앞으로 당긴다. */
void buffer_consume(buffer_t *b, size_t n);

/* 내용을 비우되 메모리는 유지한다. 큰 버퍼는 메모리를 돌려준다. */
void buffer_clear(buffer_t *b);

#endif
