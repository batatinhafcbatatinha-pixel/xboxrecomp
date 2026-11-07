#include "native_audio_mix.h"
#include <math.h>
void street2_audio_fold(NativeAudioLimiter *limiter, const float bins[32][32], int16_t out[32][2])
{
    for (unsigned i=0;i<32;i++) {
        float center=0.70710678f*bins[2][i];
        float l=bins[0][i]+center+0.70710678f*bins[4][i];
        float r=bins[1][i]+center+0.70710678f*bins[5][i];
        if(!isfinite(l)) l=0;
        if(!isfinite(r)) r=0;
        float peak=fmaxf(fabsf(l),fabsf(r));
        if(peak*limiter->gain>0.98f) limiter->gain=0.98f/peak;
        else limiter->gain+=(1.0f-limiter->gain)*0.00035f;
        l=fmaxf(-1.0f,fminf(1.0f,l*limiter->gain));
        r=fmaxf(-1.0f,fminf(1.0f,r*limiter->gain));
        out[i][0]=(int16_t)(l*32767.0f);
        out[i][1]=(int16_t)(r*32767.0f);
    }
}
