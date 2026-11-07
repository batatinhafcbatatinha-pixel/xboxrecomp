/*
 * MK Armageddon process scheduler on host fibers.
 *
 * The game runs its logic as cooperative "processes". The scheduler
 * (sub_001C4DD0) calls the run method of each process's vtable; a process can
 * sleep in the middle of any call chain, and the original code does that by
 * swapping the guest stack pointer and jumping to saved addresses
 * (`mov esp, [0x5B8B50]; jmp [0x5B8B40]`). Translated code keeps its frames
 * on the host stack, so those jumps cannot work: the scheduler's return
 * address 0x001C4FEE was dispatched as a function ("[ICALL] Failed").
 *
 * Here every process body runs on a host fiber. A sleep saves the guest
 * state exactly as the original does (registers, resume address, stack copy
 * or stack pointer, all in the process object) and switches back to the
 * scheduler's fiber; the next run restores the guest state and switches to
 * the suspended fiber, so the host frames of the sleeping call chain resume
 * where they stopped. Jumps (restart the process at a new function) start a
 * fresh fiber and discard the old one.
 *
 * Process vtables (0x55DB5C simple, 0x55DB84 stack copy, 0x55DBAC/0x55DBD4
 * own stack): +0xC kill, +0x10 run, +0x14 sleep, +0x18/+0x1C switch to/from
 * the main stack, +0x20 jump. Process object: +4 alive, +0xC sleep time
 * (float), +0x10..+0x1C ebp/ebx/esi/edi, +0x2C hook, +0x30 function,
 * +0x34 resume address, +0x40 save-area end / stack top, +0x44 save start /
 * saved esp.
 */
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include "recomp/gen/recomp_types.h"

#define CUR_PROC  MEM32(0x007350C0u)
#define RUNNING   MEM32(0x007350B4u)
#define SLEEP_SET MEM32(0x007350C4u)
#define SCHED_RET MEM32(0x005B8B40u)
#define SCHED_ESP MEM32(0x005B8B50u)
#define MAIN_SAVE MEM32(0x005B8B54u)
#define SCHED_EBP MEM32(0x005B8B60u)

enum { KIND_SIMPLE, KIND_COPY, KIND_STACK };
enum { CO_FREE, CO_RUNNING, CO_SLEEPING, CO_ZOMBIE };

/* Return addresses the original run loops push for the process function. */
#define RET_SIMPLE 0x001C50BAu
#define RET_COPY   0x001C5291u
#define RET_STACK  0x001C54C2u

typedef struct Co {
    void *fiber;
    void *ret_fiber;
    uint32_t proc;
    uint32_t sleep_sched_esp;
    int kind;
    int from_jump;
    uint32_t jump_sleep;
    uint32_t jump_esp;
    int state;
    struct Co *next;
} Co;

static RECOMP_TLS Co *t_free, *t_zombies, *t_sleepers, *t_cur;
static RECOMP_TLS void *t_thread_fiber;
/* A guest longjmp requested inside a process fiber (sub_0019C980), carried out on the thread fiber. */
static RECOMP_TLS int t_ljmp_pending;
static RECOMP_TLS uint32_t t_ljmp_buf, t_ljmp_val;
extern int recomp_guest_longjmp(uint32_t buf_va, uint32_t value);
static volatile LONG g_fibers, g_warned;

static float as_float(uint32_t v) { float f; memcpy(&f, &v, 4); return f; }

static void warn(const char *what, uint32_t a, uint32_t b)
{
    if (InterlockedIncrement(&g_warned) <= 64)
        fprintf(stderr, "[PROC] %s %08X %08X\n", what, a, b);
}

/* Calls guest code at va the way a `call` would: pushes ret_va, the callee
 * pops it. */
static void call_guest(uint32_t va, uint32_t ret_va)
{
    recomp_func_t fn = recomp_lookup_manual(va);
    if (!fn) fn = recomp_lookup(va);
    if (!fn) fn = recomp_lookup_kernel(va);
    if (!fn) {
        warn("no code at", va, ret_va);
        g_eax = 0;
        return;
    }
    g_esp -= 4;
    MEM32(g_esp) = ret_va;
    fn();
}

static float fp_pop_value(void)
{
    double v = g_fp_stack[g_fp_top];
    g_fp_top = (g_fp_top + 1) & 7;
    return (float)v;
}

static void kill_current(uint32_t ret_va)
{
    uint32_t p = CUR_PROC;
    RUNNING = 0;
    if (p && MEM32(p + 4)) {
        g_esp -= 4;
        MEM32(g_esp) = p;
        g_eax = p;
        call_guest(MEM32(MEM32(p) + 0xC), ret_va);
        g_esp += 4;
    }
}

