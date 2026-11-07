/*
 * Code the build did not translate: record it and stop.
 *
 * Static analysis cannot see every function (vtable targets, thread start routines, code reached only through
 * data). When a recompiled title reaches one, the address is appended to a list the next build reads
 * (tools/port: the address is seeded, translated natively and linked in), and the run stops with a message
 * saying so. Nothing is emulated: the next build simply contains that code.
 *
 * Only addresses inside the XBE's code sections count. A target in data, or outside the image, is a null or
 * wild function pointer, not missing code; those keep the old behaviour (the call is skipped).
 *
 *   RECOMP_MISSING_FILE   where to append the addresses (default: missing_code.txt in the working directory)
 *   RECOMP_MISSING_STOP   compile definition: stop by default (set by the generic build pipeline)
 *   RECOMP_MISSING=stop|continue  at run time: stop, or record and skip the call as before
 */
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

extern ptrdiff_t g_xbox_mem_offset;

#define XBE_BASE 0x00010000u
#define MEM32(a) (*(volatile uint32_t *)((uintptr_t)(a) + g_xbox_mem_offset))
#define MEM8(a)  (*(volatile uint8_t *)((uintptr_t)(a) + g_xbox_mem_offset))

/* 1 when va lies in an executable section of the loaded XBE that is not a data section. */
static int in_code_section(uint32_t va)
{
    if (MEM32(XBE_BASE) != 0x48454258u) /* "XBEH" */
        return 0;
    uint32_t count = MEM32(XBE_BASE + 0x11C), headers = MEM32(XBE_BASE + 0x120);
    if (count > 256 || headers < XBE_BASE)
        return 0;
    for (uint32_t i = 0; i < count; i++) {
        uint32_t h = headers + i * 56u;
        uint32_t flags = MEM32(h), start = MEM32(h + 4), size = MEM32(h + 8), name = MEM32(h + 20);
        if (!(flags & 4u) || va < start || va >= start + size)
            continue;
        char n[9] = {0};
        for (int k = 0; k < 8 && name; k++)
            n[k] = (char)MEM8(name + (uint32_t)k);
        if (!strcmp(n, ".data") || !strcmp(n, ".rdata") || !strncmp(n, "$$", 2) || !strcmp(n, ".XTLID"))
            return 0;
        return 1;
    }
    return 0;
}

/* Returns 1 when the address was missing code (recorded; the run stops when configured to, see above),
 * 0 when it is not code at all (the caller skips the call as before). */
int xbox_missing_code(uint32_t va, const char *how)
{
    static uint32_t seen[64];
    static unsigned nseen;
    if (!in_code_section(va))
        return 0;
    for (unsigned i = 0; i < nseen && i < 64; i++)
        if (seen[i] == va)
            return 1;
    if (nseen < 64)
        seen[nseen++] = va;

    const char *path = getenv("RECOMP_MISSING_FILE");
    FILE *f = fopen(path && *path ? path : "missing_code.txt", "a");
    if (f) {
        fprintf(f, "0x%08X %s\n", va, how ? how : "call");
        fclose(f);
    }
    /* Builds of the generic pipeline (RECOMP_MISSING_STOP) stop; older per-game builds keep going until they move
     * onto it. RECOMP_MISSING=stop|continue overrides either. */
    const char *mode = getenv("RECOMP_MISSING");
#ifdef RECOMP_MISSING_STOP
    int stop = !(mode && !strcmp(mode, "continue"));
#else
    int stop = mode && !strcmp(mode, "stop");
#endif
    if (!stop) {
        fprintf(stderr, "[MISSING] code at 0x%08X (%s) was not translated; recorded, call skipped\n", va,
                how ? how : "call");
        return 1;
    }
    fprintf(stderr,
            "\n[MISSING] The game reached code at 0x%08X (%s) that this build did not translate.\n"
            "          It was recorded in %s: build again and it will be translated natively.\n"
            "          (RECOMP_MISSING=continue skips such calls instead of stopping.)\n",
            va, how ? how : "call", path && *path ? path : "missing_code.txt");
    fflush(stderr);
    fflush(stdout);
    exit(3);
}
