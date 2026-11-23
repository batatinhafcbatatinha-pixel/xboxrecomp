/*
 * port_gfx: Vulkan presentation with FSR 1.0 and NVIDIA DLSS for the recompilers' runtimes (see port_gfx.h).
 *
 * Vulkan comes from the system loader at run time; no Vulkan SDK or import library is needed to build. Windows only
 * for now (the window surface is Win32); elsewhere port_gfx_open reports that and the runtime keeps its own path.
 *
 * Frame flow, per present:
 *   pixels -> staging buffer -> frame image (or the runtime's own image)
 *   then one of:  frame --blit (nearest/linear)--> window
 *                 frame --EASU--> intermediate --RCAS--> window              (FSR)
 *                 frame --DLSS--> intermediate --RCAS (or linear blit)--> window
 * The window's image is cleared first, so a frame kept at its aspect gets black bars.
 */
#include "port_gfx.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#define VK_USE_PLATFORM_WIN32_KHR 1
#endif
#define VK_NO_PROTOTYPES 1
#include <vulkan/vulkan.h>

#include "port_gfx_shaders.h"

#define FRAMES 2          /* frames in flight */
#define MAX_IMAGES 8      /* swapchain images */

static char g_error[256] = "port_gfx_open was not called";
static void fail(const char *what, VkResult r)
{
    snprintf(g_error, sizeof g_error, "%s failed (VkResult %d)", what, (int)r);
}

/* ------------------------------------------------------------------------------------------------ Vulkan entry points */

static PFN_vkGetInstanceProcAddr vkGetInstanceProcAddr_;

#define INSTANCE_FUNCS(X) \
    X(vkCreateInstance) X(vkEnumerateInstanceExtensionProperties) X(vkEnumerateInstanceVersion)
#define INSTANCE_LEVEL_FUNCS(X) \
    X(vkDestroyInstance) X(vkEnumeratePhysicalDevices) X(vkGetPhysicalDeviceProperties) \
    X(vkGetPhysicalDeviceProperties2) X(vkGetPhysicalDeviceQueueFamilyProperties) \
    X(vkGetPhysicalDeviceMemoryProperties) X(vkEnumerateDeviceExtensionProperties) X(vkCreateDevice) \
    X(vkGetDeviceProcAddr) X(vkDestroySurfaceKHR) X(vkGetPhysicalDeviceSurfaceSupportKHR) \
    X(vkGetPhysicalDeviceSurfaceCapabilitiesKHR) X(vkGetPhysicalDeviceSurfaceFormatsKHR) \
    X(vkGetPhysicalDeviceSurfacePresentModesKHR) X(vkGetPhysicalDeviceFormatProperties)
#if defined(_WIN32)
#define SURFACE_FUNCS(X) X(vkCreateWin32SurfaceKHR)
#else
#define SURFACE_FUNCS(X)
#endif
#define DEVICE_FUNCS(X) \
    X(vkDestroyDevice) X(vkGetDeviceQueue) X(vkDeviceWaitIdle) X(vkQueueSubmit) X(vkQueueWaitIdle) \
    X(vkCreateSwapchainKHR) X(vkDestroySwapchainKHR) X(vkGetSwapchainImagesKHR) X(vkAcquireNextImageKHR) \
    X(vkQueuePresentKHR) X(vkCreateImage) X(vkDestroyImage) X(vkCreateImageView) X(vkDestroyImageView) \
    X(vkGetImageMemoryRequirements) X(vkAllocateMemory) X(vkFreeMemory) X(vkBindImageMemory) \
    X(vkCreateBuffer) X(vkDestroyBuffer) X(vkGetBufferMemoryRequirements) X(vkBindBufferMemory) \
    X(vkMapMemory) X(vkUnmapMemory) X(vkCreateSampler) X(vkDestroySampler) X(vkCreateShaderModule) \
    X(vkDestroyShaderModule) X(vkCreateDescriptorSetLayout) X(vkDestroyDescriptorSetLayout) \
    X(vkCreatePipelineLayout) X(vkDestroyPipelineLayout) X(vkCreateDescriptorPool) X(vkDestroyDescriptorPool) \
    X(vkAllocateDescriptorSets) X(vkUpdateDescriptorSets) X(vkCreateRenderPass) X(vkDestroyRenderPass) \
    X(vkCreateFramebuffer) X(vkDestroyFramebuffer) X(vkCreateGraphicsPipelines) X(vkDestroyPipeline) \
    X(vkCreateCommandPool) X(vkDestroyCommandPool) X(vkAllocateCommandBuffers) X(vkResetCommandBuffer) \
    X(vkBeginCommandBuffer) X(vkEndCommandBuffer) X(vkCreateFence) X(vkDestroyFence) X(vkWaitForFences) \
    X(vkResetFences) X(vkCreateSemaphore) X(vkDestroySemaphore) X(vkCmdPipelineBarrier) \
    X(vkCmdCopyBufferToImage) X(vkCmdBeginRenderPass) X(vkCmdEndRenderPass) X(vkCmdBindPipeline) \
    X(vkCmdBindDescriptorSets) X(vkCmdPushConstants) X(vkCmdSetViewport) X(vkCmdSetScissor) X(vkCmdDraw) \
    X(vkCmdClearColorImage)

#define DECLARE(name) static PFN_##name name##_;
INSTANCE_FUNCS(DECLARE)
INSTANCE_LEVEL_FUNCS(DECLARE)
SURFACE_FUNCS(DECLARE)
DEVICE_FUNCS(DECLARE)
#undef DECLARE

/* ------------------------------------------------------------------------------------------------------- state */

typedef struct Image {
    VkImage image;
    VkDeviceMemory memory;
    VkImageView view;
    VkFormat format;
    uint32_t width, height;
} Image;

static struct {
    void *library;
    PortGfxConfig config;
    int scaler;                         /* in use */
    VkInstance instance;
    VkPhysicalDevice physical;
    VkPhysicalDeviceMemoryProperties memory;
    char device_name[256];
    VkDevice device;
    uint32_t family;
    VkQueue queue;
    VkSurfaceKHR surface;
    void *window;

    VkSwapchainKHR swapchain;
    VkFormat swap_format;
    VkExtent2D swap_extent;
    uint32_t image_count;
    VkImage images[MAX_IMAGES];
    VkImageView views[MAX_IMAGES];
    VkFramebuffer framebuffers[MAX_IMAGES];
    VkSemaphore rendered[MAX_IMAGES];

    VkRenderPass pass_window, pass_intermediate;
    VkSampler nearest, linear;
    VkDescriptorSetLayout set_layout;
    VkPipelineLayout layout;
    VkDescriptorPool pool;
    VkPipeline blit_window, easu_intermediate, rcas_window;

    VkCommandPool commands;
    struct {
        VkCommandBuffer cmd;
        VkFence done;
        VkSemaphore acquired;
        VkDescriptorSet source_set, intermediate_set;
        VkBuffer staging;
        VkDeviceMemory staging_memory;
        void *staging_map;
        VkDeviceSize staging_size;
    } frame[FRAMES];
    uint32_t frame_index;

    Image source;                       /* the pixels' image */
    Image intermediate;                 /* EASU / DLSS output, at the drawn size */
    VkFramebuffer intermediate_fb;
    int source_fresh;                   /* layout still UNDEFINED */
    int intermediate_fresh;
} g;

/* ------------------------------------------------------------------------------------------------------- DLSS */

/* NVIDIA NGX, the driver's DLSS runtime, called through _nvngx.dll's exports (the public NGX Vulkan API; the static
 * library of NVIDIA's SDK is a thin loader for the same functions). The network itself is nvngx_dlss.dll, next to the
 * game. Windows only. */
typedef struct NgxImageViewInfo {
    VkImageView view;
    VkImage image;
    VkImageSubresourceRange range;
    VkFormat format;
    unsigned int width, height;
} NgxImageViewInfo;
typedef struct NgxResource {
    union { NgxImageViewInfo image; struct { VkBuffer buffer; unsigned int size; } buffer; } resource;
    int type;              /* 0: image view */
    unsigned char read_write;
} NgxResource;
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

