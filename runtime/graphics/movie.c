// Adapted from Zelda-Mobile host/movie.c (Apache-2.0); see third_party/zelda-mobile/SOURCE.json.
// Pre-rendered cutscenes (Event/Movie/*.webm, VP9 1920x1080 30 fps, video only). The SDK's movie::Decoder decodes
// through NVDEC (NvMMLite -> NvRmChannelOpen on /dev/nvhost-nvdec), which this host does not provide: its worker
// aborted the game (2162-0001) as the first movie started. The decoder is taken over at construction with a host
// implementation of the SDK's MediaCodec-style contract on top of movie_video (libvpx, portable to iOS/Android);
// movie::Extractor, Buffer and MediaData stay the SDK's (software WebM parsing and plain containers).
//
// Game protocol, v1.4.3 main 0x2b44300-0x2b45e60 (read from code; slots index the object's vtable):
//   setup: Extractor slot 5 GetTrackConfiguration -> "mime" video/avc|vp8|vp9 -> Decoder(type 1|2|3, output 4 = NV12,
//     mode 1, movie::GetThreadPriority()) -> slot 3 RegisterClientEvents({input, output, format, error} EventType*)
//     -> slot 2 Configure(track MediaData: width/height) -> slot 14 GetOutputBufferSize (unused) -> Extractor SelectTrack.
//   slot 5 Start. Worker: input event -> slot 9 AcquireInputBufferIndex(&i, &more) until non-zero; read thread ->
//     slot 10 GetInputBuffer -> Extractor Read/GetSampleTime/Advance -> slot 11 SendInputBufferForDecode(i, off, size,
//     pts, flags 4 at end); output event -> slot 12 AcquireOutputBufferIndex(&i, &pts, &flags, &more): flags 4 ends
//     playback with slot 15 Release, other frames wait for their time; format event -> slot 4 GetOutputFormat
//     (width/height/stride/crop). Each displayed frame: Buffer over the game's NV12 texture memory (Y w*h, then
//     interleaved UV) -> slot 13 GetOutputBuffer(i, &buffer) -> slot 15 ReleaseOutputBufferIndex(i).
//   end/skip: slot 8 Flush + slot 6 Stop; destroy: slot 8 Flush + slot 1 (deleting destructor).
// Unused slots abort with their name instead of returning a guessed success.
#include "dispatch.h"
#include "host_compat.h"
#include "movie.h"
#include "threads.h"
#include "movie_video.h"
#include "movie_audio.h"
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

enum { ST_OK = 0, ST_FAILED = 0x1001, ST_TRY_AGAIN = 0x1008 }; // movie::Status: the game tests only for non-zero.
enum { FLAG_EOS = 4, TYPE_VP8 = 2, TYPE_VP9 = 3, TYPE_VORBIS = 5, OUTPUT_PCM16 = 1, OUTPUT_TEXTURE = 3, OUTPUT_NV12 = 4 };
enum { INPUTS = 4, OUTPUTS = 8, INPUT_CAPACITY = 4 << 20, STACK = 512 << 10 };
enum { FREE, ACQUIRED, QUEUED, READY };

typedef struct {
    pthread_mutex_t lock; pthread_cond_t wake, idle;
    void *events[4]; // input available, output available, output format changed, error
    int type, priority, width, height, announced_width, announced_height, texture_output;
    int sample_rate, channels;
    movie_audio *audio;
    zelda_video *video;
    uint8_t *input[INPUTS]; int input_state[INPUTS];
    struct { int index, offset, size; int64_t pts; uint32_t flags; } queue[INPUTS]; int queue_head, queue_count;
    uint8_t *output[OUTPUTS]; size_t output_capacity, output_size[OUTPUTS]; int output_state[OUTPUTS];
    int64_t output_pts[OUTPUTS]; uint32_t output_flags[OUTPUTS]; int ready[OUTPUTS], ready_head, ready_count;
    int started, busy, quit, thread_created, last_error, ended;
    void *thread, *stack;
    uint64_t frames, decode_ns;
    _Atomic uint64_t flushes;
} movie_decoder;

