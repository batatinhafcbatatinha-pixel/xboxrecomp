/*
 * D3D11 presentation for the framebuffer window (fb_present.c).
 *
 * GDI shows each frame with StretchDIBits on a 16 ms timer, scaled on the
 * CPU. This puts a D3D11 device and a swap chain on the same window and
 * presents the flipped frame through it, vsync-paced. The frame (the guest's
 * A8R8G8B8, i.e. B8G8R8A8 in memory order) is uploaded into its own texture
 * and drawn into a back buffer the size of the window's client area,
 * letterboxed to the frame's aspect ratio on black -- the window can be any
 * size (or borderless fullscreen) without stretching the picture.
 *
 * Opt-in with RECOMP_D3D_PRESENT=1; anything that fails falls back to GDI.
 * Linear filtering, RECOMP_PRESENT_FILTER=point for sharp pixels or =fsr for AMD FSR 1.0 (EASU);
 * RECOMP_PRESENT_ASPECT=stretch fills the window (ultrawide) instead of keeping the frame's shape;
 * RECOMP_PRESENT_VSYNC=0 presents without waiting for the display, as the
 * SDL window does.
 */
#if defined(_WIN32)
#define COBJMACROS
#include <windows.h>
#include <d3d11.h>
#include <dxgi.h>
#include <d3dcompiler.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include "gpu_present.h"

static ID3D11Device             *s_dev;
static ID3D11DeviceContext      *s_ctx;
static IDXGISwapChain           *s_chain;
static ID3D11RenderTargetView   *s_rtv;
static ID3D11Texture2D          *s_frame;      /* the guest frame, w x h */
static ID3D11ShaderResourceView *s_srv;
static ID3D11SamplerState       *s_samp;
static ID3D11VertexShader       *s_vs;
static ID3D11PixelShader        *s_ps;
static HWND                      s_hwnd;
static uint32_t                  s_w, s_h;      /* frame size */
static uint32_t                  s_bw, s_bh;    /* back buffer (client) size */
static int                       s_vsync = 1;
static int                       s_fsr;         /* RECOMP_PRESENT_FILTER=fsr */
static int                       s_stretch;     /* RECOMP_PRESENT_ASPECT=stretch: fill the window */

/* A triangle covering the viewport, from the vertex id; uv 0..1. */
static const char s_hlsl[] =
    "Texture2D t : register(t0); SamplerState s : register(s0);\n"
    "struct V { float4 p : SV_Position; float2 uv : TEXCOORD0; };\n"
    "V vs(uint i : SV_VertexID) { V o; float2 q = float2((i << 1) & 2, i & 2);\n"
    "  o.uv = q; o.p = float4(q * float2(2, -2) + float2(-1, 1), 0, 1); return o; }\n"
    "float4 ps(V i) : SV_Target { return float4(t.Sample(s, i.uv).rgb, 1); }\n";

/* AMD FidelityFX Super Resolution 1.0, EASU (MIT; Copyright (c) 2021 Advanced Micro Devices): the 32-bit path of
 * ffx_fsr1.h, as in the ports' other presenters. Enlarges the frame to the window while rebuilding its edges. */
