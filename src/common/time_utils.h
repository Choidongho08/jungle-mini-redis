#ifndef TIME_UTILS_H
#define TIME_UTILS_H

#include <stdint.h>

/* CLOCK_MONOTONIC 기준 밀리초. TTL과 유휴 시간에 같은 기준을 사용한다. */
int64_t now_ms(void);

#endif
