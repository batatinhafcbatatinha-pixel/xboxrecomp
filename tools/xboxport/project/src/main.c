/* Generic port host (tools/xboxport): any original-Xbox game, no game-specific code.
 *
 * Loads the game's default.xbe from the game folder (argv[1], default "game_files"), maps the Xbox memory layout
 * (the kernel thunk table is taken from the XBE header), starts the kernel replacement with the game folder as D:\
 * and calls the translated entry point. Graphics and sound go through the toolkit's emulated hardware (NV2A, APU),
 * as in templates/new-game. */
#include <xbox/xboxrecomp.h>
#include <dbghelp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

extern RECOMP_TLS uint32_t g_eax, g_ecx, g_edx, g_esp;
extern RECOMP_TLS uint32_t g_ebx, g_esi, g_edi;
extern ptrdiff_t g_xbox_mem_offset;
extern int recomp_dispatch_init(void);
extern void xbe_entry_point(void);

typedef struct MCPXAPUState MCPXAPUState;
extern MCPXAPUState *mcpx_apu_init_standalone(uint8_t *ram_ptr);
extern MCPXAPUState *g_apu_state;
extern bool apu_hook_handle_mmio(PCONTEXT ctx, uintptr_t fault_addr, uint32_t fault_xbox_va, int is_write);
#define APU_TRAP_BASE 0xFE800000u
#define APU_TRAP_END  0xFE880000u

/* Crash report: the guest registers and the translated function (sub_XXXXXXXX) the fault happened in. */
static LONG CALLBACK veh_handler(PEXCEPTION_POINTERS ep)
{
    if (ep->ExceptionRecord->ExceptionCode != EXCEPTION_ACCESS_VIOLATION)
        return EXCEPTION_CONTINUE_SEARCH;
    uintptr_t fault = ep->ExceptionRecord->ExceptionInformation[1];
    if (fault >= 0xFD000000 && fault < 0xFE000000)
        return EXCEPTION_CONTINUE_SEARCH;   /* NV2A MMIO: handled by the GPU emulation's own handler */
    uint32_t va = (uint32_t)(fault - (uintptr_t)g_xbox_mem_offset);
    if (va >= APU_TRAP_BASE && va < APU_TRAP_END &&
            apu_hook_handle_mmio(ep->ContextRecord, fault, va, ep->ExceptionRecord->ExceptionInformation[0] ? 1 : 0))
        return EXCEPTION_CONTINUE_EXECUTION;
    fprintf(stderr, "[CRASH] access violation at RIP=0x%llX, address 0x%llX (%s), Xbox VA 0x%08X\n",
            (unsigned long long)ep->ContextRecord->Rip, (unsigned long long)fault,
            ep->ExceptionRecord->ExceptionInformation[0] ? "write" : "read", va);
    fprintf(stderr, "  eax=%08X ecx=%08X edx=%08X ebx=%08X esi=%08X edi=%08X esp=%08X\n",
            g_eax, g_ecx, g_edx, g_ebx, g_esi, g_edi, g_esp);
    char buf[sizeof(SYMBOL_INFO) + 256];
    SYMBOL_INFO *sym = (SYMBOL_INFO *)buf;
    DWORD64 disp = 0;
    memset(buf, 0, sizeof(buf));
    sym->SizeOfStruct = sizeof(SYMBOL_INFO);
    sym->MaxNameLen = 255;
    if (SymFromAddr(GetCurrentProcess(), (DWORD64)ep->ContextRecord->Rip, &disp, sym))
        fprintf(stderr, "  in %s+0x%llX\n", sym->Name, (unsigned long long)disp);
    fflush(stderr);
    return EXCEPTION_CONTINUE_SEARCH;
}

static int load_xbe(const char *path, void **data, size_t *bytes)
{
    FILE *f = fopen(path, "rb");
    if (!f) return 0;
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (size <= 0) { fclose(f); return 0; }
    *data = malloc((size_t)size);
    if (!*data) { fclose(f); return 0; }
    *bytes = fread(*data, 1, (size_t)size, f);
    fclose(f);
    if (*bytes != (size_t)size || memcmp(*data, "XBEH", 4)) { free(*data); *data = NULL; return 0; }
    return 1;
}

int main(int argc, char **argv)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    setvbuf(stderr, NULL, _IONBF, 0);
    const char *game = argc > 1 ? argv[1] : "game_files";
    char xbe[MAX_PATH];
    void *data = NULL;
    size_t bytes = 0;
    if (snprintf(xbe, sizeof(xbe), "%s\\default.xbe", game) >= (int)sizeof(xbe) || !load_xbe(xbe, &data, &bytes)) {
        fprintf(stderr, "default.xbe missing or not an XBE: %s\n", xbe);
        return 2;
    }
    SymSetOptions(SYMOPT_DEFERRED_LOADS | SYMOPT_UNDNAME);
    SymInitialize(GetCurrentProcess(), NULL, TRUE);
    AddVectoredExceptionHandler(1, veh_handler);
    if (!xbox_MemoryLayoutInit(data, bytes)) {
        fprintf(stderr, "Cannot map the Xbox memory layout (the address range may be in use)\n");
        free(data);
        return 3;
    }
    g_xbox_mem_offset = xbox_GetMemoryOffset();
    g_apu_state = mcpx_apu_init_standalone((uint8_t *)xbox_GetMemoryBase());
    xbox_kernel_init();
    {
        extern void xbox_path_init(const char *game_dir, const char *save_dir);
        xbox_path_init(game, NULL);
    }
    xbox_kernel_bridge_init();
    g_esp = XBOX_STACK_TOP;
    if (!recomp_dispatch_init())
        fprintf(stderr, "[BOOT] flat dispatch unavailable; indirect calls use the binary search\n");
    xbox_WatchdogStart();
    xbe_entry_point();
    xbox_kernel_shutdown();
    xbox_MemoryLayoutShutdown();
    free(data);
    return 0;
}
