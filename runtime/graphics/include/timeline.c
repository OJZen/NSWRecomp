// Bounded event trace for scene frame analysis; inactive cost is one relaxed load per hook.
#include "timeline.h"
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <unistd.h>

enum { CAPACITY = 1 << 20 };
static ZeldaTimelineEvent *events;
static _Atomic bool recording;
static _Atomic uint64_t used;
static _Atomic unsigned writers;

uint64_t zelda_timeline_now(void) {
    return atomic_load_explicit(&recording, memory_order_relaxed) ? clock_gettime_nsec_np(CLOCK_UPTIME_RAW) : 0;
}
void zelda_timeline_span(unsigned kind, uint64_t begin, uint64_t end, uint64_t a, uint64_t b) {
    if (!begin || !atomic_load_explicit(&recording, memory_order_relaxed)) return;
    atomic_fetch_add(&writers, 1);
    uint64_t index = atomic_fetch_add(&used, 1);
    if (atomic_load(&recording) && index < CAPACITY) {
        uint64_t thread = 0; pthread_threadid_np(NULL, &thread);
        events[index] = (ZeldaTimelineEvent){begin, end, a, b, kind, (uint32_t)thread};
    }
    atomic_fetch_sub(&writers, 1);
}
void zelda_timeline(unsigned kind, uint64_t begin, uint64_t a, uint64_t b) {
    if (begin) zelda_timeline_span(kind, begin, clock_gettime_nsec_np(CLOCK_UPTIME_RAW), a, b);
}
bool zelda_timeline_start(void) {
    if (atomic_load(&recording)) return false;
    if (!events && !(events = malloc(sizeof *events * CAPACITY))) return false;
    atomic_store(&used, 0); atomic_store(&recording, true);
    return true;
}
static _Atomic unsigned dump_frames;
void zelda_passdump_request(unsigned frames) { atomic_store(&dump_frames, frames); }
unsigned zelda_passdump_take(void) {
    return atomic_load_explicit(&dump_frames, memory_order_relaxed) ? atomic_exchange(&dump_frames, 0) : 0;
}
static _Atomic int conditional_mode = -1;
bool zelda_conditional_mode_request(unsigned mode) {
    if (mode > 2) return false;
    atomic_store(&conditional_mode, (int)mode); return true;
}
int zelda_conditional_mode_take(void) {
    return atomic_load_explicit(&conditional_mode, memory_order_relaxed) < 0 ? -1 : atomic_exchange(&conditional_mode, -1);
}
bool zelda_timeline_stop(const char *path) {
    if (!atomic_exchange(&recording, false)) return false;
    while (atomic_load(&writers)) usleep(100);
    uint64_t count = atomic_load(&used); if (count > CAPACITY) count = CAPACITY;
    FILE *file = fopen(path, "w");
    if (!file) return false;
    fputs("begin_ns\tend_ns\tkind\tthread\ta\tb\n", file);
    for (uint64_t i = 0; i < count; i++)
        fprintf(file, "%llu\t%llu\t%u\t%u\t%llu\t%llu\n", (unsigned long long)events[i].begin, (unsigned long long)events[i].end,
            events[i].kind, events[i].thread, (unsigned long long)events[i].a, (unsigned long long)events[i].b);
    return !fclose(file) && atomic_load(&used) <= CAPACITY;
}
