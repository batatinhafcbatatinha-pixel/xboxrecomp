/**
 * XAudio2 Audio Output Backend
 *
 * Provides low-latency audio output via XAudio2 (Win7+).
 * Called from the APU monitor frame to submit mixed samples.
 * Falls back gracefully if XAudio2 is unavailable.
 */

#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <math.h>
#include "apu_xaudio2.h"

/* The XAudio2 backend is Windows-only; apu_sdl2.c is the POSIX one. */
#if defined(_WIN32)

#define COBJMACROS
#include <windows.h>
#include <xaudio2.h>

#pragma comment(lib, "xaudio2.lib")
#pragma comment(lib, "ole32.lib")

#define XA2_SAMPLE_RATE   48000
#define XA2_CHANNELS      2
#define XA2_BUF_SAMPLES   1024   /* ~21ms per submission */
#define XA2_NUM_BUFS      32     /* slots; the queue is bounded in samples */
/* Queue depth in samples. The producer (the APU frame thread, paced by the
 * host clock) and the sound card's clock drift apart and the producer has
 * scheduling jitter, so the queue is kept around XA2_TARGET by nudging the
 * playback rate (inaudible, <= 0.5%) instead of dropping or starving blocks:
 * either one is a click. Only far beyond XA2_MAX is a block dropped. */
#define XA2_TARGET        2048   /* ~43ms */
#define XA2_MAX           8192   /* ~170ms */

static IXAudio2               *g_xa2 = NULL;
static IXAudio2MasteringVoice *g_xa2_master = NULL;
static IXAudio2SourceVoice    *g_xa2_source = NULL;
static int16_t                 g_xa2_bufs[XA2_NUM_BUFS][XA2_BUF_SAMPLES][2];
static int                     g_xa2_next_buf = 0;
static int                     g_xa2_initialized = 0;
static int                     g_xa2_frames_written = 0;
static int                     g_xa2_size[XA2_NUM_BUFS];
static float                   g_xa2_avg = XA2_TARGET, g_xa2_ratio = 1.0f;
static unsigned                g_xa2_underruns, g_xa2_drops, g_xa2_submits;
static float                   g_xa2_volume = -1.0f;   /* asked before the voice existed */
static int                     g_xa2_drain_expected;

int xa2_init(void)
{
    HRESULT hr;
    int com_initialized;
    WAVEFORMATEX wfx = { 0 };

    if (g_xa2_initialized) return 1;

    hr = CoInitializeEx(NULL, COINIT_MULTITHREADED);
    if (FAILED(hr) && hr != RPC_E_CHANGED_MODE) {
        fprintf(stderr, "[XA2] CoInitializeEx failed: 0x%08lX\n", hr);
        return 0;
    }
    com_initialized = SUCCEEDED(hr);

    hr = XAudio2Create(&g_xa2, 0, XAUDIO2_DEFAULT_PROCESSOR);
    if (FAILED(hr) || !g_xa2) {
        fprintf(stderr, "[XA2] XAudio2Create failed: 0x%08lX\n", hr);
        goto fail;
    }

    hr = IXAudio2_CreateMasteringVoice(g_xa2, &g_xa2_master,
        XA2_CHANNELS, XA2_SAMPLE_RATE, 0, NULL, NULL, 0);
    if (FAILED(hr)) {
        fprintf(stderr, "[XA2] CreateMasteringVoice failed: 0x%08lX\n", hr);
        goto fail;
    }

    wfx.wFormatTag      = WAVE_FORMAT_PCM;
    wfx.nChannels       = XA2_CHANNELS;
    wfx.nSamplesPerSec  = XA2_SAMPLE_RATE;
    wfx.wBitsPerSample  = 16;
    wfx.nBlockAlign     = XA2_CHANNELS * 2;
    wfx.nAvgBytesPerSec = XA2_SAMPLE_RATE * wfx.nBlockAlign;

    hr = IXAudio2_CreateSourceVoice(g_xa2, &g_xa2_source,
        &wfx, 0, XAUDIO2_DEFAULT_FREQ_RATIO, NULL, NULL, NULL);
    if (FAILED(hr)) {
        fprintf(stderr, "[XA2] CreateSourceVoice failed: 0x%08lX\n", hr);
        goto fail;
    }

    hr = IXAudio2SourceVoice_Start(g_xa2_source, 0, XAUDIO2_COMMIT_NOW);
    if (FAILED(hr)) {
        fprintf(stderr, "[XA2] Start failed: 0x%08lX\n", hr);
        goto fail;
    }

    g_xa2_next_buf = 0;
    g_xa2_initialized = 1;
    g_xa2_frames_written = 0;
    if (g_xa2_volume >= 0.0f)
        IXAudio2MasteringVoice_SetVolume(g_xa2_master, g_xa2_volume, XAUDIO2_COMMIT_NOW);

    fprintf(stderr, "[XA2] XAudio2 initialized (%d Hz stereo 16-bit, %d x %d-sample buffers)\n",
            XA2_SAMPLE_RATE, XA2_NUM_BUFS, XA2_BUF_SAMPLES);
    return 1;

fail:
    xa2_shutdown();
    /* Failed initialization still runs on the COM-initializing thread. */
    if (com_initialized) CoUninitialize();
    return 0;
}

