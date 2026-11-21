/**
 * Xbox Input Compatibility Layer
 *
 * Translates the Xbox controller API to a host gamepad backend.
 * Handles the structural differences between the Xbox gamepad
 * (analog buttons as bytes, separate trigger channels) and the host
 * (digital face buttons, trigger axes).
 *
 *   _WIN32 -> Windows XInput, and SDL3.dll (when present next to the
 *             executable) for what XInput cannot see: DualShock 3/4,
 *             DualSense, USB adapters for PS2 and original Xbox pads, and
 *             other generic controllers
 *   POSIX  -> SDL2 GameController
 */

#include "xinput_xbox.h"
#include <stdio.h>
#include "keyboard.h"
#include "mkb_input.h"
#include <stdlib.h>
#include <string.h>

/* Merge the keyboard on top of a pad on port 0 rather than only standing in
 * for a missing one.
 *
 * The first version put the keyboard behind XInput's failure, on the
 * assumption that with nothing plugged in the call would fail. It does
 * not: under Wine XInputGetState returns ERROR_SUCCESS and a gamepad
 * with every button at rest, so the fallback was unreachable and
 * pressing a key did nothing at all.
 *
 * Merging is also the better rule. A real pad keeps working -- its
 * buttons are already in pState and the keyboard only adds to them --
 * and there is no special case left to get wrong. */
static void keyboard_merge(XBOX_INPUT_STATE *pState)
{
    XBOX_INPUT_STATE kb;
    int i;

    if (!xbox_InputKeyboardPoll(&kb))
        return;
    pState->Gamepad.wButtons |= kb.Gamepad.wButtons;
    for (i = 0; i < 8; i++)
        if (kb.Gamepad.bAnalogButtons[i] > pState->Gamepad.bAnalogButtons[i])
            pState->Gamepad.bAnalogButtons[i] = kb.Gamepad.bAnalogButtons[i];
    if (kb.Gamepad.sThumbLX) pState->Gamepad.sThumbLX = kb.Gamepad.sThumbLX;
    if (kb.Gamepad.sThumbLY) pState->Gamepad.sThumbLY = kb.Gamepad.sThumbLY;
    if (kb.Gamepad.sThumbRX) pState->Gamepad.sThumbRX = kb.Gamepad.sThumbRX;
    if (kb.Gamepad.sThumbRY) pState->Gamepad.sThumbRY = kb.Gamepad.sThumbRY;
    /* The input layer records edges, so an unchanged packet number is
     * read as the same state and the press never happens. */
    pState->dwPacketNumber = kb.dwPacketNumber;
}

/* ======================================================================== */
#if defined(_WIN32)
/* ====================  XInput backend  ================================== */
/* ======================================================================== */

#include <xinput.h>
#pragma comment(lib, "xinput.lib")

/* The keyboard on port 0 (RECOMP_KEYBOARD=1) comes from keyboard.c, shared
 * with the SDL backend; the GDI window in fb_present.c records the keys. */

static BOOL  g_controller_connected[XBOX_MAX_CONTROLLERS] = { FALSE };
static BOOL  g_sdl_port_connected[XBOX_MAX_CONTROLLERS] = { FALSE };
static DWORD g_last_packet[XBOX_MAX_CONTROLLERS] = { 0 };

/* ---- SDL3, for every pad that is not an XInput device ---------------------
 *
 * Loaded at run time: without SDL3.dll next to the executable nothing changes.
 * An XInput pad keeps its slot and the XInput path; SDL pads fill the ports
 * XInput leaves empty, in the order they were connected. SDL also lists XInput
 * pads, so as many Xbox-type pads as XInput reports are skipped; an Xbox pad
 * XInput does not see (some USB adapters for the original pad) still counts.
 * SDL's own numbers are used directly so that no header is needed to build.
 */