#define MOVIE_BASE 0x7f00100000ull
#define MOVIE_WORKER (MOVIE_BASE + 19 * 4)
static _Thread_local Cpu *movie_cpu;
uint64_t graphics_guest_export(const char *name);
static struct {
    uint64_t signal, create_thread, start_thread, wait_thread, destroy_thread, release;
    uint64_t set_data, set_range, base, capacity, offset, size, find_int32, find_string, find_buffer, set_int32, set_rect, set_string;
} sdk;
static struct { uint64_t alloc, free, user; } allocator;
static pthread_once_t sdk_once = PTHREAD_ONCE_INIT;

static uint64_t symbol(const char *name) {
    uint64_t address = graphics_guest_export(name);
    if (!address || !rt_find_function(address)) die("movie: compiled SDK symbol missing: %s", name);
    return address;
}
static uint64_t movie_call(uint64_t address, const uint64_t *args, size_t count) {
    if (!movie_cpu || count > 8 || !rt_find_function(address)) die("movie: invalid compiled callback");
    Cpu callback = *movie_cpu;
    callback.pc = address; callback.x[30] = 0; callback.excl_addr = UINT64_MAX;
    memcpy(callback.x, args, count * sizeof *args);
    rt_dispatch(&callback);
    if (callback.sp != movie_cpu->sp) die("movie: unbalanced callback stack");
    movie_cpu->fpsr = callback.fpsr;
    return callback.x[0];
}
#define INVOKE(address, ...) movie_call(address, (uint64_t[]){__VA_ARGS__}, sizeof((uint64_t[]){__VA_ARGS__}) / sizeof(uint64_t))

static void resolve(void) { // First decoder construction, on the game's movie thread.
    sdk.signal = symbol("_ZN2nn2os11SignalEventEPNS0_9EventTypeE");
    sdk.create_thread = symbol("_ZN2nn2os12CreateThreadEPNS0_10ThreadTypeEPFvPvES3_S3_mi");
    sdk.start_thread = symbol("_ZN2nn2os11StartThreadEPNS0_10ThreadTypeE");
    sdk.wait_thread = symbol("_ZN2nn2os10WaitThreadEPNS0_10ThreadTypeE");
    sdk.destroy_thread = symbol("_ZN2nn2os13DestroyThreadEPNS0_10ThreadTypeE");
    sdk.release = symbol("_ZdlPv");
    sdk.set_data = symbol("_ZN5movie6Buffer18SetDataAndCapacityEPvm");
    sdk.set_range = symbol("_ZN5movie6Buffer8SetRangeEii");
    sdk.base = symbol("_ZNK5movie6Buffer4BaseEv");
    sdk.capacity = symbol("_ZNK5movie6Buffer8CapacityEv");
    sdk.offset = symbol("_ZNK5movie6Buffer6OffsetEv");
    sdk.size = symbol("_ZNK5movie6Buffer4SizeEv");
    sdk.find_string = symbol("_ZNK5movie9MediaData10FindStringEPKcPS2_");
    sdk.find_buffer = symbol("_ZNK5movie9MediaData10FindBufferEPKcPPNS_6BufferE");
    sdk.find_int32 = symbol("_ZNK5movie9MediaData9FindInt32EPKcPi");
    sdk.set_int32 = symbol("_ZN5movie9MediaData8SetInt32EPKci");
    sdk.set_rect = symbol("_ZN5movie9MediaData7SetRectEPKciiii");
    sdk.set_string = symbol("_ZN5movie9MediaData9SetStringEPKcS2_");
}

static movie_decoder *M(void *self) { return ((movie_decoder **)self)[1]; }
static uint64_t monotonic_ns(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return (uint64_t)t.tv_sec * 1000000000ull + (uint64_t)t.tv_nsec; }
static void signal_event(movie_decoder *m, int which) { if (m->events[which]) INVOKE(sdk.signal, (uintptr_t)m->events[which]); }

static int free_output(movie_decoder *m) {
    for (int i = 0; i < OUTPUTS; i++) if (m->output_state[i] == FREE) return i;
    return -1;
}
static void wait_movie(movie_decoder *m, pthread_cond_t *condition) {
    const struct timespec interval = {0, 100000000};
    pthread_cond_timedwait_relative_np(condition, &m->lock, &interval);
    pthread_mutex_unlock(&m->lock);
    // Shutdown must also reach workers waiting for movie input or an output slot.
    threads_checkpoint();
    pthread_mutex_lock(&m->lock);
}
// Worker side, lock held: wait for a free output slot unless the stream is being flushed or torn down.
static int claim_output(movie_decoder *m, uint64_t generation, const _Atomic uint64_t *current) {
    int slot;
    while ((slot = free_output(m)) < 0 && !m->quit && atomic_load(current) == generation) wait_movie(m, &m->wake);
    return m->quit || atomic_load(current) != generation ? -1 : slot;
}