static const char s_hlsl_fsr[] =
    "float Lu(float3 c) { return c.b * 0.5 + (c.r * 0.5 + c.g); }\n"
    "float3 Ld(int2 p, int2 n) { return t.Load(int3(clamp(p, int2(0, 0), n - 1), 0)).rgb; }\n"
    "void Es(inout float2 dir, inout float len, float w, float a, float b, float c, float d, float e) {\n"
    "  float lx = max(abs(d - c), abs(c - b)); lx = lx > 0.0 ? 1.0 / lx : 0.0; float dx = d - b; dir.x += dx * w;\n"
    "  lx = saturate(abs(dx) * lx); len += lx * lx * w;\n"
    "  float ly = max(abs(e - c), abs(c - a)); ly = ly > 0.0 ? 1.0 / ly : 0.0; float dy = e - a; dir.y += dy * w;\n"
    "  ly = saturate(abs(dy) * ly); len += ly * ly * w; }\n"
    "void Et(inout float3 ac, inout float aw, float2 o, float2 dir, float2 len2, float lob, float clp, float3 c) {\n"
    "  float2 v = float2(o.x * dir.x + o.y * dir.y, o.x * (-dir.y) + o.y * dir.x) * len2;\n"
    "  float d2 = min(dot(v, v), clp); float wb = 0.4 * d2 - 1.0, wa = lob * d2 - 1.0;\n"
    "  wb *= wb; wa *= wa; wb = 1.5625 * wb - 0.5625; float w = wb * wa; ac += c * w; aw += w; }\n"
    "float4 ps_fsr(V i) : SV_Target {\n"
    "  uint w0, h0; t.GetDimensions(w0, h0); int2 n = int2(w0, h0);\n"
    "  float2 pp = i.uv * float2(n) - 0.5; float2 fp = floor(pp); pp -= fp; int2 q = int2(fp);\n"
    "  float3 b = Ld(q + int2(0, -1), n), c = Ld(q + int2(1, -1), n), e = Ld(q + int2(-1, 0), n), f = Ld(q, n),\n"
    "         g = Ld(q + int2(1, 0), n), h = Ld(q + int2(2, 0), n), ii = Ld(q + int2(-1, 1), n), j = Ld(q + int2(0, 1), n),\n"
    "         k = Ld(q + int2(1, 1), n), l = Ld(q + int2(2, 1), n), m = Ld(q + int2(0, 2), n), o = Ld(q + int2(1, 2), n);\n"
    "  float bL = Lu(b), cL = Lu(c), eL = Lu(e), fL = Lu(f), gL = Lu(g), hL = Lu(h), iL = Lu(ii), jL = Lu(j),\n"
    "        kL = Lu(k), lL = Lu(l), mL = Lu(m), oL = Lu(o);\n"
    "  float2 dir = float2(0.0, 0.0); float len = 0.0;\n"
    "  Es(dir, len, (1.0 - pp.x) * (1.0 - pp.y), bL, eL, fL, gL, jL); Es(dir, len, pp.x * (1.0 - pp.y), cL, fL, gL, hL, kL);\n"
    "  Es(dir, len, (1.0 - pp.x) * pp.y, fL, iL, jL, kL, mL); Es(dir, len, pp.x * pp.y, gL, jL, kL, lL, oL);\n"
    "  float dr = dot(dir, dir); bool z = dr < 1.0 / 32768.0; dr = z ? 1.0 : rsqrt(dr); dir.x = z ? 1.0 : dir.x; dir *= dr;\n"
    "  len = len * 0.5; len *= len; float st = dot(dir, dir) / max(abs(dir.x), abs(dir.y));\n"
    "  float2 len2 = float2(1.0 + (st - 1.0) * len, 1.0 - 0.5 * len);\n"
    "  float lob = 0.5 + ((1.0 / 4.0 - 0.04) - 0.5) * len; float clp = 1.0 / lob;\n"
    "  float3 ac = float3(0.0, 0.0, 0.0); float aw = 0.0;\n"
    "  Et(ac, aw, float2(0.0, -1.0) - pp, dir, len2, lob, clp, b); Et(ac, aw, float2(1.0, -1.0) - pp, dir, len2, lob, clp, c);\n"
    "  Et(ac, aw, float2(-1.0, 1.0) - pp, dir, len2, lob, clp, ii); Et(ac, aw, float2(0.0, 1.0) - pp, dir, len2, lob, clp, j);\n"
    "  Et(ac, aw, float2(0.0, 0.0) - pp, dir, len2, lob, clp, f); Et(ac, aw, float2(-1.0, 0.0) - pp, dir, len2, lob, clp, e);\n"
    "  Et(ac, aw, float2(1.0, 1.0) - pp, dir, len2, lob, clp, k); Et(ac, aw, float2(2.0, 1.0) - pp, dir, len2, lob, clp, l);\n"
    "  Et(ac, aw, float2(2.0, 0.0) - pp, dir, len2, lob, clp, h); Et(ac, aw, float2(1.0, 0.0) - pp, dir, len2, lob, clp, g);\n"
    "  Et(ac, aw, float2(1.0, 2.0) - pp, dir, len2, lob, clp, o); Et(ac, aw, float2(0.0, 2.0) - pp, dir, len2, lob, clp, m);\n"
    "  float3 mn = min(min(f, g), min(j, k)), mx = max(max(f, g), max(j, k));\n"
    "  return float4(min(mx, max(mn, ac / aw)), 1); }\n";