#define SDLPAD_MAX 8
typedef struct SDLPad SDLPad;
static struct {
    unsigned char (*Init)(DWORD);
    unsigned char (*SetHint)(const char *, const char *);
    int (*AddMappings)(const char *);
    unsigned int *(*GetGamepads)(int *);
    SDLPad *(*Open)(unsigned int);
    void (*Close)(SDLPad *);
    unsigned char (*Connected)(SDLPad *);
    int (*GetType)(SDLPad *);
    unsigned int (*GetId)(SDLPad *);
    const char *(*GetName)(SDLPad *);
    void (*Update)(void);
    unsigned char (*Button)(SDLPad *, int);
    short (*Axis)(SDLPad *, int);
    unsigned char (*Rumble)(SDLPad *, unsigned short, unsigned short, unsigned int);
    void (*Free)(void *);
} sdl;
static int sdl_state;                       /* 0 not tried, 1 running, -1 absent */
static SDLPad *sdl_pads[SDLPAD_MAX];
static int sdl_xbox_type[SDLPAD_MAX];
static DWORD sdl_scan_tick, sdl_update_tick;
static CRITICAL_SECTION sdl_lock;

static void sdl_start(void)
{
    wchar_t path[MAX_PATH];
    char dir[MAX_PATH * 3], file[MAX_PATH * 3 + 32];
    HMODULE m;
    DWORD n;
    wchar_t *slash;

    if (sdl_state)
        return;
    sdl_state = -1;
    n = GetModuleFileNameW(NULL, path, MAX_PATH);
    if (!n || n >= MAX_PATH || !(slash = wcsrchr(path, L'\\')))
        return;
    slash[1] = 0;
    if (wcslen(path) + 9 >= MAX_PATH)
        return;
    wcscat(path, L"SDL3.dll");
    m = LoadLibraryExW(path, NULL, LOAD_WITH_ALTERED_SEARCH_PATH);
    if (!m)
        return;
#define SDLF(field, name) if (!(*(FARPROC *)&sdl.field = GetProcAddress(m, name))) return
    SDLF(Init, "SDL_Init");
    SDLF(SetHint, "SDL_SetHint");
    SDLF(AddMappings, "SDL_AddGamepadMappingsFromFile");
    SDLF(GetGamepads, "SDL_GetGamepads");
    SDLF(Open, "SDL_OpenGamepad");
    SDLF(Close, "SDL_CloseGamepad");
    SDLF(Connected, "SDL_GamepadConnected");
    SDLF(GetType, "SDL_GetGamepadType");
    SDLF(GetId, "SDL_GetGamepadID");
    SDLF(GetName, "SDL_GetGamepadName");
    SDLF(Update, "SDL_UpdateGamepads");
    SDLF(Button, "SDL_GetGamepadButton");
    SDLF(Axis, "SDL_GetGamepadAxis");
    SDLF(Rumble, "SDL_RumbleGamepad");
    SDLF(Free, "SDL_free");
#undef SDLF
    /* There is no SDL window: pads must work anyway, on SDL's own thread. */
    sdl.SetHint("SDL_JOYSTICK_ALLOW_BACKGROUND_EVENTS", "1");
    sdl.SetHint("SDL_JOYSTICK_THREAD", "1");
    if (!sdl.Init(0x2000u))                 /* SDL_INIT_GAMEPAD */
        return;
    n = WideCharToMultiByte(CP_UTF8, 0, path, -1, dir, sizeof dir, NULL, NULL);
    if (n) {
        char *end = strrchr(dir, '\\');
        if (end) {
            end[1] = 0;
            snprintf(file, sizeof file, "%sgamecontrollerdb.txt", dir);
            sdl.AddMappings(file);
        }
    }
    InitializeCriticalSection(&sdl_lock);
    sdl_state = 1;
}

