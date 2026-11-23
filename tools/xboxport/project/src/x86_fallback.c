/* Generic port (tools/xboxport): guest code the translation does not hold runs emulated instead of being skipped.
 *
 * A call into untranslated code (an unresolved direct-call stub, or an indirect call whose target has no translated
 * function) used to be skipped, which leaves the game's state wrong: it crashes soon after. Here the x86 code runs
 * on Unicorn (an emulated CPU, unicorn.dll next to the game) over the game's own memory, from the target until a
 * ret takes the stack above where it started (back in translated code: the caller, or for a jump into the middle
 * of a function, that function's caller). Whenever the emulated code reaches an address that does have a
 * translated function (a game function or a kernel thunk), emulation pauses, the native function runs with the
 * registers handed over, and emulation resumes where it returns. The address is recorded in missing_code.txt as
 * before, so the next build translates it natively.
 *
 * Built with XBOXPORT_FALLBACK (tools/xboxport fetches Unicorn); without it every entry point reports "not run" and
 * the old skip applies. */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "recomp_types.h"

#ifdef XBOXPORT_FALLBACK
#include <windows.h>
#include <unicorn/unicorn.h>

/* An x87 register as Unicorn reads and writes it: 64-bit mantissa, then sign and exponent. */
typedef struct { uint64_t mantissa; uint16_t exponent; } uc_x86_float80;

/* unicorn.dll is loaded the first time it is needed (its import library is MSVC's, which the mingw linker does not
 * take); without it the fallback reports "not run" and untranslated code is skipped as before. */
static struct {
    __typeof__(&uc_open) open;
    __typeof__(&uc_hook_add) hook_add;
    __typeof__(&uc_mem_map_ptr) mem_map_ptr;
    __typeof__(&uc_emu_start) emu_start;
    __typeof__(&uc_emu_stop) emu_stop;
    __typeof__(&uc_reg_write) reg_write;
    __typeof__(&uc_reg_read) reg_read;
    __typeof__(&uc_strerror) strerror_;
    __typeof__(&uc_mem_map) mem_map;
    __typeof__(&uc_mem_write) mem_write;
    __typeof__(&uc_mem_unmap) mem_unmap;
    int state;   /* 0: not tried, 1: loaded, -1: unavailable */
} uc_api;

static int load_unicorn(void)
{
    if (uc_api.state)
        return uc_api.state > 0;
    uc_api.state = -1;
    HMODULE dll = LoadLibraryA("unicorn.dll");
    if (!dll) {
        fprintf(stderr, "[FALLBACK] unicorn.dll not found: untranslated code is skipped\n");
        return 0;
    }
#define UC_LOAD(field, name) if (!(*(FARPROC *)&uc_api.field = GetProcAddress(dll, name))) return 0
    UC_LOAD(open, "uc_open"); UC_LOAD(hook_add, "uc_hook_add"); UC_LOAD(mem_map_ptr, "uc_mem_map_ptr");
    UC_LOAD(emu_start, "uc_emu_start"); UC_LOAD(emu_stop, "uc_emu_stop"); UC_LOAD(reg_write, "uc_reg_write");
    UC_LOAD(reg_read, "uc_reg_read"); UC_LOAD(strerror_, "uc_strerror"); UC_LOAD(mem_map, "uc_mem_map");
    UC_LOAD(mem_write, "uc_mem_write"); UC_LOAD(mem_unmap, "uc_mem_unmap");
#undef UC_LOAD
    uc_api.state = 1;
    if (getenv("RECOMP_FALLBACK_TRACE"))
        fprintf(stderr, "[FALLBACK] unicorn.dll at %p\n", (void *)dll);
    return 1;
}
#define uc_open uc_api.open
#define uc_hook_add uc_api.hook_add
#define uc_mem_map_ptr uc_api.mem_map_ptr
#define uc_emu_start uc_api.emu_start
#define uc_emu_stop uc_api.emu_stop
#define uc_reg_write uc_api.reg_write
#define uc_reg_read uc_api.reg_read
#define uc_strerror uc_api.strerror_
#define uc_mem_map uc_api.mem_map
#define uc_mem_write uc_api.mem_write
#define uc_mem_unmap uc_api.mem_unmap

