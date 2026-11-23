/* port_dlss: DLSS super resolution through NGX's Direct3D 12 entry points (see port_dlss.h). */
#include "port_dlss.h"

#include <stdio.h>
#include <string.h>

#include "port_ngx.h"

static char g_error[256] = "port_dlss_d3d12_start was not called";

typedef unsigned (*PfnInit)(unsigned long long, const wchar_t *, void *, const NgxFeatureInfo *, int);
typedef unsigned (*PfnParams)(NgxParameter **);
typedef unsigned (*PfnDestroy)(NgxParameter *);
typedef unsigned (*PfnCreate)(void *, int, NgxParameter *, void **);
typedef unsigned (*PfnEvaluate)(void *, const void *, const NgxParameter *, void *);
typedef unsigned (*PfnRelease)(void *);
typedef unsigned (*PfnShutdown)(void);

typedef void (*NgxSetD3D12)(NgxParameter *, const char *, void *);
static void ngx_resource(NgxParameter *p, const char *k, void *v) { ((NgxSetD3D12)p->vtable[NGX_SET_D3D12])(p, k, v); }

static struct {
    void *library, *device;
    PfnInit init;
    PfnParams capability, allocate;
    PfnDestroy destroy;
    PfnCreate create;
    PfnEvaluate evaluate;
    PfnRelease release;
    PfnShutdown shutdown;
    int up;      /* NGX initialised */
    int ready;   /* DLSS super resolution available */
    NgxParameter *params;
    void *feature;
    uint32_t rw, rh, ow, oh;
    int quality;
    /* DLSS Neural Rendering (feature 18): its own parameter block and feature */
    NgxParameter *nr_params;
    void *nr_feature;
    uint32_t nr_w, nr_h;
    PortDlssNrSettings nr_settings;
    int nr_failed;
} d;

const char *port_dlss_error(void) { return g_error; }
int port_dlss_ready(void) { return d.ready; }

int port_dlss_d3d12_start(void *device)
{
    static const wchar_t *paths[1];
    static wchar_t exe_dir[1024];
    NgxFeatureInfo info;
    NgxParameter *caps = NULL;
    unsigned r;
    if (d.up)
        return d.ready;
    memset(&d, 0, sizeof d);
    if (!device || !(d.library = ngx_load())) {
        snprintf(g_error, sizeof g_error, "no NVIDIA NGX runtime (_nvngx.dll) on this machine");
        return 0;
    }
    d.device = device;
    d.init = (PfnInit)NGX_SYM(d.library, "NVSDK_NGX_D3D12_Init");
    d.capability = (PfnParams)NGX_SYM(d.library, "NVSDK_NGX_D3D12_GetCapabilityParameters");
    d.allocate = (PfnParams)NGX_SYM(d.library, "NVSDK_NGX_D3D12_AllocateParameters");
    d.destroy = (PfnDestroy)NGX_SYM(d.library, "NVSDK_NGX_D3D12_DestroyParameters");
    d.create = (PfnCreate)NGX_SYM(d.library, "NVSDK_NGX_D3D12_CreateFeature");
    d.evaluate = (PfnEvaluate)NGX_SYM(d.library, "NVSDK_NGX_D3D12_EvaluateFeature_C");
    if (!d.evaluate)   /* the driver runtime exports it without the static library's _C suffix (as for Vulkan) */
        d.evaluate = (PfnEvaluate)NGX_SYM(d.library, "NVSDK_NGX_D3D12_EvaluateFeature");
    d.release = (PfnRelease)NGX_SYM(d.library, "NVSDK_NGX_D3D12_ReleaseFeature");
    /* Not Shutdown1(device): on the Vulkan side the driver runtime faults in it after frames were evaluated. */
    d.shutdown = (PfnShutdown)NGX_SYM(d.library, "NVSDK_NGX_D3D12_Shutdown");
    if (!d.init || !d.capability || !d.allocate || !d.create || !d.evaluate || !d.release) {
        snprintf(g_error, sizeof g_error, "the NGX runtime lacks the Direct3D 12 entry points");
        return 0;
    }
#if defined(_WIN32)
    {
        DWORD n = GetModuleFileNameW(NULL, exe_dir, 1023);
        wchar_t *slash;
        exe_dir[n < 1023 ? n : 1023] = 0;
        if ((slash = wcsrchr(exe_dir, L'\\'))) *slash = 0;
    }
#endif
    paths[0] = exe_dir;   /* nvngx_dlss.dll next to the game */
    memset(&info, 0, sizeof info);
    info.paths.paths = paths;
    info.paths.count = 1;
    r = d.init(0x5047u, exe_dir, device, &info, NGX_VERSION_API);
    if (!NGX_OK(r)) {
        snprintf(g_error, sizeof g_error, "NGX did not start (0x%08X)", r);
        return 0;
    }
    d.up = 1;   /* NGX stays up for Neural Rendering even without super resolution */
    if (!NGX_OK(d.allocate(&d.params)) || !d.params) {
        snprintf(g_error, sizeof g_error, "NGX parameters could not be allocated");
        return 0;
    }
    if (!NGX_OK(d.capability(&caps)) || !caps || !ngx_get_i(caps, "SuperSampling.Available", 0)) {
        snprintf(g_error, sizeof g_error, "DLSS is not available on this card or driver (needs an RTX card and "
                                          "nvngx_dlss.dll next to the game)");
        return 0;
    }
    d.ready = 1;
    g_error[0] = 0;
    return 1;
}