/* Open newly connected pads and drop the ones that left; twice a second. */
static void sdl_scan(void)
{
    int n = 0, i, j;
    unsigned int *ids;

    for (i = 0; i < SDLPAD_MAX; i++)
        if (sdl_pads[i] && !sdl.Connected(sdl_pads[i])) {
            sdl.Close(sdl_pads[i]);
            sdl_pads[i] = NULL;
        }
    ids = sdl.GetGamepads(&n);
    for (i = 0; ids && i < n; i++) {
        int known = 0, slot = -1;
        for (j = 0; j < SDLPAD_MAX; j++) {
            if (sdl_pads[j] && sdl.GetId(sdl_pads[j]) == ids[i])
                known = 1;
            if (!sdl_pads[j] && slot < 0)
                slot = j;
        }
        if (!known && slot >= 0 && (sdl_pads[slot] = sdl.Open(ids[i])) != NULL) {
            int type = sdl.GetType(sdl_pads[slot]);
            const char *name = sdl.GetName(sdl_pads[slot]);
            sdl_xbox_type[slot] = type == 2 || type == 3;   /* XBOX360, XBOXONE */
            fprintf(stderr, "[INPUT] SDL pad %d: %s (type %d)\n", slot, name ? name : "?", type);
        }
    }
    if (ids)
        sdl.Free(ids);
}

/* The SDL pad behind host port `port`, whose XInput slot is empty. */
static SDLPad *sdl_pad_for(DWORD port)
{
    int xinput_pads = 0, skip_ports = 0, i;
    DWORD now = GetTickCount();

    if (sdl_state != 1)
        return NULL;
    if (!sdl_scan_tick || now - sdl_scan_tick >= 500) {
        sdl_scan_tick = now ? now : 1;
        sdl_scan();
    }
    for (i = 0; i < XBOX_MAX_CONTROLLERS; i++) {
        if (g_controller_connected[i])
            xinput_pads++;
        else if ((DWORD)i < port)
            skip_ports++;
    }
    for (i = 0; i < SDLPAD_MAX; i++) {
        if (!sdl_pads[i])
            continue;
        if (sdl_xbox_type[i] && xinput_pads > 0) {
            xinput_pads--;
            continue;
        }
        if (skip_ports-- == 0)
            return sdl_pads[i];
    }
    return NULL;
}

static DWORD sdl_get_state(DWORD port, XINPUT_STATE *out)
{
    static XINPUT_GAMEPAD last[XBOX_MAX_CONTROLLERS];
    static DWORD packet[XBOX_MAX_CONTROLLERS];
    static const struct { int button; WORD bit; } map[] = {
        { 0, XINPUT_GAMEPAD_A }, { 1, XINPUT_GAMEPAD_B }, { 2, XINPUT_GAMEPAD_X }, { 3, XINPUT_GAMEPAD_Y },
        { 4, XINPUT_GAMEPAD_BACK }, { 6, XINPUT_GAMEPAD_START }, { 7, XINPUT_GAMEPAD_LEFT_THUMB },
        { 8, XINPUT_GAMEPAD_RIGHT_THUMB }, { 9, XINPUT_GAMEPAD_LEFT_SHOULDER },
        { 10, XINPUT_GAMEPAD_RIGHT_SHOULDER }, { 11, XINPUT_GAMEPAD_DPAD_UP },
        { 12, XINPUT_GAMEPAD_DPAD_DOWN }, { 13, XINPUT_GAMEPAD_DPAD_LEFT }, { 14, XINPUT_GAMEPAD_DPAD_RIGHT },
    };
    SDLPad *pad;
    DWORD now, result = ERROR_DEVICE_NOT_CONNECTED;
    int i;

    if (sdl_state != 1)
        return result;
    EnterCriticalSection(&sdl_lock);
    now = GetTickCount();
    pad = sdl_pad_for(port);
    if (pad) {
        XINPUT_GAMEPAD g;
        memset(&g, 0, sizeof g);
        if (now != sdl_update_tick) {
            sdl_update_tick = now;
            sdl.Update();
        }
        for (i = 0; i < (int)(sizeof map / sizeof map[0]); i++)
            if (sdl.Button(pad, map[i].button))
                g.wButtons |= map[i].bit;
        g.bLeftTrigger = (BYTE)(sdl.Axis(pad, 4) >> 7);
        g.bRightTrigger = (BYTE)(sdl.Axis(pad, 5) >> 7);
        g.sThumbLX = sdl.Axis(pad, 0);
        g.sThumbLY = (SHORT)(-1 - sdl.Axis(pad, 1));   /* SDL's Y axis points down */
        g.sThumbRX = sdl.Axis(pad, 2);
        g.sThumbRY = (SHORT)(-1 - sdl.Axis(pad, 3));
        if (memcmp(&g, &last[port], sizeof g)) {
            last[port] = g;
            packet[port]++;
        }
        out->dwPacketNumber = packet[port];
        out->Gamepad = g;
        result = ERROR_SUCCESS;
    }
    LeaveCriticalSection(&sdl_lock);
    return result;
}

