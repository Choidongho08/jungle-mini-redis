#ifndef MINI_REDIS_RESP_H
#define MINI_REDIS_RESP_H

#include <stddef.h>

#include "server.h"

/* RESP 프로토콜 한도 (Redis 기본값과 같음, design §6.2, §7) */
#define PROTO_MAX_BULK_LEN ((long long)512 * 1024 * 1024) /* proto-max-bulk-len */
#define PROTO_MAX_MULTIBULK (1024 * 1024)                 /* 한 요청의 최대 인자 수 */
#define PROTO_INLINE_MAX_SIZE (64 * 1024)                 /* "\r\n" 없는 한 줄의 최대 길이 */

/* ---- 응답 직렬화 (RESP2) ---- */

void reply_simple(client_t *c, const char *s);             /* +OK\r\n */
void reply_error(client_t *c, const char *msg);            /* -ERR <msg>\r\n */
void reply_error_raw(client_t *c, const char *s, size_t n);/* -<s>\r\n (접두어 직접 지정) */
void reply_integer(client_t *c, long long v);              /* :1\r\n */
void reply_bulk(client_t *c, const char *p, size_t len);   /* $5\r\nhello\r\n */
void reply_null(client_t *c);                              /* $-1\r\n */

/* client_process_input() 은 server.h 에 선언되어 있고 resp.c 에서 구현한다. */

#endif
