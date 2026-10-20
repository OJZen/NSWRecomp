// Adapted from Zelda-Mobile host/audio_mixer.c; Apache-2.0, see third_party/zelda-mobile/LICENSE.
#include "audio_mixer.h"
#include "audio_voice.h"
#include <mach/mach.h>
#include "mach_memory.h"
#include <stdlib.h>
#include <stdio.h>

enum { CHANNELS = 24, SAMPLES = 240, UNUSED_MIX = 0x7fffffff };
typedef struct { uint8_t enabled, pad[3]; float b[3], a[2]; } biquad;
typedef struct { float z[2]; } filter_state;
typedef struct {
    audio_voice cursor;
    uint8_t *data[4]; uint64_t bytes[4]; int16_t context[4][3], coefficients[16];
    uint32_t channels, format, mix, splitter, resources[6];
    uint8_t predictor; int16_t history[2];
    biquad filters[2]; filter_state filter[6][2];
    float volume, previous_volume, previous[6], current[6];
} source;
typedef struct { uint32_t id; float volume[24]; uint8_t used, pad[11]; } channel;
typedef struct {
    float volume; uint32_t rate, count; uint8_t used, dirty, pad[2];
    uint32_t id, effects, node, reserved[2];
    float matrix[24][24]; uint32_t destination, splitter, reserved2;
} mix;
typedef struct { uint32_t count, destinations[24]; } splitter;
typedef struct {
    uint32_t magic, id; float volume[24]; uint32_t mix;
    biquad filters[2]; uint8_t used, reset, pad[10];
} destination;
typedef struct { uint64_t address, size; uint32_t state, pad[3]; } pool;
_Static_assert(sizeof(biquad) == 24 && sizeof(channel) == 0x70 && sizeof(mix) == 0x930 && sizeof(destination) == 0xa8, "REV15 audio routing ABI");

static uint32_t config[13], pool_count, mix_count;
static source *sources;
static channel *channels;
static float (*previous_channel)[24];
static mix *mixes;
static splitter *splitters;
static destination *destinations;
static float (*previous_destination)[24];
static filter_state (*destination_filters)[2];
static pool *pools;
static float *buffers;
static uint32_t *mix_offsets, *mix_order;
static uint8_t *effects, *sinks;
static size_t wave_bytes;
static float device_volume = 1;
static void close_effects(void);
static void update_effects(const uint8_t *parameters);

