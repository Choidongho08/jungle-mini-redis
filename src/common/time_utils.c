#include "common/time_utils.h"

#include <stdio.h>
#include <stdlib.h>
#include <time.h>

/* 시스템 날짜 변경의 영향을 받지 않는 단조 시계를 TTL과 유휴 시간에 사용한다. */
int64_t now_ms(void)
{
    struct timespec time;
    if (clock_gettime(CLOCK_MONOTONIC, &time) < 0) {
        perror("clock_gettime");
        exit(EXIT_FAILURE);
    }
    return (int64_t)time.tv_sec * 1000 + time.tv_nsec / 1000000;
}
