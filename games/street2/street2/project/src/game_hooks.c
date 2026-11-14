/* Street 2 engine hooks are implemented in native_engine_batch.c and
 * native_command_worker.c, routed by hle/game_hooks.json and CMake.
 * Street 1 frontend addresses must not be transplanted.
 * PC settings: --settings, F1 or Back+Start, or Back where the game's
 * screens show the "PC Settings" hint (hle_options.c).
 */
#include <stdio.h>
#include "recomp/gen/recomp_types.h"

int guest_cpu_native_enter(void);
void guest_cpu_native_leave(int token);

/* Front-end screen loads: 0x1793E0(const char *name) builds "<name>.big"
 * (its "na" check and "%s.big" sit at 0x2D86FC/0x2D8700), the
 * counterpart of Street 1's 0xA2BB0. */
void sub_001793E0_gen(void);
void game_screen_loaded(const char *name);

static void screen_load_hook(void);

/* Motion blur: the post-FX state (0x3404B8) pushes its motion blur to the
 * renderer in 0x14DC10(this = state): [this+0x58] on/off, +0x54 amount,
 * +0x59/+0x5C its second part, into [[this+0xC]+0x2BA/+0x140/+0x2BC..]
 * (0x14AFA0 sets the blur passes up). With the PC setting off the game's own
 * "off" path runs: the flag is cleared before the call; the script effects
 * write it again on their next update. Not verified in game. */
void sub_0014DC10_gen(void);
int hle_options_motion_blur(void);

void sub_0014DC10(void)
{
    if (g_ecx && !hle_options_motion_blur())
        MEM8(g_ecx + 0x58) = 0;
    sub_0014DC10_gen();
}

void sub_001793E0(void)
{
    int smp = guest_cpu_native_enter();
    screen_load_hook();
    guest_cpu_native_leave(smp);
}

static void screen_load_hook(void)
{
    char name[48];
    uint32_t p = MEM32(g_esp + 4);
    int i;
    for (i = 0; i < 47 && p && MEM8(p + (uint32_t)i); i++)
        name[i] = (char)MEM8(p + (uint32_t)i);
    name[i] = 0;
    sub_001793E0_gen();
    game_screen_loaded(name);
}

/* Not in this file: hand-written native versions of three hot game functions (skinning palette blend,
 * bone matrix builder, matrix multiply). They were written by following the translated code, so they are
 * not distributed. In this build those functions are not routed anywhere: the translated functions run.
 * A developer who has them keeps the previous file as the git-ignored project/local/game_hooks.c and
 * hle/local/game_hooks.json; CMake and hle/apply-hle-wrappers.py then use those instead. */
