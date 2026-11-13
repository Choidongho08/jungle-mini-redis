#include "buffer.h"

#include <stdlib.h>
#include <string.h>

#include "util.h"

/* 이 크기보다 큰 버퍼는 비울 때 메모리를 반납한다.
 * 1MB 값을 한 번 받은 클라이언트가 계속 1MB 를 붙잡고 있지 않게 하기 위함. */
#define BUFFER_KEEP_MAX (64 * 1024)

void buffer_init(buffer_t *b) {
    b->data = NULL;
    b->len = 0;
    b->cap = 0;
}

void buffer_free(buffer_t *b) {
    free(b->data);
    buffer_init(b);
}

void buffer_reserve(buffer_t *b, size_t extra) {
    if (b->cap - b->len >= extra) return;
    size_t need = b->len + extra;
    size_t cap = b->cap ? b->cap : 64;
    while (cap < need) cap *= 2; /* 2배씩 늘려 append 를 평균 O(1) 로 */
    b->data = xrealloc(b->data, cap);
    b->cap = cap;
}

void buffer_append(buffer_t *b, const void *p, size_t n) {
    if (n == 0) return;
    buffer_reserve(b, n);
    memcpy(b->data + b->len, p, n);
    b->len += n;
}

void buffer_append_str(buffer_t *b, const char *s) {
    buffer_append(b, s, strlen(s));
}

void buffer_consume(buffer_t *b, size_t n) {
    if (n >= b->len) {
        buffer_clear(b);
        return;
    }
    memmove(b->data, b->data + n, b->len - n);
    b->len -= n;
}

void buffer_clear(buffer_t *b) {
    b->len = 0;
    if (b->cap > BUFFER_KEEP_MAX) buffer_free(b);
}