static void reap_zombies(void)
{
    Co *z = t_zombies, *keep = NULL;
    void *self = GetCurrentFiber();
    while (z) {
        Co *n = z->next;
        if (z->fiber == self) {
            z->next = keep;
            keep = z;
        } else {
            DeleteFiber(z->fiber);
            free(z);
            InterlockedDecrement(&g_fibers);
        }
        z = n;
    }
    t_zombies = keep;
}

static void unlink_sleeper(Co *c)
{
    Co **pp = &t_sleepers;
    while (*pp && *pp != c) pp = &(*pp)->next;
    if (*pp) *pp = c->next;
    c->next = NULL;
}

static Co *find_sleeper(uint32_t proc)
{
    Co *c;
    for (c = t_sleepers; c; c = c->next)
        if (c->proc == proc) return c;
    return NULL;
}

static void make_zombie(Co *c)
{
    c->state = CO_ZOMBIE;
    c->next = t_zombies;
    t_zombies = c;
}

/* After the process function returned (or a jump restarted the loop): the
 * code after `call [proc+0x30]` in the three run loops. */
static void run_loop(Co *c)
{
    uint32_t ret = c->kind == KIND_SIMPLE ? RET_SIMPLE : c->kind == KIND_COPY ? RET_COPY : RET_STACK;
    uint32_t call_esp, proc;
    float s;

    if (c->from_jump) {
        call_esp = c->jump_esp;
        g_esp = call_esp;
        g_fp_top = (g_fp_top + 7) & 7;           /* fld [0x5B8B38] before the ret */
        g_fp_stack[g_fp_top] = as_float(c->jump_sleep);
        goto after_call;
    }
    call_esp = c->kind == KIND_STACK ? MEM32(c->proc + 0x44) : SCHED_ESP;
    for (;;) {
        g_esp = call_esp;
        g_ebp = SCHED_EBP;
        proc = CUR_PROC;
        g_eax = g_edx = proc;
        call_guest(MEM32(proc + 0x30), ret);
    after_call:
        g_esp = call_esp;
        s = fp_pop_value();
        proc = CUR_PROC;
        memcpy((void *)XBOX_PTR(proc + 0xC), &s, 4);
        if (c->kind != KIND_SIMPLE)
            MEM32(proc + 0x44) = MEM32(proc + 0x40);
        if (s != 0.0f) break;                    /* only an exact 0 runs it again */
    }
    proc = CUR_PROC;
    if (MEM32(proc + 0x2C)) {
        g_ecx = proc;
        call_guest(MEM32(proc + 0x2C), ret + 0x48);
        if (MEM32(CUR_PROC + 4) == 0) return;
    }
    proc = CUR_PROC;
    if (MEMF(proc + 0xC) < 0.0f) {
        g_esp = SCHED_ESP;
        kill_current(ret + 0x9C);
    }
}

static void WINAPI co_main(void *param)
{
    Co *c = (Co *)param;
    for (;;) {
        t_cur = c;
        reap_zombies();
        run_loop(c);
        c->state = CO_FREE;
        c->next = t_free;
        t_free = c;
        t_cur = NULL;
        SwitchToFiber(c->ret_fiber);
    }
}

static Co *get_free(void)
{
    Co *c = t_free;
    if (c) {
        t_free = c->next;
    } else {
        c = (Co *)calloc(1, sizeof *c);
        c->fiber = CreateFiberEx(64 * 1024, 1024 * 1024, FIBER_FLAG_FLOAT_SWITCH, co_main, c);
        if (!c->fiber) {
            fprintf(stderr, "[PROC] CreateFiberEx failed (%lu), %ld fibers\n", GetLastError(), g_fibers);
            abort();
        }
        if (InterlockedIncrement(&g_fibers) % 64 == 0)
            fprintf(stderr, "[PROC] %ld process fibers\n", g_fibers);
    }
    c->next = NULL;
    c->state = CO_RUNNING;
    c->from_jump = 0;
    return c;
}

static void ensure_thread_fiber(void)
{
    if (t_thread_fiber) return;
    if (IsThreadAFiber()) t_thread_fiber = GetCurrentFiber();
    else t_thread_fiber = ConvertThreadToFiberEx(NULL, FIBER_FLAG_FLOAT_SWITCH);
}