int port_dlss_d3d12_evaluate(void *list, void *color, void *depth, void *motion, void *output,
                             uint32_t rw, uint32_t rh, uint32_t ow, uint32_t oh, int quality,
                             float jitter_x, float jitter_y, int reset)
{
    if (!d.ready || !list || !color || !depth || !motion || !output || ow > rw * 3 || oh > rh * 3)
        return 0;
    if (!d.feature || d.rw != rw || d.rh != rh || d.ow != ow || d.oh != oh || d.quality != quality) {
        if (d.feature) { d.release(d.feature); d.feature = NULL; }
        ngx_ui(d.params, "Width", rw);
        ngx_ui(d.params, "Height", rh);
        ngx_ui(d.params, "OutWidth", ow);
        ngx_ui(d.params, "OutHeight", oh);
        ngx_i(d.params, "PerfQualityValue", quality);
        ngx_i(d.params, "DLSS.Feature.Create.Flags", NGX_FLAG_MV_LOW_RES | NGX_FLAG_AUTO_EXPOSURE);
        ngx_ui(d.params, "CreationNodeMask", 1);
        ngx_ui(d.params, "VisibilityNodeMask", 1);
        ngx_i(d.params, "DLSS.Enable.Output.Subrects", 0);
        if (!NGX_OK(d.create(list, NGX_FEATURE_SUPER_SAMPLING, d.params, &d.feature)) || !d.feature) {
            snprintf(g_error, sizeof g_error, "DLSS refused %ux%u -> %ux%u", rw, rh, ow, oh);
            d.feature = NULL;
            return 0;
        }
        d.rw = rw; d.rh = rh; d.ow = ow; d.oh = oh; d.quality = quality;
        reset = 1;
    }
    ngx_resource(d.params, "Color", color);
    ngx_resource(d.params, "Output", output);
    ngx_resource(d.params, "Depth", depth);
    ngx_resource(d.params, "MotionVectors", motion);
    ngx_f(d.params, "Jitter.Offset.X", jitter_x);
    ngx_f(d.params, "Jitter.Offset.Y", jitter_y);
    ngx_f(d.params, "MV.Scale.X", 1.0f);
    ngx_f(d.params, "MV.Scale.Y", 1.0f);
    ngx_i(d.params, "Reset", reset);
    ngx_f(d.params, "Sharpness", 0.0f);
    ngx_ui(d.params, "DLSS.Render.Subrect.Dimensions.Width", rw);
    ngx_ui(d.params, "DLSS.Render.Subrect.Dimensions.Height", rh);
    return NGX_OK(d.evaluate(list, d.feature, d.params, NULL));
}

/* ------------------------------------------------------------------ DLSS Neural Rendering ("DLSS 5", feature 18)
 * NVIDIA does not publish this feature: its parameter keys (DLSSNR.*) are those its nvngx_dlssnr.dll reads, as open
 * tools document them (video2dlssnr, MIT). It is asked of the driver's NGX core like any other feature; the core
 * loads nvngx_dlssnr.dll from the game's folder, the player's own copy (nothing here downloads it). */

void port_dlss_nr_defaults(PortDlssNrSettings *s)
{
    memset(s, 0, sizeof *s);
    s->intensity = 0.5f;
    s->local_structure = 0.5f;
    s->local_tone = 0.5f;
    s->auto_mask = 1;
    s->ui_correction = 1;
}

static int nr_dll_present(void)
{
#if defined(_WIN32)
    wchar_t path[1100];
    DWORD n = GetModuleFileNameW(NULL, path, 1000);
    wchar_t *slash;
    path[n < 1000 ? n : 1000] = 0;
    if ((slash = wcsrchr(path, L'\\'))) slash[1] = 0;
    wcscat(path, L"nvngx_dlssnr.dll");
    return GetFileAttributesW(path) != INVALID_FILE_ATTRIBUTES;
#else
    return 0;
#endif
}

