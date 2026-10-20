// Adapted from Zelda-Mobile host/audio_voice.h; Apache-2.0, see third_party/zelda-mobile/LICENSE.
#pragma once
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include <math.h>

// REV15 wire layout: VoiceInParameter2 has two 24-byte biquad parameters.
typedef struct {
    uint64_t address, size;
    uint32_t start, end;
    uint8_t loop, end_of_stream, sent, reserved;
    int32_t loops;
    uint64_t context, context_size;
    uint32_t loop_start, loop_end;
} audio_wave;
_Static_assert(sizeof(audio_wave) == 0x38, "AudioRenderer wave buffer ABI");
typedef struct {
    audio_wave waves[4];
    bool used, ready[4];
    uint8_t state, head;
    uint16_t flags;
    uint32_t rate, offset, consumed;
    uint64_t frame, played, loops;
    double pitch, fraction;
} audio_voice;

static inline uint32_t audio_u32(const void *p) { uint32_t n; memcpy(&n, p, 4); return n; }
static inline void audio_consume(audio_voice *v) {
    if (v->waves[v->head].end_of_stream) v->played = 0;
    v->ready[v->head] = false; v->head = (v->head + 1) % 4;
    v->offset = 0; v->loops = 0; v->consumed++;
}
static inline void audio_advance_samples(audio_voice *v, uint64_t remaining) {
    while (remaining && v->ready[v->head]) {
        const audio_wave *w = &v->waves[v->head];
        uint32_t length = w->end - w->start;
        if (v->loops && w->loop_start && w->loop_end) length = w->loop_end - w->loop_start;
        uint64_t count = length - v->offset;
        if (count > remaining) count = remaining;
        remaining -= count; v->offset += (uint32_t)count; v->played += count;
        if (v->offset == length) {
            if (w->loop && (w->loops < 0 || v->loops < (uint32_t)w->loops)) {
                v->offset = 0; v->loops++;
                if (v->flags & 1) v->played = 0;
                // Skip complete loops arithmetically, including long host scheduling delays.
                length = w->loop_start && w->loop_end ? w->loop_end - w->loop_start : w->end - w->start;
                uint64_t loops = remaining / length;
                if (w->loops >= 0 && loops > (uint32_t)w->loops - v->loops) loops = (uint32_t)w->loops - v->loops;
                remaining -= loops * length; v->loops += loops;
                if (!(v->flags & 1)) v->played += loops * length;
            } else audio_consume(v);
        }
    }
    if (!v->ready[v->head]) v->fraction = 0; // Underrun time must not consume a subsequently queued buffer.
}
static inline void audio_advance(audio_voice *v, uint64_t frame, uint32_t renderer_rate) {
    uint64_t elapsed = frame - v->frame; v->frame = frame;
    if (!v->used || v->state || !elapsed) return;
    double samples = v->fraction + elapsed * ((v->flags & 2) ? renderer_rate : v->rate * v->pitch) / 200.0;
    uint64_t remaining = (uint64_t)samples; v->fraction = samples - remaining;
    audio_advance_samples(v, remaining);
}
static inline bool audio_voice_update(audio_voice *v, const uint8_t *p, uint64_t frame, uint32_t renderer_rate) {
    if (!p[9]) { *v = (audio_voice){.frame = frame}; return true; }
    uint32_t rate = audio_u32(p + 12), channels = audio_u32(p + 24);
    uint32_t count = audio_u32(p + 0x54), head = audio_u32(p + 0x58);
    float pitch; memcpy(&pitch, p + 28, 4);
    uint16_t flags; memcpy(&flags, p + 0x174, 2);
    if (p[8] > 1 || p[9] > 1 || p[10] > 2 || count > 4 || head > 3 ||
        rate < 8000 || rate > 192000 || !channels || channels > 6 ||
        !isfinite(pitch) || pitch < 0 || pitch > 8 || flags > 3 || p[0x171] > 4 ||
        (p[11] != 2 && p[11] != 5 && p[11] != 6) || (p[11] == 6 && channels != 1)) return false;
    audio_wave incoming[4]; memcpy(incoming, p + 0x78, sizeof incoming);
    for (unsigned i = 0; i < 4; i++) {
        const audio_wave *w = &incoming[i];
        if (w->sent > 1) return false;
        if (w->sent) continue;
        uint64_t samples = p[11] == 6 ? w->size / 8 * 14 : w->size / channels / (p[11] == 2 ? 2 : 4);
        // An ADPCM tail has one header byte and up to seven bytes holding two samples each.
        if (p[11] == 6 && w->size % 8) samples += (w->size % 8 - 1) * 2;
        if (!w->address || !w->size || w->size > (1u << 30) || w->address > UINT64_MAX - w->size ||
            w->start >= w->end || w->end > samples || w->loop > 1 || w->end_of_stream > 1 || w->loops < -1 ||
            (w->loop_start && w->loop_end && (w->loop_start >= w->loop_end || w->loop_end > samples))) return false;
    }
    if (p[8] || !v->used) *v = (audio_voice){.used = true, .frame = frame, .head = head};
    else audio_advance(v, frame, renderer_rate);
    for (unsigned i = 0; i < 4; i++) if (!incoming[i].sent) {
        if (v->ready[i]) return false;
        v->waves[i] = incoming[i]; v->ready[i] = true;
    }
    for (unsigned i = 0; i < p[0x171]; i++) audio_consume(v);
    if (p[10] == 1 && v->state != 1) {
        for (unsigned i = 0; i < 4; i++) if (v->ready[v->head]) audio_consume(v);
        v->played = 0; v->fraction = 0;
    }
    v->state = p[10]; v->flags = flags; v->rate = rate; v->pitch = pitch;
    return true;
}