void xa2_shutdown(void)
{
    if (g_xa2_source) {
        IXAudio2SourceVoice_Stop(g_xa2_source, 0, XAUDIO2_COMMIT_NOW);
        IXAudio2SourceVoice_FlushSourceBuffers(g_xa2_source);
        g_xa2_source->lpVtbl->DestroyVoice(g_xa2_source);
        g_xa2_source = NULL;
    }
    if (g_xa2_master) {
        g_xa2_master->lpVtbl->DestroyVoice(g_xa2_master);
        g_xa2_master = NULL;
    }
    if (g_xa2) {
        IXAudio2_Release(g_xa2);
        g_xa2 = NULL;
    }

    if (g_xa2_initialized)
        fprintf(stderr, "[XA2] Shut down (%d frames written)\n", g_xa2_frames_written);
    g_xa2_initialized = 0;
}

int xa2_is_active(void)
{
    return g_xa2_initialized;
}

/* Submit a buffer of mixed samples to XAudio2.
 * Called from APU frame thread. Returns 1 if buffer was submitted. */
static int xa2_queue(const int16_t *samples, int n)
{
    XAUDIO2_BUFFER xbuf;
    int idx = g_xa2_next_buf;
    if (samples)
        memcpy(g_xa2_bufs[idx], samples, (size_t)n * XA2_CHANNELS * sizeof(int16_t));
    else
        memset(g_xa2_bufs[idx], 0, (size_t)n * XA2_CHANNELS * sizeof(int16_t));
    memset(&xbuf, 0, sizeof(xbuf));
    xbuf.AudioBytes = (UINT32)(n * XA2_CHANNELS * (int)sizeof(int16_t));
    xbuf.pAudioData = (const BYTE *)g_xa2_bufs[idx];
    if (FAILED(IXAudio2SourceVoice_SubmitSourceBuffer(g_xa2_source, &xbuf, NULL)))
        return 0;
    g_xa2_size[idx] = n;
    g_xa2_next_buf = (idx + 1) % XA2_NUM_BUFS;
    return 1;
}

