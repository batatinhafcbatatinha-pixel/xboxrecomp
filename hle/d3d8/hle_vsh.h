#include "hle_game.h"
#if HLE_GAME == HLE_GAME_BLACK
// Adapted generic renderer; Black bindings verified in analysis/native-gpu-bindings.json.
#else /* street1 street2 mka */
#endif /* HLE_GAME */
#ifndef HLE_VSH_H
#define HLE_VSH_H
#include <stddef.h>
#include <stdint.h>
#if HLE_GAME == HLE_GAME_MKA

/* Shared vertex fetch and VSOut ABI, including the Draw constant buffer. */
const char *hle_vsh_prelude(void);
#else /* street1 street2 black */
#endif /* HLE_GAME */

/* Xbox vertex shader microcode (count instructions, 4 dwords each) -> HLSL
 * vs_5_0 source. *clip_space tells whether the XDK screen-space suffix was
 * found (position is clip space) or the shader writes screen space itself.
 * Returns the source length, or 0 with a message in error. */
size_t hle_vsh_hlsl(const uint32_t (*program)[4], unsigned count,
                    char *out, size_t cap, int *clip_space, char *error, size_t ecap);

#endif