static void publish(movie_decoder *m, int slot, size_t size, int64_t pts, uint32_t flags) {
    m->output_size[slot] = size; m->output_pts[slot] = pts; m->output_flags[slot] = flags; m->output_state[slot] = READY;
    m->ready[(m->ready_head + m->ready_count++) % OUTPUTS] = slot;
}

static void worker(void *argument) {
    movie_decoder *m = argument;
    pthread_mutex_lock(&m->lock);
    while (!m->quit) {
        if (!m->started || !m->queue_count || free_output(m) < 0) { wait_movie(m, &m->wake); continue; }
        typeof(m->queue[0]) job = m->queue[m->queue_head];
        m->queue_head = (m->queue_head + 1) % INPUTS; m->queue_count--;
        uint64_t generation = atomic_load(&m->flushes);
        m->busy = 1;
        pthread_mutex_unlock(&m->lock);
        uint64_t start = monotonic_ns();
        const void *input = job.size ? m->input[job.index] + job.offset : NULL;
        int status = m->audio ? movie_audio_decode(m->audio, input, (size_t)job.size) :
                               zelda_video_decode(m->video, input, (size_t)job.size);
        uint64_t spent = monotonic_ns() - start; // decode and conversion only, not the wait for a free output slot
        pthread_mutex_lock(&m->lock);
        if (status) { m->last_error = ST_FAILED; fprintf(stderr, "MOVIE decode error pts=%lld size=%d\n", (long long)job.pts, job.size); }
        int changed = 0, produced = 0;
        for (;;) {
            if (status) break;
            int slot = claim_output(m, generation, &m->flushes);
            if (slot < 0) break;
            int width = 0, height = 0;
            pthread_mutex_unlock(&m->lock);
            uint64_t convert = monotonic_ns();
            size_t frame_bytes=0;
            int frame = m->audio ? movie_audio_frame(m->audio, m->output[slot], m->output_capacity, &frame_bytes) :
                m->texture_output ? zelda_video_frame_rgba(m->video, m->output[slot], m->output_capacity, &width, &height) :
                zelda_video_frame(m->video, m->output[slot], m->output_capacity, &width, &height);
            spent += monotonic_ns() - convert;
            pthread_mutex_lock(&m->lock);
            if (frame < 0) die("movie: decoded frame %dx%d is incompatible with configured %dx%d output", width, height, m->width, m->height);
            if (!frame) break;
            if (m->type==TYPE_VORBIS && !m->frames) changed=1;
            if (width != m->announced_width || height != m->announced_height) { m->announced_width = width; m->announced_height = height; changed = 1; }
            if(m->type!=TYPE_VORBIS) frame_bytes=m->texture_output ? (size_t)width * height * 4 : zelda_video_nv12_size(width,height);
            publish(m, slot, frame_bytes, job.pts, 0);
            produced++; m->frames++;
        }
        m->decode_ns += spent;
        if (job.flags & FLAG_EOS) {
            int slot = claim_output(m, generation, &m->flushes);
            if (slot >= 0) { publish(m, slot, 0, job.pts, FLAG_EOS); produced++; }
            if (!m->ended++) fprintf(stderr, "MOVIE end frames=%llu decode_ms_per_frame=%.2f\n", (unsigned long long)m->frames, m->frames ? m->decode_ns / 1e6 / m->frames : 0.0);
        }
        if (atomic_load(&m->flushes) == generation) m->input_state[job.index] = FREE;
        m->busy = 0;
        pthread_cond_broadcast(&m->idle);
        pthread_mutex_unlock(&m->lock);
        // Format first, as MediaCodec does, so the game re-reads the size before taking the frame.
        if (status) signal_event(m, 3);
        if (changed) signal_event(m, 2);
        if (produced) signal_event(m, 1);
        signal_event(m, 0);
        pthread_mutex_lock(&m->lock);
    }
    pthread_mutex_unlock(&m->lock);
}