/* vtable +0x10: run the current process until it sleeps, returns or exits. */
static void proc_run(int kind)
{
    uint32_t ret, proc, sched_esp, sched_ebp;
    uint32_t ebx = g_ebx, esi = g_esi, edi = g_edi;
    Co *c, *sleeper, *outer = t_cur;   /* set when a process runs a scheduler */
    void *self;

    POP32(g_esp, ret);
    SCHED_RET = ret;
    SCHED_ESP = sched_esp = g_esp;
    SCHED_EBP = sched_ebp = g_ebp;
    ensure_thread_fiber();
    self = GetCurrentFiber();
    proc = CUR_PROC;
    sleeper = find_sleeper(proc);

    if (kind != KIND_SIMPLE && MEM32(proc + 0x40) > MEM32(proc + 0x44)) {
        if (!sleeper) {
            warn("saved process without a fiber", proc, MEM32(proc + 0x34));
            g_esp = sched_esp; g_ebp = sched_ebp;
            return;
        }
        unlink_sleeper(sleeper);
        if (kind == KIND_COPY) {
            uint32_t end = MEM32(proc + 0x40), start = MEM32(proc + 0x44);
            uint32_t dst = sched_esp, src = end;
            if (sleeper->sleep_sched_esp != sched_esp)
                warn("copy process resumed at another scheduler esp", sleeper->sleep_sched_esp, sched_esp);
            g_esp = sched_esp - (end - start);
            while (src > start) {
                src -= 4; dst -= 4;
                MEM32(dst) = MEM32(src);
            }
        } else {
            g_esp = MEM32(proc + 0x44);
        }
        g_ebp = MEM32(proc + 0x10);
        g_ebx = MEM32(proc + 0x14);
        g_esi = MEM32(proc + 0x18);
        g_edi = MEM32(proc + 0x1C);
        g_eax = proc;
        c = sleeper;
        c->state = CO_RUNNING;
    } else {
        if (sleeper) {                          /* the old body was abandoned */
            unlink_sleeper(sleeper);
            make_zombie(sleeper);
        }
        c = get_free();
        c->proc = proc;
        c->kind = kind;
    }
    c->ret_fiber = self;
    SwitchToFiber(c->fiber);
    t_cur = outer;
    if (t_ljmp_pending) {
        if (outer) {                         /* still inside a process: keep unwinding fibers */
            t_cur = NULL;
            make_zombie(outer);
            SwitchToFiber(outer->ret_fiber);
        }
        t_ljmp_pending = 0;
        reap_zombies();
        if (!recomp_guest_longjmp(t_ljmp_buf, t_ljmp_val))
            warn("longjmp buffer unknown on the thread fiber", t_ljmp_buf, t_ljmp_val);
    }
    reap_zombies();
    g_esp = sched_esp;
    g_ebp = sched_ebp;
    g_ebx = ebx; g_esi = esi; g_edi = edi;
}

/* vtable +0x14: sleep for [0x7350C4] and come back here on the next run. */
static void proc_sleep(int kind)
{
    uint32_t proc = CUR_PROC, ret;
    Co *c = t_cur;

    if (!c || c->proc != proc) {
        warn("sleep outside its process fiber", proc, MEM32(g_esp));
        return;
    }
    MEM32(proc + 0x10) = g_ebp;
    MEM32(proc + 0x14) = g_ebx;
    MEM32(proc + 0x18) = g_esi;
    MEM32(proc + 0x1C) = g_edi;
    MEM32(proc + 0xC) = SLEEP_SET;
    POP32(g_esp, ret);
    MEM32(proc + 0x34) = ret;
    if (kind == KIND_COPY) {
        uint32_t src = SCHED_ESP, dst = MEM32(proc + 0x40);
        while (src > g_esp) {
            src -= 4; dst -= 4;
            MEM32(dst) = MEM32(src);
        }
        MEM32(proc + 0x44) = dst;
    } else {
        MEM32(proc + 0x44) = g_esp;
    }
    if (MEM32(proc + 0x2C)) {
        g_ecx = proc;
        call_guest(MEM32(proc + 0x2C), kind == KIND_COPY ? 0x001C53F2u : 0x001C55EBu);
    }
    c->sleep_sched_esp = SCHED_ESP;
    c->state = CO_SLEEPING;
    c->next = t_sleepers;
    t_sleepers = c;
    t_cur = NULL;
    g_esp = SCHED_ESP;
    g_ebp = SCHED_EBP;
    SwitchToFiber(c->ret_fiber);
    /* resumed by proc_run, guest registers restored */
    t_cur = c;
    reap_zombies();
    if (MEM32(proc + 0x34) != ret)
        warn("resume address changed while asleep", ret, MEM32(proc + 0x34));
}

