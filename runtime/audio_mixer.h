// Adapted from Zelda-Mobile host/audio_mixer.h; Apache-2.0, see third_party/zelda-mobile/LICENSE.
#pragma once
#include <stdbool.h>
#include <stdint.h>

bool audio_mixer_open(const uint32_t configuration[13]);
void audio_mixer_close(void);
bool audio_mixer_update(const uint8_t *input, const uint32_t header[16], uint64_t frame, uint8_t *voice_status);
bool audio_mixer_render(uint64_t frame, float *stereo);
void audio_mixer_volume(float volume);
void audio_mixer_effect_status(uint8_t *output);