static void fill_format(movie_decoder *m, void *media) {
    if(m->type==TYPE_VORBIS) {
        INVOKE(sdk.set_string,(uintptr_t)media,(uintptr_t)"mime",(uintptr_t)"audio/raw");
        INVOKE(sdk.set_int32,(uintptr_t)media,(uintptr_t)"sample-rate",m->sample_rate);
        INVOKE(sdk.set_int32,(uintptr_t)media,(uintptr_t)"channel-count",m->channels); return;
    }
    int width = m->announced_width ? m->announced_width : m->width, height = m->announced_height ? m->announced_height : m->height;
    INVOKE(sdk.set_string, (uintptr_t)media, (uintptr_t)"mime", (uintptr_t)(m->type == TYPE_VP9 ? "video/x-vnd.on2.vp9" : "video/x-vnd.on2.vp8"));
    INVOKE(sdk.set_int32, (uintptr_t)media, (uintptr_t)"width", width); INVOKE(sdk.set_int32, (uintptr_t)media, (uintptr_t)"height", height);
    INVOKE(sdk.set_int32, (uintptr_t)media, (uintptr_t)"stride", width); INVOKE(sdk.set_int32, (uintptr_t)media, (uintptr_t)"slice-height", height);
    INVOKE(sdk.set_rect, (uintptr_t)media, (uintptr_t)"crop", 0, 0, width - 1, height - 1);
}

// Drops queued input and undelivered frames; the decoder restarts from the next key frame the game sends.
static void flush_locked(movie_decoder *m) {
    atomic_fetch_add(&m->flushes, 1);
    pthread_cond_broadcast(&m->wake);
    while (m->busy) wait_movie(m, &m->idle);
    for (int i = 0; i < INPUTS; i++) m->input_state[i] = FREE;
    for (int i = 0; i < OUTPUTS; i++) m->output_state[i] = FREE;
    m->queue_head = m->queue_count = m->ready_head = m->ready_count = 0; m->ended = 0;
    if (m->audio && movie_audio_reset(m->audio)) die("movie: Vorbis decoder restart failed");
    if (m->video) {
        zelda_video_close(m->video);
        if (!(m->video = zelda_video_open(m->type, 4))) die("movie: decoder restart failed");
    }
}

static void destroy(void *self) {
    movie_decoder *m = M(self);
    if (!m) return;
    pthread_mutex_lock(&m->lock);
    m->quit = 1; pthread_cond_broadcast(&m->wake);
    pthread_mutex_unlock(&m->lock);
    if (m->thread_created) { INVOKE(sdk.wait_thread, (uintptr_t)m->thread); INVOKE(sdk.destroy_thread, (uintptr_t)m->thread); }
    zelda_video_close(m->video);
    movie_audio_close(m->audio);
    for (int i = 0; i < INPUTS; i++) if (m->input[i]) INVOKE(allocator.free, (uintptr_t)m->input[i], allocator.user);
    for (int i = 0; i < OUTPUTS; i++) free(m->output[i]);
    if (m->stack) INVOKE(allocator.free, (uintptr_t)m->stack, allocator.user);
    if (m->thread) INVOKE(allocator.free, (uintptr_t)m->thread, allocator.user);
    fprintf(stderr, "MOVIE decoder destroyed frames=%llu\n", (unsigned long long)m->frames);
    pthread_cond_destroy(&m->wake); pthread_cond_destroy(&m->idle); pthread_mutex_destroy(&m->lock);
    free(m);
    ((movie_decoder **)self)[1] = NULL;
}
static void decoder_delete(void *self) { destroy(self); INVOKE(sdk.release, (uintptr_t)self); }