/* A host pad as XInput would report it, whichever API it came through. */
DWORD xbox_InputGetRaw(DWORD dwPort, XINPUT_STATE *out)
{
    DWORD result = XInputGetState(dwPort, out);
    if (dwPort >= XBOX_MAX_CONTROLLERS)
        return result;
    g_controller_connected[dwPort] = result == ERROR_SUCCESS;
    if (result != ERROR_SUCCESS)
        result = sdl_get_state(dwPort, out);
    g_sdl_port_connected[dwPort] = !g_controller_connected[dwPort] && result == ERROR_SUCCESS;
    return result;
}

void xbox_InputInit(void)
{
    for (DWORD i = 0; i < XBOX_MAX_CONTROLLERS; i++) {
        XINPUT_STATE state;
        DWORD result = XInputGetState(i, &state);
        g_controller_connected[i] = (result == ERROR_SUCCESS);
    }
    sdl_start();
}

DWORD xbox_InputGetState(DWORD dwPort, XBOX_INPUT_STATE *pState)
{
    XINPUT_STATE xi_state;
    DWORD result;

    if (dwPort >= XBOX_MAX_CONTROLLERS || !pState)
        return ERROR_DEVICE_NOT_CONNECTED;

    result = xbox_InputGetRaw(dwPort, &xi_state);
    if (result != ERROR_SUCCESS) {
        g_controller_connected[dwPort] = FALSE;
        if (dwPort == 0) {
            /* Keyboard and mouse stand in for an absent pad, so the game is
             * playable with no controller plugged in. */
            int kb;
            memset(pState, 0, sizeof *pState);
            kb = xbox_InputKeyboardPoll(pState);
            if (mkb_merge(dwPort, pState))
                g_controller_connected[dwPort] = TRUE;
            if (kb || mkb_enabled())
                return ERROR_SUCCESS;
        }
        return result;
    }

    g_last_packet[dwPort] = xi_state.dwPacketNumber;

    memset(pState, 0, sizeof(XBOX_INPUT_STATE));
    pState->dwPacketNumber = xi_state.dwPacketNumber;
    pState->Gamepad.wButtons = xi_state.Gamepad.wButtons & 0x00FF;

    pState->Gamepad.bAnalogButtons[XBOX_BUTTON_A] =
        (xi_state.Gamepad.wButtons & XINPUT_GAMEPAD_A) ? 255 : 0;
    pState->Gamepad.bAnalogButtons[XBOX_BUTTON_B] =
        (xi_state.Gamepad.wButtons & XINPUT_GAMEPAD_B) ? 255 : 0;
    pState->Gamepad.bAnalogButtons[XBOX_BUTTON_X] =
        (xi_state.Gamepad.wButtons & XINPUT_GAMEPAD_X) ? 255 : 0;
    pState->Gamepad.bAnalogButtons[XBOX_BUTTON_Y] =
        (xi_state.Gamepad.wButtons & XINPUT_GAMEPAD_Y) ? 255 : 0;
    pState->Gamepad.bAnalogButtons[XBOX_BUTTON_BLACK] =
        (xi_state.Gamepad.wButtons & XINPUT_GAMEPAD_LEFT_SHOULDER) ? 255 : 0;
    pState->Gamepad.bAnalogButtons[XBOX_BUTTON_WHITE] =
        (xi_state.Gamepad.wButtons & XINPUT_GAMEPAD_RIGHT_SHOULDER) ? 255 : 0;
    pState->Gamepad.bAnalogButtons[XBOX_BUTTON_LTRIGGER] = xi_state.Gamepad.bLeftTrigger;
    pState->Gamepad.bAnalogButtons[XBOX_BUTTON_RTRIGGER] = xi_state.Gamepad.bRightTrigger;

    pState->Gamepad.sThumbLX = xi_state.Gamepad.sThumbLX;
    pState->Gamepad.sThumbLY = xi_state.Gamepad.sThumbLY;
    pState->Gamepad.sThumbRX = xi_state.Gamepad.sThumbRX;
    pState->Gamepad.sThumbRY = xi_state.Gamepad.sThumbRY;

    if (dwPort == 0)
        keyboard_merge(pState);

    /* Mouse+keyboard merges on top of a real pad too: the mouse owns the
     * aim stick while it is moving, WASD owns movement while held. */
    if (dwPort == 0)
        mkb_merge(dwPort, pState);

    return ERROR_SUCCESS;
}

