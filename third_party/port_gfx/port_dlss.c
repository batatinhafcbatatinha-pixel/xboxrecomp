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
    int ready;
    NgxParameter *params;
    void *feature;
    uint32_t rw, rh, ow, oh;
    int quality;
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
    if (d.ready)
        return 1;
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
    if (!NGX_OK(d.capability(&caps)) || !caps || !ngx_get_i(caps, "SuperSampling.Available", 0)) {
        snprintf(g_error, sizeof g_error, "DLSS is not available on this card or driver (needs an RTX card and "
                                          "nvngx_dlss.dll next to the game)");
        if (d.shutdown) d.shutdown();
        return 0;
    }
    if (!NGX_OK(d.allocate(&d.params)) || !d.params) {
        snprintf(g_error, sizeof g_error, "NGX parameters could not be allocated");
        if (d.shutdown) d.shutdown();
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

void port_dlss_d3d12_stop(void)
{
    if (d.feature && d.release) d.release(d.feature);
    if (d.params && d.destroy) d.destroy(d.params);
    if (d.ready && d.shutdown) d.shutdown();
    memset(&d, 0, sizeof d);
}
