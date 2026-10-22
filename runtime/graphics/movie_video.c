// Adapted from Zelda-Mobile host/movie_video.c (Apache-2.0).
// libvpx backend for the movie HLE: VP8/VP9 software decoding to the NV12 layout the game's movie renderer reads.
#include "movie_video.h"
#include <stdlib.h>
#include <string.h>
#include <vpx/vp8dx.h>
#include <vpx/vpx_decoder.h>

struct zelda_video { vpx_codec_ctx_t codec; vpx_codec_iter_t iter; int open; };

zelda_video *zelda_video_open(int codec, int threads) {
    vpx_codec_iface_t *iface = codec == ZELDA_VIDEO_VP9 ? vpx_codec_vp9_dx() : codec == ZELDA_VIDEO_VP8 ? vpx_codec_vp8_dx() : NULL;
    zelda_video *video = iface ? calloc(1, sizeof *video) : NULL;
    if (!video) return NULL;
    vpx_codec_dec_cfg_t config = {.threads = threads > 0 ? (unsigned)threads : 1};
    if (vpx_codec_dec_init(&video->codec, iface, &config, 0) != VPX_CODEC_OK) { free(video); return NULL; }
    video->open = 1;
    return video;
}

void zelda_video_close(zelda_video *video) {
    if (!video) return;
    if (video->open) vpx_codec_destroy(&video->codec);
    free(video);
}

int zelda_video_decode(zelda_video *video, const void *data, size_t size) {
    // A new sample invalidates the frame iterator of the previous one; callers drain frames first.
    video->iter = NULL;
    if (size > UINT32_MAX) return -1;
    return vpx_codec_decode(&video->codec, size ? data : NULL, (unsigned)size, NULL, 0) == VPX_CODEC_OK ? 0 : -1;
}

size_t zelda_video_nv12_size(int width, int height) {
    return width > 0 && height > 0 ? (size_t)width * (size_t)height + (size_t)((width + 1) / 2) * 2 * (size_t)((height + 1) / 2) : 0;
}

int zelda_video_frame(zelda_video *video, uint8_t *nv12, size_t capacity, int *width, int *height) {
    vpx_image_t *image = vpx_codec_get_frame(&video->codec, &video->iter);
    if (!image) return 0;
    if (image->fmt != VPX_IMG_FMT_I420 || image->x_chroma_shift != 1 || image->y_chroma_shift != 1) return -1;
    int w = (int)image->d_w, h = (int)image->d_h, cw = (w + 1) / 2, ch = (h + 1) / 2;
    if (zelda_video_nv12_size(w, h) > capacity) return -2;
    for (int y = 0; y < h; y++) memcpy(nv12 + (size_t)y * w, image->planes[VPX_PLANE_Y] + (size_t)y * image->stride[VPX_PLANE_Y], (size_t)w);
    uint8_t *uv = nv12 + (size_t)w * h;
    for (int y = 0; y < ch; y++) {
        const uint8_t *u = image->planes[VPX_PLANE_U] + (size_t)y * image->stride[VPX_PLANE_U];
        const uint8_t *v = image->planes[VPX_PLANE_V] + (size_t)y * image->stride[VPX_PLANE_V];
        uint8_t *row = uv + (size_t)y * cw * 2;
        for (int x = 0; x < cw; x++) { row[2 * x] = u[x]; row[2 * x + 1] = v[x]; }
    }
    *width = w; *height = h;
    return 1;
}
