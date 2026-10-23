#pragma once
#include <stdbool.h>
#include <stdint.h>

typedef struct { int64_t seconds; uint8_t source[16]; } TimePoint;
typedef struct { int64_t offset; TimePoint timestamp; } TimeSystemContext;
typedef struct {
    uint8_t shared[0x1000];
    TimeSystemContext system;
    int64_t anchor_ns;
    bool initialized;
} TimeClock;

// Immutable per-process wall-clock anchor. Steady nanoseconds must use the same
// host monotonic origin as rt_ticks(). All access is serialized by the service.
bool time_clock_initialize(TimeClock *clock, int64_t steady_ns, int64_t posix_seconds,
                           const uint8_t source[16]);
bool time_clock_point(const TimeClock *clock, int64_t steady_ns, TimePoint *point);
bool time_clock_current(const TimeClock *clock, int64_t steady_ns, int64_t *seconds);
