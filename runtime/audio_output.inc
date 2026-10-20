// Adapted from Zelda-Mobile host/audio_output.inc; Apache-2.0, see third_party/zelda-mobile/LICENSE.
// CoreAudio and acquire/release queue ordering follow d2r-mobile/dm/host/hle_audio.c.
#include <AudioToolbox/AudioToolbox.h>
#include <TargetConditionals.h>

#define AUDIO_RING_FRAMES 4096u
static float audio_ring[AUDIO_RING_FRAMES][2];
static _Atomic uint32_t audio_read_cursor, audio_write_cursor;
static _Atomic uint64_t audio_callbacks, audio_output_samples, audio_underruns;
static uint64_t audio_overruns;
static AudioUnit audio_unit;
// Host-side mute: the ring is still drained at the device rate, so guest audio timing and counters are unchanged.
static _Atomic bool audio_output_muted;

static void audio_output_pull(float *output, uint32_t frames) {
    uint32_t read = atomic_load_explicit(&audio_read_cursor, memory_order_relaxed);
    uint32_t write = atomic_load_explicit(&audio_write_cursor, memory_order_acquire);
    uint32_t count = write - read;
    if (count > frames) count = frames;
    uint64_t nonzero = 0;
    for (uint32_t i = 0; i < count; i++) for (unsigned ch = 0; ch < 2; ch++) {
        float sample = audio_ring[(read + i) & (AUDIO_RING_FRAMES - 1)][ch];
        output[i * 2 + ch] = sample; nonzero += sample != 0;
    }
    memset(output + count * 2, 0, (frames - count) * 2 * sizeof *output);
    atomic_store_explicit(&audio_read_cursor, read + count, memory_order_release);
    atomic_fetch_add_explicit(&audio_output_samples, nonzero, memory_order_relaxed);
    atomic_fetch_add_explicit(&audio_underruns, frames - count, memory_order_relaxed);
}
static void audio_output_push(const float *samples, uint32_t frames) {
    uint32_t write = atomic_load_explicit(&audio_write_cursor, memory_order_relaxed);
    uint32_t read = atomic_load_explicit(&audio_read_cursor, memory_order_acquire);
    if (frames > AUDIO_RING_FRAMES - (write - read)) { audio_overruns += frames; return; }
    for (uint32_t i = 0; i < frames; i++) memcpy(audio_ring[(write + i) & (AUDIO_RING_FRAMES - 1)], samples + i * 2, 2 * sizeof *samples);
    atomic_store_explicit(&audio_write_cursor, write + frames, memory_order_release);
}
static OSStatus audio_output_callback(void *context, AudioUnitRenderActionFlags *flags,
    const AudioTimeStamp *timestamp, UInt32 bus, UInt32 frames, AudioBufferList *buffers) {
    (void)context; (void)flags; (void)timestamp; (void)bus;
    if (buffers->mNumberBuffers != 1 || buffers->mBuffers[0].mDataByteSize < frames * 2 * sizeof(float)) return kAudio_ParamError;
    atomic_fetch_add_explicit(&audio_callbacks, 1, memory_order_relaxed);
    audio_output_pull(buffers->mBuffers[0].mData, frames);
    if (atomic_load_explicit(&audio_output_muted, memory_order_relaxed)) memset(buffers->mBuffers[0].mData, 0, frames * 2 * sizeof(float));
    return noErr;
}
void audio_mute(bool muted) {
    if (atomic_exchange(&audio_output_muted, muted) != muted) fprintf(stderr, "AUDIO host mute=%d\n", muted);
}
bool audio_muted(void) { return atomic_load(&audio_output_muted); }
static void audio_output_close(void) {
    if (audio_unit) {
        AudioOutputUnitStop(audio_unit); AudioUnitUninitialize(audio_unit);
        AudioComponentInstanceDispose(audio_unit); audio_unit = NULL;
    }
}
static bool audio_output_open(uint32_t rate) {
    atomic_store(&audio_read_cursor, 0); atomic_store(&audio_write_cursor, 0);
    atomic_store(&audio_callbacks, 0); atomic_store(&audio_output_samples, 0);
    atomic_store(&audio_underruns, 0); audio_overruns = 0;
    const char *setting = getenv("DM_AUDIO");
    if (setting && !strcmp(setting, "0")) return true;
    AudioComponentDescription description = {.componentType = kAudioUnitType_Output,
#if TARGET_OS_IPHONE
        .componentSubType = kAudioUnitSubType_RemoteIO,
#else
        .componentSubType = kAudioUnitSubType_DefaultOutput,
#endif
        .componentManufacturer = kAudioUnitManufacturer_Apple};
    AudioComponent component = AudioComponentFindNext(NULL, &description);
    if (!component || AudioComponentInstanceNew(component, &audio_unit)) return false;
    AudioStreamBasicDescription format = {.mSampleRate = rate, .mFormatID = kAudioFormatLinearPCM,
        .mFormatFlags = kAudioFormatFlagIsFloat | kAudioFormatFlagIsPacked, .mBytesPerPacket = 8,
        .mFramesPerPacket = 1, .mBytesPerFrame = 8, .mChannelsPerFrame = 2, .mBitsPerChannel = 32};
    AURenderCallbackStruct callback = {audio_output_callback, NULL};
    if (AudioUnitSetProperty(audio_unit, kAudioUnitProperty_StreamFormat, kAudioUnitScope_Input, 0, &format, sizeof format) ||
        AudioUnitSetProperty(audio_unit, kAudioUnitProperty_SetRenderCallback, kAudioUnitScope_Input, 0, &callback, sizeof callback) ||
        AudioUnitInitialize(audio_unit)) { audio_output_close(); return false; }
    return true;
}