/* One emulator for the process, made at start-up (xboxport_fallback_init, before the runtime lays out the guest
 * address space, so the emulator's own code buffers are not placed in a gap of it) and used by one thread at a
 * time; the lock is let go while a native function runs (it may wait on other threads). */
static uc_engine *g_uc;
static CRITICAL_SECTION g_uc_lock;
static RECOMP_TLS uint32_t t_native;        /* translated address the emulation paused at, 0 if none */
static RECOMP_TLS uint32_t t_entry;         /* where the current emulation started (not a pause point) */

static recomp_func_t translated(uint32_t va)
{
    recomp_func_t fn = recomp_lookup(va);
    return fn ? fn : recomp_lookup_kernel(va);
}

/* Guest memory mapped into the emulator, undone after each stretch of emulation: the runtime commits, frees and
 * protects pages of its windows as the game runs, so a mapping is only trusted while the guest code it was made for
 * runs (unicorn.dll would otherwise write into pages released since). */
static RECOMP_TLS uint32_t t_mapped_base[1024], t_mapped_size[1024];
static RECOMP_TLS unsigned t_mapped;

static void unmap_all(uc_engine *uc)
{
    while (t_mapped) {
        t_mapped--;
        uc_mem_unmap(uc, t_mapped_base[t_mapped], t_mapped_size[t_mapped]);
    }
}

/* Maps guest memory into the emulator on first touch: the 64 KB block around the address when the runtime has it
 * committed, else just its page. The emulator then reads and writes the game's own memory. */
static bool on_unmapped(uc_engine *uc, uc_mem_type type, uint64_t address, int size, int64_t value, void *user)
{
    (void)type; (void)size; (void)value; (void)user;
    for (uint64_t span = 0x10000; span >= 0x1000; span >>= 4) {
        uint64_t base = address & ~(span - 1);
        uint8_t *host = (uint8_t *)(uintptr_t)XBOX_PTR((uint32_t)base);
        MEMORY_BASIC_INFORMATION mbi;
        if (!VirtualQuery(host, &mbi, sizeof mbi) || mbi.State != MEM_COMMIT)
            continue;
        if ((uint8_t *)mbi.BaseAddress > host || (uint8_t *)mbi.BaseAddress + mbi.RegionSize < host + span)
            continue;
        /* The guest windows are not all writable (mirrors, apertures): the emulator gets the same rights, so a
         * write the runtime would have to handle fails the emulation instead of faulting inside unicorn.dll. */
        DWORD protect = mbi.Protect & 0xFFu;
        if ((mbi.Protect & PAGE_GUARD) || protect == PAGE_NOACCESS)
            continue;
        uint32_t perms = UC_PROT_READ | UC_PROT_EXEC;
        if (protect == PAGE_READWRITE || protect == PAGE_EXECUTE_READWRITE || protect == PAGE_WRITECOPY ||
            protect == PAGE_EXECUTE_WRITECOPY)
            perms |= UC_PROT_WRITE;
        if (t_mapped >= 1024)
            unmap_all(uc);
        if (uc_mem_map_ptr(uc, base, (size_t)span, perms, host) == UC_ERR_OK) {
            t_mapped_base[t_mapped] = (uint32_t)base;
            t_mapped_size[t_mapped++] = (uint32_t)span;
            return true;
        }
    }
    return false;
}

static RECOMP_TLS uint32_t t_entry_esp;     /* esp when the emulation was asked for */
static RECOMP_TLS int t_left;               /* stopped at a ret out of the frame the emulation started in */
static RECOMP_TLS uint32_t t_left_esp;      /* esp after that ret */

/* Every instruction, before it runs: a ret (ret, ret N, rep ret) that takes esp above the value the emulation
 * started with leaves the frame it began in: control goes back to translated code (the caller, or the caller of the
 * function that jumped here). Emulation stops there and the ret is completed by hand. */
