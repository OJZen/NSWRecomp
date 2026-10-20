// Adapted from Zelda-Mobile host/audio.h; Apache-2.0, see third_party/zelda-mobile/LICENSE.
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
bool audio_open(const void *configuration, size_t size);
void audio_close(void);
uint32_t audio_event(void);
uint32_t audio_value(unsigned command);
bool audio_set(unsigned command, uint32_t value);
void audio_volume(float value);
void audio_mute(bool muted);  // host speaker switch; the guest keeps mixing at the same rate
bool audio_muted(void);
size_t audio_update(const void *input, size_t length, void *output, size_t capacity);