int gpu_present_enabled(void)
{
    static int on = -1;
    if (on < 0) {
        const char *e = getenv("RECOMP_D3D_PRESENT");
        on = e && *e && *e != '0';
    }
    return on;
}

static void client_size(uint32_t *w, uint32_t *h)
{
    RECT r;
    GetClientRect(s_hwnd, &r);
    *w = r.right > r.left ? (uint32_t)(r.right - r.left) : 1;
    *h = r.bottom > r.top ? (uint32_t)(r.bottom - r.top) : 1;
}

static int make_rtv(void)
{
    ID3D11Texture2D *back = NULL;
    HRESULT hr = IDXGISwapChain_GetBuffer(s_chain, 0, &IID_ID3D11Texture2D, (void **)&back);
    if (FAILED(hr))
        return 0;
    hr = ID3D11Device_CreateRenderTargetView(s_dev, (ID3D11Resource *)back, NULL, &s_rtv);
    ID3D11Texture2D_Release(back);
    return SUCCEEDED(hr);
}

static ID3DBlob *compile(const char *entry, const char *target)
{
    ID3DBlob *code = NULL, *err = NULL;
    char source[sizeof s_hlsl + sizeof s_hlsl_fsr];
    memcpy(source, s_hlsl, sizeof s_hlsl - 1);
    memcpy(source + sizeof s_hlsl - 1, s_hlsl_fsr, sizeof s_hlsl_fsr);
    HRESULT hr = D3DCompile(source, strlen(source), "present", NULL, NULL, entry, target,
                            0, 0, &code, &err);
    if (FAILED(hr)) {
        fprintf(stderr, "  [D3D] present shader %s: %s\n", entry,
                err ? (const char *)ID3D10Blob_GetBufferPointer(err) : "failed");
        if (err) ID3D10Blob_Release(err);
        return NULL;
    }
    if (err) ID3D10Blob_Release(err);
    return code;
}

