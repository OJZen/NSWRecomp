// Adapted from Zelda-Mobile host/performance.c (Apache-2.0); see third_party/zelda-mobile/LICENSE.
#include "performance.h"
#include <pthread.h>
#include <stdlib.h>
#include <math.h>

enum { WINDOW = 120 };
static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static struct { double interval, gpu, cpu, draws, upload; } samples[WINDOW];
static double present_intervals[WINDOW];
static unsigned count, cursor, present_count, present_cursor;
static uint64_t last_frame, last_cpu, last_draws, last_upload, presented, dropped;
static double last_timestamp, last_rendered;
static double pacing_fps;
static int pacing_override;
static unsigned render_width, render_height;
void performance_reset_window(void) {
    pthread_mutex_lock(&lock);
    count = cursor = present_count = present_cursor = 0;
    last_timestamp = last_rendered = 0;
    pthread_mutex_unlock(&lock);
}

void zelda_perf_present(uint64_t frame, double timestamp, double gpu_ms, uint64_t cpu_ns, uint64_t draws, uint64_t uploaded, double rendered_at, double target, int forced) {
    pthread_mutex_lock(&lock);
    if (!isfinite(rendered_at) || rendered_at <= last_rendered || (last_rendered && frame <= last_frame)) {
        pthread_mutex_unlock(&lock); return;
    }
    if (isfinite(timestamp) && timestamp < 0) dropped++;
    if (isfinite(timestamp) && timestamp > last_timestamp) {
        if (last_timestamp) {
            present_intervals[present_cursor] = (timestamp - last_timestamp) * 1000;
            present_cursor = (present_cursor + 1) % WINDOW; if (present_count < WINDOW) present_count++;
        }
        last_timestamp = timestamp; presented++;
    }
    if (isfinite(rendered_at) && rendered_at > last_rendered && (!last_rendered || frame > last_frame)) {
        pacing_fps = target; pacing_override = forced;
        if (last_rendered) {
            uint64_t elapsed = frame - last_frame;
            samples[cursor].interval = (rendered_at - last_rendered) * 1000 / elapsed;
            samples[cursor].gpu = gpu_ms >= 0 && isfinite(gpu_ms) ? gpu_ms : -1;
            samples[cursor].cpu = cpu_ns >= last_cpu ? (cpu_ns - last_cpu) / 1e6 / elapsed : 0;
            samples[cursor].draws = draws >= last_draws ? (double)(draws - last_draws) / elapsed : 0;
            samples[cursor].upload = uploaded >= last_upload ? (uploaded - last_upload) / 1048576.0 / elapsed : 0;
            cursor = (cursor + 1) % WINDOW; if (count < WINDOW) count++;
        }
        last_rendered = rendered_at; last_frame = frame; last_cpu = cpu_ns; last_draws = draws; last_upload = uploaded;
    }
    pthread_mutex_unlock(&lock);
}
static int compare(const void *a, const void *b) { double x = *(const double *)a, y = *(const double *)b; return (x > y) - (x < y); }
bool performance_snapshot(ZeldaPerformance *out) {
    double intervals[WINDOW], actual[WINDOW], total = 0, present_total = 0, gpu = 0; unsigned gpu_count = 0;
    pthread_mutex_lock(&lock);
    *out = (ZeldaPerformance){.frame=last_frame,.presented=presented,.dropped=dropped,.samples=count,
        .last_presented=last_timestamp,.last_rendered=last_rendered,.fps=-1,.gpu_ms=-1,
        .pacing_fps=pacing_fps,.pacing_override=pacing_override,.render_width=render_width,.render_height=render_height};
    unsigned np = present_count;
    for (unsigned i = 0; i < np; i++) { actual[i] = present_intervals[i]; present_total += actual[i]; }
    for (unsigned i = 0; i < count; i++) {
        intervals[i] = samples[i].interval; total += intervals[i];
        if (samples[i].gpu >= 0) { gpu += samples[i].gpu; gpu_count++; }
        out->submit_cpu_ms += samples[i].cpu; out->draws += samples[i].draws; out->upload_mb += samples[i].upload;
    }
    pthread_mutex_unlock(&lock);
    if (!out->samples || total <= 0) return false;
    qsort(intervals, out->samples, sizeof(double), compare);
    out->render_fps = 1000 * out->samples / total; out->render_p95_ms = intervals[(out->samples * 95 + 99) / 100 - 1];
    if (np && present_total > 0) {
        qsort(actual, np, sizeof(double), compare);
        out->fps = 1000 * np / present_total; out->frame_p95_ms = actual[(np * 95 + 99) / 100 - 1];
    }
    out->gpu_ms = gpu_count ? gpu / gpu_count : -1;
    out->gpu_samples = gpu_count;
    out->submit_cpu_ms /= out->samples; out->draws /= out->samples; out->upload_mb /= out->samples;
    return true;
}
void zelda_perf_render_size(unsigned width, unsigned height) {
    pthread_mutex_lock(&lock); render_width = width; render_height = height; pthread_mutex_unlock(&lock);
}
unsigned performance_history(float *interval_ms, float *gpu_ms, unsigned capacity) {
    pthread_mutex_lock(&lock);
    unsigned n = count < capacity ? count : capacity;
    for (unsigned i = 0; i < n; i++) {
        unsigned slot = (cursor + WINDOW - n + i) % WINDOW;
        interval_ms[i] = (float)samples[slot].interval; gpu_ms[i] = (float)samples[slot].gpu;
    }
    pthread_mutex_unlock(&lock);
    return n;
}

enum { GPU_WINDOW = 2048 };
typedef struct { double start, end; } GpuInterval;
static GpuInterval gpu_intervals[GPU_WINDOW];
static unsigned gpu_count, gpu_cursor;
static double gpu_lost_until;
void performance_gpu_interval(double start, double end) {
    if (!isfinite(start) || !isfinite(end) || start <= 0 || end <= start) return;
    pthread_mutex_lock(&lock);
    if (gpu_count == GPU_WINDOW) gpu_lost_until = fmax(gpu_lost_until, gpu_intervals[gpu_cursor].end);
    gpu_intervals[gpu_cursor] = (GpuInterval){start, end};
    gpu_cursor = (gpu_cursor + 1) % GPU_WINDOW;
    if (gpu_count < GPU_WINDOW) gpu_count++;
    pthread_mutex_unlock(&lock);
}
static int compare_gpu(const void *a, const void *b) {
    return compare(&((const GpuInterval *)a)->start, &((const GpuInterval *)b)->start);
}
double performance_gpu_busy(double now) {
    if (!isfinite(now) || now < 2) return -1;
    GpuInterval recent[GPU_WINDOW]; unsigned count = 0;
    double begin = now - 2;
    pthread_mutex_lock(&lock);
    if (!gpu_count || gpu_lost_until > begin) { pthread_mutex_unlock(&lock); return -1; }
    for (unsigned i = 0; i < gpu_count; i++) if (gpu_intervals[i].end > begin && gpu_intervals[i].start < now)
        recent[count++] = (GpuInterval){fmax(begin, gpu_intervals[i].start), fmin(now, gpu_intervals[i].end)};
    pthread_mutex_unlock(&lock);
    qsort(recent, count, sizeof *recent, compare_gpu);
    double end = begin, busy = 0;
    for (unsigned i = 0; i < count; i++) if (recent[i].end > end) {
        busy += recent[i].end - fmax(end, recent[i].start); end = recent[i].end;
    }
    return busy * 50; // union seconds / two-second window * 100
}
