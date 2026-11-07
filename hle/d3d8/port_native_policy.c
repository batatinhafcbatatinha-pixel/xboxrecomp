#include "hle_game.h"
#if HLE_GAME == HLE_GAME_MKA
/* MK Armageddon PC policy: API replacements only. Unsupported hardware batches
 * stop the run instead of silently selecting a command/device emulator. */
#else /* street2 */
/* Street 2 PC policy: API replacements only. Unsupported hardware batches
 * stop the run instead of silently selecting a command/device emulator. */
#endif /* HLE_GAME */
#include <stdio.h>
#include <stdlib.h>

static void port_env(const char *name, const char *value)
{
#ifdef _WIN32
    _putenv_s(name, value ? value : "");
#else
    if (value) setenv(name, value, 1); else unsetenv(name);
#endif
}

void hle_port_native_init(void)
{
    extern volatile int xbox_nv2a_native;
    xbox_nv2a_native = 1;
    static const char *const removed[] = {
        "RECOMP_PB_EXEC", "RECOMP_PB_SCAN", "RECOMP_NV2A_TRACE",
        "RECOMP_DSP", "RECOMP_USB", "RECOMP_AC97_READY",
        "RECOMP_HLE_LIBAPI", "RECOMP_HLE_LIBDRAW", "RECOMP_HLE_LIBSTATE",
        "RECOMP_DS_SHADOW", "RECOMP_HLE_APICHECK"
    };
    for (unsigned i = 0; i < sizeof removed / sizeof removed[0]; i++)
        port_env(removed[i], NULL);
    port_env("RECOMP_HLE_PRIMARY", "1");
    port_env("RECOMP_HLE_NOGPU", "1");
    port_env("RECOMP_AUDIO_NATIVE", "1");
    port_env("RECOMP_DS_NATIVE", "1");
    port_env("RECOMP_INPUT_NATIVE", "1");
    port_env("RECOMP_HLE_STALLDIAG", "0");
#if HLE_GAME == HLE_GAME_MKA
    fprintf(stderr, "[PORT] MK Armageddon native API development build; hardware command fallback prohibited\n");
#else /* street2 */
    fprintf(stderr, "[PORT] Street 2 native API development build; hardware command fallback prohibited\n");
#endif /* HLE_GAME */
}

void hle_port_native_missing(const char *boundary, unsigned caller)
{
    fprintf(stderr, "[NATIVE-MISSING] %s; guest caller=%08X. Native replacement required; stopping without hardware fallback.\n",
            boundary, caller);
    fflush(NULL);
    _Exit(78);
}
