/* CPU state effects of the title's D3D8 state setters. NV2A method writes
 * are consumed directly from their source state by the native renderer. */
#include "hle_game.h"
#include <stdint.h>
#include <string.h>
#include "recomp/gen/recomp_types.h"
#include "hle_d3d11.h"

#define DEVICE MEM32(GA_NATIVE_STATES_DEVICE)
#define DIRTY MEM32(GA_NATIVE_STATES_DIRTY)

extern void hle_api_apply_target_scale(uint32_t dev, uint32_t viewport);
extern void hle_api_viewport_transform(uint32_t dev);
extern int hle_api_render_store(int id, uint32_t value);
extern void hle_draw_set_simple(uint32_t method, uint32_t value);

static float state_float(uint32_t value)
{
    float result;
    memcpy(&result, &value, sizeof result);
    return result;
}

/* GA_NATIVE_STATES's depth caches. Other results of this helper are GPU methods. */
static void viewport_depth(uint32_t dev)
{
    uint32_t shader = MEM32(dev + 0x794u);
    uint32_t base = dev + ((MEM8(shader + 4u) & 0x12u) ? 0x1C10u : 0x1C08u);
    uint32_t bit = (MEM8(shader + 4u) & 0x12u) ? 2u : 1u;
    if (!(MEM8(dev + 0x1C18u) & bit)) {
        if (MEM32(GA_NATIVE_STATES_2) == 2u) {
            MEMF(base) = MEMF(dev + 0x944u) * MEMF(dev + 0x93Cu) * MEMF(dev + 0x948u);
            MEM32(base + 4u) = MEM32(dev + 0x948u);
        } else if (bit == 1u) {
            MEMF(base) = MEMF(dev + 0xEF0u) * MEMF(dev + 0x948u);
            MEMF(base + 4u) = MEMF(dev + 0xEF4u) * MEMF(dev + 0x948u);
        } else {
            MEM32(base) = 0;
            MEM32(base + 4u) = MEM32(dev + 0x948u);
        }
    }
}

void hle_state_transform(uint32_t index, uint32_t matrix)
{
    uint32_t dev = DEVICE;
    for (uint32_t i = 0; i < 16; ++i)
        MEM32(dev + 0xC60u + index * 64u + i * 4u) = MEM32(matrix + i * 4u);
    DIRTY |= MEM32(GA_NATIVE_STATES_3 + index * 4u);
    if (index == 1) {
        float m10 = MEMF(matrix + 40u), m11 = MEMF(matrix + 44u);
        float m14 = MEMF(matrix + 56u), m15 = MEMF(matrix + 60u);
        if (m10 == m11 || m10 == 0.0f) {
            MEMF(dev + 0x93Cu) = 0.0f;
            MEMF(dev + 0x940u) = 1.0f;
        } else {
            MEMF(dev + 0x93Cu) = m15 - (m14 / m10) * m11;
            MEMF(dev + 0x940u) = ((m15 - m14) / (m10 - m11)) * m11 + m15;
        }
        MEM32(dev + 8u) &= ~2u;
        MEMF(dev + 0x944u) = 1.0f / MEMF(dev + 0x940u);
        if (MEMF(matrix + 12u) == 0.0f && MEMF(matrix + 28u) == 0.0f &&
            m11 == 0.0f && MEM32(matrix + 60u) == 0x3F800000u)
            MEM32(dev + 8u) |= 2u;
        hle_api_viewport_transform(dev);
    }
}

static void texcoord_index(uint32_t stage, uint32_t value)
{
    uint32_t dev = DEVICE, mode = value & 0xFFFF0000u;
    uint32_t generated = mode == 0x10000u || mode == 0x30000u;
    MEM32(GA_NATIVE_STATES_4 + stage * 128u) = value;
    MEM8(GA_NATIVE_STATES_5 + stage) = (uint8_t)((mode ? stage : value) + 9u);
    if (!MEM32(dev + 0x950u) && generated) DIRTY |= 0x200u;
    MEM32(dev + 0x950u) = (MEM32(dev + 0x950u) & ~(1u << (stage & 31u))) |
                              (generated << (stage & 31u));
    DIRTY |= 0x47Fu;
}

void hle_state_texture(uint32_t stage, uint32_t state, uint32_t value)
{
    if (state < 12u) {
        DIRTY |= 1u << (stage & 31u);
    } else if (state == 12u) {
        DIRTY |= value < 25u ? 0x800u : 0x480Fu;
    } else if (state < 22u) {
        DIRTY |= MEM32(GA_NATIVE_STATES_6 + state * 4u);
    } else if (state == 28u) {
        texcoord_index(stage, value);
        return;
    } else if (state > 30u) {
        return;
    }
    MEM32(GA_NATIVE_STATES_7 + stage * 128u + state * 4u) = value;
}

