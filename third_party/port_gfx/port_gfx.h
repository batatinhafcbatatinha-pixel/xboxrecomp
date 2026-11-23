/*
 * port_gfx: Vulkan presentation for the recompilers' runtimes, with FSR 1.0 and NVIDIA DLSS.
 *
 * Shared by PS2Recomp, PSPRecomp and xboxrecomp; this folder (port-installer/gfx) is the reference copy, each
 * recompiler vendors it under third_party/port_gfx. C, so C and C++ runtimes both use it; Vulkan is loaded at run
 * time (vulkan-1.dll / libvulkan.so.1), so a machine without Vulkan only gets 0 from port_gfx_open and the runtime
 * keeps its own Direct3D or OpenGL path.
 *
 * A runtime hands it frames in one of two ways:
 *   - pixels it has in memory (port_gfx_present_pixels): software renderers, CPU-side framebuffers;
 *   - a Vulkan image it rendered itself with the device port_gfx made (port_gfx_device, port_gfx_present_image):
 *     a runtime's own Vulkan renderer, which then gets presentation, scaling and DLSS from here.
 * With depth and motion vectors from the renderer (port_gfx_set_dlss_inputs) DLSS works as designed; without them it
 * still upscales, treating the picture as still between frames.
 *
 * Scaling (PortGfxConfig.scaler):
 *   PORT_GFX_NEAREST, PORT_GFX_LINEAR       the frame enlarged by the sampler
 *   PORT_GFX_FSR                            AMD FidelityFX Super Resolution 1.0 (EASU, then RCAS sharpening)
 *   PORT_GFX_DLSS                           NVIDIA DLSS super resolution (RTX cards with nvngx_dlss.dll next to
 *                                           the game), then RCAS; FSR when DLSS cannot start
 */
#ifndef PORT_GFX_H
#define PORT_GFX_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

enum { PORT_GFX_NEAREST = 0, PORT_GFX_LINEAR = 1, PORT_GFX_FSR = 2, PORT_GFX_DLSS = 3 };

/* DLSS quality: how much smaller than the window the frame DLSS starts from is up to the runtime; these are NVIDIA's
 * presets for the network's tuning. */
enum { PORT_GFX_DLSS_PERFORMANCE = 0, PORT_GFX_DLSS_BALANCED = 1, PORT_GFX_DLSS_QUALITY = 2,
       PORT_GFX_DLSS_ULTRA_PERFORMANCE = 3, PORT_GFX_DLSS_DLAA = 5 };

typedef struct PortGfxConfig {
    int vsync;            /* 1: wait for the refresh (FIFO); 0: present at once (mailbox, else immediate) */
    int adapter;          /* DXGI adapter index (the launcher's GPU list, matched by LUID); -1: the first discrete GPU */
    int scaler;           /* PORT_GFX_* */
    int stretch;          /* 1: fill the window; 0: keep the frame's aspect, with bars */
    float sharpness;      /* RCAS, in stops: 0 = sharpest, 2 = soft; FSR and DLSS */
    int dlss_quality;     /* PORT_GFX_DLSS_* */
    float aspect;         /* the frame's display aspect (e.g. 4/3 for a 640x448 PS2 frame); 0 = its pixel size's */
} PortGfxConfig;

/* Defaults: vsync on, the first discrete GPU, FSR, aspect kept, sharpness 0.25, DLSS quality preset. */
void port_gfx_defaults(PortGfxConfig *config);

/* Reads PortGfxConfig from environment variables (each optional), the way the port tools pass the launcher's settings:
 *   PORT_GFX_VSYNC=0|1  PORT_GFX_ADAPTER=<n>  PORT_GFX_SCALER=nearest|linear|fsr|dlss  PORT_GFX_STRETCH=0|1
 *   PORT_GFX_SHARPNESS=<stops>  PORT_GFX_DLSS_QUALITY=performance|balanced|quality|ultra_performance|dlaa */
void port_gfx_config_from_env(PortGfxConfig *config);

/* Vulkan on the window (HWND on Windows). 1 when ready; 0 (with port_gfx_error) when Vulkan, a device or the window's
 * surface is missing -- the runtime then presents its own way. */
int port_gfx_open(void *window, const PortGfxConfig *config);
void port_gfx_close(void);
const char *port_gfx_error(void);
const char *port_gfx_device_name(void);
/* The scaler actually in use (DLSS falls back to FSR when it cannot start). */
int port_gfx_scaler(void);

/* Shows one frame of B8G8R8A8 pixels (the guest's A8R8G8B8 in memory order), pitch in bytes. Returns 1 when it was
 * presented with vsync pacing (the caller need not sleep), 0 otherwise. The pixels are copied before it returns. */
int port_gfx_present_pixels(const void *pixels, uint32_t width, uint32_t height, uint32_t pitch);

/* For a runtime rendering with Vulkan itself: the instance, physical device, device, graphics queue and its family
 * port_gfx made (VkInstance, VkPhysicalDevice, VkDevice, VkQueue as void*). NULL before port_gfx_open. Submissions to
 * the queue must not overlap port_gfx's (call from the thread that presents). */
void *port_gfx_instance(void);
void *port_gfx_physical_device(void);
void *port_gfx_device(void);
void *port_gfx_queue(void);
uint32_t port_gfx_queue_family(void);

/* Shows a Vulkan image the runtime rendered (VkImage, VkImageView, VkFormat as integers), in layout
 * VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, sampled-capable; wait_semaphore (VkSemaphore or 0) is signalled when the
 * runtime's rendering is done. The image stays the runtime's. */
int port_gfx_present_image(uint64_t image, uint64_t view, uint32_t format, uint32_t width, uint32_t height,
                           uint64_t wait_semaphore);

/* What DLSS needs from a renderer for the next port_gfx_present_image: depth (view, image, format) at the render
 * size, motion vectors (R16G16 float, pixels, at the render size) and the frame's sub-pixel jitter. Zeros mean none. */
void port_gfx_set_dlss_inputs(uint64_t depth_image, uint64_t depth_view, uint32_t depth_format,
                              uint64_t motion_image, uint64_t motion_view, uint32_t motion_format,
                              float jitter_x, float jitter_y, int reset);

#ifdef __cplusplus
}
#endif

#endif