static void ngx_ui(NgxParameter *p, const char *k, unsigned int v) { ((NgxSetUI)p->vtable[NGX_SET_UI])(p, k, v); }
static void ngx_i(NgxParameter *p, const char *k, int v) { ((NgxSetI)p->vtable[NGX_SET_I])(p, k, v); }
static void ngx_f(NgxParameter *p, const char *k, float v) { ((NgxSetF)p->vtable[NGX_SET_F])(p, k, v); }
static void ngx_p(NgxParameter *p, const char *k, void *v) { ((NgxSetP)p->vtable[NGX_SET_VOIDP])(p, k, v); }
static int ngx_get_i(const NgxParameter *p, const char *k, int def)
{
    int v = def;
    return (((NgxGetI)p->vtable[NGX_GET_I])(p, k, &v) & 0xFFF00000u) == 0xBAD00000u ? def : v;
}

#define NGX_OK(r) ((((unsigned)(r)) & 0xFFF00000u) != 0xBAD00000u)
#define NGX_VERSION_API 0x14
#define NGX_FEATURE_SUPER_SAMPLING 1
#define NGX_FLAG_MV_LOW_RES 2
#define NGX_FLAG_AUTO_EXPOSURE 64

typedef unsigned (*PfnNgxInit)(unsigned long long, const wchar_t *, VkInstance, VkPhysicalDevice, VkDevice,
                               PFN_vkGetInstanceProcAddr, PFN_vkGetDeviceProcAddr, const NgxFeatureInfo *, int);
typedef unsigned (*PfnNgxParams)(NgxParameter **);
typedef unsigned (*PfnNgxCreate1)(VkDevice, VkCommandBuffer, int, NgxParameter *, void **);
typedef unsigned (*PfnNgxEvaluate)(VkCommandBuffer, const void *, const NgxParameter *, void *);
typedef unsigned (*PfnNgxRelease)(void *);
typedef unsigned (*PfnNgxShutdown1)(VkDevice);

static struct {
    void *library;
    PfnNgxInit init;
    PfnNgxParams capability, allocate;
    unsigned (*destroy)(NgxParameter *);
    PfnNgxCreate1 create;
    PfnNgxEvaluate evaluate;
    PfnNgxRelease release;
    PfnNgxShutdown1 shutdown;
    int ready;
    NgxParameter *params;
    void *feature;
    uint32_t render_w, render_h, out_w, out_h;
    Image depth, motion;               /* zeroed stand-ins when the renderer has none */
    int stand_ins_fresh;
    /* the renderer's own, for the next present */
    NgxResource user_depth, user_motion;
    int has_depth, has_motion;
    float jitter_x, jitter_y;
    int reset;
} ngx;

/* Device extensions DLSS needs, enabled when the device has them. */
static const char *const kNgxDeviceExtensions[] = {
    "VK_NVX_binary_import", "VK_NVX_image_view_handle", "VK_KHR_push_descriptor",
};

#if defined(_WIN32)
static void *ngx_load(void)
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
static void *ngx_load(void) { return NULL; }
#define NGX_SYM(lib, name) NULL
#endif

static int ngx_start(void)
{
    static const wchar_t *paths[1];
    static wchar_t exe_dir[1024];
    NgxFeatureInfo info;
    NgxParameter *caps = NULL;
    unsigned r;
    if (!(ngx.library = ngx_load())) {
        snprintf(g_error, sizeof g_error, "DLSS: no NVIDIA NGX runtime (_nvngx.dll) on this machine");
        return 0;
    }
    ngx.init = (PfnNgxInit)NGX_SYM(ngx.library, "NVSDK_NGX_VULKAN_Init");
    ngx.capability = (PfnNgxParams)NGX_SYM(ngx.library, "NVSDK_NGX_VULKAN_GetCapabilityParameters");
    ngx.allocate = (PfnNgxParams)NGX_SYM(ngx.library, "NVSDK_NGX_VULKAN_AllocateParameters");
    ngx.destroy = (unsigned (*)(NgxParameter *))NGX_SYM(ngx.library, "NVSDK_NGX_VULKAN_DestroyParameters");
    ngx.create = (PfnNgxCreate1)NGX_SYM(ngx.library, "NVSDK_NGX_VULKAN_CreateFeature1");
    ngx.evaluate = (PfnNgxEvaluate)NGX_SYM(ngx.library, "NVSDK_NGX_VULKAN_EvaluateFeature_C");
    ngx.release = (PfnNgxRelease)NGX_SYM(ngx.library, "NVSDK_NGX_VULKAN_ReleaseFeature");
    ngx.shutdown = (PfnNgxShutdown1)NGX_SYM(ngx.library, "NVSDK_NGX_VULKAN_Shutdown1");
    if (!ngx.init || !ngx.capability || !ngx.allocate || !ngx.create || !ngx.evaluate || !ngx.release) {
        snprintf(g_error, sizeof g_error, "DLSS: the NGX runtime lacks the Vulkan entry points");
        return 0;
    }
#if defined(_WIN32)
    {
        DWORD n = GetModuleFileNameW(NULL, exe_dir, 1023);
        wchar_t *slash = wcsrchr(exe_dir, L'\\');
        exe_dir[n < 1023 ? n : 1023] = 0;
        if (slash) *slash = 0;
    }
#endif
    paths[0] = exe_dir;   /* nvngx_dlss.dll next to the game */
    memset(&info, 0, sizeof info);
    info.paths.paths = paths;
    info.paths.count = 1;
    r = ngx.init(0x5047u /* any id: DLSS needs none registered */, exe_dir, g.instance, g.physical, g.device,
                 vkGetInstanceProcAddr_, vkGetDeviceProcAddr_, &info, NGX_VERSION_API);
    if (!NGX_OK(r)) {
        snprintf(g_error, sizeof g_error, "DLSS: NGX did not start (0x%08X)", r);
        return 0;
    }
    if (!NGX_OK(ngx.capability(&caps)) || !caps || !ngx_get_i(caps, "SuperSampling.Available", 0)) {
        snprintf(g_error, sizeof g_error, "DLSS: not available on this card or driver (needs an RTX card and "
                                          "nvngx_dlss.dll next to the game)");
        if (ngx.shutdown) ngx.shutdown(g.device);
        return 0;
    }
    ngx.ready = 1;
    return 1;
}

/* ---------------------------------------------------------------------------------------------- small helpers */

static uint32_t memory_type(uint32_t bits, VkMemoryPropertyFlags want)
{
    for (uint32_t i = 0; i < g.memory.memoryTypeCount; ++i)
        if ((bits & (1u << i)) && (g.memory.memoryTypes[i].propertyFlags & want) == want)
            return i;
    return UINT32_MAX;
}

static void destroy_image(Image *im)
{
    if (im->view) vkDestroyImageView_(g.device, im->view, NULL);
    if (im->image) vkDestroyImage_(g.device, im->image, NULL);
    if (im->memory) vkFreeMemory_(g.device, im->memory, NULL);
    memset(im, 0, sizeof *im);
}

