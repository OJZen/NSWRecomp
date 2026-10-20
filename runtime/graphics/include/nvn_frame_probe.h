#include <stdint.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <time.h>

typedef struct { uint64_t wall, cpu; } zelda_frame_probe;
static zelda_frame_probe zelda_frame_probe_start(void) {
    static int enabled = -1;
    if (enabled < 0) { const char *value = getenv("ZELDA_FRAME_PROBE"); enabled = value && atoi(value); }
    return enabled ? (zelda_frame_probe){now_ns(), clock_gettime_nsec_np(CLOCK_THREAD_CPUTIME_ID)} : (zelda_frame_probe){0};
}
static void zelda_frame_probe_note(uint64_t frame, const char *phase, zelda_frame_probe start) {
    if (!start.wall) return;
    uint64_t wall = now_ns() - start.wall;
    if (wall < 10000000) return;
    uint64_t cpu = clock_gettime_nsec_np(CLOCK_THREAD_CPUTIME_ID) - start.cpu;
    static _Atomic unsigned logged;
    if (atomic_fetch_add(&logged, 1) >= 64) return;
    LOG("FRAME_HOST_STALL frame=%llu phase=%s wall_ms=%.3f cpu_ms=%.3f",
        (unsigned long long)frame, phase, wall / 1e6, cpu / 1e6);
}