static int configure(void *self, void *media) {
    movie_decoder *m = M(self);
    int width = 0, height = 0;
    if (m->type!=TYPE_VORBIS && (!(INVOKE(sdk.find_int32, (uintptr_t)media, (uintptr_t)"width", (uintptr_t)&width) & 1) || !(INVOKE(sdk.find_int32, (uintptr_t)media, (uintptr_t)"height", (uintptr_t)&height) & 1) || width <= 0 || height <= 0 || width > 8192 || height > 8192))
        die("movie: track format has no usable width/height");
    pthread_mutex_lock(&m->lock);
    if (m->video || m->audio) { pthread_mutex_unlock(&m->lock); return ST_FAILED; }
    m->width = width; m->height = height;
    if(m->type==TYPE_VORBIS) {
        const char *mime = NULL;
        bool has_mime = INVOKE(sdk.find_string,(uintptr_t)media,(uintptr_t)"mime",(uintptr_t)&mime) & 1;
        bool has_rate = INVOKE(sdk.find_int32,(uintptr_t)media,(uintptr_t)"sample-rate",(uintptr_t)&m->sample_rate) & 1;
        bool has_channels = INVOKE(sdk.find_int32,(uintptr_t)media,(uintptr_t)"channel-count",(uintptr_t)&m->channels) & 1;
        if (!has_mime || !mime || !kernel_access((uintptr_t)mime,13,1) || memcmp(mime,"audio/vorbis",13) ||
            !has_rate || !has_channels || m->sample_rate<8000 || m->sample_rate>192000 || m->channels<1 || m->channels>8)
            die("movie: unsupported Vorbis track rate=%d channels=%d",m->sample_rate,m->channels);
        const char *keys[] = {"csd-0", "csd-1"};
        const void *headers[2]; size_t sizes[2];
        for (unsigned i=0; i<2; i++) {
            void *buffer = NULL;
            if (!(INVOKE(sdk.find_buffer,(uintptr_t)media,(uintptr_t)keys[i],(uintptr_t)&buffer)&1) || !buffer)
                die("movie: missing Vorbis %s",keys[i]);
            uintptr_t base = INVOKE(sdk.base,(uintptr_t)buffer);
            size_t capacity = INVOKE(sdk.capacity,(uintptr_t)buffer);
            int offset = (int)INVOKE(sdk.offset,(uintptr_t)buffer), size = (int)INVOKE(sdk.size,(uintptr_t)buffer);
            if (offset<0 || size<=0 || size>(1<<20) || (size_t)offset>capacity || (size_t)size>capacity-(size_t)offset ||
                base>UINTPTR_MAX-(size_t)offset || !kernel_access(base+(size_t)offset,(size_t)size,1))
                die("movie: invalid Vorbis %s buffer",keys[i]);
            headers[i] = (const void *)(base+(size_t)offset); sizes[i] = (size_t)size;
        }
        m->audio = movie_audio_open(headers[0],sizes[0],headers[1],sizes[1],m->sample_rate,m->channels);
        if (!m->audio) die("movie: Vorbis headers rejected");
        m->output_capacity = (size_t)8192*m->channels*2;
    } else m->output_capacity = m->texture_output ? (size_t)width * height * 4 : zelda_video_nv12_size(width, height);
    if (m->output_capacity > (128u << 20) / OUTPUTS) die("movie: output buffers exceed the 128 MiB bound");
    if (m->type!=TYPE_VORBIS && !(m->video = zelda_video_open(m->type, 4))) die("movie: libvpx decoder unavailable for type %d", m->type);
    // Extractor reads and SDK synchronization validate these buffers against the guest memory map.
    for (int i = 0; i < INPUTS; i++)
        if (!(m->input[i] = (void *)(uintptr_t)INVOKE(allocator.alloc, INPUT_CAPACITY, 16, allocator.user))) die("movie: input buffer allocation failed");
    for (int i = 0; i < OUTPUTS; i++) if (!(m->output[i] = malloc(m->output_capacity))) die("movie: output buffer allocation failed");
    pthread_mutex_unlock(&m->lock);
    fprintf(stderr, "MOVIE configure codec=%s %dx%d rate=%d channels=%d inputs=%d outputs=%d\n", m->type==TYPE_VORBIS?"vorbis":m->type==TYPE_VP9?"vp9":"vp8", width, height,m->sample_rate,m->channels, INPUTS, OUTPUTS);
    return ST_OK;
}
static int register_events(void *self, void **events) { memcpy(M(self)->events, events, sizeof M(self)->events); return ST_OK; }
static int output_format(void *self, void *media) {
    movie_decoder *m = M(self);
    pthread_mutex_lock(&m->lock); fill_format(m, media); pthread_mutex_unlock(&m->lock);
    return ST_OK;
}
static int start(void *self) {
    movie_decoder *m = M(self);
    pthread_mutex_lock(&m->lock);
    if (!m->video && !m->audio) { pthread_mutex_unlock(&m->lock); return ST_FAILED; }
    m->started = 1;
    if (!m->thread_created) {
        // A guest thread, so the worker can signal the game's nn::os events.
        if (!allocator.alloc || !(m->stack = (void *)(uintptr_t)INVOKE(allocator.alloc, STACK, 0x1000, allocator.user))) die("movie: worker stack allocation failed");
        if (!(m->thread = (void *)(uintptr_t)INVOKE(allocator.alloc, 0x400, 16, allocator.user))) die("movie: worker object allocation failed");
        uint32_t result = INVOKE(sdk.create_thread, (uintptr_t)m->thread, MOVIE_WORKER, (uintptr_t)m, (uintptr_t)m->stack, STACK, m->priority);
        if (result) die("movie: worker thread creation failed %#x", result);
        INVOKE(sdk.start_thread, (uintptr_t)m->thread); m->thread_created = 1;
    }
    pthread_cond_broadcast(&m->wake);
    pthread_mutex_unlock(&m->lock);
    fprintf(stderr, "MOVIE start\n");
    signal_event(m, 0);
    return ST_OK;
}
static int stop(void *self) {
    movie_decoder *m = M(self);
    pthread_mutex_lock(&m->lock); m->started = 0; pthread_mutex_unlock(&m->lock);
    fprintf(stderr, "MOVIE stop frames=%llu\n", (unsigned long long)m->frames);
    return ST_OK;
}
static int flush(void *self) {
    movie_decoder *m = M(self);
    pthread_mutex_lock(&m->lock); flush_locked(m); int started = m->started; pthread_mutex_unlock(&m->lock);
    if (started) signal_event(m, 0);
    return ST_OK;
}
static int acquire_input(void *self, int *index, int *more) {
    movie_decoder *m = M(self); int found = -1, others = 0;
    pthread_mutex_lock(&m->lock);
    for (int i = 0; i < INPUTS; i++) if (m->input_state[i] == FREE) { if (found < 0) found = i; else others++; }
    if (found >= 0) m->input_state[found] = ACQUIRED;
    pthread_mutex_unlock(&m->lock);
    *index = found; *more = found >= 0 ? others : 0;
    return found >= 0 ? ST_OK : ST_TRY_AGAIN;
}
static int input_buffer(void *self, int index, void *buffer) {
    movie_decoder *m = M(self);
    if (index < 0 || index >= INPUTS || m->input_state[index] != ACQUIRED) return ST_FAILED;
    INVOKE(sdk.set_data, (uintptr_t)buffer, (uintptr_t)m->input[index], INPUT_CAPACITY); INVOKE(sdk.set_range, (uintptr_t)buffer, 0, 0);
    return ST_OK;
}
static int send_input(void *self, int index, int offset, int size, int64_t pts, uint32_t flags) {
    movie_decoder *m = M(self);
    pthread_mutex_lock(&m->lock);
    if (index < 0 || index >= INPUTS || m->input_state[index] != ACQUIRED || offset < 0 || size < 0 || (int64_t)offset + size > INPUT_CAPACITY) {
        pthread_mutex_unlock(&m->lock); return ST_FAILED;
    }
    m->input_state[index] = QUEUED;
    m->queue[(m->queue_head + m->queue_count++) % INPUTS] = (typeof(m->queue[0])){index, offset, size, pts, flags};
    pthread_cond_broadcast(&m->wake);
    pthread_mutex_unlock(&m->lock);
    return ST_OK;
}
static int acquire_output(void *self, int *index, int64_t *pts, uint32_t *flags, int *more) {
    movie_decoder *m = M(self); int slot = -1;
    pthread_mutex_lock(&m->lock);
    if (m->ready_count) {
        slot = m->ready[m->ready_head]; m->ready_head = (m->ready_head + 1) % OUTPUTS; m->ready_count--;
        m->output_state[slot] = ACQUIRED; *pts = m->output_pts[slot]; *flags = m->output_flags[slot];
    }
    *more = m->ready_count;
    pthread_mutex_unlock(&m->lock);
    *index = slot;
    return slot >= 0 ? ST_OK : ST_TRY_AGAIN;
}
static int acquired(movie_decoder *m, int index) {
    pthread_mutex_lock(&m->lock);
    int held = index >= 0 && index < OUTPUTS && m->output_state[index] == ACQUIRED;
    pthread_mutex_unlock(&m->lock);
    return held;
}
static int output_buffer(void *self, int index, void *buffer) {
    movie_decoder *m = M(self);
    // An acquired slot is only touched by the game until it is released.
    if (!acquired(m, index)) return ST_FAILED;
    size_t size = m->output_size[index], offset = (size_t)INVOKE(sdk.offset, (uintptr_t)buffer);
    size_t capacity = INVOKE(sdk.capacity, (uintptr_t)buffer);
#ifdef NSW_VULKAN_VIDEO
    if (m->texture_output) {
        uint64_t address = INVOKE(sdk.base, (uintptr_t)buffer);
        if (offset || capacity != sizeof(uint64_t) || !kernel_access(address, sizeof(uint64_t), 1) || !size) return ST_FAILED;
        // Buffer::Base is the opaque NVN texture object itself. Its first word
        // is backend-specific storage, not another guest texture pointer.
        void *texture = (void *)(uintptr_t)address;
        unsigned width = m->announced_width ? m->announced_width : m->width, height = m->announced_height ? m->announced_height : m->height;
        if (size != (size_t)width * height * 4 || !movie_write_rgba(texture, m->output[index], width, height)) return ST_FAILED;
        return ST_OK;
    }
#endif
    if (offset > capacity || size > capacity - offset) return ST_FAILED;
    uint8_t *destination = (uint8_t *)INVOKE(sdk.base, (uintptr_t)buffer) + offset;
    // The destination is the game's texture memory; let the dirty-page tracker see the host write.
    if (size) { dm_guest_write_prepare(destination, size); memcpy(destination, m->output[index], size); }
    INVOKE(sdk.set_range, (uintptr_t)buffer, offset, size);
    return ST_OK;
}
static int output_buffer_size(void *self, int *count, int *size) {
    movie_decoder *m = M(self); *count = OUTPUTS; *size = (int)m->output_capacity;
    return ST_OK;
}
static int release_output(void *self, int index) {
    movie_decoder *m = M(self);
    pthread_mutex_lock(&m->lock);
    int valid = index >= 0 && index < OUTPUTS && m->output_state[index] == ACQUIRED;
    if (valid) { m->output_state[index] = FREE; pthread_cond_broadcast(&m->wake); }
    pthread_mutex_unlock(&m->lock);
    return valid ? ST_OK : ST_FAILED;
}
static int last_error(void *self) {
    movie_decoder *m = M(self);
    pthread_mutex_lock(&m->lock); int error = m->last_error; pthread_mutex_unlock(&m->lock);
    return error;
}
static int output_buffer_media(void *self, int index, void *buffer, void *media) {
    int status = output_buffer(self, index, buffer);
    if (!status) output_format(self, media);
    return status;
}
static int output_buffer_format(void *self, int index, void *media) {
    if (!acquired(M(self), index)) return ST_FAILED;
    return output_format(self, media);
}
static void reset_trap(void) { die("movie: Decoder slot 7 Reset is outside the game's protocol"); }