static void on_code(uc_engine *uc, uint64_t address, uint32_t size, void *user)
{
    (void)size; (void)user;
    uint32_t pc = (uint32_t)address;
    uint8_t op = MEM8(pc);
    if (op == 0xF3 && MEM8(pc + 1) == 0xC3)
        op = 0xC3;
    if (op != 0xC3 && op != 0xC2)
        return;
    uint32_t esp = 0;
    uc_reg_read(uc, UC_X86_REG_ESP, &esp);
    uint32_t after = esp + 4u + (op == 0xC2 ? MEM16(pc + 1) : 0u);
    if (after > t_entry_esp) {
        t_left = 1;
        t_left_esp = after;
        uc_emu_stop(uc);
    }
}

/* Port I/O (the hardware setup code): as the runtime does for translated code, reads give 0, writes are dropped. */
static uint32_t on_in(uc_engine *uc, uint32_t port, int size, void *user) { (void)uc; (void)port; (void)size; (void)user; return 0; }
static void on_out(uc_engine *uc, uint32_t port, int size, uint32_t value, void *user)
{ (void)uc; (void)port; (void)size; (void)value; (void)user; }

/* Every basic block, before it runs: a translated function pauses the emulation (it runs natively). */
static void on_block(uc_engine *uc, uint64_t address, uint32_t size, void *user)
{
    (void)size; (void)user;
    if ((uint32_t)address != t_entry && translated((uint32_t)address)) {
        t_native = (uint32_t)address;
        uc_emu_stop(uc);
    }
}

/* fs: the guest's thread block (SEH, TLS). In 32-bit mode the emulator takes segment bases from a descriptor table:
 * one of its own, outside the game's memory, holding a data segment based at the runtime's fs base. */
#define FALLBACK_GDT 0xFFFFF000u
static void set_fs(uc_engine *uc, uint32_t base)
{
    uint8_t gdt[16] = {0};
    uint8_t *d = gdt + 8;                          /* selector 0x08: base, limit 4 GB, data rw, present, 32-bit */
    d[0] = 0xFF; d[1] = 0xFF;
    d[2] = (uint8_t)base; d[3] = (uint8_t)(base >> 8); d[4] = (uint8_t)(base >> 16);
    d[5] = 0x93; d[6] = 0xCF; d[7] = (uint8_t)(base >> 24);
    uc_mem_write(uc, FALLBACK_GDT, gdt, sizeof gdt);
    uc_x86_mmr gdtr = {0, FALLBACK_GDT, sizeof gdt - 1, 0};
    uc_reg_write(uc, UC_X86_REG_GDTR, &gdtr);
    uint32_t selector = 0x08;
    uc_reg_write(uc, UC_X86_REG_FS, &selector);
}

static uc_engine *engine(void) { return g_uc; }

void xboxport_fallback_init(void)
{
    if (g_uc || (getenv("RECOMP_FALLBACK") && !strcmp(getenv("RECOMP_FALLBACK"), "0")) || !load_unicorn())
        return;
    uc_engine *uc;
    if (uc_open(UC_ARCH_X86, UC_MODE_32, &uc) != UC_ERR_OK)
        return;
    uc_mem_map(uc, FALLBACK_GDT, 0x1000, UC_PROT_READ | UC_PROT_WRITE);
    uc_hook h1, h2, h3, h4, h5;
    uc_hook_add(uc, &h1, UC_HOOK_MEM_UNMAPPED, (void *)on_unmapped, NULL, 1, 0);
    uc_hook_add(uc, &h2, UC_HOOK_BLOCK, (void *)on_block, NULL, 1, 0);
    uc_hook_add(uc, &h3, UC_HOOK_CODE, (void *)on_code, NULL, 1, 0);
    uc_hook_add(uc, &h4, UC_HOOK_INSN, (void *)on_in, NULL, 1, 0, UC_X86_INS_IN);
    uc_hook_add(uc, &h5, UC_HOOK_INSN, (void *)on_out, NULL, 1, 0, UC_X86_INS_OUT);
    /* Run once so the translation cache is set up now, before the guest address space exists. */
    uint8_t probe = 0xC3;
    uc_mem_map(uc, 0xFFFFE000u, 0x1000, UC_PROT_ALL);
    uc_mem_write(uc, 0xFFFFE000u, &probe, 1);
    uc_emu_start(uc, 0xFFFFE000u, 0xFFFFE000u, 0, 1);
    uc_mem_unmap(uc, 0xFFFFE000u, 0x1000);
    InitializeCriticalSection(&g_uc_lock);
    g_uc = uc;
}