int gpu_present_init(HWND hwnd, uint32_t width, uint32_t height)
{
    DXGI_SWAP_CHAIN_DESC sd;
    D3D11_TEXTURE2D_DESC td;
    D3D11_SAMPLER_DESC smp;
    ID3DBlob *vsb, *psb;
    HRESULT hr;
    const char *v = getenv("RECOMP_PRESENT_VSYNC"), *f = getenv("RECOMP_PRESENT_FILTER");

    const char *a = getenv("RECOMP_PRESENT_ASPECT");
    if (v && v[0] == '0')
        s_vsync = 0;
    s_fsr = f && (f[0] == 'f' || f[0] == 'F');
    s_stretch = a && (a[0] == 's' || a[0] == 'S');
    s_hwnd = hwnd;
    s_w = width; s_h = height;
    client_size(&s_bw, &s_bh);
    ZeroMemory(&sd, sizeof sd);
    sd.BufferDesc.Width  = s_bw;
    sd.BufferDesc.Height = s_bh;
    sd.BufferDesc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    sd.SampleDesc.Count  = 1;
    sd.BufferUsage       = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.BufferCount       = 1;
    sd.OutputWindow      = hwnd;
    sd.Windowed          = TRUE;
    sd.SwapEffect        = DXGI_SWAP_EFFECT_DISCARD;

    hr = D3D11CreateDeviceAndSwapChain(NULL, D3D_DRIVER_TYPE_HARDWARE, NULL, 0,
                                       NULL, 0, D3D11_SDK_VERSION, &sd,
                                       &s_chain, &s_dev, NULL, &s_ctx);
    if (FAILED(hr)) {
        fprintf(stderr, "  [D3D] device/swap chain failed (0x%08lX); staying on GDI\n",
                (unsigned long)hr);
        return 0;
    }
    {
        /* Alt+Enter is the window's (borderless fullscreen, fb_present.c),
         * not DXGI's exclusive mode at 640x480. */
        IDXGIFactory *fac = NULL;
        if (SUCCEEDED(IDXGISwapChain_GetParent(s_chain, &IID_IDXGIFactory, (void **)&fac))) {
            IDXGIFactory_MakeWindowAssociation(fac, hwnd, DXGI_MWA_NO_ALT_ENTER);
            IDXGIFactory_Release(fac);
        }
    }
    ZeroMemory(&td, sizeof td);
    td.Width = width; td.Height = height; td.MipLevels = 1; td.ArraySize = 1;
    td.Format = DXGI_FORMAT_B8G8R8A8_UNORM; td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_DEFAULT; td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    ZeroMemory(&smp, sizeof smp);
    smp.Filter = (f && (f[0] == 'p' || f[0] == 'P')) ? D3D11_FILTER_MIN_MAG_MIP_POINT
                                                     : D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    smp.AddressU = smp.AddressV = smp.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    smp.MaxLOD = D3D11_FLOAT32_MAX;
    vsb = compile("vs", "vs_4_0");
    psb = compile(s_fsr ? "ps_fsr" : "ps", "ps_4_0");
    if (!vsb || !psb || !make_rtv()
        || FAILED(ID3D11Device_CreateTexture2D(s_dev, &td, NULL, &s_frame))
        || FAILED(ID3D11Device_CreateShaderResourceView(s_dev, (ID3D11Resource *)s_frame, NULL, &s_srv))
        || FAILED(ID3D11Device_CreateSamplerState(s_dev, &smp, &s_samp))
        || FAILED(ID3D11Device_CreateVertexShader(s_dev, ID3D10Blob_GetBufferPointer(vsb),
                                                  ID3D10Blob_GetBufferSize(vsb), NULL, &s_vs))
        || FAILED(ID3D11Device_CreatePixelShader(s_dev, ID3D10Blob_GetBufferPointer(psb),
                                                 ID3D10Blob_GetBufferSize(psb), NULL, &s_ps))) {
        fprintf(stderr, "  [D3D] present resources failed; staying on GDI\n");
        if (vsb) ID3D10Blob_Release(vsb);
        if (psb) ID3D10Blob_Release(psb);
        gpu_present_shutdown();
        return 0;
    }
    ID3D10Blob_Release(vsb);
    ID3D10Blob_Release(psb);
    fprintf(stderr, "  [D3D] presenting through D3D11 (%ux%u frame, vsync %s, %s filter, %s)\n",
            width, height, s_vsync ? "on" : "off",
            s_fsr ? "FSR 1.0" : smp.Filter == D3D11_FILTER_MIN_MAG_MIP_POINT ? "point" : "linear",
            s_stretch ? "stretched to the window" : "aspect kept");
    return 1;
}

/* Returns 0 when nothing was shown: the window is occluded or the display
 * is off, and Present does not wait for vsync then -- the caller must pace
 * itself instead of spinning. The pixels are read before this returns. */
