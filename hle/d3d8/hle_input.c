#include "hle_game.h"
/*
 * Controller input in C: the XDK XInput/XPP API without the USB stack.
 *
 * The game links XAPI's XPP layer, which drives the console's OHCI USB host
 * controller and an XID driver; the runtime answers that with a register
 * model of the controller (xboxrecomp/src/usb/ohci.c) and an emulated pad on
 * its bus (usb_gamepad.c). With RECOMP_INPUT_NATIVE=1 (launcher default) the
 * functions below replace the API itself -- routed by hle/input_functions.json
 * through hle/apply-hle-wrappers.py -- so XInitDevices never starts the USB
 * driver, main.c skips the OHCI model and no USB interrupt, DPC or timer runs.
 *
 * The pad state still comes from usb_gamepad_report(): host XInput / SDL pad n,
 * the keyboard on pad 0 (RECOMP_KEYBOARD) and the test scripts
 * (RECOMP_PAD_SCRIPT / RECOMP_PAD_LIVE / RECOMP_PAD_PRESS). Its 20-byte report
 * is the XID report the original driver copied: bytes 2..19 are XINPUT_GAMEPAD.
 *
 * Ports: port 0 is always connected (keyboard and scripts drive it, as the
 * one emulated pad did); ports 1-3 follow host pads 1-3, plus every port below
 * RECOMP_USB_PADS. Memory units: none (the MU device type reports nothing, so
 * the game never calls XMountMUA).
 *
 * Device-type objects (g_DeviceType_Gamepad GA_INPUT, _MU GA_INPUT_2) keep the
 * library's layout -- +0 connected, +4 changed, +8 previous -- and the C
 * XGetDevices/XGetDeviceChanges do the same bit arithmetic as the library.
 */
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include "recomp/gen/recomp_types.h"
#include "hle_win32.h"
#if HLE_GAME == HLE_GAME_STREET1

int xbox_no_hardware(void);
#else /* street2 mka */
#endif /* HLE_GAME */

void FN_INPUT_GEN(void);   /* XInitDevices(count, types) */
void FN_INPUT_2_GEN(void);   /* XGetDevices(type) */
void FN_INPUT_3_GEN(void);   /* XGetDeviceChanges(type, ins, rem) */
void FN_INPUT_4_GEN(void);   /* XInputOpen(type, port, slot, poll) */
void FN_INPUT_5_GEN(void);   /* XInputClose(h) */
void FN_INPUT_6_GEN(void);   /* XInputGetCapabilities(h, caps) */
void FN_INPUT_7_GEN(void);   /* XInputGetState(h, state) */
void FN_INPUT_8_GEN(void);   /* XInputSetState(h, feedback) */

/* xboxrecomp/src/usb/usb_gamepad.c and src/input/xinput_device.c */
int usb_gamepad_report(int pad, uint8_t *out, int max);
void xbox_InputInit(void);
BOOL xbox_InputIsConnected(DWORD port);
typedef struct { WORD left, right; } HostVibration;
DWORD xbox_InputSetState(DWORD port, const void *vibration);

#define TYPE_GAMEPAD  GA_INPUT_TYPE_GAMEPAD
#define TYPE_MU       GA_INPUT_TYPE_MU
#define NPORTS        4
#define HANDLE_BASE   0x00C0DE00u        /* opaque; only these functions read it */

#define ERR_SUCCESS            0u
#define ERR_IO_PENDING         997u
#define ERR_NOT_CONNECTED      1167u

typedef struct {
    int open;
    uint32_t packet;
    uint8_t last[18];                    /* XINPUT_GAMEPAD as last reported */
    int have_last;
    WORD rumble_l, rumble_r;
} Port;