static void f80_to_uc(double v, uc_x86_float80 *out)
{
    uint64_t mant;
    out->exponent = recomp_f80_store(v, &mant);
    out->mantissa = mant;
}

/* The runtime's registers (thread-local globals) into the emulator, and back. */
static void to_uc(uc_engine *uc, uint32_t pc)
{
    uint32_t r[] = {g_eax, g_ecx, g_edx, g_ebx, g_esp, g_ebp, g_esi, g_edi, pc, 0x202u};
    int ids[] = {UC_X86_REG_EAX, UC_X86_REG_ECX, UC_X86_REG_EDX, UC_X86_REG_EBX, UC_X86_REG_ESP,
                 UC_X86_REG_EBP, UC_X86_REG_ESI, UC_X86_REG_EDI, UC_X86_REG_EIP, UC_X86_REG_EFLAGS};
    for (int i = 0; i < 10; i++)
        uc_reg_write(uc, ids[i], &r[i]);
    set_fs(uc, g_fs_base);
    for (int i = 0; i < 8; i++) {   /* physical x87 registers: ST(i) = g_fp_stack[(top + i) & 7] on both sides */
        uc_x86_float80 f;
        f80_to_uc(g_fp_stack[i], &f);
        uc_reg_write(uc, UC_X86_REG_FP0 + i, &f);
    }
    uint32_t cw = g_fp_control_word, sw = ((uint32_t)(g_fp_top & 7) << 11) | g_fp_cc, tag = 0;
    uc_reg_write(uc, UC_X86_REG_FPCW, &cw);
    uc_reg_write(uc, UC_X86_REG_FPSW, &sw);
    uc_reg_write(uc, UC_X86_REG_FPTAG, &tag);
}

static uint32_t from_uc(uc_engine *uc)
{
    uint32_t *r[] = {&g_eax, &g_ecx, &g_edx, &g_ebx, &g_esp, &g_ebp, &g_esi, &g_edi};
    int ids[] = {UC_X86_REG_EAX, UC_X86_REG_ECX, UC_X86_REG_EDX, UC_X86_REG_EBX, UC_X86_REG_ESP,
                 UC_X86_REG_EBP, UC_X86_REG_ESI, UC_X86_REG_EDI};
    for (int i = 0; i < 8; i++)
        uc_reg_read(uc, ids[i], r[i]);
    for (int i = 0; i < 8; i++) {
        uc_x86_float80 f;
        uc_reg_read(uc, UC_X86_REG_FP0 + i, &f);
        g_fp_stack[i] = recomp_f80_load(f.mantissa, f.exponent);
    }
    uint32_t cw = 0, sw = 0, pc = 0;
    uc_reg_read(uc, UC_X86_REG_FPCW, &cw);
    uc_reg_read(uc, UC_X86_REG_FPSW, &sw);
    g_fp_control_word = (uint16_t)cw;
    g_fp_top = (int)((sw >> 11) & 7);
    g_fp_cc = (uint16_t)(sw & 0x4700u);
    uc_reg_read(uc, UC_X86_REG_EIP, &pc);
    return pc;
}

