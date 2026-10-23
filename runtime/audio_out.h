// Original NSWRecomp code (MIT). Bounded, silent AudioOut timing for initialization.
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
bool audio_out_open(unsigned rate, unsigned channels);
void audio_out_close(void);
uint32_t audio_out_event(void);
bool audio_out_start(bool started);
bool audio_out_append(uint64_t tag, const uint64_t buffer[5]);
size_t audio_out_released(uint64_t *tags, size_t capacity);
bool audio_out_contains(uint64_t tag);
uint64_t audio_out_value(unsigned command);
bool audio_out_flush(void);
bool audio_out_volume(float *volume, bool set);