static Port s_port[NPORTS];
static int s_native = -1;
static int s_inited;
static int s_min_pads = 1;               /* RECOMP_USB_PADS */
static int s_log;
#if HLE_GAME == HLE_GAME_STREET1
#else /* street2 mka */
static int s_poll_trace = -1;
#endif /* HLE_GAME */
static uint32_t s_host_mask;             /* ports 1-3 with a host pad */
static DWORD s_host_probe_tick;
#if HLE_GAME == HLE_GAME_MKA
/* USB enumeration delay. MK's input init (sub_0026A740) opens the pads that
 * XGetDevices already reports but never sets their "connected" flag (bit 1
 * of 0x831C0C + port*0x20); only the per-frame reader (sub_0026A9C0) does,
 * when XGetDeviceChanges reports an insertion. On the console the hub has not
 * enumerated anything that early, so the pad always arrives as an insertion.
 * Reporting it at once left port 0 opened but "disconnected": every
 * button check (sub_000A10F0) skipped it -- no movie skip, no menu input.
 * RECOMP_INPUT_ENUM_MS (default 500) is how long nothing is attached. */
static DWORD s_enum_tick, s_enum_ms = 500;
static int s_enum_done;
#else /* street1 street2 */
#endif /* HLE_GAME */
static uint64_t s_calls[8];

int hle_input_native_enabled(void)
{
    if (s_native < 0) {
        const char *e = getenv("RECOMP_INPUT_NATIVE");
        s_native = e && *e && *e != '0';
    }
    return s_native;
}

static void input_init(void)
{
    const char *e;
    if (s_inited)
        return;
    s_inited = 1;
    e = getenv("RECOMP_USB_PADS");
    if (e && atoi(e) >= 1 && atoi(e) <= NPORTS)
        s_min_pads = atoi(e);
#if HLE_GAME == HLE_GAME_MKA
    e = getenv("RECOMP_INPUT_ENUM_MS");
    if (e)
        s_enum_ms = (DWORD)atoi(e);
#else /* street1 street2 */
#endif /* HLE_GAME */
    e = getenv("RECOMP_INPUT_LOG");
    s_log = e ? atoi(e) : 0;
    xbox_InputInit();
    fprintf(stderr, "[INPUT-NATIVE] XInput API in C: no USB stack, %d port(s) always connected\n",
            s_min_pads);
}

/* Host pads behind ports 1-3. An empty XInput slot is slow to ask about, so
 * the probe runs at most twice a second; the game asks every frame. */
static uint32_t connected_mask(void)
{
    DWORD now = GetTickCount();
    uint32_t mask = (1u << s_min_pads) - 1u;
    if (!s_host_probe_tick || now - s_host_probe_tick >= 500) {
        uint32_t m = 0;
        int i;
        uint8_t rep[20];
        s_host_probe_tick = now ? now : 1;
        for (i = 1; i < NPORTS; i++) {
            usb_gamepad_report(i, rep, 20);     /* refreshes the connected flag */
            if (xbox_InputIsConnected((DWORD)i))
                m |= 1u << i;
        }
        if (m != s_host_mask)
            fprintf(stderr, "[INPUT-NATIVE] host pads on ports 1-3: mask %X\n", m);
        s_host_mask = m;
    }
    return mask | s_host_mask;
}

static uint32_t type_mask(uint32_t type)
{
#if HLE_GAME == HLE_GAME_MKA
    if (!s_enum_done) {
        if (s_enum_tick && GetTickCount() - s_enum_tick < s_enum_ms)
            return 0;
        s_enum_done = 1;
    }
#else /* street1 street2 */
#endif /* HLE_GAME */
    return type == TYPE_GAMEPAD ? connected_mask() : 0;
}

static int handle_port(uint32_t h)
{
    uint32_t p = h - HANDLE_BASE - 1u;
    return p < NPORTS && s_port[p].open ? (int)p : -1;
}

/* stdcall return: value in eax, pop the return address and the arguments. */
static void ret_stdcall(uint32_t value, int nargs)
{
    g_eax = value;
    g_esp += 4u + 4u * (uint32_t)nargs;
}

int guest_cpu_native_enter(void);
void guest_cpu_native_leave(int token);

#define ARG(i) MEM32(g_esp + 4u + 4u * (i))

