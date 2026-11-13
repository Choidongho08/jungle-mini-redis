#include "miniredis/clock.h"

#include <time.h>

static int64_t timespec_ms(clockid_t id) {
    struct timespec ts;
    /* 지원되는 clockid 에서 clock_gettime 은 실패하지 않는다. */
    if (clock_gettime(id, &ts) != 0) {
        return 0;
    }
    return (int64_t)ts.tv_sec * 1000 + (int64_t)(ts.tv_nsec / 1000000);
}

static int64_t system_wall_ms(void *ctx) {
    (void)ctx;
    return timespec_ms(CLOCK_REALTIME);
}

static int64_t system_mono_ms(void *ctx) {
    (void)ctx;
    return timespec_ms(CLOCK_MONOTONIC);
}

static const mr_clock system_clock = {system_wall_ms, system_mono_ms, NULL};

const mr_clock *mr_clock_system(void) { return &system_clock; }

int64_t mr_clock_wall_ms(const mr_clock *clk) { return clk->wall_ms(clk->ctx); }

int64_t mr_clock_mono_ms(const mr_clock *clk) { return clk->mono_ms(clk->ctx); }
