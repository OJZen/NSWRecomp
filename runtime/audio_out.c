// Original NSWRecomp code (MIT). No device, mixing or audible output is provided.
#include "audio_out.h"
#include "kernel.h"
#include <math.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

enum { QUEUE_LIMIT = 64 };
static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_t thread;
static atomic_bool running;
static bool started;
static unsigned sample_rate, channel_count, head, count, released_head, released_count;
static uint32_t event_read, event_write;
static struct { uint64_t tag, frames; } pending[QUEUE_LIMIT];
static uint64_t released[QUEUE_LIMIT], played, last_tick, fraction;
static float volume;

static uint64_t now_ns(void) {
    struct timespec time; if (clock_gettime(CLOCK_MONOTONIC, &time)) abort();
    return (uint64_t)time.tv_sec * 1000000000 + time.tv_nsec;
}
static void signal_released(void) {
    uint64_t r[31] = {event_write};
    if (!kernel_svc(0x11, r) || r[0]) abort();
}
static void release_front(void) {
    released[(released_head + released_count++) % QUEUE_LIMIT] = pending[head].tag;
    head = (head + 1) % QUEUE_LIMIT; count--;
}
// Lock held; advance only submitted PCM, preserving partial-buffer position across Stop/Start.
static void advance(void) {
    uint64_t time = now_ns(), elapsed = time - last_tick; last_tick = time;
    if (!started || !count) { fraction = 0; return; }
    uint64_t scaled = (elapsed % 1000000000) * sample_rate + fraction;
    uint64_t frames = elapsed / 1000000000 * sample_rate + scaled / 1000000000;
    fraction = scaled % 1000000000;
    unsigned before = released_count;
    while (count && frames) {
        uint64_t take = frames < pending[head].frames ? frames : pending[head].frames;
        pending[head].frames -= take; frames -= take; played += take;
        if (!pending[head].frames) release_front();
    }
    if (released_count != before) signal_released();
}
static void *clock_main(void *unused) {
    (void)unused; const struct timespec interval = {0, 5000000};
    while (atomic_load(&running)) {
        nanosleep(&interval, NULL);
        pthread_mutex_lock(&lock); advance(); pthread_mutex_unlock(&lock);
    }
    return NULL;
}
bool audio_out_open(unsigned rate, unsigned channels) {
    if (rate != 48000 || (channels != 1 && channels != 2 && channels != 6) || atomic_load(&running)) return false;
    if (!kernel_event_create(&event_write, &event_read)) return false;
    sample_rate = rate; channel_count = channels; started = false; volume = 1;
    head = count = released_head = released_count = 0; played = fraction = 0; last_tick = now_ns();
    atomic_store(&running, true);
    if (pthread_create(&thread, NULL, clock_main, NULL)) {
        atomic_store(&running, false);
        uint64_t r[31] = {event_read}; kernel_svc(0x16,r); r[0] = event_write; kernel_svc(0x16,r);
        return false;
    }
    if (atexit(audio_out_close)) { audio_out_close(); return false; }
    fprintf(stderr,"AUDIO OUT silent queue initialized rate=%u channels=%u\n",rate,channels);
    return true;
}
void audio_out_close(void) {
    if (!atomic_exchange(&running,false)) return;
    pthread_join(thread,NULL);
    uint64_t r[31] = {event_read}; kernel_svc(0x16,r); r[0] = event_write; kernel_svc(0x16,r);
    fprintf(stderr,"AUDIO OUT closed consumed_frames=%llu output_samples=0\n",(unsigned long long)played);
    event_read = event_write = 0;
}
uint32_t audio_out_event(void) { return atomic_load(&running) ? kernel_event_copy(event_read) : 0; }
bool audio_out_start(bool value) {
    if (!atomic_load(&running)) return false;
    pthread_mutex_lock(&lock); advance(); started = value; pthread_mutex_unlock(&lock); return true;
}
static bool contains(uint64_t tag) {
    for (unsigned i=0;i<count;i++) if (pending[(head+i)%QUEUE_LIMIT].tag==tag) return true;
    for (unsigned i=0;i<released_count;i++) if (released[(released_head+i)%QUEUE_LIMIT]==tag) return true;
    return false;
}
bool audio_out_append(uint64_t tag, const uint64_t b[5]) {
    if (!atomic_load(&running) || !tag || !b || !b[3] || b[2]>(16u<<20) || b[4]>b[2] || b[3]>b[2]-b[4] ||
        b[1]>UINT64_MAX-b[4] || !kernel_access(b[1]+b[4],(size_t)b[3],1) || b[3]%(channel_count*2)) return false;
    pthread_mutex_lock(&lock); advance();
    bool ok = count+released_count<QUEUE_LIMIT && !contains(tag);
    if (ok) { unsigned tail=(head+count++)%QUEUE_LIMIT; pending[tail].tag=tag; pending[tail].frames=b[3]/(channel_count*2); }
    pthread_mutex_unlock(&lock); return ok;
}
size_t audio_out_released(uint64_t *tags, size_t capacity) {
    pthread_mutex_lock(&lock); advance();
    size_t n = capacity < released_count ? capacity : released_count;
    for (size_t i=0;i<n;i++) tags[i]=released[(released_head+i)%QUEUE_LIMIT];
    released_head=(released_head+(unsigned)n)%QUEUE_LIMIT; released_count-=(unsigned)n;
    pthread_mutex_unlock(&lock); return n;
}
bool audio_out_contains(uint64_t tag) {
    pthread_mutex_lock(&lock); bool result=contains(tag); pthread_mutex_unlock(&lock); return result;
}
uint64_t audio_out_value(unsigned command) {
    pthread_mutex_lock(&lock); advance();
    uint64_t result = command==0 ? !started : command==9 ? count : command==10 ? played : UINT64_MAX;
    pthread_mutex_unlock(&lock); return result;
}
bool audio_out_flush(void) {
    pthread_mutex_lock(&lock); advance(); bool flushed=count && !started;
    if (flushed) { while(count) release_front(); signal_released(); }
    pthread_mutex_unlock(&lock); return flushed;
}
bool audio_out_volume(float *value, bool set) {
    if (!value || (set && (!isfinite(*value) || *value<0 || *value>1))) return false;
    pthread_mutex_lock(&lock); if(set) volume=*value; else *value=volume; pthread_mutex_unlock(&lock); return true;
}
