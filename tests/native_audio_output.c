#include <assert.h>
#include <stdbool.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../runtime/audio_output.inc"
int main(void) {
    setenv("DM_AUDIO", "0", 1); assert(audio_output_open(48000) && !audio_unit);
    float samples[] = {.5, -.5, .25, -.25}, out[4] = {0};
    AudioBufferList buffers = {.mNumberBuffers = 1, .mBuffers = {{2, sizeof out, out}}};
    audio_output_push(samples, 2); audio_mute(true); assert(audio_muted());
    assert(!audio_output_callback(NULL, NULL, NULL, 0, 2, &buffers));
    assert(atomic_load(&audio_read_cursor) == 2 && atomic_load(&audio_output_samples) == 4);
    for (unsigned i = 0; i < 4; i++) assert(out[i] == 0);
    audio_mute(false); assert(!audio_muted()); audio_output_push(samples, 2);
    assert(!audio_output_callback(NULL, NULL, NULL, 0, 2, &buffers));
    assert(!memcmp(samples, out, sizeof out));
    buffers.mBuffers[0].mDataByteSize = 1;
    assert(audio_output_callback(NULL, NULL, NULL, 0, 2, &buffers) == kAudio_ParamError);
    audio_output_close(); return 0;
}
