#ifndef MINIREDIS_CLOCK_H
#define MINIREDIS_CLOCK_H

#include <stdint.h>

/*
 * 시간 소스 추상화.
 *
 * 두 종류의 시계를 구분한다.
 *  - wall_ms : 유닉스 epoch 기준 밀리초(CLOCK_REALTIME). 키의 만료 시각은
 *              프로세스 재시작 후에도 의미가 있어야 하므로(AOF에 절대 시각으로
 *              기록) 반드시 wall clock 이어야 한다. 대가: 시스템 시계가 뒤로
 *              가면 키가 더 오래 살고, 앞으로 가면 일찍 만료된다(Redis 동일).
 *  - mono_ms : 단조 증가 밀리초(CLOCK_MONOTONIC). 재부팅 간 의미가 없으므로
 *              저장하지 않고, sweep 시간 예산·fsync 주기처럼 "경과 시간"
 *              측정에만 쓴다. 시계 조정에 영향받지 않는다.
 *
 * 테스트는 ctx 로 가짜 시계를 주입해 시간을 결정적으로 제어한다.
 */
typedef struct mr_clock {
    int64_t (*wall_ms)(void *ctx);
    int64_t (*mono_ms)(void *ctx);
    void *ctx;
} mr_clock;

/* 시스템 시계(CLOCK_REALTIME/CLOCK_MONOTONIC). 정적 수명, 해제 금지. */
const mr_clock *mr_clock_system(void);

/* clk 의 wall/mono 시각(ms). clk 는 NULL 이 아니어야 한다. O(1). */
int64_t mr_clock_wall_ms(const mr_clock *clk);
int64_t mr_clock_mono_ms(const mr_clock *clk);

#endif /* MINIREDIS_CLOCK_H */
