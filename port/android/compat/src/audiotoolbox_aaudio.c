/* CoreAudio output units over AAudio: see compat/include/AudioToolbox/AudioToolbox.h. */
#include <AudioToolbox/AudioToolbox.h>
#include <aaudio/AAudio.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

struct CompatAudioComponent { int unused; };
struct CompatAudioUnit { AudioStreamBasicDescription format; AURenderCallbackStruct callback; AAudioStream *stream; double sample_time; };
static struct CompatAudioComponent output_component;

AudioComponent AudioComponentFindNext(AudioComponent after, const AudioComponentDescription *d) {
    return !after && d->componentType == kAudioUnitType_Output ? &output_component : NULL;
}
OSStatus AudioComponentInstanceNew(AudioComponent c, AudioComponentInstance *out) {
    (void)c; *out = calloc(1, sizeof **out); return *out ? noErr : -1;
}
OSStatus AudioUnitSetProperty(AudioUnit u, UInt32 id, UInt32 scope, UInt32 element, const void *data, UInt32 size) {
    (void)scope; (void)element;
    if (id == kAudioUnitProperty_StreamFormat && size == sizeof u->format) { memcpy(&u->format, data, size); return noErr; }
    if (id == kAudioUnitProperty_SetRenderCallback && size == sizeof u->callback) { memcpy(&u->callback, data, size); return noErr; }
    return -1;
}

/* Master volume (the in-game overlay's Audio setting, TOTK_VOLUME 0..100) and silence while its menu has the game paused. */
static volatile float audio_gain = -1;
void compat_audio_set_gain(float gain) { audio_gain = gain < 0 ? 0 : gain > 1 ? 1 : gain; }
int overlay_open(void) __attribute__((weak));
static aaudio_data_callback_result_t on_data(AAudioStream *s, void *user, void *audio, int32_t frames) {
    (void)s;
    struct CompatAudioUnit *u = user;
    UInt32 channels = u->format.mChannelsPerFrame;
    AudioBufferList list = {1, {{channels, (UInt32)frames * channels * (UInt32)sizeof(float), audio}}};
    struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t);
    AudioTimeStamp ts = {u->sample_time, (uint64_t)t.tv_sec * 1000000000ull + (uint64_t)t.tv_nsec};
    AudioUnitRenderActionFlags flags = 0;
    u->callback.inputProc(u->callback.inputProcRefCon, &flags, &ts, 0, (UInt32)frames, &list);
    if (audio_gain < 0) { const char *v = getenv("TOTK_VOLUME"); audio_gain = v ? (float)atoi(v) / 100.0f : 1.0f; }
    float gain = overlay_open && overlay_open() ? 0.0f : audio_gain;
    if (gain != 1.0f) { float *f = audio; for (int32_t i = 0; i < frames * (int32_t)channels; i++) f[i] *= gain; }
    u->sample_time += frames;
    return AAUDIO_CALLBACK_RESULT_CONTINUE;
}
static void on_error(AAudioStream *s, void *user, aaudio_result_t error) {
    (void)s; (void)user;
    fprintf(stderr, "[totk-audio] AAudio stream error %d (%s)\n", error, AAudio_convertResultToText(error));
}

OSStatus AudioUnitInitialize(AudioUnit u) {
    if (u->format.mFormatID != kAudioFormatLinearPCM || !(u->format.mFormatFlags & kAudioFormatFlagIsFloat) || !u->callback.inputProc) return -1;
    AAudioStreamBuilder *b;
    if (AAudio_createStreamBuilder(&b) != AAUDIO_OK) return -1;
    AAudioStreamBuilder_setDirection(b, AAUDIO_DIRECTION_OUTPUT);
    AAudioStreamBuilder_setFormat(b, AAUDIO_FORMAT_PCM_FLOAT);
    AAudioStreamBuilder_setChannelCount(b, (int32_t)u->format.mChannelsPerFrame);
    AAudioStreamBuilder_setSampleRate(b, (int32_t)u->format.mSampleRate);
    AAudioStreamBuilder_setPerformanceMode(b, AAUDIO_PERFORMANCE_MODE_LOW_LATENCY);
    AAudioStreamBuilder_setUsage(b, AAUDIO_USAGE_GAME);
    AAudioStreamBuilder_setContentType(b, AAUDIO_CONTENT_TYPE_SONIFICATION);
    AAudioStreamBuilder_setDataCallback(b, on_data, u);
    AAudioStreamBuilder_setErrorCallback(b, on_error, u);
    aaudio_result_t r = AAudioStreamBuilder_openStream(b, &u->stream);
    AAudioStreamBuilder_delete(b);
    if (r != AAUDIO_OK) { fprintf(stderr, "[totk-audio] AAudio open failed: %s\n", AAudio_convertResultToText(r)); u->stream = NULL; return -1; }
    fprintf(stderr, "[totk-audio] AAudio stream %d Hz, %d ch, burst %d frames, buffer %d frames\n", AAudioStream_getSampleRate(u->stream),
            AAudioStream_getChannelCount(u->stream), AAudioStream_getFramesPerBurst(u->stream), AAudioStream_getBufferSizeInFrames(u->stream));
    return noErr;
}
OSStatus AudioOutputUnitStart(AudioUnit u) { return u->stream && AAudioStream_requestStart(u->stream) == AAUDIO_OK ? noErr : -1; }
OSStatus AudioOutputUnitStop(AudioUnit u) { return u->stream && AAudioStream_requestStop(u->stream) == AAUDIO_OK ? noErr : -1; }
OSStatus AudioUnitUninitialize(AudioUnit u) { if (u->stream) { AAudioStream_close(u->stream); u->stream = NULL; } return noErr; }
OSStatus AudioComponentInstanceDispose(AudioComponentInstance u) { if (u) { AudioUnitUninitialize(u); free(u); } return noErr; }