DWORD xbox_InputSetState(DWORD dwPort, const XBOX_VIBRATION *pVibration)
{
    XINPUT_VIBRATION xi_vib;

    if (dwPort >= XBOX_MAX_CONTROLLERS || !pVibration)
        return ERROR_DEVICE_NOT_CONNECTED;

    xi_vib.wLeftMotorSpeed = pVibration->wLeftMotorSpeed;
    xi_vib.wRightMotorSpeed = pVibration->wRightMotorSpeed;
    if (!g_controller_connected[dwPort] && sdl_state == 1) {
        SDLPad *pad;
        DWORD result = ERROR_DEVICE_NOT_CONNECTED;
        EnterCriticalSection(&sdl_lock);
        pad = sdl_pad_for(dwPort);
        if (pad && sdl.Rumble(pad, xi_vib.wLeftMotorSpeed, xi_vib.wRightMotorSpeed, 1000))
            result = ERROR_SUCCESS;
        LeaveCriticalSection(&sdl_lock);
        return result;
    }
    return XInputSetState(dwPort, &xi_vib);
}

BOOL xbox_InputIsConnected(DWORD dwPort)
{
    if (dwPort >= XBOX_MAX_CONTROLLERS) return FALSE;
    return g_controller_connected[dwPort] || g_sdl_port_connected[dwPort];
}

DWORD xbox_InputGetCapabilities(DWORD dwPort, DWORD dwFlags, XBOX_INPUT_CAPABILITIES *pCaps)
{
    XINPUT_CAPABILITIES xi_caps;
    DWORD result;

    if (dwPort >= XBOX_MAX_CONTROLLERS || !pCaps)
        return ERROR_DEVICE_NOT_CONNECTED;

    result = XInputGetCapabilities(dwPort, dwFlags, &xi_caps);
    if (result != ERROR_SUCCESS) {
        if (!g_sdl_port_connected[dwPort]) return result;
        memset(pCaps, 0, sizeof(XBOX_INPUT_CAPABILITIES));
        pCaps->Type = XINPUT_DEVTYPE_GAMEPAD;
        pCaps->SubType = XINPUT_DEVSUBTYPE_GAMEPAD;
        return ERROR_SUCCESS;
    }

    memset(pCaps, 0, sizeof(XBOX_INPUT_CAPABILITIES));
    pCaps->Type = xi_caps.Type;
    pCaps->SubType = xi_caps.SubType;
    pCaps->Flags = xi_caps.Flags;
    return ERROR_SUCCESS;
}

