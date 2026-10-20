// Adapted from Zelda-Mobile host/audio.c; Apache-2.0, see third_party/zelda-mobile/LICENSE.
#include "audio.h"
#include "kernel.h"
#include "audio_voice.h"
#include "audio_mixer.h"
#include <pthread.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <pthread/qos.h>

#include "audio_output.inc"

static uint32_t configuration[13], event_write, event_read, time_limit = 100;
static _Atomic bool running, started;
static _Atomic uint64_t rendered_frames;
static pthread_t clock_thread;
static pthread_mutex_t mixer_mutex = PTHREAD_MUTEX_INITIALIZER;
static _Atomic bool mixer_failed;
static FILE *audio_dump;
static uint64_t mixed_samples;
static float audio_peak;
static void *audio_clock(void *unused) {
    (void)unused;
    pthread_set_qos_class_self_np(QOS_CLASS_USER_INTERACTIVE, 0);
    const struct timespec period = {.tv_nsec = 5000000};
    uint64_t last = clock_gettime_nsec_np(CLOCK_MONOTONIC);
    while (atomic_load(&running)) {
        nanosleep(&period, NULL);
        uint64_t now = clock_gettime_nsec_np(CLOCK_MONOTONIC), frames = (now - last) / 5000000;
        last += frames * 5000000;
        if (frames && atomic_load(&started)) {
            // A debugger or sleep must not queue minutes of stale sound on resume.
            if (frames > 4) frames = 4;
            for (uint64_t i = 0; i < frames; i++) {
                float stereo[480];
                pthread_mutex_lock(&mixer_mutex);
                if (!atomic_load(&started)) { pthread_mutex_unlock(&mixer_mutex); break; }
                uint64_t frame = atomic_load(&rendered_frames) + 1;
                bool ok = audio_mixer_render(frame, stereo);
                if (ok) atomic_store(&rendered_frames, frame);
                if (ok && audio_unit) audio_output_push(stereo, configuration[1]);
                pthread_mutex_unlock(&mixer_mutex);
                if (!ok) { atomic_store(&mixer_failed, true); break; }
                for (unsigned n = 0; n < configuration[1] * 2; n++) {
                    mixed_samples += stereo[n] != 0; audio_peak = fmaxf(audio_peak, fabsf(stereo[n]));
                }
                if (audio_dump) fwrite(stereo, sizeof(float) * 2, configuration[1], audio_dump);
                if (!(frame % 2000)) fprintf(stderr, "AUDIO frames=%llu mixed_nonzero=%llu output_nonzero=%llu callbacks=%llu underrun_frames=%llu overrun_frames=%llu peak=%.5f\n",
                    (unsigned long long)frame, (unsigned long long)mixed_samples, (unsigned long long)atomic_load(&audio_output_samples),
                    (unsigned long long)atomic_load(&audio_callbacks), (unsigned long long)atomic_load(&audio_underruns), (unsigned long long)audio_overruns, audio_peak);
            }
            uint64_t regs[31] = {event_write};
            if (!kernel_svc(0x11, regs) || regs[0]) abort();
            if (atomic_load(&mixer_failed)) break;
        }
    }
    // CoreAudio teardown runs on this native stack, not the service caller's small guest stack.
    pthread_mutex_lock(&mixer_mutex);
    audio_output_close();
    pthread_mutex_unlock(&mixer_mutex);
    return NULL;
}
void audio_close(void) {
    if (!atomic_exchange(&running, false)) return;
    pthread_join(clock_thread, NULL);
    pthread_mutex_lock(&mixer_mutex);
    audio_mixer_close();
    pthread_mutex_unlock(&mixer_mutex);
    if (audio_dump) { fclose(audio_dump); audio_dump = NULL; }
    fprintf(stderr, "AUDIO closed frames=%llu mixed_nonzero=%llu output_nonzero=%llu callbacks=%llu peak=%.5f\n",
        (unsigned long long)atomic_load(&rendered_frames), (unsigned long long)mixed_samples,
        (unsigned long long)atomic_load(&audio_output_samples), (unsigned long long)atomic_load(&audio_callbacks), audio_peak);
    uint64_t regs[31] = {event_read}; kernel_svc(0x16, regs);
    regs[0] = event_write; kernel_svc(0x16, regs);
}
bool audio_open(const void *data, size_t size) {
    if (!data || size < sizeof configuration || atomic_load(&running)) return false;
    uint32_t candidate[13]; memcpy(candidate, data, sizeof candidate);
    if ((candidate[0] != 48000 && candidate[0] != 32000) || candidate[1] != candidate[0] / 200 ||
        !candidate[4] || candidate[4] > 1024 || candidate[5] > 256 || candidate[6] > 256) return false;
    if (!kernel_event_create(&event_write, &event_read)) return false;
    memcpy(configuration, candidate, sizeof configuration);
    if (!audio_mixer_open(candidate)) goto failed;
    if (!audio_output_open(candidate[0])) { audio_mixer_close(); goto failed; }
    const char *dump = getenv("DM_AUDIO_DUMP");
    if (dump && !(audio_dump = fopen(dump, "wb"))) { audio_output_close(); audio_mixer_close(); goto failed; }
    mixed_samples = 0; audio_peak = 0; atomic_store(&mixer_failed, false);
    atomic_store(&started, false); atomic_store(&running, true);
    atomic_store(&rendered_frames, 0);
    if (pthread_create(&clock_thread, NULL, audio_clock, NULL)) {
        atomic_store(&running, false);
        audio_output_close(); audio_mixer_close();
        if (audio_dump) { fclose(audio_dump); audio_dump = NULL; }
        goto failed;
    }
    if (atexit(audio_close)) { audio_close(); return false; }
    fprintf(stderr, "AUDIO renderer initialized rate=%u channels=2 mode=%s\n", candidate[0], audio_unit ? "CoreAudio" : "muted");
    return true;
failed:;
    uint64_t regs[31] = {event_read}; kernel_svc(0x16, regs);
    regs[0] = event_write; kernel_svc(0x16, regs); return false;
}
uint32_t audio_event(void) { return atomic_load(&running) ? kernel_event_copy(event_read) : 0; }
uint32_t audio_value(unsigned command) {
    if (command <= 2) return configuration[command];
    if (command == 3) return atomic_load(&started) ? 0 : 1;
    if (command == 9) return time_limit;
    return UINT32_MAX;
}
bool audio_set(unsigned command, uint32_t value) {
    if (command == 5 || command == 6) {
        bool on = command == 5;
        pthread_mutex_lock(&mixer_mutex);
        if (atomic_load(&started) == on) { pthread_mutex_unlock(&mixer_mutex); return true; }
        if (on) {
            // Do not render ahead while CoreAudio's initial start is still blocking.
            static const float silence[1440] = {0};
            if (audio_unit) {
                audio_output_push(silence, configuration[1] * 3);
                if (AudioOutputUnitStart(audio_unit)) { pthread_mutex_unlock(&mixer_mutex); return false; }
                audio_output_push(silence, configuration[1] * 3);
            }
            atomic_store(&started, true);
        } else {
            atomic_store(&started, false);
            if (audio_unit && AudioOutputUnitStop(audio_unit)) { atomic_store(&started, true); pthread_mutex_unlock(&mixer_mutex); return false; }
            atomic_store(&audio_read_cursor, atomic_load(&audio_write_cursor));
        }
        pthread_mutex_unlock(&mixer_mutex);
        return true;
    }
    if (command == 8 && value <= 100) { time_limit = value; return true; }
    return false;
}

