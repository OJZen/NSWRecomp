// Adapted from Zelda-Mobile host/movie_video.h (Apache-2.0).
#pragma once
#include <stddef.h>
#include <stdint.h>
// Platform-neutral video decoding behind the movie HLE (host/movie.c). The libvpx backend builds for macOS, iOS and
// Android alike; a platform hardware decoder can replace movie_video.c without touching the guest-facing layer.
enum { ZELDA_VIDEO_VP8 = 2, ZELDA_VIDEO_VP9 = 3 }; // movie::DecoderType values the game selects by MIME type.
typedef struct zelda_video zelda_video;
zelda_video *zelda_video_open(int codec, int threads);
void zelda_video_close(zelda_video *video);
// Feeds one compressed sample (NULL/0 drains the decoder at end of stream); returns 0 or a negative error.
int zelda_video_decode(zelda_video *video, const void *data, size_t size);
// Writes the next decoded frame as NV12 (Y rows, then interleaved UV rows; stride = width) and reports its size.
// Returns 1 for a frame, 0 when none is pending, negative when it does not fit or is not 8-bit 4:2:0.
int zelda_video_frame(zelda_video *video, uint8_t *nv12, size_t capacity, int *width, int *height);
// Texture-output alternative: RGBA8 using the frame's YCbCr matrix and range.
int zelda_video_frame_rgba(zelda_video *video, uint8_t *rgba, size_t capacity, int *width, int *height);
size_t zelda_video_nv12_size(int width, int height);