static int create_image(Image *im, VkFormat format, uint32_t w, uint32_t h, VkImageUsageFlags usage)
{
    VkImageCreateInfo ci = {VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    VkMemoryRequirements req;
    VkMemoryAllocateInfo ai = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    VkImageViewCreateInfo vi = {VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    VkResult r;
    destroy_image(im);
    ci.imageType = VK_IMAGE_TYPE_2D;
    ci.format = format;
    ci.extent.width = w; ci.extent.height = h; ci.extent.depth = 1;
    ci.mipLevels = 1; ci.arrayLayers = 1;
    ci.samples = VK_SAMPLE_COUNT_1_BIT;
    ci.tiling = VK_IMAGE_TILING_OPTIMAL;
    ci.usage = usage;
    ci.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    if ((r = vkCreateImage_(g.device, &ci, NULL, &im->image)) != VK_SUCCESS) { fail("vkCreateImage", r); return 0; }
    vkGetImageMemoryRequirements_(g.device, im->image, &req);
    ai.allocationSize = req.size;
    ai.memoryTypeIndex = memory_type(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (ai.memoryTypeIndex == UINT32_MAX || vkAllocateMemory_(g.device, &ai, NULL, &im->memory) != VK_SUCCESS ||
        vkBindImageMemory_(g.device, im->image, im->memory, 0) != VK_SUCCESS) {
        snprintf(g_error, sizeof g_error, "image memory (%ux%u)", w, h);
        destroy_image(im);
        return 0;
    }
    vi.image = im->image;
    vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
    vi.format = format;
    vi.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    vi.subresourceRange.levelCount = 1;
    vi.subresourceRange.layerCount = 1;
    if ((r = vkCreateImageView_(g.device, &vi, NULL, &im->view)) != VK_SUCCESS) {
        fail("vkCreateImageView", r);
        destroy_image(im);
        return 0;
    }
    im->format = format; im->width = w; im->height = h;
    return 1;
}

static void barrier(VkCommandBuffer cmd, VkImage image, VkImageLayout from, VkImageLayout to,
                    VkAccessFlags src_access, VkAccessFlags dst_access,
                    VkPipelineStageFlags src_stage, VkPipelineStageFlags dst_stage)
{
    VkImageMemoryBarrier b = {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    b.srcAccessMask = src_access; b.dstAccessMask = dst_access;
    b.oldLayout = from; b.newLayout = to;
    b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = image;
    b.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    b.subresourceRange.levelCount = 1;
    b.subresourceRange.layerCount = 1;
    vkCmdPipelineBarrier_(cmd, src_stage, dst_stage, 0, 0, NULL, 0, NULL, 1, &b);
}

static VkShaderModule shader(const uint32_t *code, size_t bytes)
{
    VkShaderModuleCreateInfo ci = {VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    VkShaderModule m = VK_NULL_HANDLE;
    ci.codeSize = bytes;
    ci.pCode = code;
    vkCreateShaderModule_(g.device, &ci, NULL, &m);
    return m;
}

/* ------------------------------------------------------------------------------------------------- configuration */

void port_gfx_defaults(PortGfxConfig *c)
{
    memset(c, 0, sizeof *c);
    c->vsync = 1;
    c->adapter = -1;
    c->scaler = PORT_GFX_FSR;
    c->sharpness = 0.25f;
    c->dlss_quality = PORT_GFX_DLSS_QUALITY;
}

void port_gfx_config_from_env(PortGfxConfig *c)
{
    const char *v;
    if ((v = getenv("PORT_GFX_VSYNC")) && *v) c->vsync = *v != '0';
    if ((v = getenv("PORT_GFX_ADAPTER")) && *v) c->adapter = atoi(v);
    if ((v = getenv("PORT_GFX_STRETCH")) && *v) c->stretch = *v != '0';
    if ((v = getenv("PORT_GFX_SHARPNESS")) && *v) c->sharpness = (float)atof(v);
    if ((v = getenv("PORT_GFX_SCALER")) && *v)
        c->scaler = (v[0] == 'n' || v[0] == 'p') ? PORT_GFX_NEAREST : v[0] == 'l' ? PORT_GFX_LINEAR
                  : v[0] == 'd' ? PORT_GFX_DLSS : PORT_GFX_FSR;
    if ((v = getenv("PORT_GFX_DLSS_QUALITY")) && *v)
        c->dlss_quality = v[0] == 'p' ? PORT_GFX_DLSS_PERFORMANCE : v[0] == 'b' ? PORT_GFX_DLSS_BALANCED
                        : v[0] == 'u' ? PORT_GFX_DLSS_ULTRA_PERFORMANCE : v[0] == 'd' ? PORT_GFX_DLSS_DLAA
                        : PORT_GFX_DLSS_QUALITY;
}

const char *port_gfx_error(void) { return g_error; }
const char *port_gfx_device_name(void) { return g.device_name; }
int port_gfx_scaler(void) { return g.scaler; }
void *port_gfx_instance(void) { return (void *)g.instance; }
void *port_gfx_physical_device(void) { return (void *)g.physical; }
void *port_gfx_device(void) { return (void *)g.device; }
void *port_gfx_queue(void) { return (void *)g.queue; }
uint32_t port_gfx_queue_family(void) { return g.family; }

/* ----------------------------------------------------------------------------------------- instance and device */

static int load_vulkan(void)
{
#if defined(_WIN32)
    g.library = LoadLibraryA("vulkan-1.dll");
    if (g.library)
        vkGetInstanceProcAddr_ = (PFN_vkGetInstanceProcAddr)(void *)GetProcAddress((HMODULE)g.library,
                                                                                   "vkGetInstanceProcAddr");
#endif
    if (!vkGetInstanceProcAddr_) {
        snprintf(g_error, sizeof g_error, "no Vulkan loader (vulkan-1.dll) on this machine");
        return 0;
    }
#define LOAD(name) name##_ = (PFN_##name)vkGetInstanceProcAddr_(NULL, #name);
    INSTANCE_FUNCS(LOAD)
#undef LOAD
    return vkCreateInstance_ != NULL;
}

static int has_extension(const VkExtensionProperties *list, uint32_t n, const char *name)
{
    for (uint32_t i = 0; i < n; ++i)
        if (!strcmp(list[i].extensionName, name))
            return 1;
    return 0;
}

static int create_instance(void)
{
    const char *extensions[4];
    uint32_t count = 0, version = VK_API_VERSION_1_0;   /* a 1.0 loader has no vkEnumerateInstanceVersion */
    VkApplicationInfo app = {VK_STRUCTURE_TYPE_APPLICATION_INFO};
    VkInstanceCreateInfo ci = {VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    VkResult r;
    if (vkEnumerateInstanceVersion_)
        vkEnumerateInstanceVersion_(&version);
    extensions[count++] = VK_KHR_SURFACE_EXTENSION_NAME;
#if defined(_WIN32)
    extensions[count++] = VK_KHR_WIN32_SURFACE_EXTENSION_NAME;
#endif
    app.pApplicationName = "port_gfx";
    app.pEngineName = "port_gfx";
    app.apiVersion = version >= VK_API_VERSION_1_2 ? VK_API_VERSION_1_2 : version >= VK_API_VERSION_1_1 ? VK_API_VERSION_1_1
                                                                                         : VK_API_VERSION_1_0;
    ci.pApplicationInfo = &app;
    ci.enabledExtensionCount = count;
    ci.ppEnabledExtensionNames = extensions;
    if ((r = vkCreateInstance_(&ci, NULL, &g.instance)) != VK_SUCCESS) {
        if (r == VK_ERROR_INCOMPATIBLE_DRIVER)
            snprintf(g_error, sizeof g_error, "no graphics card here supports Vulkan (no working Vulkan driver)");
        else
            fail("vkCreateInstance", r);
        return 0;
    }
#define LOAD(name) name##_ = (PFN_##name)vkGetInstanceProcAddr_(g.instance, #name);
    INSTANCE_LEVEL_FUNCS(LOAD)
    SURFACE_FUNCS(LOAD)
#undef LOAD
    vkGetDeviceProcAddr_ = vkGetDeviceProcAddr_ ? vkGetDeviceProcAddr_
                                                : (PFN_vkGetDeviceProcAddr)vkGetInstanceProcAddr_(g.instance,
                                                                                                  "vkGetDeviceProcAddr");
    return 1;
}

/* The launcher lists GPUs by DXGI index; Vulkan devices carry the same LUID. */
static int adapter_luid(int index, uint8_t luid[8])
{
#if defined(_WIN32)
    typedef HRESULT(WINAPI * PfnFactory)(REFIID, void **);
    static const GUID iid_factory1 = {0x770aae78, 0xf26f, 0x4dba, {0xa8, 0x29, 0x25, 0x3c, 0x83, 0xd1, 0xb3, 0x87}};
    HMODULE dxgi = LoadLibraryA("dxgi.dll");
    PfnFactory create = dxgi ? (PfnFactory)(void *)GetProcAddress(dxgi, "CreateDXGIFactory1") : NULL;
    void *factory = NULL, *adapter = NULL;
    int ok = 0;
    if (index < 0 || !create || FAILED(create(&iid_factory1, &factory)) || !factory)
        return 0;
    /* IDXGIFactory1::EnumAdapters1 is slot 12, IDXGIAdapter1::GetDesc1 slot 10 (COM vtables). */
    typedef HRESULT(STDMETHODCALLTYPE * PfnEnum)(void *, UINT, void **);
    typedef HRESULT(STDMETHODCALLTYPE * PfnDesc)(void *, void *);
    typedef ULONG(STDMETHODCALLTYPE * PfnRelease)(void *);
    if (SUCCEEDED(((PfnEnum)(*(void ***)factory)[12])(factory, (UINT)index, &adapter)) && adapter) {
        struct { WCHAR d[128]; UINT v, dev, sub, rev; SIZE_T a, b, c; LUID luid; UINT flags; } desc;
        if (SUCCEEDED(((PfnDesc)(*(void ***)adapter)[10])(adapter, &desc))) {
            memcpy(luid, &desc.luid, 8);
            ok = 1;
        }
        ((PfnRelease)(*(void ***)adapter)[2])(adapter);
    }
    ((PfnRelease)(*(void ***)factory)[2])(factory);
    return ok;
#else
    (void)index; (void)luid;
    return 0;
#endif
}

static int pick_device(void)
{
    VkPhysicalDevice list[16];
    uint32_t n = 16, best = UINT32_MAX;
    uint8_t want[8];
    int by_luid = adapter_luid(g.config.adapter, want);
    vkEnumeratePhysicalDevices_(g.instance, &n, list);
    if (n == 0) {
        snprintf(g_error, sizeof g_error, "no graphics card here supports Vulkan");
        return 0;
    }
    for (uint32_t i = 0; i < n && best == UINT32_MAX; ++i) {
        if (by_luid && vkGetPhysicalDeviceProperties2_) {
            VkPhysicalDeviceIDProperties id = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ID_PROPERTIES};
            VkPhysicalDeviceProperties2 p2 = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2};
            p2.pNext = &id;
            vkGetPhysicalDeviceProperties2_(list[i], &p2);
            if (id.deviceLUIDValid && !memcmp(id.deviceLUID, want, 8))
                best = i;
        }
    }
    for (uint32_t i = 0; i < n && best == UINT32_MAX; ++i) {
        VkPhysicalDeviceProperties p;
        vkGetPhysicalDeviceProperties_(list[i], &p);
        if (p.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU)
            best = i;
    }
    if (best == UINT32_MAX)
        best = 0;
    g.physical = list[best];
    {
        VkPhysicalDeviceProperties p;
        vkGetPhysicalDeviceProperties_(g.physical, &p);
        snprintf(g.device_name, sizeof g.device_name, "%s", p.deviceName);
    }
    vkGetPhysicalDeviceMemoryProperties_(g.physical, &g.memory);
    return 1;
}

static int create_surface(void *window)
{
#if defined(_WIN32)
    VkWin32SurfaceCreateInfoKHR ci = {VK_STRUCTURE_TYPE_WIN32_SURFACE_CREATE_INFO_KHR};
    VkResult r;
    ci.hinstance = GetModuleHandleW(NULL);
    ci.hwnd = (HWND)window;
    if (!vkCreateWin32SurfaceKHR_ || (r = vkCreateWin32SurfaceKHR_(g.instance, &ci, NULL, &g.surface)) != VK_SUCCESS) {
        snprintf(g_error, sizeof g_error, "the window's Vulkan surface could not be made");
        return 0;
    }
    return 1;
#else
    (void)window;
    snprintf(g_error, sizeof g_error, "port_gfx presents on Windows only for now");
    return 0;
#endif
}

static int create_device(void)
{
    VkQueueFamilyProperties families[16];
    uint32_t nf = 16, ne = 0;
    VkExtensionProperties *available;
    const char *extensions[8];
    uint32_t count = 0;
    float priority = 1.0f;
    VkDeviceQueueCreateInfo qi = {VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
    VkDeviceCreateInfo ci = {VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
    VkResult r;

    vkGetPhysicalDeviceQueueFamilyProperties_(g.physical, &nf, families);
    g.family = UINT32_MAX;
    for (uint32_t i = 0; i < nf && g.family == UINT32_MAX; ++i) {
        VkBool32 present = VK_FALSE;
        vkGetPhysicalDeviceSurfaceSupportKHR_(g.physical, i, g.surface, &present);
        if ((families[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) && present)
            g.family = i;
    }
    if (g.family == UINT32_MAX) {
        snprintf(g_error, sizeof g_error, "%s cannot draw and present to the window from one queue", g.device_name);
        return 0;
    }
    vkEnumerateDeviceExtensionProperties_(g.physical, NULL, &ne, NULL);
    available = (VkExtensionProperties *)calloc(ne ? ne : 1, sizeof *available);
    vkEnumerateDeviceExtensionProperties_(g.physical, NULL, &ne, available);
    extensions[count++] = VK_KHR_SWAPCHAIN_EXTENSION_NAME;
    if (g.config.scaler == PORT_GFX_DLSS)
        for (size_t i = 0; i < sizeof kNgxDeviceExtensions / sizeof *kNgxDeviceExtensions; ++i)
            if (has_extension(available, ne, kNgxDeviceExtensions[i]))
                extensions[count++] = kNgxDeviceExtensions[i];
    free(available);

    qi.queueFamilyIndex = g.family;
    qi.queueCount = 1;
    qi.pQueuePriorities = &priority;
    ci.queueCreateInfoCount = 1;
    ci.pQueueCreateInfos = &qi;
    ci.enabledExtensionCount = count;
    ci.ppEnabledExtensionNames = extensions;
    if ((r = vkCreateDevice_(g.physical, &ci, NULL, &g.device)) != VK_SUCCESS) { fail("vkCreateDevice", r); return 0; }
#define LOAD(name) name##_ = (PFN_##name)vkGetDeviceProcAddr_(g.device, #name);
    DEVICE_FUNCS(LOAD)
#undef LOAD
    vkGetDeviceQueue_(g.device, g.family, 0, &g.queue);
    return 1;
}

/* ------------------------------------------------------------------------------------------------- swapchain */

static void destroy_pipelines(void);

static void destroy_swapchain(void)
{
    for (uint32_t i = 0; i < g.image_count; ++i) {
        if (g.framebuffers[i]) vkDestroyFramebuffer_(g.device, g.framebuffers[i], NULL);
        if (g.views[i]) vkDestroyImageView_(g.device, g.views[i], NULL);
        if (g.rendered[i]) vkDestroySemaphore_(g.device, g.rendered[i], NULL);
        g.framebuffers[i] = VK_NULL_HANDLE; g.views[i] = VK_NULL_HANDLE; g.rendered[i] = VK_NULL_HANDLE;
    }
    g.image_count = 0;
}

static VkRenderPass make_pass(VkFormat format, VkAttachmentLoadOp load, VkImageLayout final_layout)
{
    VkAttachmentDescription a = {0};
    VkAttachmentReference ref = {0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
    VkSubpassDescription sub = {0};
    VkSubpassDependency dep = {0};
    VkRenderPassCreateInfo ci = {VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO};
    VkRenderPass pass = VK_NULL_HANDLE;
    a.format = format;
    a.samples = VK_SAMPLE_COUNT_1_BIT;
    a.loadOp = load;
    a.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    a.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    a.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    a.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    a.finalLayout = final_layout;
    sub.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    sub.colorAttachmentCount = 1;
    sub.pColorAttachments = &ref;
    dep.srcSubpass = VK_SUBPASS_EXTERNAL;
    dep.srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT |
                       VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT;
    dep.dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    dep.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    ci.attachmentCount = 1; ci.pAttachments = &a;
    ci.subpassCount = 1; ci.pSubpasses = &sub;
    ci.dependencyCount = 1; ci.pDependencies = &dep;
    vkCreateRenderPass_(g.device, &ci, NULL, &pass);
    return pass;
}

static int create_swapchain(void)
{
    VkSurfaceCapabilitiesKHR caps;
    VkSurfaceFormatKHR formats[32];
    VkPresentModeKHR modes[8];
    uint32_t nf = 32, nm = 8;
    VkSwapchainCreateInfoKHR ci = {VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR};
    VkSwapchainKHR old = g.swapchain;
    VkResult r;

    vkGetPhysicalDeviceSurfaceCapabilitiesKHR_(g.physical, g.surface, &caps);
    if (caps.currentExtent.width == 0 || caps.currentExtent.height == 0)
        return 0;   /* minimised */
    vkGetPhysicalDeviceSurfaceFormatsKHR_(g.physical, g.surface, &nf, formats);
    vkGetPhysicalDeviceSurfacePresentModesKHR_(g.physical, g.surface, &nm, modes);
    ci.imageFormat = formats[0].format;
    ci.imageColorSpace = formats[0].colorSpace;
    for (uint32_t i = 0; i < nf; ++i)
        if (formats[i].format == VK_FORMAT_B8G8R8A8_UNORM || formats[i].format == VK_FORMAT_R8G8B8A8_UNORM) {
            ci.imageFormat = formats[i].format;
            ci.imageColorSpace = formats[i].colorSpace;
            break;
        }
    ci.presentMode = VK_PRESENT_MODE_FIFO_KHR;   /* always there */
    if (!g.config.vsync) {
        for (uint32_t i = 0; i < nm; ++i)
            if (modes[i] == VK_PRESENT_MODE_MAILBOX_KHR) ci.presentMode = modes[i];
        if (ci.presentMode == VK_PRESENT_MODE_FIFO_KHR)
            for (uint32_t i = 0; i < nm; ++i)
                if (modes[i] == VK_PRESENT_MODE_IMMEDIATE_KHR) ci.presentMode = modes[i];
    }
    ci.surface = g.surface;
    ci.minImageCount = caps.minImageCount + 1;
    if (caps.maxImageCount && ci.minImageCount > caps.maxImageCount) ci.minImageCount = caps.maxImageCount;
    if (ci.minImageCount > MAX_IMAGES) ci.minImageCount = MAX_IMAGES;
    ci.imageExtent = caps.currentExtent;
    ci.imageArrayLayers = 1;
    ci.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    ci.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
    ci.preTransform = caps.currentTransform;
    ci.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
    ci.clipped = VK_TRUE;
    ci.oldSwapchain = old;

    vkDeviceWaitIdle_(g.device);
    destroy_swapchain();
    if ((r = vkCreateSwapchainKHR_(g.device, &ci, NULL, &g.swapchain)) != VK_SUCCESS) {
        fail("vkCreateSwapchainKHR", r);
        g.swapchain = VK_NULL_HANDLE;
        if (old) vkDestroySwapchainKHR_(g.device, old, NULL);
        return 0;
    }
    if (old) vkDestroySwapchainKHR_(g.device, old, NULL);
    if (g.swap_format != ci.imageFormat) {
        /* The window pass and its pipelines follow the format. */
        destroy_pipelines();
        if (g.pass_window) vkDestroyRenderPass_(g.device, g.pass_window, NULL);
        g.pass_window = VK_NULL_HANDLE;
    }
    g.swap_format = ci.imageFormat;
    g.swap_extent = ci.imageExtent;
    g.image_count = MAX_IMAGES;
    vkGetSwapchainImagesKHR_(g.device, g.swapchain, &g.image_count, g.images);
    if (!g.pass_window)
        g.pass_window = make_pass(g.swap_format, VK_ATTACHMENT_LOAD_OP_CLEAR, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR);
    for (uint32_t i = 0; i < g.image_count; ++i) {
        VkImageViewCreateInfo vi = {VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
        VkFramebufferCreateInfo fi = {VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};
        VkSemaphoreCreateInfo si = {VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
        vi.image = g.images[i];
        vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
        vi.format = g.swap_format;
        vi.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        vi.subresourceRange.levelCount = 1;
        vi.subresourceRange.layerCount = 1;
        vkCreateImageView_(g.device, &vi, NULL, &g.views[i]);
        fi.renderPass = g.pass_window;
        fi.attachmentCount = 1;
        fi.pAttachments = &g.views[i];
        fi.width = g.swap_extent.width;
        fi.height = g.swap_extent.height;
        fi.layers = 1;
        vkCreateFramebuffer_(g.device, &fi, NULL, &g.framebuffers[i]);
        vkCreateSemaphore_(g.device, &si, NULL, &g.rendered[i]);
    }
    return 1;
}

/* ------------------------------------------------------------------------------------------------- pipelines */

typedef struct Push { float source_w, source_h, output_w, output_h, offset_x, offset_y, sharpness, pad; } Push;

static VkPipeline make_pipeline(VkRenderPass pass, VkShaderModule vert, VkShaderModule frag)
{
    VkPipelineShaderStageCreateInfo stages[2] = {{VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO},
                                                 {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO}};
    VkPipelineVertexInputStateCreateInfo vin = {VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    VkPipelineInputAssemblyStateCreateInfo ia = {VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
    VkPipelineViewportStateCreateInfo vp = {VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
    VkPipelineRasterizationStateCreateInfo rs = {VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
    VkPipelineMultisampleStateCreateInfo ms = {VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
    VkPipelineColorBlendAttachmentState att = {0};
    VkPipelineColorBlendStateCreateInfo cb = {VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
    VkDynamicState dyn[2] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo ds = {VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
    VkGraphicsPipelineCreateInfo ci = {VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
    VkPipeline p = VK_NULL_HANDLE;
    stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT; stages[0].module = vert; stages[0].pName = "main";
    stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT; stages[1].module = frag; stages[1].pName = "main";
    ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    vp.viewportCount = 1; vp.scissorCount = 1;
    rs.polygonMode = VK_POLYGON_MODE_FILL; rs.cullMode = VK_CULL_MODE_NONE; rs.lineWidth = 1.0f;
    ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
    att.colorWriteMask = 0xF;
    cb.attachmentCount = 1; cb.pAttachments = &att;
    ds.dynamicStateCount = 2; ds.pDynamicStates = dyn;
    ci.stageCount = 2; ci.pStages = stages;
    ci.pVertexInputState = &vin; ci.pInputAssemblyState = &ia; ci.pViewportState = &vp;
    ci.pRasterizationState = &rs; ci.pMultisampleState = &ms; ci.pColorBlendState = &cb; ci.pDynamicState = &ds;
    ci.layout = g.layout;
    ci.renderPass = pass;
    vkCreateGraphicsPipelines_(g.device, VK_NULL_HANDLE, 1, &ci, NULL, &p);
    return p;
}

static void destroy_pipelines(void)
{
    if (g.blit_window) vkDestroyPipeline_(g.device, g.blit_window, NULL);
    if (g.rcas_window) vkDestroyPipeline_(g.device, g.rcas_window, NULL);
    g.blit_window = g.rcas_window = VK_NULL_HANDLE;
}

static int create_pipelines(void)
{
    VkShaderModule vert = shader(PORT_GFX_FULLSCREEN_VERT, sizeof PORT_GFX_FULLSCREEN_VERT);
    VkShaderModule blit = shader(PORT_GFX_BLIT_FRAG, sizeof PORT_GFX_BLIT_FRAG);
    VkShaderModule easu = shader(PORT_GFX_EASU_FRAG, sizeof PORT_GFX_EASU_FRAG);
    VkShaderModule rcas = shader(PORT_GFX_RCAS_FRAG, sizeof PORT_GFX_RCAS_FRAG);
    destroy_pipelines();
    if (!g.pass_intermediate)
        g.pass_intermediate = make_pass(VK_FORMAT_R8G8B8A8_UNORM, VK_ATTACHMENT_LOAD_OP_DONT_CARE,
                                        VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    g.blit_window = make_pipeline(g.pass_window, vert, blit);
    g.rcas_window = make_pipeline(g.pass_window, vert, rcas);
    if (!g.easu_intermediate)
        g.easu_intermediate = make_pipeline(g.pass_intermediate, vert, easu);
    vkDestroyShaderModule_(g.device, vert, NULL);
    vkDestroyShaderModule_(g.device, blit, NULL);
    vkDestroyShaderModule_(g.device, easu, NULL);
    vkDestroyShaderModule_(g.device, rcas, NULL);
    if (!g.blit_window || !g.rcas_window || !g.easu_intermediate) {
        snprintf(g_error, sizeof g_error, "the presentation pipelines could not be made");
        return 0;
    }
    return 1;
}

static int create_fixed(void)
{
    VkSamplerCreateInfo si = {VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    VkDescriptorSetLayoutBinding b = {0};
    VkDescriptorSetLayoutCreateInfo li = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    VkPushConstantRange pr = {VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(Push)};
    VkPipelineLayoutCreateInfo pi = {VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    VkDescriptorPoolSize ps = {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, FRAMES * 2};
    VkDescriptorPoolCreateInfo pci = {VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    VkCommandPoolCreateInfo cpi = {VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};

    si.magFilter = si.minFilter = VK_FILTER_LINEAR;
    si.addressModeU = si.addressModeV = si.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    si.maxLod = 0.0f;
    vkCreateSampler_(g.device, &si, NULL, &g.linear);
    si.magFilter = si.minFilter = VK_FILTER_NEAREST;
    vkCreateSampler_(g.device, &si, NULL, &g.nearest);

    b.binding = 0;
    b.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    b.descriptorCount = 1;
    b.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    li.bindingCount = 1; li.pBindings = &b;
    vkCreateDescriptorSetLayout_(g.device, &li, NULL, &g.set_layout);
    pi.setLayoutCount = 1; pi.pSetLayouts = &g.set_layout;
    pi.pushConstantRangeCount = 1; pi.pPushConstantRanges = &pr;
    vkCreatePipelineLayout_(g.device, &pi, NULL, &g.layout);
    pci.maxSets = FRAMES * 2;
    pci.poolSizeCount = 1; pci.pPoolSizes = &ps;
    vkCreateDescriptorPool_(g.device, &pci, NULL, &g.pool);

    cpi.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    cpi.queueFamilyIndex = g.family;
    vkCreateCommandPool_(g.device, &cpi, NULL, &g.commands);
    for (int f = 0; f < FRAMES; ++f) {
        VkCommandBufferAllocateInfo ai = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
        VkFenceCreateInfo fi = {VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
        VkSemaphoreCreateInfo smi = {VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
        VkDescriptorSetLayout layouts[2] = {g.set_layout, g.set_layout};
        VkDescriptorSetAllocateInfo di = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
        VkDescriptorSet sets[2];
        ai.commandPool = g.commands;
        ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        ai.commandBufferCount = 1;
        vkAllocateCommandBuffers_(g.device, &ai, &g.frame[f].cmd);
        fi.flags = VK_FENCE_CREATE_SIGNALED_BIT;
        vkCreateFence_(g.device, &fi, NULL, &g.frame[f].done);
        vkCreateSemaphore_(g.device, &smi, NULL, &g.frame[f].acquired);
        di.descriptorPool = g.pool;
        di.descriptorSetCount = 2;
        di.pSetLayouts = layouts;
        if (vkAllocateDescriptorSets_(g.device, &di, sets) != VK_SUCCESS) {
            snprintf(g_error, sizeof g_error, "descriptor sets");
            return 0;
        }
        g.frame[f].source_set = sets[0];
        g.frame[f].intermediate_set = sets[1];
    }
    return g.linear && g.nearest && g.layout && g.pool && g.commands;
}

/* --------------------------------------------------------------------------------------------- open and close */

int port_gfx_open(void *window, const PortGfxConfig *config)
{
    port_gfx_close();
    memset(&g, 0, sizeof g);
    if (config) g.config = *config; else port_gfx_defaults(&g.config);
    g.window = window;
    g.scaler = g.config.scaler;
    if (!load_vulkan() || !create_instance() || !pick_device() || !create_surface(window) || !create_device() ||
        !create_fixed() || !create_swapchain() || !create_pipelines()) {
        char saved[sizeof g_error];
        memcpy(saved, g_error, sizeof saved);
        port_gfx_close();
        memcpy(g_error, saved, sizeof saved);
        return 0;
    }
    if (g.scaler == PORT_GFX_DLSS && !ngx_start()) {
        fprintf(stderr, "[port_gfx] %s; using FSR\n", g_error);
        g.scaler = PORT_GFX_FSR;
    }
    fprintf(stderr, "[port_gfx] Vulkan on %s (%ux%u, %s, %s)\n", g.device_name, g.swap_extent.width,
            g.swap_extent.height, g.config.vsync ? "vsync" : "no vsync",
            g.scaler == PORT_GFX_DLSS ? "DLSS" : g.scaler == PORT_GFX_FSR ? "FSR 1.0"
            : g.scaler == PORT_GFX_NEAREST ? "nearest" : "linear");
    g_error[0] = 0;
    return 1;
}

static void ngx_close(void)
{
    if (ngx.feature && ngx.release) ngx.release(ngx.feature);
    if (ngx.params && ngx.destroy) ngx.destroy(ngx.params);
    if (ngx.ready && ngx.shutdown) ngx.shutdown(g.device);
    if (g.device) { destroy_image(&ngx.depth); destroy_image(&ngx.motion); }
    memset(&ngx, 0, sizeof ngx);
}

void port_gfx_close(void)
{
    if (g.device) {
        vkDeviceWaitIdle_(g.device);
        ngx_close();
        destroy_swapchain();
        destroy_pipelines();
        if (g.easu_intermediate) vkDestroyPipeline_(g.device, g.easu_intermediate, NULL);
        if (g.intermediate_fb) vkDestroyFramebuffer_(g.device, g.intermediate_fb, NULL);
        destroy_image(&g.source);
        destroy_image(&g.intermediate);
        for (int f = 0; f < FRAMES; ++f) {
            if (g.frame[f].staging) vkDestroyBuffer_(g.device, g.frame[f].staging, NULL);
            if (g.frame[f].staging_memory) vkFreeMemory_(g.device, g.frame[f].staging_memory, NULL);
            if (g.frame[f].done) vkDestroyFence_(g.device, g.frame[f].done, NULL);
            if (g.frame[f].acquired) vkDestroySemaphore_(g.device, g.frame[f].acquired, NULL);
        }
        if (g.commands) vkDestroyCommandPool_(g.device, g.commands, NULL);
        if (g.pool) vkDestroyDescriptorPool_(g.device, g.pool, NULL);
        if (g.layout) vkDestroyPipelineLayout_(g.device, g.layout, NULL);
        if (g.set_layout) vkDestroyDescriptorSetLayout_(g.device, g.set_layout, NULL);
        if (g.linear) vkDestroySampler_(g.device, g.linear, NULL);
        if (g.nearest) vkDestroySampler_(g.device, g.nearest, NULL);
        if (g.pass_window) vkDestroyRenderPass_(g.device, g.pass_window, NULL);
        if (g.pass_intermediate) vkDestroyRenderPass_(g.device, g.pass_intermediate, NULL);
        if (g.swapchain) vkDestroySwapchainKHR_(g.device, g.swapchain, NULL);
        vkDestroyDevice_(g.device, NULL);
    }
    if (g.instance) {
        if (g.surface) vkDestroySurfaceKHR_(g.instance, g.surface, NULL);
        vkDestroyInstance_(g.instance, NULL);
    }
    {
        void *library = g.library;
        memset(&g, 0, sizeof g);
        g.library = library;   /* the loader stays loaded: unloading it under a driver is asking for trouble */
    }
}

/* ------------------------------------------------------------------------------------------------- presenting */

static void set_descriptor(VkDescriptorSet set, VkImageView view, VkSampler sampler)
{
    VkDescriptorImageInfo ii = {sampler, view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    VkWriteDescriptorSet w = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
    w.dstSet = set;
    w.descriptorCount = 1;
    w.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    w.pImageInfo = &ii;
    vkUpdateDescriptorSets_(g.device, 1, &w, 0, NULL);
}

static void draw(VkCommandBuffer cmd, VkRenderPass pass, VkFramebuffer fb, VkExtent2D fb_size, VkPipeline pipeline,
                 VkDescriptorSet set, float x, float y, float w, float h, const Push *push)
{
    VkClearValue clear = {{{0.0f, 0.0f, 0.0f, 1.0f}}};
    VkRenderPassBeginInfo bi = {VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
    VkViewport vp = {x, y, w, h, 0.0f, 1.0f};
    VkRect2D sc = {{(int32_t)x, (int32_t)y}, {(uint32_t)w, (uint32_t)h}};
    bi.renderPass = pass;
    bi.framebuffer = fb;
    bi.renderArea.extent = fb_size;
    bi.clearValueCount = 1;
    bi.pClearValues = &clear;
    vkCmdBeginRenderPass_(cmd, &bi, VK_SUBPASS_CONTENTS_INLINE);
    vkCmdBindPipeline_(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
    vkCmdBindDescriptorSets_(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, g.layout, 0, 1, &set, 0, NULL);
    vkCmdPushConstants_(cmd, g.layout, VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof *push, push);
    vkCmdSetViewport_(cmd, 0, 1, &vp);
    vkCmdSetScissor_(cmd, 0, 1, &sc);
    vkCmdDraw_(cmd, 3, 1, 0, 0);
    vkCmdEndRenderPass_(cmd);
}

/* The intermediate image (EASU or DLSS output) at w x h. */
static int ensure_intermediate(uint32_t w, uint32_t h)
{
    VkFramebufferCreateInfo fi = {VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};
    if (g.intermediate.image && g.intermediate.width == w && g.intermediate.height == h)
        return 1;
    vkDeviceWaitIdle_(g.device);
    if (g.intermediate_fb) vkDestroyFramebuffer_(g.device, g.intermediate_fb, NULL);
    g.intermediate_fb = VK_NULL_HANDLE;
    if (!create_image(&g.intermediate, VK_FORMAT_R8G8B8A8_UNORM, w, h,
                      VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_STORAGE_BIT))
        return 0;
    fi.renderPass = g.pass_intermediate;
    fi.attachmentCount = 1;
    fi.pAttachments = &g.intermediate.view;
    fi.width = w; fi.height = h; fi.layers = 1;
    vkCreateFramebuffer_(g.device, &fi, NULL, &g.intermediate_fb);
    g.intermediate_fresh = 1;
    if (ngx.feature) { ngx.release(ngx.feature); ngx.feature = NULL; }   /* DLSS's output size changed */
    return 1;
}

static NgxResource ngx_resource(VkImage image, VkImageView view, VkFormat format, uint32_t w, uint32_t h, int rw)
{
    NgxResource r;
    memset(&r, 0, sizeof r);
    r.resource.image.view = view;
    r.resource.image.image = image;
    r.resource.image.range.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    r.resource.image.range.levelCount = 1;
    r.resource.image.range.layerCount = 1;
    r.resource.image.format = format;
    r.resource.image.width = w;
    r.resource.image.height = h;
    r.type = 0;
    r.read_write = (unsigned char)rw;
    return r;
}

/* DLSS from the frame (render size) into the intermediate (output size), recorded into cmd. 0 when it cannot. */
static int ngx_evaluate(VkCommandBuffer cmd, VkImage image, VkImageView view, VkFormat format, uint32_t rw,
                        uint32_t rh, uint32_t ow, uint32_t oh)
{
    NgxResource color, output, depth, motion;
    static const VkClearColorValue zero = {{0.0f, 0.0f, 0.0f, 0.0f}};
    VkImageSubresourceRange range = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    if (!ngx.ready)
        return 0;
    if (!ngx.params && !NGX_OK(ngx.allocate(&ngx.params)))
        return 0;
    if (!ngx.depth.image || ngx.depth.width != rw || ngx.depth.height != rh) {
        /* Stand-ins for a renderer without depth or motion: the picture taken as still. */
        if (!create_image(&ngx.depth, VK_FORMAT_R32_SFLOAT, rw, rh, VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT) ||
            !create_image(&ngx.motion, VK_FORMAT_R16G16_SFLOAT, rw, rh, VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT))
            return 0;
        ngx.stand_ins_fresh = 1;
    }
    if (ngx.stand_ins_fresh) {
        VkImage both[2] = {ngx.depth.image, ngx.motion.image};
        for (int i = 0; i < 2; ++i) {
            barrier(cmd, both[i], VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0,
                    VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
            vkCmdClearColorImage_(cmd, both[i], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &zero, 1, &range);
            barrier(cmd, both[i], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                    VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                    VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);
        }
        ngx.stand_ins_fresh = 0;
    }
    if (!ngx.feature || ngx.render_w != rw || ngx.render_h != rh || ngx.out_w != ow || ngx.out_h != oh) {
        if (ngx.feature) { ngx.release(ngx.feature); ngx.feature = NULL; }
        ngx_ui(ngx.params, "Width", rw);
        ngx_ui(ngx.params, "Height", rh);
        ngx_ui(ngx.params, "OutWidth", ow);
        ngx_ui(ngx.params, "OutHeight", oh);
        ngx_i(ngx.params, "PerfQualityValue", g.config.dlss_quality);
        ngx_i(ngx.params, "DLSS.Feature.Create.Flags", NGX_FLAG_MV_LOW_RES | NGX_FLAG_AUTO_EXPOSURE);
        ngx_ui(ngx.params, "CreationNodeMask", 1);
        ngx_ui(ngx.params, "VisibilityNodeMask", 1);
        ngx_i(ngx.params, "DLSS.Enable.Output.Subrects", 0);
        if (!NGX_OK(ngx.create(g.device, cmd, NGX_FEATURE_SUPER_SAMPLING, ngx.params, &ngx.feature)) || !ngx.feature) {
            ngx.feature = NULL;
            return 0;
        }
        ngx.render_w = rw; ngx.render_h = rh; ngx.out_w = ow; ngx.out_h = oh;
        ngx.reset = 1;
    }
    color = ngx_resource(image, view, format, rw, rh, 0);
    output = ngx_resource(g.intermediate.image, g.intermediate.view, g.intermediate.format, ow, oh, 1);
    depth = ngx.has_depth ? ngx.user_depth : ngx_resource(ngx.depth.image, ngx.depth.view, ngx.depth.format, rw, rh, 0);
    motion = ngx.has_motion ? ngx.user_motion
                            : ngx_resource(ngx.motion.image, ngx.motion.view, ngx.motion.format, rw, rh, 0);
    ngx_p(ngx.params, "Color", &color);
    ngx_p(ngx.params, "Output", &output);
    ngx_p(ngx.params, "Depth", &depth);
    ngx_p(ngx.params, "MotionVectors", &motion);
    ngx_f(ngx.params, "Jitter.Offset.X", ngx.jitter_x);
    ngx_f(ngx.params, "Jitter.Offset.Y", ngx.jitter_y);
    ngx_f(ngx.params, "MV.Scale.X", 1.0f);
    ngx_f(ngx.params, "MV.Scale.Y", 1.0f);
    ngx_i(ngx.params, "Reset", ngx.reset);
    ngx_f(ngx.params, "Sharpness", 0.0f);
    ngx_ui(ngx.params, "DLSS.Render.Subrect.Dimensions.Width", rw);
    ngx_ui(ngx.params, "DLSS.Render.Subrect.Dimensions.Height", rh);
    /* The output in GENERAL for DLSS's writes. */
    barrier(cmd, g.intermediate.image, g.intermediate_fresh ? VK_IMAGE_LAYOUT_UNDEFINED : VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
            VK_IMAGE_LAYOUT_GENERAL, VK_ACCESS_SHADER_READ_BIT, VK_ACCESS_SHADER_WRITE_BIT,
            VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);
    g.intermediate_fresh = 0;
    if (!NGX_OK(ngx.evaluate(cmd, ngx.feature, ngx.params, NULL))) {
        barrier(cmd, g.intermediate.image, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, 0,
                VK_ACCESS_SHADER_READ_BIT, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);
        return 0;
    }
    barrier(cmd, g.intermediate.image, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
            VK_ACCESS_SHADER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
            VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);
    ngx.reset = 0;
    ngx.has_depth = ngx.has_motion = 0;   /* per frame */
    return 1;
}

/* Scales the frame (in SHADER_READ_ONLY) onto the acquired window image and submits; cmd is recording. */
static int finish(VkCommandBuffer cmd, uint32_t index, VkImage image, VkImageView view, VkFormat format,
                  uint32_t w, uint32_t h, VkSemaphore extra_wait)
{
    int f = (int)g.frame_index;
    float ww = (float)g.swap_extent.width, wh = (float)g.swap_extent.height;
    float aspect = g.config.aspect > 0.0f ? g.config.aspect : (float)w / (float)h;
    float dw = ww, dh = wh, dx, dy;
    int scaler = g.scaler, upscaled = 0;
    Push push;
    VkSubmitInfo si = {VK_STRUCTURE_TYPE_SUBMIT_INFO};
    VkSemaphore waits[2];
    VkPipelineStageFlags stages[2] = {VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                                      VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT};
    VkPresentInfoKHR pi = {VK_STRUCTURE_TYPE_PRESENT_INFO_KHR};
    VkResult r;

    if (!g.config.stretch) {
        if (ww / wh > aspect) dw = floorf(wh * aspect + 0.5f); else dh = floorf(ww / aspect + 0.5f);
    }
    dx = floorf((ww - dw) * 0.5f);
    dy = floorf((wh - dh) * 0.5f);
    memset(&push, 0, sizeof push);
    push.source_w = (float)w; push.source_h = (float)h;
    push.sharpness = g.config.sharpness;

    /* An enlarged frame goes through the intermediate (FSR or DLSS); otherwise straight to the window. */
    if ((scaler == PORT_GFX_FSR || scaler == PORT_GFX_DLSS) && (dw > (float)w || dh > (float)h)) {
        /* DLSS enlarges at most 3x per axis; beyond that its output is enlarged again linearly. */
        uint32_t ow = (uint32_t)dw, oh = (uint32_t)dh;
        if (scaler == PORT_GFX_DLSS) {
            if (ow > w * 3) ow = w * 3;
            if (oh > h * 3) oh = h * 3;
        }
        if (ensure_intermediate(ow, oh)) {
            set_descriptor(g.frame[f].intermediate_set, g.intermediate.view, g.linear);
            if (scaler == PORT_GFX_DLSS && ngx_evaluate(cmd, image, view, format, w, h, ow, oh)) {
                upscaled = 1;
            } else {
                Push e = push;
                VkExtent2D ext = {ow, oh};
                e.output_w = (float)ow; e.output_h = (float)oh;
                set_descriptor(g.frame[f].source_set, view, g.linear);
                draw(cmd, g.pass_intermediate, g.intermediate_fb, ext, g.easu_intermediate, g.frame[f].source_set,
                     0.0f, 0.0f, (float)ow, (float)oh, &e);
                g.intermediate_fresh = 0;
                upscaled = 1;
            }
            if (upscaled) {
                Push p2 = push;
                int exact = ow == (uint32_t)dw && oh == (uint32_t)dh;
                p2.source_w = (float)ow; p2.source_h = (float)oh;
                p2.output_w = dw; p2.output_h = dh; p2.offset_x = dx; p2.offset_y = dy;
                draw(cmd, g.pass_window, g.framebuffers[index], g.swap_extent,
                     exact ? g.rcas_window : g.blit_window, g.frame[f].intermediate_set, dx, dy, dw, dh, &p2);
            }
        }
    }
    if (!upscaled) {
        set_descriptor(g.frame[f].source_set, view, scaler == PORT_GFX_NEAREST ? g.nearest : g.linear);
        push.output_w = dw; push.output_h = dh; push.offset_x = dx; push.offset_y = dy;
        draw(cmd, g.pass_window, g.framebuffers[index], g.swap_extent, g.blit_window, g.frame[f].source_set,
             dx, dy, dw, dh, &push);
    }
    vkEndCommandBuffer_(cmd);

    waits[0] = g.frame[f].acquired;
    waits[1] = extra_wait;
    si.waitSemaphoreCount = extra_wait ? 2 : 1;
    si.pWaitSemaphores = waits;
    si.pWaitDstStageMask = stages;
    si.commandBufferCount = 1;
    si.pCommandBuffers = &cmd;
    si.signalSemaphoreCount = 1;
    si.pSignalSemaphores = &g.rendered[index];
    if ((r = vkQueueSubmit_(g.queue, 1, &si, g.frame[f].done)) != VK_SUCCESS) {
        fail("vkQueueSubmit", r);
        return 0;
    }
    pi.waitSemaphoreCount = 1;
    pi.pWaitSemaphores = &g.rendered[index];
    pi.swapchainCount = 1;
    pi.pSwapchains = &g.swapchain;
    pi.pImageIndices = &index;
    r = vkQueuePresentKHR_(g.queue, &pi);
    g.frame_index = (g.frame_index + 1) % FRAMES;
    if (r == VK_ERROR_OUT_OF_DATE_KHR || r == VK_SUBOPTIMAL_KHR) {
        create_swapchain();
        return 0;
    }
    return r == VK_SUCCESS && g.config.vsync;
}

/* Waits for this frame slot, acquires a window image and starts the command buffer. -1: nothing to draw into. */
static int begin(uint32_t *index)
{
    int f = (int)g.frame_index;
    VkCommandBufferBeginInfo bi = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    VkResult r;
    if (!g.device)
        return -1;
    if (!g.swapchain && !create_swapchain())
        return -1;
    vkWaitForFences_(g.device, 1, &g.frame[f].done, VK_TRUE, UINT64_MAX);
    r = vkAcquireNextImageKHR_(g.device, g.swapchain, UINT64_MAX, g.frame[f].acquired, VK_NULL_HANDLE, index);
    if (r == VK_ERROR_OUT_OF_DATE_KHR) {
        create_swapchain();
        return -1;
    }
    if (r != VK_SUCCESS && r != VK_SUBOPTIMAL_KHR)
        return -1;
    if (g.pass_window && !g.blit_window && !create_pipelines())
        return -1;
    vkResetFences_(g.device, 1, &g.frame[f].done);
    vkResetCommandBuffer_(g.frame[f].cmd, 0);
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer_(g.frame[f].cmd, &bi);
    return f;
}

int port_gfx_present_pixels(const void *pixels, uint32_t width, uint32_t height, uint32_t pitch)
{
    uint32_t index;
    int f;
    VkCommandBuffer cmd;
    VkDeviceSize bytes = (VkDeviceSize)width * height * 4;
    VkBufferImageCopy copy = {0};
    if (!pixels || !width || !height || (f = begin(&index)) < 0)
        return 0;
    cmd = g.frame[f].cmd;
    if (g.frame[f].staging_size < bytes) {
        VkBufferCreateInfo bci = {VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
        VkMemoryRequirements req;
        VkMemoryAllocateInfo ai = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
        if (g.frame[f].staging) vkDestroyBuffer_(g.device, g.frame[f].staging, NULL);
        if (g.frame[f].staging_memory) vkFreeMemory_(g.device, g.frame[f].staging_memory, NULL);
        g.frame[f].staging = VK_NULL_HANDLE; g.frame[f].staging_memory = VK_NULL_HANDLE; g.frame[f].staging_size = 0;
        bci.size = bytes;
        bci.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
        vkCreateBuffer_(g.device, &bci, NULL, &g.frame[f].staging);
        vkGetBufferMemoryRequirements_(g.device, g.frame[f].staging, &req);
        ai.allocationSize = req.size;
        ai.memoryTypeIndex = memory_type(req.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                                             VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
        if (ai.memoryTypeIndex == UINT32_MAX || vkAllocateMemory_(g.device, &ai, NULL, &g.frame[f].staging_memory) != VK_SUCCESS) {
            vkEndCommandBuffer_(cmd);
            return 0;
        }
        vkBindBufferMemory_(g.device, g.frame[f].staging, g.frame[f].staging_memory, 0);
        vkMapMemory_(g.device, g.frame[f].staging_memory, 0, VK_WHOLE_SIZE, 0, &g.frame[f].staging_map);
        g.frame[f].staging_size = bytes;
    }
    for (uint32_t y = 0; y < height; ++y)
        memcpy((uint8_t *)g.frame[f].staging_map + (size_t)y * width * 4, (const uint8_t *)pixels + (size_t)y * pitch,
               (size_t)width * 4);
    if (!g.source.image || g.source.width != width || g.source.height != height) {
        vkDeviceWaitIdle_(g.device);
        if (!create_image(&g.source, VK_FORMAT_B8G8R8A8_UNORM, width, height,
                          VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT)) {
            vkEndCommandBuffer_(cmd);
            return 0;
        }
        g.source_fresh = 1;
    }
    barrier(cmd, g.source.image, g.source_fresh ? VK_IMAGE_LAYOUT_UNDEFINED : VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
            VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_ACCESS_SHADER_READ_BIT, VK_ACCESS_TRANSFER_WRITE_BIT,
            VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
    g.source_fresh = 0;
    copy.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    copy.imageSubresource.layerCount = 1;
    copy.imageExtent.width = width; copy.imageExtent.height = height; copy.imageExtent.depth = 1;
    vkCmdCopyBufferToImage_(cmd, g.frame[f].staging, g.source.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);
    barrier(cmd, g.source.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
            VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
            VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);
    return finish(cmd, index, g.source.image, g.source.view, g.source.format, width, height, VK_NULL_HANDLE);
}

int port_gfx_present_image(uint64_t image, uint64_t view, uint32_t format, uint32_t width, uint32_t height,
                           uint64_t wait_semaphore)
{
    uint32_t index;
    int f;
    if (!image || !view || (f = begin(&index)) < 0)
        return 0;
    return finish(g.frame[f].cmd, index, (VkImage)(uintptr_t)image, (VkImageView)(uintptr_t)view, (VkFormat)format,
                  width, height, (VkSemaphore)(uintptr_t)wait_semaphore);
}

void port_gfx_set_dlss_inputs(uint64_t depth_image, uint64_t depth_view, uint32_t depth_format,
                              uint64_t motion_image, uint64_t motion_view, uint32_t motion_format,
                              float jitter_x, float jitter_y, int reset)
{
    uint32_t w = ngx.render_w, h = ngx.render_h;
    ngx.has_depth = depth_image && depth_view;
    ngx.has_motion = motion_image && motion_view;
    if (ngx.has_depth)
        ngx.user_depth = ngx_resource((VkImage)(uintptr_t)depth_image, (VkImageView)(uintptr_t)depth_view,
                                      (VkFormat)depth_format, w, h, 0);
    if (ngx.has_motion)
        ngx.user_motion = ngx_resource((VkImage)(uintptr_t)motion_image, (VkImageView)(uintptr_t)motion_view,
                                       (VkFormat)motion_format, w, h, 0);
    ngx.jitter_x = jitter_x;
    ngx.jitter_y = jitter_y;
    if (reset) ngx.reset = 1;
}