static uint32_t read32(const uint8_t *p) { uint32_t value; memcpy(&value, p, 4); return value; }
static size_t update_locked(const void *input, size_t length, void *output, size_t capacity) {
    if (!atomic_load(&running)) { fputs("AUDIO update rejected: renderer is shutting down or closed\n", stderr); return 0; }
    if (atomic_load(&mixer_failed)) { fputs("AUDIO update rejected: mixer previously failed\n", stderr); return 0; }
    if (length < 64 || !input || !output) return 0;
    uint32_t h[16]; memcpy(h, input, sizeof h);
    const uint8_t *data = input;
    size_t total = 64;
    for (unsigned i = 1; i < 15; i++) { if (h[i] > length - total) return 0; total += h[i]; }
    unsigned pools = configuration[4] * 4 + configuration[6];
    if (total != h[15] || h[0] != 0x3f564552 || h[0] != configuration[12] || h[1] != 16 ||
        h[2] != pools * 32 || h[3] != configuration[4] * 0x188 || h[4] != configuration[4] * 0x70 ||
        h[5] != configuration[6] * 0xc0 || h[7] != configuration[5] * 0x140 || h[8] != 16) return 0;
    uint32_t out[16] = {h[0], 0xb0, pools * 16, configuration[4] * 16, 0,
        configuration[6] * 0x90, 0, configuration[5] * 32, 16, 0, 16};
    size_t output_size = 64;
    for (unsigned i = 1; i < 15; i++) output_size += out[i];
    if (output_size > capacity) return 0;
    out[15] = (uint32_t)output_size;
    uint64_t frames = atomic_load(&rendered_frames);
    uint8_t *result = output; memset(result, 0, output_size); memcpy(result, out, sizeof out);
    if (!audio_mixer_update(data, h, frames, result + 64 + out[2])) {
        fprintf(stderr, "AUDIO rejected routing frame=%llu mix_bytes=%u splitter_bytes=%u\n", (unsigned long long)frames, h[6], h[9]);
        return 0;
    }
    for (unsigned i = 0; i < pools; i++) {
        uint32_t state = read32(data + 64 + h[1] + i * 32 + 16);
        if (state > 6) return 0;
        state = state == 4 ? 5 : state == 2 ? 3 : 0;
        memcpy(result + 64 + i * 16, &state, 4);
    }
    size_t effects = 64 + out[2] + out[3];
    audio_mixer_effect_status(result + effects);
    memcpy(result + output_size - 16, &frames, sizeof frames);
    return output_size;
}
size_t audio_update(const void *input, size_t length, void *output, size_t capacity) {
    pthread_mutex_lock(&mixer_mutex);
    size_t result = update_locked(input, length, output, capacity);
    pthread_mutex_unlock(&mixer_mutex);
    return result;
}
void audio_volume(float value) {
    pthread_mutex_lock(&mixer_mutex); audio_mixer_volume(value); pthread_mutex_unlock(&mixer_mutex);
}