int port_dlss_nr_d3d12_evaluate(void *list, void *color, void *depth, void *motion, void *output,
                                uint32_t width, uint32_t height, const PortDlssNrSettings *s, int reset)
{
    PortDlssNrSettings def;
    if (!s) { port_dlss_nr_defaults(&def); s = &def; }
    if (!d.up || d.nr_failed || !list || !color || !depth || !motion || !output || !width || !height)
        return 0;
    if (!d.nr_feature || d.nr_w != width || d.nr_h != height || memcmp(&d.nr_settings, s, sizeof *s) != 0) {
        if (d.nr_feature) { d.release(d.nr_feature); d.nr_feature = NULL; }
        if (!nr_dll_present()) {
            snprintf(g_error, sizeof g_error, "DLSS 5: nvngx_dlssnr.dll is not next to the game (it is the player's "
                                              "own; NVIDIA does not publish it)");
            d.nr_failed = 1;
            return 0;
        }
        if (!d.nr_params && (!NGX_OK(d.allocate(&d.nr_params)) || !d.nr_params)) {
            d.nr_failed = 1;
            return 0;
        }
        /* Everything the model reads is latched at create. */
        ngx_ui(d.nr_params, "Width", width);
        ngx_ui(d.nr_params, "Height", height);
        ngx_ui(d.nr_params, "OutWidth", width);
        ngx_ui(d.nr_params, "OutHeight", height);
        ngx_ui(d.nr_params, "PerfQualityValue", 2);
        ngx_ui(d.nr_params, "CreationNodeMask", 1);
        ngx_ui(d.nr_params, "VisibilityNodeMask", 1);
        ngx_ui(d.nr_params, "DLSSNR.Enabled", 1);
        ngx_ui(d.nr_params, "DLSSNR.Width", width);
        ngx_ui(d.nr_params, "DLSSNR.Height", height);
        ngx_ui(d.nr_params, "DLSSNR.Hint.Render.Preset", (unsigned)s->preset);
        ngx_f(d.nr_params, "DLSSNR.Intensity", s->intensity);
        ngx_ui(d.nr_params, "DLSSNR.Style", (unsigned)s->style);
        ngx_f(d.nr_params, "DLSSNR.LocalStructureStrength", s->local_structure);
        ngx_f(d.nr_params, "DLSSNR.LocalToneStrength", s->local_tone);
        ngx_ui(d.nr_params, "DLSSNR.UseAutoMask", (unsigned)s->auto_mask);
        ngx_ui(d.nr_params, "DLSSNR.UICorrection", (unsigned)s->ui_correction);
        if (!NGX_OK(d.create(list, PORT_DLSS_FEATURE_NEURAL_RENDERING, d.nr_params, &d.nr_feature)) || !d.nr_feature) {
            snprintf(g_error, sizeof g_error, "DLSS 5: the NGX runtime did not create Neural Rendering (the card, the "
                                              "driver or that nvngx_dlssnr.dll does not support it)");
            d.nr_feature = NULL;
            d.nr_failed = 1;   /* not again this run */
            return 0;
        }
        d.nr_w = width; d.nr_h = height; d.nr_settings = *s;
        reset = 1;
    }
    ngx_resource(d.nr_params, "DLSSNR.Color", color);
    ngx_resource(d.nr_params, "DLSSNR.Depth", depth);
    ngx_resource(d.nr_params, "DLSSNR.MVec", motion);
    ngx_resource(d.nr_params, "DLSSNR.Output", output);
    ngx_ui(d.nr_params, "DLSSNR.Enabled", 1);
    ngx_ui(d.nr_params, "DLSSNR.Reset", reset ? 1u : 0u);
    ngx_ui(d.nr_params, "DLSSNR.DepthInverted", 0);
    {
        static const char *const bases[] = {"DLSSNR.ColorSubrectBaseX", "DLSSNR.ColorSubrectBaseY",
            "DLSSNR.OutputSubrectBaseX", "DLSSNR.OutputSubrectBaseY", "DLSSNR.DepthSubrectBaseX",
            "DLSSNR.DepthSubrectBaseY", "DLSSNR.MVecSubrectBaseX", "DLSSNR.MVecSubrectBaseY"};
        static const char *const widths[] = {"DLSSNR.ColorSubrectWidth", "DLSSNR.OutputSubrectWidth",
            "DLSSNR.DepthSubrectWidth", "DLSSNR.MVecSubrectWidth"};
        static const char *const heights[] = {"DLSSNR.ColorSubrectHeight", "DLSSNR.OutputSubrectHeight",
            "DLSSNR.DepthSubrectHeight", "DLSSNR.MVecSubrectHeight"};
        for (int i = 0; i < 8; ++i) ngx_ui(d.nr_params, bases[i], 0);
        for (int i = 0; i < 4; ++i) { ngx_ui(d.nr_params, widths[i], width); ngx_ui(d.nr_params, heights[i], height); }
    }
    ngx_f(d.nr_params, "DLSSNR.MVecScaleX", 1.0f);
    ngx_f(d.nr_params, "DLSSNR.MVecScaleY", 1.0f);
    if (!NGX_OK(d.evaluate(list, d.nr_feature, d.nr_params, NULL))) {
        snprintf(g_error, sizeof g_error, "DLSS 5: evaluation refused");
        return 0;
    }
    return 1;
}

int port_dlss_nr_failed(void) { return d.nr_failed; }

void port_dlss_d3d12_stop(void)
{
    if (d.nr_feature && d.release) d.release(d.nr_feature);
    if (d.nr_params && d.destroy) d.destroy(d.nr_params);
    if (d.feature && d.release) d.release(d.feature);
    if (d.params && d.destroy) d.destroy(d.params);
    if (d.up && d.shutdown) d.shutdown();
    memset(&d, 0, sizeof d);
}