int xa2_submit_samples(const int16_t *samples, int num_samples)
{
    XAUDIO2_VOICE_STATE state;
    int copy_samples, queued = 0;
    float ratio;

    if (!g_xa2_initialized || !g_xa2_source) return 0;

    IXAudio2SourceVoice_GetState(g_xa2_source, &state, XAUDIO2_VOICE_NOSAMPLESPLAYED);
    if (state.BuffersQueued >= XA2_NUM_BUFS - 2) {
        g_xa2_drops++;
        return 0;
    }
    /* Samples still queued: the newest BuffersQueued slots. */
    for (UINT32 i = 1; i <= state.BuffersQueued; i++)
        queued += g_xa2_size[(g_xa2_next_buf + XA2_NUM_BUFS - (int)i) % XA2_NUM_BUFS];
    if (queued > XA2_MAX) {
        g_xa2_drops++;
        return 0;
    }
    if (state.BuffersQueued == 0 && g_xa2_frames_written) {
        /* Starved: the gap already happened; start again with a cushion so
         * it does not chatter. A drain the frame thread announced
         * (xa2_expect_drain) is not an underrun. */
        if (!g_xa2_drain_expected)
            g_xa2_underruns++;
        g_xa2_drain_expected = 0;
        xa2_queue(NULL, XA2_TARGET / 2);
        queued = XA2_TARGET / 2;
    }

    copy_samples = (num_samples > XA2_BUF_SAMPLES) ? XA2_BUF_SAMPLES : num_samples;
    if (!xa2_queue(samples, copy_samples)) return 0;
    g_xa2_frames_written++;

    /* Rate nudge from the smoothed depth: fuller than the target plays a
     * little faster, emptier a little slower. */
    g_xa2_avg += ((float)queued - g_xa2_avg) * 0.02f;
    ratio = 1.0f + 0.005f * (g_xa2_avg - XA2_TARGET) / XA2_TARGET;
    ratio = ratio < 0.995f ? 0.995f : ratio > 1.005f ? 1.005f : ratio;
    if (fabsf(ratio - g_xa2_ratio) > 0.0001f) {
        g_xa2_ratio = ratio;
        IXAudio2SourceVoice_SetFrequencyRatio(g_xa2_source, ratio, XAUDIO2_COMMIT_NOW);
    }
    if (++g_xa2_submits % 2000 == 0)
        fprintf(stderr, "[XA2] queue %.0f samples (target %d), rate %.4f, underruns %u, drops %u\n",
                g_xa2_avg, XA2_TARGET, g_xa2_ratio, g_xa2_underruns, g_xa2_drops);
    return 1;
}

int xa2_get_buffer_size(void)
{
    return XA2_BUF_SAMPLES;
}

/* Output volume, 0..1, on the mastering voice: the in-game options' volume
 * and the silence while the options pause the game. */
void xa2_set_volume(float volume)
{
    g_xa2_volume = volume;
    if (g_xa2_master)
        IXAudio2MasteringVoice_SetVolume(g_xa2_master, volume, XAUDIO2_COMMIT_NOW);
}


/* The pacing contract of the APU frame thread (apu_core.c), over the
 * rate-nudged queue above. The frame thread keeps the queue between the
 * marks xa2_pacing_marks derives from buffer size x count, so the count
 * reported is the one that puts the high mark at XA2_TARGET: the depth the
 * rate nudge aims for. */
int xa2_get_buffer_count(void)
{
    return XA2_TARGET / XA2_BUF_SAMPLES;
}


/* Samples handed over and not yet played. */
int xa2_queued_samples(void)
{
    XAUDIO2_VOICE_STATE state;
    int queued = 0;

    if (!g_xa2_initialized || !g_xa2_source) return 0;
    IXAudio2SourceVoice_GetState(g_xa2_source, &state, XAUDIO2_VOICE_NOSAMPLESPLAYED);
    for (UINT32 i = 1; i <= state.BuffersQueued; i++)
        queued += g_xa2_size[(g_xa2_next_buf + XA2_NUM_BUFS - (int)i) % XA2_NUM_BUFS];
    if (state.BuffersQueued)
        g_xa2_drain_expected = 0;
    return queued;
}

/* The frame thread is about to pause: the next empty queue is on purpose,
 * so xa2_submit_samples does not count it as an underrun. */
void xa2_expect_drain(void)
{
    g_xa2_drain_expected = 1;
}

unsigned xa2_underruns(void)
{
    return g_xa2_underruns;
}

/* Samples the device has consumed since init. */
uint64_t xa2_samples_played(void)
{
    XAUDIO2_VOICE_STATE state;

    if (!g_xa2_initialized || !g_xa2_source) return 0;
    IXAudio2SourceVoice_GetState(g_xa2_source, &state, 0);
    return (uint64_t)state.SamplesPlayed;
}

#else /* !_WIN32: apu_sdl2.c implements the same contract over SDL2 */

typedef int apu_xaudio2_empty_translation_unit;

#endif /* _WIN32 */