static uint64_t u64(const void *p) { uint64_t value; memcpy(&value, p, 8); return value; }
static float f32(const void *p) { float value; memcpy(&value, p, 4); return value; }
static bool volume_valid(float v) { return isfinite(v) && fabsf(v) <= 128; }
static bool filters_valid(const biquad filters[2]) {
    for (unsigned i = 0; i < 2; i++) {
        if (filters[i].enabled > 1) return false;
        if (filters[i].enabled) {
            for (unsigned j = 0; j < 3; j++) if (!volume_valid(filters[i].b[j])) return false;
            for (unsigned j = 0; j < 2; j++) if (!volume_valid(filters[i].a[j])) return false;
        }
    }
    return true;
}
static float filter_sample(float x, const biquad *p, filter_state *s) {
    if (!p->enabled) { *s = (filter_state){0}; return x; }
    float y = x * p->b[0] + s->z[0];
    s->z[0] = x * p->b[1] + y * p->a[0] + s->z[1];
    s->z[1] = x * p->b[2] + y * p->a[1];
    return y;
}
static bool memory_range(uint64_t address, uint64_t size) {
    if (!address || !size || address > UINT64_MAX - size) return false;
    for (unsigned i = 0; i < pool_count; i++) {
        const pool *p = &pools[i];
        if (p->state >= 4 && p->state <= 5 && address >= p->address && size <= p->size && address - p->address <= p->size - size) return true;
    }
    return false;
}
static bool read_memory(void *out, uint64_t address, size_t size) {
    mach_vm_size_t got = 0;
    return memory_range(address, size) && !mach_vm_read_overwrite(mach_task_self(), address, size, (mach_vm_address_t)out, &got) && got == size;
}
static void release_wave(source *s, unsigned slot) {
    free(s->data[slot]); s->data[slot] = NULL;
    wave_bytes -= s->bytes[slot]; s->bytes[slot] = 0;
}
static void release_source(source *s) {
    for (unsigned i = 0; i < 4; i++) release_wave(s, i);
    memset(s, 0, sizeof *s);
}
void audio_mixer_close(void) {
    close_effects();
    if (sources) for (unsigned i = 0; i < config[4]; i++) release_source(&sources[i]);
    free(sources); free(channels); free(previous_channel); free(mixes); free(splitters); free(destinations);
    free(previous_destination); free(destination_filters); free(pools); free(buffers); free(mix_offsets); free(mix_order);
    free(effects); free(sinks); sources = NULL; channels = NULL; previous_channel = NULL; mixes = NULL;
    splitters = NULL; destinations = NULL; previous_destination = NULL; destination_filters = NULL;
    pools = NULL; buffers = NULL; mix_offsets = NULL; mix_order = NULL; effects = sinks = NULL;
}
bool audio_mixer_open(const uint32_t candidate[13]) {
    if (candidate[2] > 1536 || candidate[3] >= 64 || candidate[9] > 1024 || candidate[10] > 4096) return false;
    memcpy(config, candidate, sizeof config); mix_count = config[3] + 1; pool_count = config[4] * 4 + config[6];
    sources = calloc(config[4], sizeof *sources); channels = calloc(config[4], sizeof *channels);
    previous_channel = calloc(config[4], sizeof *previous_channel); mixes = calloc(mix_count, sizeof *mixes);
    splitters = calloc(config[9] + 1, sizeof *splitters); destinations = calloc(config[10] + 1, sizeof *destinations);
    previous_destination = calloc(config[10] + 1, sizeof *previous_destination);
    destination_filters = calloc(config[10] + 1, sizeof *destination_filters);
    pools = calloc(pool_count, sizeof *pools); buffers = calloc((config[2] + 1) * SAMPLES, sizeof *buffers);
    mix_offsets = calloc(mix_count, sizeof *mix_offsets); mix_order = calloc(mix_count, sizeof *mix_order);
    effects = calloc(config[6] + 1, 0xc0); sinks = calloc(config[5] + 1, 0x140); wave_bytes = 0;
    if (!sources || !channels || !previous_channel || !mixes || !splitters || !destinations || !previous_destination ||
        !destination_filters || !pools || !buffers || !mix_offsets || !mix_order || !effects || !sinks) { audio_mixer_close(); return false; }
    for (unsigned i = 0; i < mix_count; i++) { mixes[i].destination = UNUSED_MIX; mixes[i].splitter = UINT32_MAX; }
    return true;
}
void audio_mixer_volume(float volume) { device_volume = volume; }

