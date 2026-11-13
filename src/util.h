#ifndef MINI_REDIS_UTIL_H
#define MINI_REDIS_UTIL_H

#include <stddef.h>
#include <stdint.h>

/* 할당 실패 시 메시지를 출력하고 종료한다.
 * Redis 도 메모리 부족이면 abort 하므로, 호출하는 쪽에서 NULL 검사를 하지 않는다. */
void *xmalloc(size_t size);
void *xcalloc(size_t n, size_t size);
void *xrealloc(void *ptr, size_t size);

/* 현재 시각(Unix epoch 기준 ms). 만료 시각을 절대값으로 저장하기 위해
 * CLOCK_REALTIME 을 쓴다. (design TD-005) */
int64_t mstime(void);

/* 바이트열 s[0..len) 을 64비트 부호 있는 정수로 엄격하게 변환한다.
 * Redis 의 string2ll 규칙을 따른다:
 *   - 빈 문자열, 공백, '+' 부호, 앞자리 0("0" 자체는 허용), "-0" 은 실패
 *   - int64 범위를 넘으면 실패
 * 성공하면 1 을 반환하고 *out 에 값을 쓴다. 실패하면 0. */
int util_str_to_ll(const char *s, size_t len, long long *out);

#endif
