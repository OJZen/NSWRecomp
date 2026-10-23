// Portable Vorbis packet decoding for the movie adapter. Original NSWRecomp code (MIT).
#pragma once
#include <stddef.h>
#include <stdint.h>

typedef struct movie_audio movie_audio;
// MediaCodec supplies identification and setup packets; comments carry no decoding state.
movie_audio *movie_audio_open(const void *identification, size_t identification_size,
                             const void *setup, size_t setup_size, int rate, int channels);
void movie_audio_close(movie_audio *audio);
int movie_audio_reset(movie_audio *audio);
// One raw Vorbis packet; NULL/0 drains at end of stream. Drain output before feeding again.
int movie_audio_decode(movie_audio *audio, const void *packet, size_t size);
// Interleaved signed PCM16 in Vorbis channel order; 1 for samples, 0 for no output, -1 on error.
int movie_audio_frame(movie_audio *audio, uint8_t *output, size_t capacity, size_t *bytes);