static void record(uint32_t va, const char *how)
{
    static uint32_t seen[256];
    static unsigned nseen;
    for (unsigned i = 0; i < nseen; i++)
        if (seen[i] == va)
            return;
    if (nseen < 256)
        seen[nseen++] = va;
    const char *path = getenv("RECOMP_MISSING_FILE");
    FILE *f = fopen(path && *path ? path : "missing_code.txt", "a");
    if (f) {
        fprintf(f, "0x%08X %s\n", va, how);
        fclose(f);
    }
    if (nseen <= 32)
        fprintf(stderr, "[FALLBACK] untranslated code at 0x%08X (%s): running it emulated (recorded for the next build)\n",
                va, how);
}

/* Runs the guest code at va until it leaves the stack frame it started in (see on_code). 1 when it did, 0 when it
 * could not run it (the caller then skips the call as before). */
int xboxport_fallback_run(uint32_t va, const char *how)
{
    uc_engine *uc = engine();
    if (!uc)
        return 0;
    record(va, how);
    static int trace = -1;
    if (trace < 0)
        trace = getenv("RECOMP_FALLBACK_TRACE") != NULL;
    const uint32_t entry_esp = g_esp;
    uint32_t pc = va;
    int result = 0;
    EnterCriticalSection(&g_uc_lock);
    for (unsigned rounds = 0; rounds < 1000000u; rounds++) {
        t_native = 0;
        t_left = 0;
        t_entry = pc;
        t_entry_esp = entry_esp;
        to_uc(uc, pc);
        if (trace)
            fprintf(stderr, "[FALLBACK] run 0x%08X esp=%08X (entry %08X) ebp=%08X eax=%08X ecx=%08X edx=%08X ebx=%08X "
                    "esi=%08X edi=%08X\n", pc, g_esp, entry_esp, g_ebp, g_eax, g_ecx, g_edx, g_ebx, g_esi, g_edi);
        uc_err err = uc_emu_start(uc, pc, 0xFFFFFFFFull, 0, 0);
        unmap_all(uc);
        pc = from_uc(uc);
        if (trace)
            fprintf(stderr, "[FALLBACK]   -> pc=%08X err=%d native=%08X left=%d esp=%08X eax=%08X\n", pc, (int)err,
                    t_native, t_left, g_esp, g_eax);
        if (err != UC_ERR_OK) {
            fprintf(stderr, "[FALLBACK] emulation of 0x%08X stopped at 0x%08X: %s\n", va, pc, uc_strerror(err));
            break;
        }
        if (t_left) {
            g_esp = t_left_esp;   /* the ret, done here: its return address is where the translated code goes on */
            result = 1;
            break;
        }
        if (!t_native)
            break;
        /* A translated function: run it natively; it returns to the address on top of the stack. */
        const uint32_t native = t_native, back = MEM32(g_esp);
        LeaveCriticalSection(&g_uc_lock);
        translated(native)();
        EnterCriticalSection(&g_uc_lock);
        if (g_esp > entry_esp) {
            result = 1;   /* it returned out of the frame the emulation started in */
            break;
        }
        pc = back;
    }
    LeaveCriticalSection(&g_uc_lock);
    return result;
}

static RECOMP_TLS uint32_t t_icall_target;
static void icall_trampoline(void)
{
    /* Not run: consume the return address the caller pushed, as the old skip did. */
    if (!xboxport_fallback_run(t_icall_target, "indirect call")) {
        g_esp += 4;
        g_eax = 0;
    }
}

/* recomp_lookup_manual (integration.c) asks first on every indirect call: an untranslated target in the game's code
 * gets the trampoline, which emulates it. */
recomp_func_t xboxport_fallback_lookup(uint32_t va)
{
    if (!RECOMP_ICALL_IS_CODE(va) || va >= 0xFE000000u || translated(va) || !engine())
        return (recomp_func_t)0;
    t_icall_target = va;
    return icall_trampoline;
}

#else

void xboxport_fallback_init(void) {}
int xboxport_fallback_run(uint32_t va, const char *how) { (void)va; (void)how; return 0; }
recomp_func_t xboxport_fallback_lookup(uint32_t va) { (void)va; return (recomp_func_t)0; }

#endif