// Only Cpu-dispatch tokens are stored in guest-visible virtual tables.
static const uint64_t decoder_vtable[19] = {
    MOVIE_BASE, MOVIE_BASE + 4, MOVIE_BASE + 8, MOVIE_BASE + 12, MOVIE_BASE + 16,
    MOVIE_BASE + 20, MOVIE_BASE + 24, MOVIE_BASE + 28, MOVIE_BASE + 32, MOVIE_BASE + 36,
    MOVIE_BASE + 40, MOVIE_BASE + 44, MOVIE_BASE + 48, MOVIE_BASE + 52, MOVIE_BASE + 56,
    MOVIE_BASE + 60, MOVIE_BASE + 64, MOVIE_BASE + 68, MOVIE_BASE + 72,
};

static void movie_dispatch(Cpu *c) {
    Cpu *previous = movie_cpu; movie_cpu = c;
    void *self = (void *)(uintptr_t)c->x[0];
    uint64_t *a = c->x;
    switch ((c->pc - MOVIE_BASE) / 4) {
    case 0: destroy(self); break;
    case 1: decoder_delete(self); break;
    case 2: a[0] = configure(self, (void *)(uintptr_t)a[1]); break;
    case 3: a[0] = register_events(self, (void *)(uintptr_t)a[1]); break;
    case 4: a[0] = output_format(self, (void *)(uintptr_t)a[1]); break;
    case 5: a[0] = start(self); break;
    case 6: a[0] = stop(self); break;
    case 7: reset_trap(); break;
    case 8: a[0] = flush(self); break;
    case 9: a[0] = acquire_input(self, (void *)(uintptr_t)a[1], (void *)(uintptr_t)a[2]); break;
    case 10: a[0] = input_buffer(self, (int)a[1], (void *)(uintptr_t)a[2]); break;
    case 11: a[0] = send_input(self, (int)a[1], (int)a[2], (int)a[3], (int64_t)a[4], (uint32_t)a[5]); break;
    case 12: a[0] = acquire_output(self, (void *)(uintptr_t)a[1], (void *)(uintptr_t)a[2], (void *)(uintptr_t)a[3], (void *)(uintptr_t)a[4]); break;
    case 13: a[0] = output_buffer(self, (int)a[1], (void *)(uintptr_t)a[2]); break;
    case 14: a[0] = output_buffer_size(self, (void *)(uintptr_t)a[1], (void *)(uintptr_t)a[2]); break;
    case 15: a[0] = release_output(self, (int)a[1]); break;
    case 16: a[0] = last_error(self); break;
    case 17: a[0] = output_buffer_media(self, (int)a[1], (void *)(uintptr_t)a[2], (void *)(uintptr_t)a[3]); break;
    case 18: a[0] = output_buffer_format(self, (int)a[1], (void *)(uintptr_t)a[2]); break;
    case 19: worker(self); break;
    default: die("movie: invalid callback token");
    }
    movie_cpu = previous;
}