/* ======================================================================== */
#else /* !_WIN32 */
/* ====================  SDL2 GameController backend  ===================== */
/* ======================================================================== */

#include <SDL.h>

static SDL_GameController *g_pads[XBOX_MAX_CONTROLLERS];
static BOOL  g_controller_connected[XBOX_MAX_CONTROLLERS];
static DWORD g_packet[XBOX_MAX_CONTROLLERS];

/* Set while the framebuffer window's main thread pumps SDL events
 * (src/video/fb_present_sdl.c). The pump already updates the controllers,
 * and SDL_GameControllerUpdate pumps events itself, which on macOS must only
 * happen on the main thread; so then this side only reads them. */
static SDL_atomic_t g_main_pump;

void xbox_InputSetMainPump(int on)
{
    SDL_AtomicSet(&g_main_pump, on);
}

static void update_controllers(void)
{
    if (!SDL_AtomicGet(&g_main_pump))
        SDL_GameControllerUpdate();
}

static int controller_is_open_locked(SDL_JoystickID instance_id)
{
    for (int slot = 0; slot < XBOX_MAX_CONTROLLERS; slot++) {
        if (g_pads[slot]) {
            SDL_Joystick *joystick = SDL_GameControllerGetJoystick(g_pads[slot]);
            if (joystick && SDL_JoystickInstanceID(joystick) == instance_id)
                return 1;
        }
    }
    return 0;
}

static void refresh_controllers_locked(void)
{
    int count;

    for (int slot = 0; slot < XBOX_MAX_CONTROLLERS; slot++) {
        if (g_pads[slot] && !SDL_GameControllerGetAttached(g_pads[slot])) {
            SDL_GameControllerClose(g_pads[slot]);
            g_pads[slot] = NULL;
            g_controller_connected[slot] = FALSE;
        }
    }

    count = SDL_NumJoysticks();
    for (int i = 0; i < count; i++) {
        SDL_GameController *controller;
        SDL_JoystickID instance_id;
        int slot;

        if (!SDL_IsGameController(i))
            continue;
        instance_id = SDL_JoystickGetDeviceInstanceID(i);
        if (instance_id < 0 || controller_is_open_locked(instance_id))
            continue;
        for (slot = 0; slot < XBOX_MAX_CONTROLLERS; slot++)
            if (!g_pads[slot])
                break;
        if (slot == XBOX_MAX_CONTROLLERS)
            break;
        controller = SDL_GameControllerOpen(i);
        if (controller) {
            g_pads[slot] = controller;
            g_controller_connected[slot] = TRUE;
        }
    }
}

static SDL_GameController *controller_for_port_locked(DWORD port)
{
    SDL_GameController *controller = g_pads[port];

    if (!controller || !SDL_GameControllerGetAttached(controller)) {
        refresh_controllers_locked();
        controller = g_pads[port];
    }
    g_controller_connected[port] =
        controller && SDL_GameControllerGetAttached(controller);
    return g_controller_connected[port] ? controller : NULL;
}

/* Whether any controller is open: SDL must stay up for it at shutdown. */
int xbox_InputAnyOpen(void)
{
    int any = 0;

    update_controllers();
    SDL_LockJoysticks();
    for (int i = 0; i < XBOX_MAX_CONTROLLERS; i++) {
        if (g_pads[i] && !SDL_GameControllerGetAttached(g_pads[i])) {
            SDL_GameControllerClose(g_pads[i]);
            g_pads[i] = NULL;
            g_controller_connected[i] = FALSE;
        }
        any |= g_pads[i] != NULL;
    }
    SDL_UnlockJoysticks();
    return any;
}

