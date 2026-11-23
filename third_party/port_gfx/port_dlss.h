/*
 * port_dlss: NVIDIA DLSS super resolution for runtimes that render with Direct3D 12 (PSPRecomp; the same NGX calls
 * port_gfx makes for Vulkan). Part of port-installer/gfx, vendored with port_gfx.
 *
 * Through the driver's NGX runtime (_nvngx.dll) and nvngx_dlss.dll next to the game; no NVIDIA SDK is needed to build.
 * Every call takes the runtime's own D3D12 objects as void* (ID3D12Device*, ID3D12GraphicsCommandList*,
 * ID3D12Resource*), so C and C++ runtimes use it alike.
 *
 *   port_dlss_d3d12_start(device)     once, after the device exists: 1 when DLSS can run (an RTX card, a driver with
 *                                     NGX, nvngx_dlss.dll next to the game); 0 with port_dlss_error()
 *   port_dlss_d3d12_evaluate(...)     records the upscale into a command list (NGX sets its own descriptor heaps:
 *                                     set yours again after it)
 *   port_dlss_d3d12_stop()            before the device goes
 *
 * Resource states for evaluate: color, depth and motion in a shader-resource state (NON_PIXEL_SHADER_RESOURCE |
 * PIXEL_SHADER_RESOURCE), output in UNORDERED_ACCESS (an R8G8B8A8 / R16G16B16A16 texture with UAV allowed). Depth is
 * an R32_FLOAT (or typeless) texture and motion R16G16_FLOAT, at the render size, in pixels; a renderer without them
 * passes zero-filled textures and DLSS treats the picture as still. The output may be at most 3x the render size per
 * axis.
 */
#ifndef PORT_DLSS_H
#define PORT_DLSS_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

int port_dlss_d3d12_start(void *device);
int port_dlss_d3d12_evaluate(void *command_list, void *color, void *depth, void *motion, void *output,
                             uint32_t render_width, uint32_t render_height, uint32_t output_width,
                             uint32_t output_height, int quality, float jitter_x, float jitter_y, int reset);
void port_dlss_d3d12_stop(void);
int port_dlss_ready(void);
const char *port_dlss_error(void);

#ifdef __cplusplus
}
#endif

#endif
