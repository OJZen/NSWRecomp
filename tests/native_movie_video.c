// Adapted from Zelda-Mobile tests/test_movie_video.c (Apache-2.0).
// Movie decoding backend: lossless VP9/VP8 round trips must come back as the exact NV12 bytes the game copies into
// its Y (R8) and UV (RG8) textures; the end-of-stream drain and an undersized buffer behave as the HLE expects.
#include "../runtime/graphics/movie_video.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <vpx/vp8cx.h>
#include <vpx/vpx_encoder.h>

enum { W = 64, H = 48, FRAMES = 3 };

static uint8_t pixel(int frame, int plane, int x, int y) { return (uint8_t)(frame * 37 + plane * 71 + x * 3 + y * 5); }

static size_t encode(int codec, uint8_t *stream, size_t capacity, size_t sizes[FRAMES]) {
    vpx_codec_iface_t *iface = codec == ZELDA_VIDEO_VP9 ? vpx_codec_vp9_cx() : vpx_codec_vp8_cx();
    vpx_codec_enc_cfg_t config; assert(vpx_codec_enc_config_default(iface, &config, 0) == VPX_CODEC_OK);
    config.g_w = W; config.g_h = H; config.g_timebase.num = 1; config.g_timebase.den = 30; config.g_lag_in_frames = 0;
    if (codec == ZELDA_VIDEO_VP9) { config.rc_end_usage = VPX_Q; config.rc_min_quantizer = config.rc_max_quantizer = 0; }
    else config.rc_target_bitrate = 4000;
    vpx_codec_ctx_t encoder; assert(vpx_codec_enc_init(&encoder, iface, &config, 0) == VPX_CODEC_OK);
    if (codec == ZELDA_VIDEO_VP9) assert(vpx_codec_control(&encoder, VP9E_SET_LOSSLESS, 1) == VPX_CODEC_OK);
    vpx_image_t image; assert(vpx_img_alloc(&image, VPX_IMG_FMT_I420, W, H, 1));
    size_t used = 0;
    for (int frame = 0; frame <= FRAMES; frame++) {
        if (frame < FRAMES)
            for (int plane = 0; plane < 3; plane++)
                for (int y = 0; y < (plane ? H / 2 : H); y++)
                    for (int x = 0; x < (plane ? W / 2 : W); x++) image.planes[plane][y * image.stride[plane] + x] = pixel(frame, plane, x, y);
        assert(vpx_codec_encode(&encoder, frame < FRAMES ? &image : NULL, frame, 1, 0, VPX_DL_GOOD_QUALITY) == VPX_CODEC_OK);
        vpx_codec_iter_t iter = NULL; const vpx_codec_cx_pkt_t *packet;
        while ((packet = vpx_codec_get_cx_data(&encoder, &iter)))
            if (packet->kind == VPX_CODEC_CX_FRAME_PKT) {
                int index = (int)packet->data.frame.pts;
                assert(index < FRAMES && used + packet->data.frame.sz <= capacity);
                memcpy(stream + used, packet->data.frame.buf, packet->data.frame.sz);
                sizes[index] = packet->data.frame.sz; used += packet->data.frame.sz;
            }
    }
    vpx_img_free(&image); vpx_codec_destroy(&encoder);
    return used;
}

static void round_trip(int codec, int exact) {
    static uint8_t stream[1 << 20]; size_t sizes[FRAMES] = {0};
    encode(codec, stream, sizeof stream, sizes);
    zelda_video *video = zelda_video_open(codec, 2); assert(video);
    size_t frame_size = zelda_video_nv12_size(W, H); assert(frame_size == W * H * 3 / 2);
    uint8_t *nv12 = malloc(frame_size), small[16];
    size_t offset = 0; int decoded = 0, width = 0, height = 0;
    for (int frame = 0; frame < FRAMES; frame++) {
        assert(sizes[frame] && zelda_video_decode(video, stream + offset, sizes[frame]) == 0);
        offset += sizes[frame];
        if (frame == 1) { assert(zelda_video_frame(video, small, sizeof small, &width, &height) == -2); decoded++; continue; }
        int got;
        while ((got = zelda_video_frame(video, nv12, frame_size, &width, &height)) == 1) {
            assert(width == W && height == H);
            long mismatches = 0;
            for (int y = 0; y < H; y++) for (int x = 0; x < W; x++) mismatches += nv12[y * W + x] != pixel(decoded, 0, x, y);
            for (int y = 0; y < H / 2; y++) for (int x = 0; x < W / 2; x++) {
                const uint8_t *uv = nv12 + W * H + y * W + 2 * x;
                mismatches += uv[0] != pixel(decoded, 1, x, y);
                mismatches += uv[1] != pixel(decoded, 2, x, y);
            }
            // VP8 has no lossless mode; its frames only need to stay close to the source.
            assert(exact ? mismatches == 0 : mismatches < (long)frame_size);
            decoded++;
        }
        assert(got == 0);
    }
    assert(decoded == FRAMES);
    assert(zelda_video_decode(video, NULL, 0) == 0 && zelda_video_frame(video, nv12, frame_size, &width, &height) == 0);
    zelda_video_close(video); free(nv12);
}

int main(void) {
    round_trip(ZELDA_VIDEO_VP9, 1);
    round_trip(ZELDA_VIDEO_VP8, 0);
    assert(!zelda_video_open(1, 1) && zelda_video_nv12_size(0, 4) == 0 && zelda_video_nv12_size(1920, 1080) == 1920 * 1080 * 3 / 2);
    puts("Movie video: lossless VP9 NV12 bytes, VP8 decode, end-of-stream drain and capacity check passed");
    return 0;
}
