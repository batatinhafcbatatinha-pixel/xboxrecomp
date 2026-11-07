/* 48 kHz host audio clock and output. No APU register/device/DSP state. */
#include "hle_win32.h"
#include "native_audio_mix.h"
#include "apu/apu_xaudio2.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static HANDLE thread;
static volatile LONG stop;
static void (*voices)(float bins[32][32]);
void hle_port_native_missing(const char *,unsigned);

static DWORD WINAPI audio_thread(LPVOID unused)
{
    (void)unused;
    if(!xa2_init()) hle_port_native_missing("XAudio2 output initialization",0);
    extern void hle_options_audio_ready(void);
    hle_options_audio_ready();
    FILE *capture=NULL;
    const char *path=getenv("RECOMP_NATIVE_PCM");
    if(path && *path) capture=fopen(path,"wb");
    NativeAudioLimiter limiter={1.0f};
    LARGE_INTEGER frequency,start,now;
    QueryPerformanceFrequency(&frequency);
    QueryPerformanceCounter(&start);
    uint64_t frames=0, nonzero=0;
    unsigned blocks=0;
    fprintf(stderr,"[AUDIO-NATIVE] 48000 Hz stereo, host mixer thread; no APU/DSP model\n");
    while(!InterlockedCompareExchange(&stop,0,0)) {
        int16_t pcm[512][2];
        for(unsigned block=0;block<16;block++) {
            float bins[32][32]={0};
            voices(bins);
            street2_audio_fold(&limiter,bins,(int16_t (*)[2])&pcm[block*32][0]);
        }
        for(unsigned i=0;i<512;i++) if(pcm[i][0] || pcm[i][1]) nonzero++;
        xa2_submit_samples(&pcm[0][0],512);
        if(capture) fwrite(pcm,sizeof pcm,1,capture);
        frames+=512;
        if(++blocks%470==0) {
            fprintf(stderr,"[AUDIO-NATIVE] rendered=%llu frames nonzero=%llu\n",(unsigned long long)frames,(unsigned long long)nonzero);
            fflush(stderr);
            if(capture) fflush(capture);
        }
        LONGLONG deadline=start.QuadPart+(LONGLONG)(frames*frequency.QuadPart/48000);
        for(;;) {
            QueryPerformanceCounter(&now);
            LONGLONG remaining=deadline-now.QuadPart;
            if(remaining<=0 || InterlockedCompareExchange(&stop,0,0)) break;
            DWORD ms=(DWORD)(remaining*1000/frequency.QuadPart);
            Sleep(ms>1 ? ms-1 : 0);
        }
    }
    if(capture) fclose(capture);
    xa2_shutdown();
    return 0;
}

void street2_audio_start(void (*callback)(float bins[32][32]))
{
    if(thread) return;
    voices=callback;
    stop=0;
    thread=CreateThread(NULL,0,audio_thread,NULL,0,NULL);
    if(!thread) hle_port_native_missing("native audio thread creation",0);
}
void street2_audio_stop(void)
{
    if(!thread) return;
    InterlockedExchange(&stop,1);
    WaitForSingleObject(thread,INFINITE);
    CloseHandle(thread);
    thread=NULL;
}
