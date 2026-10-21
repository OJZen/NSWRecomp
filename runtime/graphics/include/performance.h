#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "shader_status.h"
typedef struct {
    uint64_t frame, presented, dropped;
    unsigned samples, gpu_samples;
    int pacing_override;
    double pacing_fps;
    double fps, frame_p95_ms, render_fps, render_p95_ms, gpu_ms, submit_cpu_ms, draws, upload_mb, last_presented, last_rendered;
    unsigned render_width, render_height;  // the game's scene resolution (dynamic), 0 until reported
} ZeldaPerformance;
void zelda_perf_present(uint64_t frame, double timestamp, double gpu_ms, uint64_t cpu_ns, uint64_t draws, uint64_t uploaded, double rendered_at, double pacing_fps, int pacing_override);
bool performance_snapshot(ZeldaPerformance *result);
// Forget paused intervals while retaining cumulative counters and the last known render size.
void performance_reset_window(void);
void zelda_perf_render_size(unsigned width, unsigned height);
// Oldest-first render interval and GPU time (ms, -1 when unknown) of up to `capacity` recent frames.
unsigned performance_history(float *interval_ms, float *gpu_ms, unsigned capacity);
// Completed command-buffer interval union over the last two seconds, not whole-device utilization.
void performance_gpu_interval(double start, double end);
double performance_gpu_busy(double now); // -1 until known, or if the bounded history overflowed