#if HLE_GAME == HLE_GAME_STREET1
static void input_0026F733(void)
#elif HLE_GAME == HLE_GAME_MKA
static void input_004CEC4F(void)
#else /* street2 */
static void input_002A1673(void)
#endif /* HLE_GAME */
{
    if (!hle_input_native_enabled()) {
        FN_INPUT_GEN();
        return;
    }
    input_init();
    s_calls[0]++;
#if HLE_GAME == HLE_GAME_MKA
    s_enum_tick = GetTickCount();
    if (!s_enum_tick) s_enum_tick = 1;
    s_enum_done = (s_enum_ms == 0);
#else /* street1 street2 */
#endif /* HLE_GAME */
    {
        uint32_t n = ARG(0), types = ARG(1), i;
        for (i = 0; i < n && types && i < 8; i++)
            fprintf(stderr, "[INPUT-NATIVE] XInitDevices type %08X prealloc %u\n",
                    MEM32(types + 8 * i), MEM32(types + 8 * i + 4));
    }
    /* Nothing is connected until the title looks: XGetDevices then reports
     * the current set, and XGetDeviceChanges reports it as insertions. */
    MEM32(TYPE_GAMEPAD + 0) = 0; MEM32(TYPE_GAMEPAD + 4) = 0; MEM32(TYPE_GAMEPAD + 8) = 0;
    MEM32(TYPE_MU + 0) = 0; MEM32(TYPE_MU + 4) = 0; MEM32(TYPE_MU + 8) = 0;
    ret_stdcall(0, 2);
}

/* The library's +0/+4/+8 bookkeeping, fed by the host instead of the USB
 * hub driver. (+4, "went away and came back between two polls", is never
 * set: the host is sampled at poll time.) */
static void refresh_type(uint32_t type)
{
    MEM32(type) = type_mask(type);
}

#if HLE_GAME == HLE_GAME_STREET1
static void input_002704D9(void)
#elif HLE_GAME == HLE_GAME_MKA
static void input_004CFC41(void)
#else /* street2 */
static void input_002A2419(void)
#endif /* HLE_GAME */
{
    uint32_t type, cur;
    if (!hle_input_native_enabled()) {
        FN_INPUT_2_GEN();
        return;
    }
    input_init();
    s_calls[1]++;
    type = ARG(0);
    refresh_type(type);
    cur = MEM32(type);
    MEM32(type + 4) = 0;
    MEM32(type + 8) = cur;
    ret_stdcall(cur, 1);
}

#if HLE_GAME == HLE_GAME_STREET1
static void input_002704FB(void)
#elif HLE_GAME == HLE_GAME_MKA
static void input_004CFC63(void)
#else /* street2 */
static void input_002A243B(void)
#endif /* HLE_GAME */
{
    uint32_t type, pins, prem, cur, chg, prev, ins, rem, both;
    if (!hle_input_native_enabled()) {
        FN_INPUT_3_GEN();
        return;
    }
    input_init();
    s_calls[2]++;
    type = ARG(0); pins = ARG(1); prem = ARG(2);
    refresh_type(type);
    cur = MEM32(type); chg = MEM32(type + 4); prev = MEM32(type + 8);
    both = chg & prev & cur;
    ins = (cur & ~prev) | both;
    rem = (prev & ~cur) | both;
    MEM32(pins) = ins;
    MEM32(prem) = rem;
    MEM32(type + 4) = 0;
    MEM32(type + 8) = cur;
    if (ins || rem)
        fprintf(stderr, "[INPUT-NATIVE] XGetDeviceChanges %s: inserted %X removed %X\n",
                type == TYPE_GAMEPAD ? "gamepad" : type == TYPE_MU ? "MU" : "?", ins, rem);
    ret_stdcall((ins | rem) ? 1u : 0u, 3);
}