static bool source_update(source *s, const uint8_t *p, uint64_t frame) {
    if (!p[9] || p[8] || !s->cursor.used) release_source(s);
    if (!audio_voice_update(&s->cursor, p, frame, config[0])) return false;
    if (!p[9]) return true;
    s->channels = audio_u32(p + 24); s->format = p[11]; s->volume = f32(p + 32);
    memcpy(s->filters, p + 36, sizeof s->filters); memcpy(s->resources, p + 0x158, sizeof s->resources);
    s->mix = audio_u32(p + 0x70); s->splitter = audio_u32(p + 0x74);
    if (!volume_valid(s->volume) || !filters_valid(s->filters) || (s->mix != UNUSED_MIX && s->mix >= mix_count) ||
        (s->mix == UNUSED_MIX && s->splitter != UINT32_MAX && s->splitter >= config[9])) return false;
    for (unsigned c = 0; c < s->channels; c++) if (s->resources[c] >= config[4]) return false;
    bool queued = false;
    for (unsigned i = 0; i < 4; i++) queued |= s->cursor.ready[i];
    if (s->format == 6 && queued && (u64(p + 0x68) < 32 || !read_memory(s->coefficients, u64(p + 0x60), 32))) return false;
    for (unsigned i = 0; i < 4; i++) {
        if (!s->cursor.ready[i]) { release_wave(s, i); continue; }
        const audio_wave *incoming = (const audio_wave *)(const void *)(p + 0x78 + i * 0x38);
        if (incoming->sent) continue;
        release_wave(s, i);
        if (incoming->size > (32u << 20) || wave_bytes + incoming->size > (128u << 20)) return false;
        s->data[i] = malloc(incoming->size);
        if (!s->data[i] || !read_memory(s->data[i], incoming->address, incoming->size)) return false;
        s->bytes[i] = incoming->size; wave_bytes += incoming->size;
        s->cursor.waves[i].address = (uintptr_t)s->data[i];
        if (incoming->context) {
            if (incoming->context_size < 6 || !read_memory(s->context[i], incoming->context, 6)) return false;
            s->cursor.waves[i].context = (uintptr_t)s->context[i];
        }
    }
    return true;
}
static bool source_sample(source *s, float out[6]) {
    audio_voice *v = &s->cursor;
    if (!v->ready[v->head]) { memset(out, 0, 6 * sizeof *out); return true; }
    const audio_wave *w = &v->waves[v->head];
    uint32_t start = v->loops && w->loop_start && w->loop_end ? w->loop_start : w->start;
    uint32_t index = start + v->offset;
    const uint8_t *data = (const void *)(uintptr_t)w->address;
    if (s->format == 6) {
        if (!v->offset && w->context) {
            const int16_t *context = (const void *)(uintptr_t)w->context;
            s->predictor = (uint8_t)context[0]; s->history[0] = context[1]; s->history[1] = context[2];
        }
        unsigned in_block = index % 14;
        if (!in_block) s->predictor = data[index / 14 * 8];
        unsigned coefficient = s->predictor >> 4;
        if (coefficient >= 8) return false;
        int nibble = data[index / 14 * 8 + 1 + in_block / 2];
        nibble = in_block & 1 ? nibble & 15 : nibble >> 4;
        if (nibble >= 8) nibble -= 16;
        int64_t value = (int64_t)nibble * (1 << (s->predictor & 15)) * 2048 + 1024 +
            (int64_t)s->coefficients[coefficient * 2] * s->history[0] +
            (int64_t)s->coefficients[coefficient * 2 + 1] * s->history[1];
        value >>= 11; if (value > 32767) value = 32767; if (value < -32768) value = -32768;
        s->history[1] = s->history[0]; s->history[0] = (int16_t)value; out[0] = value;
    } else for (unsigned c = 0; c < s->channels; c++) {
        if (s->format == 2) { int16_t value; memcpy(&value, data + (index * s->channels + c) * 2, 2); out[c] = value; }
        else { out[c] = f32(data + (index * s->channels + c) * 4) * 32768.0f; if (!isfinite(out[c])) return false; }
    }
    unsigned slot = v->head; audio_advance_samples(v, 1);
    if (!v->ready[slot]) release_wave(s, slot);
    return true;
}
static bool source_render(source *s, float output[6][SAMPLES], uint64_t frame) {
    audio_voice *v = &s->cursor; v->frame = frame;
    memset(output, 0, 6 * SAMPLES * sizeof(float));
    if (!v->used || v->state) return true;
    double ratio = v->flags & 2 ? 1 : v->rate * v->pitch / config[0];
    for (unsigned i = 0; i < config[1]; i++) {
        float values[6] = {0};
        if (ratio == 1 && !v->fraction) { if (!source_sample(s, values)) return false; }
        else {
            // shortcut: causal linear SRC; replace with the console polyphase filter for bit-exact resampling.
            v->fraction += ratio;
            while (v->fraction >= 1) {
                v->fraction -= 1; memcpy(s->previous, s->current, sizeof s->current);
                if (!source_sample(s, s->current)) return false;
            }
            for (unsigned c = 0; c < s->channels; c++) values[c] = s->previous[c] + (s->current[c] - s->previous[c]) * v->fraction;
        }
        float volume = s->previous_volume + (s->volume - s->previous_volume) * (float)(i + 1) / config[1];
        for (unsigned c = 0; c < s->channels; c++) {
            float value = values[c];
            for (unsigned b = 0; b < 2; b++) value = filter_sample(value, &s->filters[b], &s->filter[c][b]);
            output[c][i] = value * volume;
        }
    }
    s->previous_volume = s->volume; return true;
}