int gpu_present_frame(const uint32_t *pixels)
{
    static const float black[4] = {0, 0, 0, 1};
    D3D11_VIEWPORT vp;
    uint32_t cw, ch;
    float sx, sy;

    if (!s_rtv || !pixels)
        return 0;
    client_size(&cw, &ch);
    if (cw != s_bw || ch != s_bh) {
        /* The window was resized (or went fullscreen): a back buffer its size. */
        ID3D11DeviceContext_OMSetRenderTargets(s_ctx, 0, NULL, NULL);
        ID3D11RenderTargetView_Release(s_rtv);
        s_rtv = NULL;
        if (FAILED(IDXGISwapChain_ResizeBuffers(s_chain, 0, cw, ch, DXGI_FORMAT_UNKNOWN, 0))
            || !make_rtv())
            return 0;
        s_bw = cw; s_bh = ch;
    }
    ID3D11DeviceContext_UpdateSubresource(s_ctx, (ID3D11Resource *)s_frame, 0, NULL,
                                          pixels, s_w * 4, 0);
    ID3D11DeviceContext_OMSetRenderTargets(s_ctx, 1, &s_rtv, NULL);
    ID3D11DeviceContext_ClearRenderTargetView(s_ctx, s_rtv, black);
    /* The largest rectangle of the frame's shape that fits, centred (or the whole window, stretched). */
    sx = (float)s_bw / (float)s_w;
    sy = (float)s_bh / (float)s_h;
    vp.Width = s_stretch ? (float)s_bw : (float)s_w * (sx < sy ? sx : sy);
    vp.Height = s_stretch ? (float)s_bh : (float)s_h * (sx < sy ? sx : sy);
    vp.TopLeftX = ((float)s_bw - vp.Width) * 0.5f;
    vp.TopLeftY = ((float)s_bh - vp.Height) * 0.5f;
    vp.MinDepth = 0; vp.MaxDepth = 1;
    ID3D11DeviceContext_RSSetViewports(s_ctx, 1, &vp);
    ID3D11DeviceContext_IASetPrimitiveTopology(s_ctx, D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ID3D11DeviceContext_IASetInputLayout(s_ctx, NULL);
    ID3D11DeviceContext_VSSetShader(s_ctx, s_vs, NULL, 0);
    ID3D11DeviceContext_PSSetShader(s_ctx, s_ps, NULL, 0);
    ID3D11DeviceContext_PSSetShaderResources(s_ctx, 0, 1, &s_srv);
    ID3D11DeviceContext_PSSetSamplers(s_ctx, 0, 1, &s_samp);
    ID3D11DeviceContext_Draw(s_ctx, 3, 0);
    /* Paced only when Present waited for the vblank: with RECOMP_PRESENT_VSYNC=0
     * the caller keeps its own 16 ms sleep instead of spinning a core. */
    return IDXGISwapChain_Present(s_chain, (UINT)s_vsync, 0) == S_OK && s_vsync;
}

void gpu_present_shutdown(void)
{
    if (s_ps)    { ID3D11PixelShader_Release(s_ps);           s_ps = NULL; }
    if (s_vs)    { ID3D11VertexShader_Release(s_vs);          s_vs = NULL; }
    if (s_samp)  { ID3D11SamplerState_Release(s_samp);        s_samp = NULL; }
    if (s_srv)   { ID3D11ShaderResourceView_Release(s_srv);   s_srv = NULL; }
    if (s_frame) { ID3D11Texture2D_Release(s_frame);          s_frame = NULL; }
    if (s_rtv)   { ID3D11RenderTargetView_Release(s_rtv);     s_rtv = NULL; }
    if (s_chain) { IDXGISwapChain_Release(s_chain);           s_chain = NULL; }
    if (s_ctx)   { ID3D11DeviceContext_Release(s_ctx);        s_ctx = NULL; }
    if (s_dev)   { ID3D11Device_Release(s_dev);               s_dev = NULL; }
}

int gpu_present_active(void)
{
    return s_rtv != NULL;
}
#endif