#if HLE_GAME == HLE_GAME_STREET1
static void input_002701D2(void)
#elif HLE_GAME == HLE_GAME_MKA
static void input_004CF8A5(void)
#else /* street2 */
static void input_002A2112(void)
#endif /* HLE_GAME */
{
    uint32_t type, port, slot, h = 0;
    if (!hle_input_native_enabled()) {
        FN_INPUT_4_GEN();
        return;
    }
    input_init();
    s_calls[3]++;
    type = ARG(0); port = ARG(1); slot = ARG(2);
    if (type == TYPE_GAMEPAD && port < NPORTS && slot == 0 && (type_mask(type) >> port & 1u)) {
        Port *p = &s_port[port];
        memset(p, 0, sizeof *p);
        p->open = 1;
        h = HANDLE_BASE + 1u + port;
    }
    fprintf(stderr, "[INPUT-NATIVE] XInputOpen type %08X port %u slot %u -> %08X\n", type, port, slot, h);
    ret_stdcall(h, 4);
}

#if HLE_GAME == HLE_GAME_STREET1
static void input_00270228(void)
#elif HLE_GAME == HLE_GAME_MKA
static void input_004CF8FB(void)
#else /* street2 */
static void input_002A2168(void)
#endif /* HLE_GAME */
{
    int port;
    if (!hle_input_native_enabled()) {
        FN_INPUT_5_GEN();
        return;
    }
    s_calls[4]++;
    port = handle_port(ARG(0));
    if (port >= 0) {
        HostVibration v = { 0, 0 };
        if (s_port[port].rumble_l || s_port[port].rumble_r)
            xbox_InputSetState((DWORD)port, &v);
        s_port[port].open = 0;
        fprintf(stderr, "[INPUT-NATIVE] XInputClose port %d\n", port);
    }
    ret_stdcall(0, 1);
}

/* XINPUT_CAPABILITIES is packed, 25 bytes: SubType @0, Reserved @1,
 * In.Gamepad @3 (18 bytes), Out.Rumble @21 (library GA_INPUT_3 zeroes 25 bytes,
 * lands the two GET_CAPABILITIES reports at +1 and +0x13, then clears +1).
 * Values: the emulated pad's answers -- every field supported, XID bSubType
 * 2 (gamepad "S"); wButtons' mask is 0x00FF. */
#if HLE_GAME == HLE_GAME_STREET1
static void input_00270234(void)
#elif HLE_GAME == HLE_GAME_MKA
static void input_004CF907(void)
#else /* street2 */
static void input_002A2174(void)
#endif /* HLE_GAME */
{
    uint32_t caps, i;
    int port;
    if (!hle_input_native_enabled()) {
        FN_INPUT_6_GEN();
        return;
    }
    s_calls[5]++;
    port = handle_port(ARG(0));
    caps = ARG(1);
    if (port < 0) {
        ret_stdcall(ERR_NOT_CONNECTED, 2);
        return;
    }
    for (i = 0; i < 25; i++)
        MEM8(caps + i) = 0;
    MEM8(caps + 0) = 2;
    MEM8(caps + 3) = 0xFF;
    for (i = 5; i < 25; i++)
        MEM8(caps + i) = 0xFF;
    ret_stdcall(ERR_SUCCESS, 2);
}

/* XINPUT_STATE: dwPacketNumber, then XINPUT_GAMEPAD = report bytes 2..19.
 * The packet number moves only when the state does, as on the console. */
/* The F1 menu lives in another module and publishes its vibration option as a process variable. The game may
 * not call again while a motor runs, so a change also stops or restarts the motors. */
static int s_vibration_off;

static void vibration_refresh(void)
{
    static unsigned polls;
    int off, port;
    if (polls++ & 31)
        return;
#ifdef _WIN32
    {
        char v[8] = "";
        GetEnvironmentVariableA("XBOX_VIBRATION_OFF", v, sizeof v);
        off = v[0] == '1';
    }
#else
    {
        const char *e = getenv("XBOX_VIBRATION_OFF");
        off = e && e[0] == '1';
    }
#endif
    if (off == s_vibration_off)
        return;
    s_vibration_off = off;
    for (port = 0; port < NPORTS; port++)
        if (s_port[port].open && (s_port[port].rumble_l || s_port[port].rumble_r)) {
            HostVibration v;
            v.left = off ? 0 : s_port[port].rumble_l;
            v.right = off ? 0 : s_port[port].rumble_r;
            xbox_InputSetState((DWORD)port, &v);
        }
}