static int render_special(int id, uint32_t value)
{
    uint32_t dev = DEVICE;
    if (hle_api_render_store(id, value)) return 1;
    switch (id) {
    case HLE_D3DDevice_SetRenderState_PSTextureModes:
        MEM32(dev + 0x790u) = value;
        MEM32(GA_NATIVE_STATES_8) = value;
        DIRTY |= 0x4000u;
        break;
    case HLE_D3DDevice_SetRenderState_LineWidth:
        MEM32(GA_NATIVE_STATES_9) = value;
        break;
    case HLE_D3DDevice_SetRenderState_ZBias:
        MEMF(GA_NATIVE_STATES_10 + 0x4Du * 4u) = (float)(-(double)value * MEMF(GA_NATIVE_STATES_11));
        MEMF(GA_NATIVE_STATES_10 + 0x4Eu * 4u) = (float)(-(double)value);
        for (uint32_t i = 0x4Fu; i <= 0x51u; ++i)
            MEM32(GA_NATIVE_STATES_10 + i * 4u) = value != 0;
        for (uint32_t i = 0x4Du; i <= 0x51u; ++i)
            hle_draw_set_simple(MEM32(GA_NATIVE_STATES_12 + i * 4u), MEM32(GA_NATIVE_STATES_10 + i * 4u));
        MEM32(GA_NATIVE_STATES_13) = value;
        break;
    case HLE_D3DDevice_SetRenderState_BackFillMode:
        MEM32(GA_NATIVE_STATES_14) = value;
        break;
    case HLE_D3DDevice_SetRenderState_TwoSidedLighting:
        MEM32(GA_NATIVE_STATES_15) = value;
        if (value) DIRTY |= 0x9000u;
        break;
    case HLE_D3DDevice_SetRenderState_VertexBlend:
        MEM32(GA_NATIVE_STATES_16) = value;
        DIRTY |= 0x200u;
        break;
    case HLE_D3DDevice_SetRenderState_ZEnable: {
        uint32_t old = MEM32(GA_NATIVE_STATES_2);
        MEM32(GA_NATIVE_STATES_2) = value;
        if (old == 2 || value == 2) {
            hle_api_viewport_transform(dev);
            viewport_depth(dev);
        }
        break;
    }
    case HLE_D3DDevice_SetRenderState_MultiSampleMode:
        MEM32(GA_NATIVE_STATES_17) = value;
        if (MEM32(dev + 0x1A04u) == MEM32(dev + 0x1A14u))
            hle_api_apply_target_scale(dev, 0);
        break;
    case HLE_D3DDevice_SetRenderState_MultiSampleRenderTargetMode:
        MEM32(GA_NATIVE_STATES_18) = value;
        if (MEM32(dev + 0x1A04u) != MEM32(dev + 0x1A14u))
            hle_api_apply_target_scale(dev, 0);
        break;
    case HLE_D3DDevice_SetRenderState_MultiSampleAntiAlias:
        MEM32(GA_NATIVE_STATES_19) = value;
        viewport_depth(dev);
        break;
    default:
        return 0;
    }
    return 1;
}

/* The original dispatch table is data, not executable guest code. */
void hle_state_render(uint32_t state, uint32_t value)
{
    if (state < 0x88u) {
        if (state >= 0x5Cu) DIRTY |= MEM32(GA_NATIVE_STATES_20 + state * 4u);
        MEM32(GA_NATIVE_STATES_10 + state * 4u) = value;
        if (state < 0x5Cu) hle_draw_set_simple(MEM32(GA_NATIVE_STATES_12 + state * 4u), value);
        return;
    }
    if (state < 0xA6u) {
        uint32_t target = MEM32(GA_NATIVE_STATES_21 + state * 4u);
        switch (target) {
#define HLE_API(va, name, conv, args) case 0x##va##u: render_special(HLE_##name, value); return;
#include "hle_d3d8_table.h"
#undef HLE_API
        }
    }
}

int hle_states_call(int id, const uint32_t *a)
{
    uint32_t dev = DEVICE;
    uint32_t argc = 1;
    switch (id) {
    case HLE_D3DDevice_SetTextureState_TexCoordIndex:
        texcoord_index(a[0], a[1]);
        argc = 2;
        break;
    case HLE_D3DDevice_SetTextureState_BumpEnv:
        MEM32(GA_NATIVE_STATES_7 + a[0] * 128u + a[1] * 4u) = a[2];
        argc = 3;
        break;
    case HLE_D3DDevice_SetTextureState_BorderColor:
        MEM32(GA_NATIVE_STATES_22 + a[0] * 128u) = a[1];
        argc = 2;
        break;
    case HLE_D3DDevice_SetTextureState_ColorKeyColor:
        MEM32(GA_NATIVE_STATES_23 + a[0] * 128u) = a[1];
        argc = 2;
        break;
    case HLE_D3DDevice_SetScreenSpaceOffset:
        MEMF(dev + 0xEF8u) = (float)((double)state_float(a[0]) + MEMF(GA_NATIVE_STATES_24));
        MEMF(dev + 0xEFCu) = (float)((double)state_float(a[1]) + MEMF(GA_NATIVE_STATES_24));
        viewport_depth(dev);
        argc = 2;
        break;
    case HLE_D3DDevice_SetShaderConstantMode:
        MEM32(dev + 8u) = (MEM32(dev + 8u) & ~0x200u) | ((a[0] & 0x10u) << 5);
        MEM32(dev + 0x1928u) = a[0] & ~0x10u;
        if (!(a[0] & ~0x10u)) DIRTY |= 0x1600u;
        break;
    case HLE_D3DDevice_SetBackBufferScale: {
        uint32_t scale = MEM32(dev + 0x196Cu), bb = MEM32(dev + 0x1A14u);
        uint32_t fr = MEM32(dev + 0x1A18u);
        float x = (float)((scale >> 4) & 15u) * state_float(a[0]);
        float y = (float)(scale & 15u) * state_float(a[1]);
        if (MEM32(dev + 0x1970u) != 3) {
            if (x == 1.0f && y == 1.0f) {
                if (MEM32(bb + 16u) == MEM32(fr + 16u) && MEM32(bb + 12u) == MEM32(fr + 12u))
                    MEM32(dev + 8u) &= ~0x4000u;
            } else {
                MEM32(dev + 8u) = (MEM32(dev + 8u) | 0x4000u) & ~0x100u;
            }
        }
        MEMF(dev + 0x96Cu) = x;
        MEMF(dev + 0x970u) = y;
        hle_api_apply_target_scale(dev, 0);
        argc = 2;
        break;
    }
    default:
        if (!render_special(id, a[0])) return 0;
        break;
    }
    g_esp += 4u + argc * 4u;
    return 1;
}