void xbox_InputInit(void)
{
    /* Keep the default SIGINT/SIGTERM: SDL would turn them into an SDL_QUIT
     * that nothing reads without a window, and Ctrl-C would stop working. */
    SDL_SetHint(SDL_HINT_NO_SIGNAL_HANDLERS, "1");
    if (!SDL_WasInit(SDL_INIT_GAMECONTROLLER))
        SDL_InitSubSystem(SDL_INIT_GAMECONTROLLER);
    SDL_LockJoysticks();
    refresh_controllers_locked();
    SDL_UnlockJoysticks();
}

DWORD xbox_InputGetState(DWORD dwPort, XBOX_INPUT_STATE *pState)
{
    if (dwPort >= XBOX_MAX_CONTROLLERS || !pState)
        return ERROR_DEVICE_NOT_CONNECTED;

    update_controllers();
    SDL_LockJoysticks();
    SDL_GameController *c = controller_for_port_locked(dwPort);
    if (!c) {
        SDL_UnlockJoysticks();
        if (dwPort == 0 && xbox_InputKeyboardPoll(pState))
            return ERROR_SUCCESS;
        return ERROR_DEVICE_NOT_CONNECTED;
    }

    memset(pState, 0, sizeof(XBOX_INPUT_STATE));
    pState->dwPacketNumber = ++g_packet[dwPort];

    WORD btn = 0;
    if (SDL_GameControllerGetButton(c, SDL_CONTROLLER_BUTTON_DPAD_UP))    btn |= XBOX_GAMEPAD_DPAD_UP;
    if (SDL_GameControllerGetButton(c, SDL_CONTROLLER_BUTTON_DPAD_DOWN))  btn |= XBOX_GAMEPAD_DPAD_DOWN;
    if (SDL_GameControllerGetButton(c, SDL_CONTROLLER_BUTTON_DPAD_LEFT))  btn |= XBOX_GAMEPAD_DPAD_LEFT;
    if (SDL_GameControllerGetButton(c, SDL_CONTROLLER_BUTTON_DPAD_RIGHT)) btn |= XBOX_GAMEPAD_DPAD_RIGHT;
    if (SDL_GameControllerGetButton(c, SDL_CONTROLLER_BUTTON_START))      btn |= XBOX_GAMEPAD_START;
    if (SDL_GameControllerGetButton(c, SDL_CONTROLLER_BUTTON_BACK))       btn |= XBOX_GAMEPAD_BACK;
    if (SDL_GameControllerGetButton(c, SDL_CONTROLLER_BUTTON_LEFTSTICK))  btn |= XBOX_GAMEPAD_LEFT_THUMB;
    if (SDL_GameControllerGetButton(c, SDL_CONTROLLER_BUTTON_RIGHTSTICK)) btn |= XBOX_GAMEPAD_RIGHT_THUMB;
    pState->Gamepad.wButtons = btn;

    pState->Gamepad.bAnalogButtons[XBOX_BUTTON_A] =
        SDL_GameControllerGetButton(c, SDL_CONTROLLER_BUTTON_A) ? 255 : 0;
    pState->Gamepad.bAnalogButtons[XBOX_BUTTON_B] =
        SDL_GameControllerGetButton(c, SDL_CONTROLLER_BUTTON_B) ? 255 : 0;
    pState->Gamepad.bAnalogButtons[XBOX_BUTTON_X] =
        SDL_GameControllerGetButton(c, SDL_CONTROLLER_BUTTON_X) ? 255 : 0;
    pState->Gamepad.bAnalogButtons[XBOX_BUTTON_Y] =
        SDL_GameControllerGetButton(c, SDL_CONTROLLER_BUTTON_Y) ? 255 : 0;
    pState->Gamepad.bAnalogButtons[XBOX_BUTTON_BLACK] =
        SDL_GameControllerGetButton(c, SDL_CONTROLLER_BUTTON_LEFTSHOULDER) ? 255 : 0;
    pState->Gamepad.bAnalogButtons[XBOX_BUTTON_WHITE] =
        SDL_GameControllerGetButton(c, SDL_CONTROLLER_BUTTON_RIGHTSHOULDER) ? 255 : 0;

    /* SDL trigger axes are 0..32767 -> Xbox analog button 0..255 */
    pState->Gamepad.bAnalogButtons[XBOX_BUTTON_LTRIGGER] =
        (BYTE)(SDL_GameControllerGetAxis(c, SDL_CONTROLLER_AXIS_TRIGGERLEFT) >> 7);
    pState->Gamepad.bAnalogButtons[XBOX_BUTTON_RTRIGGER] =
        (BYTE)(SDL_GameControllerGetAxis(c, SDL_CONTROLLER_AXIS_TRIGGERRIGHT) >> 7);

    /* SDL Y axis points down; the Xbox Y axis points up -- invert.
     * Use (-1 - v) so v = -32768 does not overflow SHORT. */
    pState->Gamepad.sThumbLX = SDL_GameControllerGetAxis(c, SDL_CONTROLLER_AXIS_LEFTX);
    pState->Gamepad.sThumbLY =
        (SHORT)(-1 - SDL_GameControllerGetAxis(c, SDL_CONTROLLER_AXIS_LEFTY));
    pState->Gamepad.sThumbRX = SDL_GameControllerGetAxis(c, SDL_CONTROLLER_AXIS_RIGHTX);
    pState->Gamepad.sThumbRY =
        (SHORT)(-1 - SDL_GameControllerGetAxis(c, SDL_CONTROLLER_AXIS_RIGHTY));
    SDL_UnlockJoysticks();

    if (dwPort == 0)
        keyboard_merge(pState);
    return ERROR_SUCCESS;
}