#if HLE_GAME == HLE_GAME_STREET1
static void input_0027040C(void)
#elif HLE_GAME == HLE_GAME_MKA
static void input_004CFADF(void)
#else /* street2 */
static void input_002A234C(void)
#endif /* HLE_GAME */
{
    vibration_refresh();
    uint8_t rep[20];
    uint32_t st, i;
    int port;
    Port *p;
    if (!hle_input_native_enabled()) {
        FN_INPUT_7_GEN();
        return;
    }
    s_calls[6]++;
#if HLE_GAME == HLE_GAME_STREET1
#else /* street2 mka */
    if (s_poll_trace < 0)
        s_poll_trace = getenv("RECOMP_INPUT_POLL_TRACE") != NULL;
    if (s_poll_trace)
        fprintf(stderr, "[INPUT-NATIVE] XInputGetState poll %llu\n",
                (unsigned long long)s_calls[6]);
#endif /* HLE_GAME */
    port = handle_port(ARG(0));
    st = ARG(1);
    if (port < 0 || !(connected_mask() >> port & 1u)) {
        ret_stdcall(ERR_NOT_CONNECTED, 2);
        return;
    }
    p = &s_port[port];
#if HLE_GAME == HLE_GAME_STREET1
    /* Nothing should touch the USB controllers' registers now (plain memory
     * without the OHCI model): check once in a while and say so if it does.
     * In a hardware-free run the aperture is not mapped at all and the
     * access guard reports any touch, so reading it here would be one. */
    if (s_calls[6] % 3600 == 1 && !xbox_no_hardware()) {
#else /* street2 mka */
    /* Nothing should touch the USB controllers' registers now (plain memory
     * without the OHCI model): check once in a while and say so if it does. */
    if (s_calls[6] % 3600 == 1) {
#endif /* HLE_GAME */
        static int reported;
        uint32_t off, bad = 0;
        for (off = 0; off < 0x1000 && !bad; off += 4)
            if (MEM32(0xFED00000u + off) || MEM32(0xFED08000u + off))
                bad = 0xFED00000u + off;
        if (bad && !reported++)
            fprintf(stderr, "[INPUT-NATIVE] WARNING: OHCI registers written (%08X) - USB stack running?\n", bad);
    }
    usb_gamepad_report(port, rep, 20);
    if (!p->have_last || memcmp(p->last, rep + 2, 18)) {
        memcpy(p->last, rep + 2, 18);
        p->have_last = 1;
        p->packet++;
        if (s_log > 0) {
            s_log--;
            fprintf(stderr, "[INPUT-NATIVE] port %d packet %u buttons %02X analog %02X %02X %02X %02X"
                    " LX %d LY %d\n", port, p->packet, rep[2], rep[4], rep[5], rep[6], rep[7],
                    (int16_t)(rep[12] | rep[13] << 8), (int16_t)(rep[14] | rep[15] << 8));
        }
    }
    MEM32(st) = p->packet;
    for (i = 0; i < 18; i++)
        MEM8(st + 4 + i) = p->last[i];
    ret_stdcall(ERR_SUCCESS, 2);
}

/* XINPUT_FEEDBACK: header { dwStatus, hEvent, reserved[58] }, rumble @66.
 * Completes at once: the title waits for dwStatus to leave IO_PENDING. */
#if HLE_GAME == HLE_GAME_STREET1
static void input_0027047F(void)
#elif HLE_GAME == HLE_GAME_MKA
static void input_004CFB52(void)
#else /* street2 */
static void input_002A23BF(void)
#endif /* HLE_GAME */
{
    uint32_t fb, ev;
    int port;
    if (!hle_input_native_enabled()) {
        FN_INPUT_8_GEN();
        return;
    }
    s_calls[7]++;
    port = handle_port(ARG(0));
    fb = ARG(1);
    if (port < 0) {
        ret_stdcall(ERR_NOT_CONNECTED, 2);
        return;
    }
    {
        Port *p = &s_port[port];
        HostVibration v;
        v.left = MEM16(fb + 66);
        v.right = MEM16(fb + 68);
        if (v.left != p->rumble_l || v.right != p->rumble_r) {
            p->rumble_l = v.left;
            p->rumble_r = v.right;
            if (!s_vibration_off)
                xbox_InputSetState((DWORD)port, &v);
        }
    }
    ev = MEM32(fb + 4);
    if (ev) {
        static int warned;
        if (!warned++)
            fprintf(stderr, "[INPUT-NATIVE] XInputSetState with a completion event %08X (not signalled)\n", ev);
    }
    MEM32(fb) = ERR_SUCCESS;
    ret_stdcall(ERR_IO_PENDING, 2);
}

/* Entry points: under RECOMP_GUEST_SMP the API runs under the guest kernel lock. */
#if HLE_GAME == HLE_GAME_STREET1
void sub_0026F733(void) { int s = guest_cpu_native_enter(); input_0026F733(); guest_cpu_native_leave(s); }
void sub_002704D9(void) { int s = guest_cpu_native_enter(); input_002704D9(); guest_cpu_native_leave(s); }
void sub_002704FB(void) { int s = guest_cpu_native_enter(); input_002704FB(); guest_cpu_native_leave(s); }
void sub_002701D2(void) { int s = guest_cpu_native_enter(); input_002701D2(); guest_cpu_native_leave(s); }
void sub_00270228(void) { int s = guest_cpu_native_enter(); input_00270228(); guest_cpu_native_leave(s); }
void sub_00270234(void) { int s = guest_cpu_native_enter(); input_00270234(); guest_cpu_native_leave(s); }
void sub_0027040C(void) { int s = guest_cpu_native_enter(); input_0027040C(); guest_cpu_native_leave(s); }
void sub_0027047F(void) { int s = guest_cpu_native_enter(); input_0027047F(); guest_cpu_native_leave(s); }
#elif HLE_GAME == HLE_GAME_MKA
void sub_004CEC4F(void) { int s = guest_cpu_native_enter(); input_004CEC4F(); guest_cpu_native_leave(s); }
void sub_004CFC41(void) { int s = guest_cpu_native_enter(); input_004CFC41(); guest_cpu_native_leave(s); }
void sub_004CFC63(void) { int s = guest_cpu_native_enter(); input_004CFC63(); guest_cpu_native_leave(s); }
void sub_004CF8A5(void) { int s = guest_cpu_native_enter(); input_004CF8A5(); guest_cpu_native_leave(s); }
void sub_004CF8FB(void) { int s = guest_cpu_native_enter(); input_004CF8FB(); guest_cpu_native_leave(s); }
void sub_004CF907(void) { int s = guest_cpu_native_enter(); input_004CF907(); guest_cpu_native_leave(s); }
void sub_004CFADF(void) { int s = guest_cpu_native_enter(); input_004CFADF(); guest_cpu_native_leave(s); }
void sub_004CFB52(void) { int s = guest_cpu_native_enter(); input_004CFB52(); guest_cpu_native_leave(s); }
#else /* street2 */
void sub_002A1673(void) { int s = guest_cpu_native_enter(); input_002A1673(); guest_cpu_native_leave(s); }
void sub_002A2419(void) { int s = guest_cpu_native_enter(); input_002A2419(); guest_cpu_native_leave(s); }
void sub_002A243B(void) { int s = guest_cpu_native_enter(); input_002A243B(); guest_cpu_native_leave(s); }
void sub_002A2112(void) { int s = guest_cpu_native_enter(); input_002A2112(); guest_cpu_native_leave(s); }
void sub_002A2168(void) { int s = guest_cpu_native_enter(); input_002A2168(); guest_cpu_native_leave(s); }
void sub_002A2174(void) { int s = guest_cpu_native_enter(); input_002A2174(); guest_cpu_native_leave(s); }
void sub_002A234C(void) { int s = guest_cpu_native_enter(); input_002A234C(); guest_cpu_native_leave(s); }
void sub_002A23BF(void) { int s = guest_cpu_native_enter(); input_002A23BF(); guest_cpu_native_leave(s); }
#endif /* HLE_GAME */
