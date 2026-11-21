/* Generic port (tools/xboxport): what the translated code calls when it leaves the translated set. A missing
 * function or instruction is reported (and recorded by the runtime) instead of being silently skipped. */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

typedef void (*recomp_func_t)(void);

recomp_func_t recomp_lookup_manual(uint32_t va) { (void)va; return (recomp_func_t)0; }

extern int xbox_missing_code(uint32_t va, const char *how);  /* runtime: record + stop */

void recomp_icall_fail_log(uint32_t va)
{
    xbox_missing_code(va, "indirect call");
    fprintf(stderr, "[ICALL] untranslated indirect call target 0x%08X\n", va);
    fflush(stderr);
}

void recomp_icall_not_code_log(uint32_t va)
{
    static unsigned shown;
    if (shown++ < 32) {
        fprintf(stderr, "[ICALL] target 0x%08X is not code (null or wild function pointer), skipped\n", va);
        fflush(stderr);
    }
}

/* The APU calls this diagnostic of the native DirectSound HLE (hle/d3d8) only when that HLE installs
 * g_apu_shadow_voices; a generic port has no HLE, so it is never reached. */
void hle_ds_shadow_compare(float lib[32][32], float nat[32][32]) { (void)lib; (void)nat; }

void recomp_unimpl(const char *text, uint32_t va)
{
    static unsigned shown;
    const char *trap = getenv("RECOMP_UNIMPL_TRAP");
    if (shown++ < 50) {
        fprintf(stderr, "[UNIMPL] untranslated instruction `%s` at 0x%08X\n", text, va);
        fflush(stderr);
    }
    if (trap && *trap && *trap != '0') abort();
}
