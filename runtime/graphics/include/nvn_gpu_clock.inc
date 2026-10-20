// Timestamp counter reports, in Tegra 19.2 MHz ticks. They are taken when the command buffer completes, so the gap
// between a frame's first and last report approximates its GPU time; the game's dynamic resolution reads that gap.
// The reported clock may run faster than the host clock by a scale factor, so the game's own budget maps onto a
// shorter host frame budget. The reported clock stays continuous and monotonic across scale changes.
#include <os/lock.h>
static os_unfair_lock z_clock_lock = OS_UNFAIR_LOCK_INIT;
static _Atomic double z_clock_scale = 1;
static double z_clock_last_scale = 1;
static uint64_t z_clock_real_base, z_clock_reported_base, z_clock_reported_last;

static uint64_t zelda_host_ticks(void) {
    struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t);
    return ((uint64_t)t.tv_sec * 1000000000ull + t.tv_nsec) * 12 / 625;
}
static void zelda_gpu_clock_scale(double scale) {
    if (!(scale > 0) || !isfinite(scale)) MDIE("invalid GPU clock scale %g", scale);
    atomic_store(&z_clock_scale, scale);
}
static uint64_t gpu_ticks(void) {
    static int initialized;
    if (!initialized) {   // Experiment override: ZELDA_GPU_CLOCK_SCALE=<factor>.
        initialized = 1; const char *value = getenv("ZELDA_GPU_CLOCK_SCALE");
        if (value) { zelda_gpu_clock_scale(atof(value)); MLOG("GPU clock scale %g (ZELDA_GPU_CLOCK_SCALE)", atof(value)); }
    }
    uint64_t real = zelda_host_ticks();
    os_unfair_lock_lock(&z_clock_lock);
    double scale = atomic_load(&z_clock_scale);
    if (!z_clock_real_base) { z_clock_real_base = real; z_clock_reported_base = real; z_clock_last_scale = scale; }
    if (scale != z_clock_last_scale) {   // Rebase so the reported clock continues from where it was.
        z_clock_reported_base = z_clock_reported_last > z_clock_reported_base ? z_clock_reported_last : z_clock_reported_base;
        z_clock_real_base = real; z_clock_last_scale = scale;
    }
    uint64_t reported = z_clock_reported_base + (uint64_t)((double)(real - z_clock_real_base) * scale);
    if (reported < z_clock_reported_last) reported = z_clock_reported_last;
    z_clock_reported_last = reported;
    os_unfair_lock_unlock(&z_clock_lock);
    return reported;
}
