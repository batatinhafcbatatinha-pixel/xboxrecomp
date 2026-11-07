/* Street 2: the last two NV097 push-buffer producers, replaced without their Xbox bodies.
 * Evidence (XBE 5fabb5b7..., text.asm, assets in game_files/data, 16/10/2026):
 *   001A1EF0  copies a prebuilt NV097 block through BeginPush/EndPush. Its only reference is
 *             slot +0x18 of the callback struct 0x31A840, copied by 0021CCDB to 0x34D090.
 *             No instruction reads 0x34D0A8 (only +0x04 and +0x0C are called), so it is dead.
 *   001A48D0  EAGL display-list opcode 36 (table 0x31A930, dispatcher 00197050): runs the
 *             D3DPushBuffer at [packet+8]+4 via 001A2910 -> RunPushBuffer. Opcode 36 occurs
 *             in none of the 4029 render-method streams of the 1790 EAGL objects in the game
 *             files; used opcodes are 2,3,4,5,6,9,14,15,16,20,26,29,31,34,38, none of which
 *             reaches BeginPush/EndPush/RunPushBuffer except through the native batch hooks.
 *             Its pushbuffer constructor 001A3BD0 has no reference at all.
 * Both keep the guest contract up to the point where GPU commands would be needed, then stop.
 */
#include <stdint.h>
#include "recomp/gen/recomp_types.h"
#include "recomp/gen/recomp_funcs.h"

void hle_port_native_missing(const char *, unsigned);

void sub_001A1EF0(void)
{
    hle_port_native_missing("prebuilt NV097 block 001A1EF0 (unreferenced callback slot)", MEM32(g_esp));
}

/* Opcode 36 with a zero count returns without drawing in the original. */
void sub_001A48D0(void)
{
    uint32_t packet = MEM32(0x3458D4u);
    if (MEM32(packet + 4)) {
        hle_port_native_missing("EAGL opcode 36 recorded push-buffer (absent from game assets)", MEM32(g_esp));
    }
    g_esp += 4;
}
