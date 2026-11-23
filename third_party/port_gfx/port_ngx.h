/* port_ngx.h: what port_gfx (Vulkan) and port_dlss (Direct3D 12) share to call NVIDIA NGX, the driver's DLSS
 * runtime, without NVIDIA's SDK: its common structures, its parameter object's vtable and finding _nvngx.dll.
 * Internal to this folder. */
#ifndef PORT_NGX_H
#define PORT_NGX_H

#include <stddef.h>
#include <stdint.h>
#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

typedef struct NgxPathList { const wchar_t *const *paths; unsigned int count; } NgxPathList;
typedef struct NgxLogging { void *callback; int minimum_level; unsigned char disable_other_sinks; } NgxLogging;
typedef struct NgxFeatureInfo { NgxPathList paths; void *internal; NgxLogging logging; } NgxFeatureInfo;

/* NVSDK_NGX_Parameter is a C++ interface built with MSVC: overloads of one name share a slot group, in reverse
 * declaration order. Its methods are reached through that vtable. */
enum {
    NGX_SET_VOIDP = 0, NGX_SET_D3D12, NGX_SET_D3D11, NGX_SET_I, NGX_SET_UI, NGX_SET_D, NGX_SET_F, NGX_SET_ULL,
    NGX_GET_VOIDP, NGX_GET_D3D12, NGX_GET_D3D11, NGX_GET_I, NGX_GET_UI, NGX_GET_D, NGX_GET_F, NGX_GET_ULL, NGX_RESET
};
typedef struct NgxParameter { void **vtable; } NgxParameter;
typedef void (*NgxSetUI)(NgxParameter *, const char *, unsigned int);
typedef void (*NgxSetI)(NgxParameter *, const char *, int);
typedef void (*NgxSetF)(NgxParameter *, const char *, float);
typedef void (*NgxSetP)(NgxParameter *, const char *, void *);
typedef int (*NgxGetI)(const NgxParameter *, const char *, int *);

static inline void ngx_ui(NgxParameter *p, const char *k, unsigned int v) { ((NgxSetUI)p->vtable[NGX_SET_UI])(p, k, v); }
static inline void ngx_i(NgxParameter *p, const char *k, int v) { ((NgxSetI)p->vtable[NGX_SET_I])(p, k, v); }
static inline void ngx_f(NgxParameter *p, const char *k, float v) { ((NgxSetF)p->vtable[NGX_SET_F])(p, k, v); }
static inline void ngx_p(NgxParameter *p, const char *k, void *v) { ((NgxSetP)p->vtable[NGX_SET_VOIDP])(p, k, v); }
static inline int ngx_get_i(const NgxParameter *p, const char *k, int def)
{
    int v = def;
    return (((NgxGetI)p->vtable[NGX_GET_I])(p, k, &v) & 0xFFF00000u) == 0xBAD00000u ? def : v;
}

#define NGX_OK(r) ((((unsigned)(r)) & 0xFFF00000u) != 0xBAD00000u)
#define NGX_VERSION_API 0x14
#define NGX_FEATURE_SUPER_SAMPLING 1
#define NGX_FLAG_MV_LOW_RES 2
#define NGX_FLAG_AUTO_EXPOSURE 64

#if defined(_WIN32)
static inline void *ngx_load(void)
{
    /* The driver records where its NGX core lives. */
    wchar_t dir[MAX_PATH] = L"", path[MAX_PATH + 32];
    DWORD size = sizeof dir;
    HMODULE m = NULL;
    if (RegGetValueW(HKEY_LOCAL_MACHINE, L"SOFTWARE\\NVIDIA Corporation\\Global\\NGXCore", L"FullPath",
                     RRF_RT_REG_SZ, NULL, dir, &size) == ERROR_SUCCESS && dir[0]) {
        _snwprintf(path, MAX_PATH + 31, L"%ls\\_nvngx.dll", dir);
        path[MAX_PATH + 31] = 0;
        m = LoadLibraryW(path);
    }
    if (!m)
        m = LoadLibraryW(L"_nvngx.dll");
    return m;
}
#define NGX_SYM(lib, name) ((void *)GetProcAddress((HMODULE)(lib), name))
#else
static inline void *ngx_load(void) { return NULL; }
#define NGX_SYM(lib, name) NULL
#endif

#endif
