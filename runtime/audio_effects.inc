// Adapted from Zelda-Mobile host/audio_effects.inc; Apache-2.0, see third_party/zelda-mobile/LICENSE.
// Reverb3D/limiter equations and tables follow Ryujinx.Audio (MIT; see THIRD_PARTY_NOTICES.md).
typedef struct { float *data; uint32_t size, delay, read, write; } delay_line;
typedef struct {
    uint8_t input[6], output[6]; uint16_t maximum, count; uint32_t reserved, rate;
    float room_hf, hf_reference, decay, hf_ratio, room_gain, reflections_gain, reverb_gain;
    float diffusion, reflection_delay, reverb_delay, density, dry_gain; uint32_t status;
} reverb_parameters;
typedef struct {
    delay_line fdn[4], decay1[4], decay2[4], pre, center;
    reverb_parameters parameters;
    uint32_t early_delay[20], reflection_delay;
    float early_gain, late_gain, previous_pre_gain, pre_gain, previous_pre, echo;
    float direct[4], current[4], feedback[4], previous[4];
} reverb_state;
typedef struct {
    uint8_t input[6], output[6]; uint16_t maximum, count;
    int32_t rate, lookahead_max, attack_time, release_time, lookahead;
    float attack, release, threshold, input_gain, output_gain;
    int32_t delay_min, delay_max; uint8_t status, statistics, reset_statistics, pad;
} limiter_parameters;
typedef struct {
    reverb_state *reverb;
    float *delay, detector[6], gain[6], input_peak[6], gain_min[6]; uint32_t delay_size, position[6];
} effect_state;
static effect_state effect_states[256];
static void free_effect(effect_state *s) {
    if (s->reverb) {
        for (unsigned j = 0; j < 4; j++) { free(s->reverb->fdn[j].data); free(s->reverb->decay1[j].data); free(s->reverb->decay2[j].data); }
        free(s->reverb->pre.data); free(s->reverb->center.data); free(s->reverb);
    }
    free(s->delay); memset(s, 0, sizeof *s);
}
static void close_effects(void) { for (unsigned i = 0; i < 256; i++) free_effect(&effect_states[i]); }
static void update_effects(const uint8_t *p) {
    for (unsigned i = 0; i < config[6]; i++) {
        if (p[i * 0xc0] != effects[i * 0xc0] || p[i * 0xc0 + 1]) free_effect(&effect_states[i]);
        if (p[i * 0xc0] == 7 && p[i * 0xc0 + 32 + 66]) for (unsigned c = 0; c < 6; c++) {
            effect_states[i].input_peak[c] = 0; effect_states[i].gain_min[c] = 1;
        }
    }
}
void audio_mixer_effect_status(uint8_t *output) {
    for (unsigned i = 0; i < config[6]; i++) {
        uint8_t *p = output + i * 0x90; const uint8_t *e = effects + i * 0xc0;
        p[0] = e[0] && e[2] ? 3 : 4;
        if (e[0] == 7) { memcpy(p + 16, effect_states[i].input_peak, 24); memcpy(p + 40, effect_states[i].gain_min, 24); }
    }
}
static bool write_memory(uint64_t address, const void *data, size_t size) {
    if (!memory_range(address, size)) return false;
    extern void dm_guest_write_prepare(void *, size_t);
    dm_guest_write_prepare((void *)(uintptr_t)address, size);
    return !mach_vm_write(mach_task_self(), address, (vm_offset_t)data, (mach_msg_type_number_t)size);
}
static bool effect_channels(const uint8_t *input, const uint8_t *output, unsigned count, unsigned id) {
    if (!count || count > 6) return false;
    for (unsigned c = 0; c < count; c++) if (input[c] >= mixes[id].count || output[c] >= mixes[id].count) return false;
    return true;
}
static bool auxiliary_effect(const uint8_t *p, bool enabled, unsigned id) {
    uint32_t count = audio_u32(p + 48), capacity = audio_u32(p + 56);
    uint64_t send = u64(p + 64), receive = u64(p + 80);
    if (!effect_channels(p, p + 24, count, id) || audio_u32(p + 52) != config[0] ||
        capacity < count * config[1] || capacity > (1u << 20)) return false;
    if (!enabled) {
        const uint32_t zero[2] = {0};
        if (send && (!write_memory(send + 0x40, zero, 8) || !write_memory(send + 0x4c, zero, 4))) return false;
        if (receive && (!write_memory(receive + 0x40, zero, 8) || !write_memory(receive + 0x4c, zero, 4))) return false;
        for (unsigned c = 0; c < count; c++) if (p[c] != p[24 + c]) memcpy(mix_buffer(id, p[24 + c]), mix_buffer(id, p[c]), config[1] * sizeof(float));
        return true;
    }
    uint32_t write, read;
    if (!read_memory(&write, send + 0x44, 4) || !read_memory(&read, receive + 0x40, 4) || write >= capacity || read >= capacity) return false;
    for (unsigned c = 0; c < count; c++) {
        int32_t samples[SAMPLES]; const float *input = mix_buffer(id, p[c]);
        float *output = mix_buffer(id, p[24 + c]);
        for (unsigned n = 0; n < config[1]; n++) {
            if (!isfinite(input[n])) return false;
            samples[n] = (int32_t)fmax(-2147483648.0, fmin(2147483647.0, trunc(input[n])));
        }
        uint32_t position = (write + c * config[1]) % capacity;
        uint32_t first = config[1] < capacity - position ? config[1] : capacity - position;
        if (!write_memory(send + 0x80 + position * 4, samples, first * 4) ||
            (first < config[1] && !write_memory(send + 0x80, samples + first, (config[1] - first) * 4))) return false;
        position = (read + c * config[1]) % capacity; first = config[1] < capacity - position ? config[1] : capacity - position;
        if (!read_memory(samples, receive + 0x80 + position * 4, first * 4) ||
            (first < config[1] && !read_memory(samples + first, receive + 0x80, (config[1] - first) * 4))) return false;
        for (unsigned n = 0; n < config[1]; n++) output[n] = samples[n];
    }
    write = (write + count * config[1]) % capacity; read = (read + count * config[1]) % capacity;
    return write_memory(send + 0x44, &write, 4) && write_memory(receive + 0x40, &read, 4);
}
static bool delay_open(delay_line *d, float ms) {
    d->size = (uint32_t)lrintf(config[0] / 1000 * ms); d->delay = d->size; d->write = d->delay;
    d->data = calloc(d->size + 1, sizeof(float)); return d->data != NULL;
}
static float delay_update(delay_line *d, float input) {
    d->data[d->write++] = input; float output = d->data[d->read++];
    if (d->read >= d->size) d->read = 0;
    if (d->write >= d->size) d->write = 0;
    return output;
}
static float delay_tap(const delay_line *d, uint32_t at) { return d->data[(d->write + d->size - at) % (d->size + 1)]; }
static float decay_update(delay_line *d, float input, float gain) {
    float processed = input - gain * d->data[d->read]; return delay_update(d, processed) + processed * gain;
}
static bool reverb_configure(effect_state *effect, const reverb_parameters *p) {
    static const float min_ms[4] = {5, 6, 13, 14}, max_ms[4] = {45.704f, 82.782f, 149.94f, 271.58f};
    static const float decay1[4] = {17, 13, 9, 7}, decay2[4] = {19, 11, 10, 6};
    static const float early_times[20] = {.017136f,.059154f,.16173f,.39019f,.42526f,.45541f,.68974f,.74591f,.83384f,.8595f,0,.075024f,.16879f,.2999f,.33744f,.3719f,.59901f,.71674f,.81786f,.85166f};
    if (p->rate != config[0] || p->decay <= 0 || p->hf_ratio <= 0 || p->hf_reference <= 0 ||
        p->density < 0 || p->density > 100 || p->diffusion < 0 || p->diffusion > 100 ||
        p->reflection_delay < 0 || p->reverb_delay < 0 || p->reflection_delay + p->reverb_delay > .4f) return false;
    for (unsigned i = 0; i < 12; i++) if (!isfinite(f32((const uint8_t *)p + 24 + i * 4))) return false;
    if (!effect->reverb) {
        reverb_state *r = effect->reverb = calloc(1, sizeof *r);
        if (!r) return false;
        for (unsigned i = 0; i < 4; i++) if (!delay_open(&r->fdn[i], max_ms[i]) || !delay_open(&r->decay1[i], decay1[i]) || !delay_open(&r->decay2[i], decay2[i])) return false;
        if (!delay_open(&r->pre, 400) || !delay_open(&r->center, 5)) return false;
    } else if (!memcmp(&effect->reverb->parameters, p, 72)) return true;
    reverb_state *r = effect->reverb; r->parameters = *p;
    memset(r->previous, 0, sizeof r->previous); r->previous_pre = 0;
    r->early_gain = powf(10, fminf(p->room_gain + p->reflections_gain, 5000) / 2000);
    r->late_gain = powf(10, fminf(p->room_gain + p->reverb_gain, 5000) / 2000);
    float hf = powf(10, p->room_hf / 2000);
    if (hf < 1) {
        float a = 1 - hf, b = 2 - 2 * hf * cosf((float)M_PI / 180 * 256 * p->hf_reference / p->rate);
        r->previous_pre_gain = (b - sqrtf(fmaxf(0, b * b - 4 * a * a))) / (2 * a);
    } else r->previous_pre_gain = 0;
    r->pre_gain = 1 - r->previous_pre_gain; r->echo = .006f * p->diffusion;
    r->reflection_delay = (uint32_t)lrintf(config[0] * (p->reflection_delay + p->reverb_delay));
    for (unsigned i = 0; i < 4; i++) {
        delay_line *d = &r->fdn[i];
        d->delay = (uint32_t)lrintf(config[0] / 1000 * (min_ms[i] + p->density / 100 * (max_ms[i] - min_ms[i])));
        d->write = (d->read + d->delay) % (d->size + 1);
        float a = -60 * (float)(d->delay + r->decay1[i].delay + r->decay2[i].delay) / (p->decay * p->rate);
        float b = a / p->hf_ratio, angle = (float)M_PI / 180 * 64 * p->hf_reference / p->rate;
        float c = 1 / tanf(angle), gain = powf(10, (b - a) / 40), scale = powf(10, (b + a) / 40) * .7071f;
        r->direct[i] = scale * (gain * c + 1) / (c + gain);
        r->current[i] = scale * (1 - gain * c) / (c + gain); r->feedback[i] = (c - gain) / (c + gain);
    }
    for (unsigned i = 0; i < 20; i++) r->early_delay[i] = (uint32_t)fminf(r->pre.size, nearbyintf(config[0] * (p->reflection_delay + early_times[i] * (p->reverb_delay * .9998f + .02f))));
    return true;
}
static bool reverb_effect(unsigned effect_id, const uint8_t *data, bool enabled, unsigned id) {
    static const float early_gain[20] = {.67096f,.61027f,1,.35680f,.68361f,.65978f,.51939f,.24712f,.45945f,.45021f,.64196f,.54879f,.92925f,.38270f,.72867f,.69794f,.5464f,.24563f,.45214f,.44042f};
    static const uint8_t stereo[20] = {0,0,0,1,1,1,1,0,0,0,1,1,1,0,0,0,0,1,1,1};
    static const uint8_t quad[20] = {0,0,0,1,1,1,1,2,2,2,1,1,1,0,0,0,0,3,3,3};
    static const uint8_t surround[20] = {4,0,0,1,1,1,1,2,2,2,1,1,1,0,0,0,0,3,3,3};
    reverb_parameters p; memcpy(&p, data, sizeof p);
    if (!effect_channels(p.input, p.output, p.count, id) || (p.count != 1 && p.count != 2 && p.count != 4 && p.count != 6)) return false;
    if (!enabled) {
        for (unsigned c = 0; c < p.count; c++) if (p.input[c] != p.output[c]) memcpy(mix_buffer(id, p.output[c]), mix_buffer(id, p.input[c]), config[1] * sizeof(float));
        return true;
    }
    if (!reverb_configure(&effect_states[effect_id], &p)) return false;
    reverb_state *r = effect_states[effect_id].reverb;
    static const unsigned mapping[6] = {0,1,4,5,2,3};
    for (unsigned n = 0; n < config[1]; n++) {
        float input[6], output[6] = {0}, feedback[4], values[4];
        float tap = delay_tap(&r->pre, r->reflection_delay), sum = 0;
        for (unsigned c = 0; c < p.count; c++) { input[c] = mix_buffer(id, p.input[p.count == 6 ? mapping[c] : c])[n]; sum += input[c]; }
        for (unsigned k = 0; k < 20; k++) {
            float value = delay_tap(&r->pre, r->early_delay[k]) * early_gain[k] * r->early_gain;
            unsigned channel_id = p.count == 1 ? 0 : p.count == 2 ? stereo[k] : p.count == 4 ? quad[k] : surround[k];
            output[channel_id] += value; if (p.count == 6) output[5] += value;
        }
        r->previous_pre = sum * r->pre_gain + r->previous_pre * r->previous_pre_gain;
        delay_update(&r->pre, r->previous_pre);
        for (unsigned i = 0; i < 4; i++) {
            float x = r->fdn[i].data[r->fdn[i].read]; feedback[i] = x * r->direct[i] + r->previous[i];
            r->previous[i] = x * r->current[i] + feedback[i] * r->feedback[i];
        }
        float combined[4] = {feedback[2]+feedback[1],-feedback[0]-feedback[3],feedback[0]-feedback[3],feedback[1]-feedback[2]};
        for (unsigned i = 0; i < 4; i++) {
            float v = decay_update(&r->decay1[i], tap * r->late_gain + combined[i], r->echo);
            values[i] = decay_update(&r->decay2[i], v, r->echo * -.9f); delay_update(&r->fdn[i], values[i]);
        }
        for (unsigned c = 0; c < p.count; c++) {
            float wet = c == 4 ? delay_update(&r->center, (values[2] - values[3]) * .5f) : values[c == 5 ? 3 : c];
            if (p.count == 1) wet += values[1];
            mix_buffer(id, p.output[p.count == 6 ? mapping[c] : c])[n] = output[c] + wet + input[c] * p.dry_gain;
        }
    }
    return true;
}
static bool limiter_effect(unsigned index, const uint8_t *data, bool enabled, unsigned id) {
    limiter_parameters p; memcpy(&p, data, sizeof p); effect_state *s = &effect_states[index];
    if (!effect_channels(p.input, p.output, p.count, id) || p.delay_min < 1 || p.delay_max < p.delay_min || p.delay_max > 48000 ||
        !isfinite(p.attack) || p.attack < 0 || p.attack > 1 || !isfinite(p.release) || p.release < 0 || p.release > 1 ||
        !volume_valid(p.threshold) || p.threshold <= 0 || !volume_valid(p.input_gain) || !volume_valid(p.output_gain)) return false;
    if (!s->delay || s->delay_size != (uint32_t)p.delay_max) {
        free(s->delay); s->delay = calloc((size_t)p.delay_max * 6, sizeof(float)); s->delay_size = p.delay_max;
        if (!s->delay) return false;
        for (unsigned c = 0; c < 6; c++) { s->detector[c] = 0; s->gain[c] = s->gain_min[c] = 1; s->input_peak[c] = 0; s->position[c] = 0; }
    }
    for (unsigned c = 0; c < p.count; c++) for (unsigned n = 0; n < config[1]; n++) {
        float input = mix_buffer(id, p.input[c])[n];
        if (enabled) {
            input = input / 32767 * p.input_gain; float magnitude = fabsf(input);
            s->detector[c] += (magnitude > s->detector[c] ? p.attack : p.release) * (magnitude - s->detector[c]);
            float attenuation = s->detector[c] > p.threshold ? p.threshold / s->detector[c] : 1;
            s->gain[c] += (s->gain[c] > attenuation ? p.attack : p.release) * (attenuation - s->gain[c]);
            s->input_peak[c] = fmaxf(s->input_peak[c], magnitude); s->gain_min[c] = fminf(s->gain_min[c], s->gain[c]);
            unsigned at = c * s->delay_size + s->position[c]; float delayed = s->delay[at]; s->delay[at] = input;
            s->position[c] = (s->position[c] + 1) % (uint32_t)p.delay_min;
            input = delayed * s->gain[c] * p.output_gain * 32767;
        }
        mix_buffer(id, p.output[c])[n] = input;
    }
    return true;
}
static bool process_effects(unsigned id) {
    for (unsigned order = 0; order < config[6]; order++) for (unsigned i = 0; i < config[6]; i++) {
        const uint8_t *e = effects + i * 0xc0;
        if (!e[0] || audio_u32(e + 4) != id || audio_u32(e + 24) != order) continue;
        bool ok = false;
        if (e[0] == 2) ok = auxiliary_effect(e + 32, e[2], id);
        if (e[0] == 5) ok = reverb_effect(i, e + 32, e[2], id);
        if (e[0] == 7) ok = limiter_effect(i, e + 32, e[2], id);
        if (!ok) { fprintf(stderr, "AUDIO unsupported effect=%u type=%u mix=%u\n", i, e[0], id); return false; }
    }
    return true;
}