static bool update_splitters(const uint8_t *p, size_t size) {
    if (!size) return true;
    if (size < 32 || audio_u32(p) != 0x48444e53) return false;
    uint32_t count = audio_u32(p + 4), dest_count = audio_u32(p + 8); size_t at = 32;
    if (count > config[9] || dest_count > config[10]) return false;
    for (unsigned i = 0; i < count; i++) {
        if (size - at < 16 || audio_u32(p + at) != 0x49444e53) return false;
        uint32_t id = audio_u32(p + at + 4), rate = audio_u32(p + at + 8), n = audio_u32(p + at + 12); at += 16;
        if (id >= config[9] || rate != config[0] || n > 24 || n * 4 > size - at) return false;
        splitters[id].count = n;
        for (unsigned j = 0; j < n; j++) {
            uint32_t destination_id = audio_u32(p + at); at += 4;
            if (destination_id >= config[10]) return false;
            splitters[id].destinations[j] = destination_id;
        }
        // Each variable-length splitter ends with 12 reserved bytes, even with no destinations.
        if (size - at < 12) return false;
        at += 12;
    }
    if (dest_count * sizeof(destination) > size - at) return false;
    for (unsigned i = 0; i < dest_count; i++, at += sizeof(destination)) {
        destination d; memcpy(&d, p + at, sizeof d);
        if (d.magic != 0x44444e53 || d.id >= config[10] || d.used > 1 || !filters_valid(d.filters) ||
            (d.mix != UNUSED_MIX && d.mix >= mix_count)) return false;
        for (unsigned c = 0; c < 24; c++) if (!volume_valid(d.volume[c])) return false;
        if (d.reset) memset(previous_destination[d.id], 0, sizeof previous_destination[d.id]);
        destinations[d.id] = d;
    }
    return (at + 15) / 16 * 16 == size;
}
static void mix_edges(uint32_t id, uint8_t edges[64][64]) {
    const mix *m = &mixes[id];
    if (!id || !m->used) return;
    if (m->destination != UNUSED_MIX) edges[id][m->destination] = 1;
    else if (m->splitter != UINT32_MAX) {
        const splitter *s = &splitters[m->splitter];
        for (unsigned i = 0; i < s->count; i++) {
            const destination *d = &destinations[s->destinations[i]];
            if (d->used && d->mix != UNUSED_MIX) edges[id][d->mix] = 1;
        }
    }
}
static bool order_mixes(void) {
    uint8_t edges[64][64] = {{0}}, done[64] = {0}; unsigned offset = 0;
    for (unsigned i = 0; i < mix_count; i++) {
        mix_offsets[i] = offset; if (mixes[i].used) offset += mixes[i].count;
        mix_edges(i, edges);
    }
    if (offset > config[2]) return false;
    for (unsigned n = 0; n < mix_count; n++) {
        unsigned chosen = mix_count;
        for (unsigned i = 0; i < mix_count; i++) if (!done[i]) {
            bool incoming = false;
            for (unsigned j = 0; j < mix_count; j++) if (!done[j] && edges[j][i]) incoming = true;
            if (!incoming) { chosen = i; break; }
        }
        if (chosen == mix_count) return false;
        done[chosen] = 1; mix_order[n] = chosen;
    }
    return true;
}
bool audio_mixer_update(const uint8_t *data, const uint32_t h[16], uint64_t frame, uint8_t *voice_status) {
    memcpy(pools, data + 64 + h[1], pool_count * sizeof *pools);
    for (unsigned i = 0; i < pool_count; i++) if (pools[i].state > 6 || pools[i].address > UINT64_MAX - pools[i].size) return false;
    size_t at = 64 + h[1] + h[2]; memcpy(channels, data + at, config[4] * sizeof *channels); at += h[4];
    for (unsigned i = 0; i < config[4]; i++) for (unsigned c = 0; c < 24; c++) if (!volume_valid(channels[i].volume[c])) return false;
    for (unsigned i = 0; i < config[4]; i++, at += 0x188) {
        if (!source_update(&sources[i], data + at, frame)) { fprintf(stderr, "AUDIO invalid source=%u\n", i); return false; }
        memcpy(voice_status + i * 16, &sources[i].cursor.played, 8);
        memcpy(voice_status + i * 16 + 8, &sources[i].cursor.consumed, 4);
    }
    for (unsigned i = 0; i < config[6]; i++) {
        const uint8_t *e = data + at + i * 0xc0;
        if (e[0] && ((e[0] != 2 && e[0] != 5 && e[0] != 7) || e[2] > 1 || audio_u32(e + 4) >= mix_count || audio_u32(e + 24) >= config[6])) {
            fprintf(stderr, "AUDIO invalid effect=%u type=%u\n", i, e[0]); return false;
        }
    }
    update_effects(data + at); memcpy(effects, data + at, h[5]); at += h[5];
    if (!update_splitters(data + at, h[9])) { fprintf(stderr, "AUDIO invalid splitter update\n"); return false; }
    at += h[9];
    if (h[6]) {
        if (h[6] < 32) return false;
        uint32_t n = audio_u32(data + at + 4); at += 32;
        if (n > mix_count || h[6] != 32 + n * sizeof(mix)) return false;
        for (unsigned i = 0; i < n; i++, at += sizeof(mix)) {
            mix m; memcpy(&m, data + at, sizeof m);
            if (m.id >= mix_count || m.used > 1 || m.count > 24 || !volume_valid(m.volume) ||
                (m.used && m.rate != config[0]) || (m.destination != UNUSED_MIX && m.destination >= mix_count) ||
                (m.splitter != UINT32_MAX && m.splitter >= config[9])) return false;
            for (unsigned r = 0; r < 24; r++) for (unsigned c = 0; c < 24; c++) if (!volume_valid(m.matrix[r][c])) return false;
            mixes[m.id] = m;
        }
    }
    memcpy(sinks, data + at, h[7]);
    return order_mixes();
}
static float *mix_buffer(uint32_t mix_id, unsigned channel_id) { return buffers + (mix_offsets[mix_id] + channel_id) * SAMPLES; }
static void add_buffer(float *out, const float *in, float old_volume, float volume) {
    if (!old_volume && !volume) return;
    for (unsigned i = 0; i < config[1]; i++) out[i] += in[i] * (old_volume + (volume - old_volume) * (float)(i + 1) / config[1]);
}
static void route_splitter(uint32_t id, unsigned channel_id, unsigned channel_count, const float *in, float volume) {
    if (id == UINT32_MAX) return;
    const splitter *split = &splitters[id];
    for (unsigned n = channel_id; n < split->count; n += channel_count) {
        uint32_t di = split->destinations[n]; const destination *d = &destinations[di];
        if (!d->used || d->mix == UNUSED_MIX || !mixes[d->mix].used) continue;
        float filtered[SAMPLES];
        for (unsigned i = 0; i < config[1]; i++) {
            filtered[i] = in[i];
            for (unsigned b = 0; b < 2; b++) filtered[i] = filter_sample(filtered[i], &d->filters[b], &destination_filters[di][b]);
        }
        for (unsigned c = 0; c < mixes[d->mix].count; c++) add_buffer(mix_buffer(d->mix, c), filtered, previous_destination[di][c] * volume, d->volume[c] * volume);
    }
}