/* vtable +0x20: restart the current process at a new function. The original
 * returns through the run loop's own return address. */
static void proc_jump(uint32_t loop_ret_slot, uint32_t new_esp)
{
    uint32_t proc = CUR_PROC;
    uint32_t func = MEM32(g_esp + 4), sleep = MEM32(g_esp + 8);
    uint32_t loop_ret = MEM32(loop_ret_slot);
    Co *c = t_cur, *n;
    int kind;

    MEM32(proc + 0x30) = func;
    MEM32(0x005B8B38u) = sleep;
    if (loop_ret == RET_SIMPLE) kind = KIND_SIMPLE;
    else if (loop_ret == RET_COPY) kind = KIND_COPY;
    else if (loop_ret == RET_STACK) kind = KIND_STACK;
    else kind = -1;
    if (!c || kind < 0) {
        warn("jump outside a run loop", proc, loop_ret);
        g_eax = 0;
        return;
    }
    g_ebp = SCHED_EBP;
    n = get_free();
    n->proc = proc;
    n->kind = kind;
    n->from_jump = 1;
    n->jump_sleep = sleep;
    n->jump_esp = new_esp;
    n->ret_fiber = c->ret_fiber;
    make_zombie(c);
    t_cur = NULL;
    SwitchToFiber(n->fiber);
    warn("abandoned process fiber resumed", proc, func);
}

/* Process exit (sub_001C5010, called directly by process code). */
void sub_001C5010(void)
{
    uint32_t proc = CUR_PROC;
    Co *c = t_cur;
    int kill = 1;

    if (MEM32(proc + 0x2C)) {
        g_ecx = proc;
        call_guest(MEM32(proc + 0x2C), 0x001C5024u);
        if (MEM32(CUR_PROC + 4) == 0) kill = 0;
    }
    g_esp = SCHED_ESP;
    g_ebp = SCHED_EBP;
    if (kill) kill_current(0x001C5085u);
    if (!c) {
        warn("exit outside a process fiber", proc, 0);
        return;
    }
    make_zombie(c);
    t_cur = NULL;
    g_esp = SCHED_ESP;
    g_ebp = SCHED_EBP;
    SwitchToFiber(c->ret_fiber);
    warn("exited process fiber resumed", proc, 0);
}

void sub_001C50A0(void) { proc_run(KIND_SIMPLE); }
void sub_001C51C0(void) { proc_run(KIND_COPY); }
void sub_001C5470(void) { proc_run(KIND_STACK); }
void sub_001C5340(void) { proc_sleep(KIND_COPY); }
void sub_001C55A0(void) { proc_sleep(KIND_STACK); }
void sub_001C5180(void) { proc_jump(SCHED_ESP - 4, SCHED_ESP); }
void sub_001C5430(void) { proc_jump(SCHED_ESP - 4, SCHED_ESP); }
void sub_001C5650(void) { uint32_t top = MEM32(CUR_PROC + 0x40); proc_jump(top - 4, top); }

/* vtable +0x18 / +0x1C of own-stack processes: continue on the main stack
 * and come back. The original returns by jumping to the popped address,
 * which the host return does here. */
void sub_001C5610(void)
{
    uint32_t ret;
    POP32(g_esp, ret);
    (void)ret;
    MAIN_SAVE = g_esp;
    g_esp = SCHED_ESP;
}

void sub_001C5630(void)
{
    uint32_t ret;
    POP32(g_esp, ret);
    (void)ret;
    g_esp = MAIN_SAVE;
    MAIN_SAVE = 0;
}


/* 0x19C980: the game's longjmp back to its main loop (setjmp near 0x19C863, taken on the thread's own stack).
 * Called from a process body, which here runs on its own host fiber: the CRT longjmp then unwound the wrong
 * stack (STATUS_BAD_STACK in RtlUnwind, run 20261105-020757, during LOGON.SFD). Abandon the process fibers
 * up to the thread fiber and longjmp there (proc_run). */
void sub_0019C980_gen(void);
void sub_0019C980(void)
{
    if (t_cur) {
        Co *c = t_cur;
        /* sub_0019C980(code) = longjmp(&g_mainloop_jmp 0x5B8848, code); [esp] is the return address. */
        t_ljmp_buf = 0x005B8848u;
        t_ljmp_val = MEM32(g_esp + 4);
        t_ljmp_pending = 1;
        t_cur = NULL;
        make_zombie(c);
        SwitchToFiber(c->ret_fiber);
    }
    sub_0019C980_gen();
}
