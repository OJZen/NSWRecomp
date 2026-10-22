#include "performance.h"
#include <assert.h>
#include <math.h>
static void near(double actual, double expected) { assert(fabs(actual - expected) < .0001); }
int main(void) {
    ZeldaPerformance p; assert(!performance_snapshot(&p)); assert(p.fps == -1 && p.gpu_ms == -1);
    zelda_perf_render_size(1600, 900);
    for (unsigned i = 0; i <= 120; i++)
        zelda_perf_present(i + 1, 10 + i / 30.0, 8, i * 4000000ull, i * 500, i * 1048576ull, 10 + i / 30.0, 30, 0);
    assert(performance_snapshot(&p)); assert(p.samples == 120 && p.gpu_samples == 120);
    near(p.fps, 30); near(p.render_fps, 30); near(p.frame_p95_ms, 1000.0 / 30);
    near(p.submit_cpu_ms, 4); near(p.gpu_ms, 8); near(p.draws, 500); near(p.upload_mb, 1);
    assert(p.render_width == 1600 && p.render_height == 900 && p.presented == 121 && !p.dropped);
    zelda_perf_present(121, -1, 8, 0, 0, 0, 14, 30, 0); // duplicate cannot double-count a drop
    performance_snapshot(&p); assert(!p.dropped);
    performance_reset_window();
    assert(!performance_snapshot(&p)); assert(p.render_width == 1600 && p.presented == 121);
    zelda_perf_present(122, -1, NAN, 100, 0, 0, 20, 30, 0);
    zelda_perf_present(123, 0, -1, 200, 0, 0, 20.05, 30, 0);
    assert(performance_snapshot(&p)); assert(p.dropped == 1 && p.fps == -1 && p.gpu_ms == -1);
    near(p.render_fps, 20);
    float interval[1], gpu[1]; assert(performance_history(interval, gpu, 1) == 1);
    near(interval[0], 50); assert(gpu[0] == -1);
    zelda_perf_present(124, NAN, 1, 0, 0, 0, NAN, 30, 0);
    performance_snapshot(&p); assert(p.frame == 123);
    performance_reset_window();
    for (unsigned i = 0; i < 6; i++) {
        double t = 30 + i * .016 + (i == 5 ? .084 : 0);
        zelda_perf_present(130 + i, t, 1, 0, 0, 0, t, 30, 0);
    }
    performance_snapshot(&p); near(p.frame_p95_ms, 100); near(p.render_p95_ms, 100);
    assert(performance_gpu_busy(10) == -1);
    performance_gpu_interval(NAN, 9); performance_gpu_interval(10, 9);
    assert(performance_gpu_busy(10) == -1);
    performance_gpu_interval(9, 9.5); performance_gpu_interval(8, 9.25);
    performance_gpu_interval(9.75, 10.25); // clip at snapshot time, merge out-of-order overlap
    near(performance_gpu_busy(10), 87.5);
    near(performance_gpu_busy(10.5), 75);
    near(performance_gpu_busy(13), 0);
    for (unsigned i = 0; i < 4098; i++) performance_gpu_interval(20, 20.01);
    assert(performance_gpu_busy(21) == -1); // bounded ring exhaustion is unknown, never underreported
    near(performance_gpu_busy(23), 0);
    return 0;
}