DWORD xbox_InputSetState(DWORD dwPort, const XBOX_VIBRATION *pVibration)
{
    if (dwPort >= XBOX_MAX_CONTROLLERS || !pVibration)
        return ERROR_DEVICE_NOT_CONNECTED;

    update_controllers();
    SDL_LockJoysticks();
    SDL_GameController *c = controller_for_port_locked(dwPort);
    if (!c) {
        SDL_UnlockJoysticks();
        return ERROR_DEVICE_NOT_CONNECTED;
    }

    /* SDL rumble needs a duration; refresh for ~1s on each call (the game
     * polls vibration continuously). */
    SDL_GameControllerRumble(c, pVibration->wLeftMotorSpeed,
                             pVibration->wRightMotorSpeed, 1000);
    SDL_UnlockJoysticks();
    return ERROR_SUCCESS;
}

BOOL xbox_InputIsConnected(DWORD dwPort)
{
    BOOL connected;

    if (dwPort >= XBOX_MAX_CONTROLLERS)
        return FALSE;
    update_controllers();
    SDL_LockJoysticks();
    connected = controller_for_port_locked(dwPort) != NULL;
    SDL_UnlockJoysticks();
    return connected;
}

DWORD xbox_InputGetCapabilities(DWORD dwPort, DWORD dwFlags, XBOX_INPUT_CAPABILITIES *pCaps)
{
    (void)dwFlags;
    if (dwPort >= XBOX_MAX_CONTROLLERS || !pCaps)
        return ERROR_DEVICE_NOT_CONNECTED;

    update_controllers();
    SDL_LockJoysticks();
    if (!controller_for_port_locked(dwPort)) {
        SDL_UnlockJoysticks();
        return ERROR_DEVICE_NOT_CONNECTED;
    }

    memset(pCaps, 0, sizeof(XBOX_INPUT_CAPABILITIES));
    pCaps->Type    = 1;   /* XINPUT_DEVTYPE_GAMEPAD */
    pCaps->SubType = 1;   /* XINPUT_DEVSUBTYPE_GAMEPAD */
    pCaps->Flags   = 0;
    SDL_UnlockJoysticks();
    return ERROR_SUCCESS;
}

#endif /* _WIN32 */