GuestFn movie_find_function(uint64_t address) {
    uint64_t offset = address - MOVIE_BASE;
    return !(offset & 3) && offset / 4 <= 19 ? movie_dispatch : NULL;
}

void hle__ZN5movie12SetAllocatorEPFPvmmS0_EPFvS0_S0_EPFS0_S0_mS0_ES0_(Cpu *c) {
    allocator.alloc = c->x[0]; allocator.free = c->x[1]; allocator.user = c->x[3];
    // movie::SetAllocator tail-calls this export; keep its original SDK initialization.
    c->pc = symbol("_ZN2nv2mm12SetAllocatorEPFPvmmS1_EPFvS1_S1_EPFS1_S1_mS1_ES1_");
    rt_dispatch(c);
}

void hle__ZN5movie7DecoderC1ENS_11DecoderTypeENS_19DecoderOutputFormatENS_11DecoderModeEi(Cpu *c) {
    pthread_once(&sdk_once, resolve);
    int type = (int)c->x[1], output = (int)c->x[2], mode = (int)c->x[3], priority = (int)c->x[4];
    bool texture_output = false;
#ifdef NSW_VULKAN_VIDEO
    texture_output = output == OUTPUT_TEXTURE && mode == 2;
#endif
    bool audio_output = type==TYPE_VORBIS && output==OUTPUT_PCM16 && mode==1;
    if (!audio_output && ((type != TYPE_VP9 && type != TYPE_VP8) || (!texture_output && (output != OUTPUT_NV12 || mode != 1))))
        die("movie: unsupported decoder request type=%d output=%d mode=%d", type, output, mode);
    movie_decoder *m = calloc(1, sizeof *m);
    if (!m) die("movie: decoder allocation failed");
    pthread_mutex_init(&m->lock, NULL); pthread_cond_init(&m->wake, NULL); pthread_cond_init(&m->idle, NULL);
    m->type = type; m->priority = priority; m->texture_output = texture_output;
    if (!kernel_access(c->x[0], 16, 3)) die("movie: invalid decoder object");
    dm_guest_write_prepare((void *)(uintptr_t)c->x[0], 16);
    ST64(c->x[0], (uintptr_t)decoder_vtable); ST64(c->x[0] + 8, (uintptr_t)m);
}
