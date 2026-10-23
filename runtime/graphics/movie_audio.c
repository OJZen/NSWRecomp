// Original NSWRecomp code (MIT). Uses the separately installed BSD-licensed libvorbis/libogg.
#include "movie_audio.h"
#include <vorbis/codec.h>
#include <math.h>
#include <stdlib.h>

struct movie_audio {
    vorbis_info info;
    vorbis_comment comment;
    vorbis_dsp_state dsp;
    vorbis_block block;
    int dsp_ready, block_ready;
    ogg_int64_t packet_number;
};

void movie_audio_close(movie_audio *audio) {
    if (!audio) return;
    if (audio->block_ready) vorbis_block_clear(&audio->block);
    if (audio->dsp_ready) vorbis_dsp_clear(&audio->dsp);
    vorbis_comment_clear(&audio->comment);
    vorbis_info_clear(&audio->info);
    free(audio);
}

movie_audio *movie_audio_open(const void *identification, size_t identification_size,
                             const void *setup, size_t setup_size, int rate, int channels) {
    if (!identification || !setup || !identification_size || !setup_size ||
        identification_size > (1u << 20) || setup_size > (1u << 20) ||
        rate < 8000 || rate > 192000 || channels < 1 || channels > 8) return NULL;
    movie_audio *audio = calloc(1, sizeof *audio);
    if (!audio) return NULL;
    vorbis_info_init(&audio->info); vorbis_comment_init(&audio->comment);
    // Empty vendor and comment list, with the mandatory framing bit.
    unsigned char comment[] = {3,'v','o','r','b','i','s',0,0,0,0,0,0,0,0,1};
    ogg_packet headers[] = {
        {.packet=(unsigned char *)identification, .bytes=(long)identification_size, .b_o_s=1, .packetno=0},
        {.packet=comment, .bytes=sizeof comment, .packetno=1},
        {.packet=(unsigned char *)setup, .bytes=(long)setup_size, .packetno=2},
    };
    for (unsigned i = 0; i < 3; i++)
        if (vorbis_synthesis_headerin(&audio->info, &audio->comment, &headers[i])) goto fail;
    if (audio->info.rate != rate || audio->info.channels != channels) goto fail;
    if (vorbis_synthesis_init(&audio->dsp, &audio->info)) goto fail;
    audio->dsp_ready = 1;
    if (vorbis_block_init(&audio->dsp, &audio->block)) goto fail;
    audio->block_ready = 1; audio->packet_number = 3;
    return audio;
fail:
    movie_audio_close(audio); return NULL;
}

int movie_audio_reset(movie_audio *audio) {
    if (!audio) return -1;
    audio->packet_number = 3;
    return vorbis_synthesis_restart(&audio->dsp);
}

int movie_audio_decode(movie_audio *audio, const void *data, size_t size) {
    if (!audio || size > (4u << 20) || (size && !data)) return -1;
    if (!size) return 0;
    ogg_packet packet = {.packet=(unsigned char *)data, .bytes=(long)size,
                         .granulepos=-1, .packetno=audio->packet_number++};
    if (vorbis_synthesis(&audio->block, &packet)) return -1;
    return vorbis_synthesis_blockin(&audio->dsp, &audio->block);
}

int movie_audio_frame(movie_audio *audio, uint8_t *output, size_t capacity, size_t *bytes) {
    if (!audio || !output || !bytes) return -1;
    *bytes = 0;
    float **pcm = NULL;
    int samples = vorbis_synthesis_pcmout(&audio->dsp, &pcm), channels = audio->info.channels;
    if (samples <= 0) return samples;
    size_t available = capacity / ((size_t)channels * 2);
    if (!available) return -1;
    if ((size_t)samples > available) samples = (int)available;
    for (int i = 0; i < samples; i++) for (int channel = 0; channel < channels; channel++) {
        float value = pcm[channel][i];
        if (!isfinite(value)) return -1;
        int sample = value >= 1 ? 32767 : value <= -1 ? -32768 : (int)lrintf(value * 32768);
        if (sample > 32767) sample = 32767;
        size_t at = ((size_t)i * channels + channel) * 2;
        output[at] = (uint8_t)sample; output[at + 1] = (uint8_t)((uint16_t)sample >> 8);
    }
    if (vorbis_synthesis_read(&audio->dsp, samples)) return -1;
    *bytes = (size_t)samples * channels * 2;
    return 1;
}
