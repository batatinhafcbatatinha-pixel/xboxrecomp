#ifndef STREET2_NATIVE_AUDIO_MIX_H
#define STREET2_NATIVE_AUDIO_MIX_H
#include <stdint.h>
typedef struct { float gain; } NativeAudioLimiter;
void street2_audio_fold(NativeAudioLimiter *limiter, const float bins[32][32], int16_t out[32][2]);
#endif
