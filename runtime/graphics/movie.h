#pragma once
#include "dispatch.h"

GuestFn movie_find_function(uint64_t address);
// Backend-owned texture upload; the source is host RGBA8, the texture is guest NVN metadata.
bool movie_write_rgba(void *texture, const uint8_t *rgba, unsigned width, unsigned height);
