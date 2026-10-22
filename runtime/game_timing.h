#pragma once
#include <stdbool.h>
#include <stdint.h>

bool timing_set_target(unsigned fps);
unsigned timing_target(void);
unsigned zelda_game_frame_rate(void);
double zelda_game_seconds(void);
uint64_t timing_frames(void);