#include "audio_effects.inc"

bool audio_mixer_render(uint64_t frame, float *stereo) {
    memset(buffers, 0, config[2] * SAMPLES * sizeof *buffers); memset(stereo, 0, config[1] * 2 * sizeof *stereo);
    for (unsigned i = 0; i < config[4]; i++) {
        source *s = &sources[i]; float decoded[6][SAMPLES];
        if (!source_render(s, decoded, frame)) { fprintf(stderr, "AUDIO decode failed source=%u\n", i); return false; }
        if (!s->cursor.used || s->cursor.state) continue;
        for (unsigned c = 0; c < s->channels; c++) {
            if (s->mix == UNUSED_MIX) route_splitter(s->splitter, c, s->channels, decoded[c], 1);
            else if (mixes[s->mix].used) {
                unsigned resource = s->resources[c];
                for (unsigned d = 0; d < mixes[s->mix].count; d++) add_buffer(mix_buffer(s->mix, d), decoded[c], previous_channel[resource][d], channels[resource].volume[d]);
            }
        }
    }
    for (unsigned n = 0; n < mix_count; n++) {
        unsigned id = mix_order[n]; const mix *m = &mixes[id];
        if (!m->used) continue;
        if (!process_effects(id)) return false;
        if (!id) continue;
        for (unsigned c = 0; c < m->count; c++) {
            if (m->destination == UNUSED_MIX) route_splitter(m->splitter, c, m->count, mix_buffer(id, c), m->volume);
            else if (mixes[m->destination].used) for (unsigned d = 0; d < mixes[m->destination].count; d++)
                add_buffer(mix_buffer(m->destination, d), mix_buffer(id, c), m->volume * m->matrix[c][d], m->volume * m->matrix[c][d]);
        }
    }
    for (unsigned i = 0; i < config[5]; i++) {
        const uint8_t *p = sinks + i * 0x140;
        if (!p[1]) continue;
        uint32_t count = audio_u32(p + 0x120);
        if (p[0] != 1 || (count != 1 && count != 2 && count != 6) || !mixes[0].used) return false;
        for (unsigned c = 0; c < count; c++) if (p[0x124 + c] >= mixes[0].count) return false;
        float coefficients[4] = {1, 0.70710678f, 0, 0.70710678f};
        if (p[0x12b]) memcpy(coefficients, p + 0x12c, sizeof coefficients);
        for (unsigned c = 0; c < 4; c++) if (!volume_valid(coefficients[c])) return false;
        for (unsigned j = 0; j < config[1]; j++) {
            float samples[6] = {0};
            for (unsigned c = 0; c < count; c++) samples[c] = mix_buffer(0, p[0x124 + c])[j];
            if (count == 1) samples[1] = samples[0];
            if (count == 6) {
                samples[0] = samples[0] * coefficients[0] + samples[2] * coefficients[1] + samples[3] * coefficients[2] + samples[4] * coefficients[3];
                samples[1] = samples[1] * coefficients[0] + samples[2] * coefficients[1] + samples[3] * coefficients[2] + samples[5] * coefficients[3];
            }
            for (unsigned c = 0; c < 2; c++) stereo[j * 2 + c] += samples[c] * mixes[0].volume * device_volume / 32768.0f;
        }
    }
    for (unsigned i = 0; i < config[1] * 2; i++) {
        if (!isfinite(stereo[i])) return false;
        stereo[i] = fmaxf(-1, fminf(1, stereo[i]));
    }
    for (unsigned i = 0; i < config[4]; i++) memcpy(previous_channel[i], channels[i].volume, sizeof previous_channel[i]);
    for (unsigned i = 0; i < config[10]; i++) memcpy(previous_destination[i], destinations[i].volume, sizeof previous_destination[i]);
    return true;
}
