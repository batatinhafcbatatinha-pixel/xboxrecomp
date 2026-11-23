/* port_input: see port_input.h. */
#include "port_input.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if !defined(_WIN32)
#include <strings.h>
#define _stricmp strcasecmp
#define _strnicmp strncasecmp
#endif

#if defined(_WIN32)
#include <windows.h>

/* SDL's own numbers and shapes, so that no SDL header is needed to build. */
#define SDL_INIT_JOYSTICK 0x200u
#define SDL_INIT_GAMEPAD 0x2000u
#define SDL_JOYSTICK_TYPE_GAMEPAD 1
#define SDL_JOYSTICK_TYPE_WHEEL 2
enum { SDL_PAD_SOUTH, SDL_PAD_EAST, SDL_PAD_WEST, SDL_PAD_NORTH, SDL_PAD_BACK, SDL_PAD_GUIDE, SDL_PAD_START,
       SDL_PAD_LEFT_STICK, SDL_PAD_RIGHT_STICK, SDL_PAD_LEFT_SHOULDER, SDL_PAD_RIGHT_SHOULDER, SDL_PAD_DPAD_UP,
       SDL_PAD_DPAD_DOWN, SDL_PAD_DPAD_LEFT, SDL_PAD_DPAD_RIGHT };
enum { SDL_HAT_UP = 1, SDL_HAT_RIGHT = 2, SDL_HAT_DOWN = 4, SDL_HAT_LEFT = 8 };
typedef struct { uint8_t data[16]; } SdlGuid;
typedef struct SdlPad SdlPad;
typedef struct SdlStick SdlStick;

static struct {
    uint8_t (*Init)(uint32_t);          /* SDL3 bool: one byte */
    uint8_t (*SetHint)(const char *, const char *);
    int (*AddMappingsFromFile)(const char *);
    int (*AddMapping)(const char *);
    void (*UpdateGamepads)(void);
    void (*UpdateJoysticks)(void);
    void (*FlushEvents)(uint32_t, uint32_t);
    uint32_t *(*GetJoysticks)(int *);
    uint8_t (*IsGamepad)(uint32_t);
    const char *(*NameForId)(uint32_t);
    SdlGuid (*GuidForId)(uint32_t);
    void (*GuidToString)(SdlGuid, char *, int);
    int (*TypeForId)(uint32_t);
    SdlPad *(*OpenPad)(uint32_t);
    void (*ClosePad)(SdlPad *);
    uint8_t (*PadButton)(SdlPad *, int);
    int16_t (*PadAxis)(SdlPad *, int);
    uint8_t (*RumblePad)(SdlPad *, uint16_t, uint16_t, uint32_t);
    SdlStick *(*OpenStick)(uint32_t);
    void (*CloseStick)(SdlStick *);
    int16_t (*StickAxis)(SdlStick *, int);
    uint8_t (*StickButton)(SdlStick *, int);
    uint8_t (*StickHat)(SdlStick *, int);
    int (*StickAxes)(SdlStick *);
    int (*StickButtons)(SdlStick *);
    int (*StickHats)(SdlStick *);
    uint8_t (*RumbleStick)(SdlStick *, uint16_t, uint16_t, uint32_t);
    void (*Free)(void *);
} sdl;
static int g_running;

/* A connected controller: a gamepad SDL knows the buttons of, or a raw joystick (a wheel, an unknown pad). */
typedef struct {
    uint32_t id;
    SdlPad *pad;
    SdlStick *stick;
    int wheel;
    char name[96], guid[40];
} Device;
#define MAX_DEVICES 16
static Device g_devices[MAX_DEVICES];
static int g_device_count;
static int g_players[PORT_INPUT_PADS];      /* device index per player, -1 none */
static PortPad g_pads[PORT_INPUT_PADS];
static volatile LONG g_rumble[PORT_INPUT_PADS];   /* low << 16 | high, wanted */
static uint32_t g_rumble_sent[PORT_INPUT_PADS];
static DWORD g_rumble_renew[PORT_INPUT_PADS], g_scan_at;
#endif

/* ---------------------------------------------------------------- settings */

static PortInputSettings g_set = {1, 100, 12, 100, 5, 100, 100, 2};
static char g_controller[40] = "auto";
static int g_wheel_axes[3] = {0, 2, 3}, g_pedals_full_at_min = 1;
static uint32_t g_wheel_buttons[32];
#define MAX_MAPPINGS 32
static char g_mappings[MAX_MAPPINGS][512];
static int g_mapping_count;

static int clampi(int v, int lo, int hi) { return v < lo ? lo : v > hi ? hi : v; }

static uint32_t button_word(const char *w)
{
    static const struct { const char *name; uint32_t bit; } names[] = {
        {"a", PORT_BTN_A}, {"b", PORT_BTN_B}, {"x", PORT_BTN_X}, {"y", PORT_BTN_Y}, {"lb", PORT_BTN_LB},
        {"rb", PORT_BTN_RB}, {"back", PORT_BTN_BACK}, {"start", PORT_BTN_START}, {"ls", PORT_BTN_LS},
        {"rs", PORT_BTN_RS}, {"up", PORT_BTN_UP}, {"down", PORT_BTN_DOWN}, {"left", PORT_BTN_LEFT},
        {"right", PORT_BTN_RIGHT}, {"guide", PORT_BTN_GUIDE}, {"lt", PORT_BTN_LT}, {"rt", PORT_BTN_RT}};
    for (size_t i = 0; i < sizeof names / sizeof names[0]; i++)
        if (!_stricmp(w, names[i].name))
            return names[i].bit;
    return 0;
}

static void set_wheel_buttons(const char *list)
{
    char copy[512];
    int n = 0;
    memset(g_wheel_buttons, 0, sizeof g_wheel_buttons);
    snprintf(copy, sizeof copy, "%s", list);
    for (char *tok = strtok(copy, ", "); tok && n < 32; tok = strtok(NULL, ", "))
        g_wheel_buttons[n++] = button_word(tok);
}

static char *trim(char *s)
{
    while (*s == ' ' || *s == '\t') s++;
    char *e = s + strlen(s);
    while (e > s && (e[-1] == ' ' || e[-1] == '\t' || e[-1] == '\r' || e[-1] == '\n')) *--e = 0;
    return s;
}

static void read_settings(const char *path)
{
    FILE *f = path ? fopen(path, "r") : NULL;
    char line[1024];
    int in_section = 0;
    if (!f)
        return;
    while (fgets(line, sizeof line, f)) {
        char *s = trim(line), *eq;
        if (!*s || *s == ';' || *s == '#')
            continue;
        if (*s == '[') {
            in_section = !_strnicmp(s, "[input]", 7);
            continue;
        }
        if (!in_section || !(eq = strchr(s, '=')))
            continue;
        *eq = 0;
        char *key = trim(s), *value = trim(eq + 1);
        int v = atoi(value);
        if (!_stricmp(key, "controller")) snprintf(g_controller, sizeof g_controller, "%s", value);
        else if (!_stricmp(key, "vibration")) g_set.vibration = v != 0;
        else if (!_stricmp(key, "vibration_strength")) g_set.vibration_strength = clampi(v, 0, 200);
        else if (!_stricmp(key, "stick_deadzone")) g_set.stick_deadzone = clampi(v, 0, 50);
        else if (!_stricmp(key, "stick_sensitivity")) g_set.stick_sensitivity = clampi(v, 10, 300);
        else if (!_stricmp(key, "trigger_deadzone")) g_set.trigger_deadzone = clampi(v, 0, 50);
        else if (!_stricmp(key, "mouse_sensitivity")) g_set.mouse_sensitivity = clampi(v, 10, 500);
        else if (!_stricmp(key, "wheel_sensitivity")) g_set.wheel_sensitivity = clampi(v, 10, 300);
        else if (!_stricmp(key, "wheel_deadzone")) g_set.wheel_deadzone = clampi(v, 0, 30);
        else if (!_stricmp(key, "wheel_steering_axis")) g_wheel_axes[0] = clampi(v, 0, 15);
        else if (!_stricmp(key, "wheel_throttle_axis")) g_wheel_axes[1] = clampi(v, 0, 15);
        else if (!_stricmp(key, "wheel_brake_axis")) g_wheel_axes[2] = clampi(v, 0, 15);
        else if (!_stricmp(key, "wheel_pedals_full_at_min")) g_pedals_full_at_min = v != 0;
        else if (!_stricmp(key, "wheel_buttons")) set_wheel_buttons(value);
        else if (!_strnicmp(key, "mapping.", 8) && g_mapping_count < MAX_MAPPINGS && strlen(value) < 512)
            snprintf(g_mappings[g_mapping_count++], 512, "%s", value);
    }
    fclose(f);
}

const PortInputSettings *port_input_settings(void) { return &g_set; }

/* ---------------------------------------------------------------- shaping */

/* A stick: radial dead zone, the rest rescaled to the full range, then the sensitivity. */
static void shape_stick(float *x, float *y, int deadzone, int sensitivity)
{
    float m = sqrtf(*x * *x + *y * *y), dz = deadzone / 100.0f;
    if (m <= dz || m <= 0.0f) {
        *x = *y = 0.0f;
        return;
    }
    float scaled = (m - dz) / (1.0f - dz) * (sensitivity / 100.0f);
    if (scaled > 1.0f) scaled = 1.0f;
    *x = *x / m * scaled;
    *y = *y / m * scaled;
    if (*x > 1.0f) *x = 1.0f; else if (*x < -1.0f) *x = -1.0f;
    if (*y > 1.0f) *y = 1.0f; else if (*y < -1.0f) *y = -1.0f;
}

static float shape_trigger(float t)
{
    float dz = g_set.trigger_deadzone / 100.0f;
    return t <= dz ? 0.0f : (t - dz) / (1.0f - dz);
}

#if defined(_WIN32)
/* ---------------------------------------------------------------- SDL */

static int load_sdl(const char *dir)
{
    char path[MAX_PATH];
    snprintf(path, sizeof path, "%s%sSDL3.dll", dir ? dir : "", dir && *dir ? "\\" : "");
    HMODULE lib = LoadLibraryExA(path, NULL, LOAD_WITH_ALTERED_SEARCH_PATH);
    if (!lib)
        return 0;
    int ok = 1;
#define BIND(field, name) do { *(FARPROC *)&sdl.field = GetProcAddress(lib, name); ok = ok && sdl.field; } while (0)
    BIND(Init, "SDL_Init"); BIND(SetHint, "SDL_SetHint"); BIND(AddMappingsFromFile, "SDL_AddGamepadMappingsFromFile");
    BIND(AddMapping, "SDL_AddGamepadMapping"); BIND(UpdateGamepads, "SDL_UpdateGamepads");
    BIND(UpdateJoysticks, "SDL_UpdateJoysticks"); BIND(FlushEvents, "SDL_FlushEvents");
    BIND(GetJoysticks, "SDL_GetJoysticks"); BIND(IsGamepad, "SDL_IsGamepad");
    BIND(NameForId, "SDL_GetJoystickNameForID"); BIND(GuidForId, "SDL_GetJoystickGUIDForID");
    BIND(GuidToString, "SDL_GUIDToString"); BIND(TypeForId, "SDL_GetJoystickTypeForID");
    BIND(OpenPad, "SDL_OpenGamepad"); BIND(ClosePad, "SDL_CloseGamepad"); BIND(PadButton, "SDL_GetGamepadButton");
    BIND(PadAxis, "SDL_GetGamepadAxis"); BIND(RumblePad, "SDL_RumbleGamepad");
    BIND(OpenStick, "SDL_OpenJoystick"); BIND(CloseStick, "SDL_CloseJoystick"); BIND(StickAxis, "SDL_GetJoystickAxis");
    BIND(StickButton, "SDL_GetJoystickButton"); BIND(StickHat, "SDL_GetJoystickHat");
    BIND(StickAxes, "SDL_GetNumJoystickAxes"); BIND(StickButtons, "SDL_GetNumJoystickButtons");
    BIND(StickHats, "SDL_GetNumJoystickHats"); BIND(RumbleStick, "SDL_RumbleJoystick"); BIND(Free, "SDL_free");
#undef BIND
    if (!ok)
        return 0;
    /* The window is the runtime's: controllers work without an SDL window, on SDL's own thread. */
    sdl.SetHint("SDL_JOYSTICK_ALLOW_BACKGROUND_EVENTS", "1");
    sdl.SetHint("SDL_JOYSTICK_THREAD", "1");
    if (!sdl.Init(SDL_INIT_GAMEPAD | SDL_INIT_JOYSTICK))
        return 0;
    snprintf(path, sizeof path, "%s%sgamecontrollerdb.txt", dir ? dir : "", dir && *dir ? "\\" : "");
    sdl.AddMappingsFromFile(path);
    for (int i = 0; i < g_mapping_count; i++)
        sdl.AddMapping(g_mappings[i]);   /* the player's remaps win over the database */
    return 1;
}

static void close_device(Device *d)
{
    if (d->pad) sdl.ClosePad(d->pad);
    if (d->stick) sdl.CloseStick(d->stick);
    memset(d, 0, sizeof *d);
}

/* Keeps g_devices in step with what is connected, twice a second, then deals the players out: the chosen
 * controller first, the rest in the order they came. */
static void scan(void)
{
    int count = 0;
    uint32_t *ids = sdl.GetJoysticks(&count);
    for (int i = 0; i < g_device_count; i++) {
        int still = 0;
        for (int k = 0; ids && k < count; k++) still = still || ids[k] == g_devices[i].id;
        if (!still) {
            close_device(&g_devices[i]);
            g_devices[i] = g_devices[--g_device_count];
            memset(&g_devices[g_device_count], 0, sizeof(Device));
            i--;
        }
    }
    for (int k = 0; ids && k < count && g_device_count < MAX_DEVICES; k++) {
        int known = 0;
        for (int i = 0; i < g_device_count; i++) known = known || g_devices[i].id == ids[k];
        if (known)
            continue;
        Device *d = &g_devices[g_device_count];
        memset(d, 0, sizeof *d);
        d->id = ids[k];
        const char *name = sdl.NameForId(ids[k]);
        snprintf(d->name, sizeof d->name, "%s", name ? name : "Controller");
        sdl.GuidToString(sdl.GuidForId(ids[k]), d->guid, sizeof d->guid);
        int type = sdl.TypeForId(ids[k]);
        if (sdl.IsGamepad(ids[k]) && type != SDL_JOYSTICK_TYPE_WHEEL) {
            d->pad = sdl.OpenPad(ids[k]);
        } else {
            d->stick = sdl.OpenStick(ids[k]);
            d->wheel = type == SDL_JOYSTICK_TYPE_WHEEL;
        }
        if (d->pad || d->stick)
            g_device_count++;
    }
    if (ids) sdl.Free(ids);
    int used[MAX_DEVICES] = {0}, player = 0;
    for (int p = 0; p < PORT_INPUT_PADS; p++) g_players[p] = -1;
    if (_stricmp(g_controller, "auto"))
        for (int i = 0; i < g_device_count; i++)
            if (!_stricmp(g_devices[i].guid, g_controller)) { g_players[player++] = i; used[i] = 1; break; }
    for (int i = 0; i < g_device_count && player < PORT_INPUT_PADS; i++)
        if (!used[i]) g_players[player++] = i;
}

static void read_gamepad(Device *d, PortPad *p)
{
    static const struct { int sdl; uint32_t bit; } map[] = {
        {SDL_PAD_SOUTH, PORT_BTN_A}, {SDL_PAD_EAST, PORT_BTN_B}, {SDL_PAD_WEST, PORT_BTN_X}, {SDL_PAD_NORTH, PORT_BTN_Y},
        {SDL_PAD_LEFT_SHOULDER, PORT_BTN_LB}, {SDL_PAD_RIGHT_SHOULDER, PORT_BTN_RB}, {SDL_PAD_BACK, PORT_BTN_BACK},
        {SDL_PAD_START, PORT_BTN_START}, {SDL_PAD_LEFT_STICK, PORT_BTN_LS}, {SDL_PAD_RIGHT_STICK, PORT_BTN_RS},
        {SDL_PAD_DPAD_UP, PORT_BTN_UP}, {SDL_PAD_DPAD_DOWN, PORT_BTN_DOWN}, {SDL_PAD_DPAD_LEFT, PORT_BTN_LEFT},
        {SDL_PAD_DPAD_RIGHT, PORT_BTN_RIGHT}, {SDL_PAD_GUIDE, PORT_BTN_GUIDE}};
    for (size_t i = 0; i < sizeof map / sizeof map[0]; i++)
        if (sdl.PadButton(d->pad, map[i].sdl)) p->buttons |= map[i].bit;
    p->lx = sdl.PadAxis(d->pad, 0) / 32767.0f;
    p->ly = sdl.PadAxis(d->pad, 1) / 32767.0f;
    p->rx = sdl.PadAxis(d->pad, 2) / 32767.0f;
    p->ry = sdl.PadAxis(d->pad, 3) / 32767.0f;
    p->lt = sdl.PadAxis(d->pad, 4) / 32767.0f;
    p->rt = sdl.PadAxis(d->pad, 5) / 32767.0f;
}

/* A raw joystick: a wheel (steering, throttle, brake from the configured axes) or an unknown pad (axes 0/1 the left
 * stick, 2/3 the right, the hat the d-pad); its buttons by wheel_buttons. */
static void read_joystick(Device *d, PortPad *p)
{
    int axes = sdl.StickAxes(d->stick), buttons = sdl.StickButtons(d->stick);
    for (int b = 0; b < buttons && b < 32; b++)
        if (sdl.StickButton(d->stick, b)) p->buttons |= g_wheel_buttons[b];
    if (sdl.StickHats(d->stick) > 0) {
        uint8_t hat = sdl.StickHat(d->stick, 0);
        if (hat & SDL_HAT_UP) p->buttons |= PORT_BTN_UP;
        if (hat & SDL_HAT_DOWN) p->buttons |= PORT_BTN_DOWN;
        if (hat & SDL_HAT_LEFT) p->buttons |= PORT_BTN_LEFT;
        if (hat & SDL_HAT_RIGHT) p->buttons |= PORT_BTN_RIGHT;
    }
    if (d->wheel) {
        float steer = g_wheel_axes[0] < axes ? sdl.StickAxis(d->stick, g_wheel_axes[0]) / 32767.0f : 0.0f;
        float dz = g_set.wheel_deadzone / 100.0f, mag = fabsf(steer);
        steer = mag <= dz ? 0.0f : (steer > 0 ? 1.0f : -1.0f) * (mag - dz) / (1.0f - dz) * (g_set.wheel_sensitivity / 100.0f);
        p->lx = steer > 1.0f ? 1.0f : steer < -1.0f ? -1.0f : steer;
        for (int k = 0; k < 2; k++) {
            int axis = g_wheel_axes[1 + k];
            float raw = axis < axes ? sdl.StickAxis(d->stick, axis) / 32767.0f : (g_pedals_full_at_min ? 1.0f : -1.0f);
            float pedal = g_pedals_full_at_min ? (1.0f - raw) * 0.5f : (raw + 1.0f) * 0.5f;
            pedal = pedal < 0.0f ? 0.0f : pedal > 1.0f ? 1.0f : pedal;
            if (k == 0) p->rt = pedal; else p->lt = pedal;
        }
        return;
    }
    if (axes > 0) p->lx = sdl.StickAxis(d->stick, 0) / 32767.0f;
    if (axes > 1) p->ly = sdl.StickAxis(d->stick, 1) / 32767.0f;
    if (axes > 2) p->rx = sdl.StickAxis(d->stick, 2) / 32767.0f;
    if (axes > 3) p->ry = sdl.StickAxis(d->stick, 3) / 32767.0f;
}

/* A rumble lasts a set time in SDL: renewed while it should run, so a lost controller cannot leave a motor on. */
static void apply_rumble(DWORD now)
{
    for (int p = 0; p < PORT_INPUT_PADS; p++) {
        int i = g_players[p];
        uint32_t wanted = g_set.vibration ? (uint32_t)g_rumble[p] : 0u;
        if (i < 0) { g_rumble_sent[p] = 0; continue; }
        if (wanted == g_rumble_sent[p] && (wanted == 0 || (LONG)(now - g_rumble_renew[p]) < 0))
            continue;
        uint16_t lo = (uint16_t)(wanted >> 16), hi = (uint16_t)wanted;
        if (g_devices[i].pad) sdl.RumblePad(g_devices[i].pad, lo, hi, 1000);
        else if (g_devices[i].stick) sdl.RumbleStick(g_devices[i].stick, lo, hi, 1000);
        g_rumble_sent[p] = wanted;
        g_rumble_renew[p] = now + 500;
    }
}
#endif

/* ---------------------------------------------------------------- API */

int port_input_start(const char *exe_dir, const char *game_dir)
{
    char path[1024];
    set_wheel_buttons("a,b,x,y,lb,rb,back,start,ls,rs");
    const char *appdata = getenv("APPDATA");
    if (appdata) {
        snprintf(path, sizeof path, "%s\\port-launcher\\input.ini", appdata);
        read_settings(path);
    }
    if (!game_dir)
        game_dir = getenv("PORT_INPUT_GAME_DIR");   /* set by the recompilers' play steps */
    if (game_dir && *game_dir) {
        snprintf(path, sizeof path, "%s\\input.ini", game_dir);
        read_settings(path);
    }
#if defined(_WIN32)
    if (!g_running) {
        g_running = load_sdl(exe_dir);
        for (int p = 0; p < PORT_INPUT_PADS; p++) g_players[p] = -1;
        if (g_running) scan();
        fprintf(stderr, "[input] %s; %d controller(s)\n", g_running ? "SDL3: every controller SDL knows" :
                "SDL3.dll not found: the runtime's own input", g_running ? g_device_count : 0);
    }
    return g_running;
#else
    (void)exe_dir;
    return 0;
#endif
}

void port_input_update(void)
{
#if defined(_WIN32)
    if (!g_running)
        return;
    sdl.UpdateGamepads();
    sdl.UpdateJoysticks();
    sdl.FlushEvents(0u, 0xFFFFFFFFu);   /* nothing here reads SDL's event queue */
    DWORD now = GetTickCount();
    if ((LONG)(now - g_scan_at) >= 0) {
        g_scan_at = now + 500;
        scan();
    }
    for (int p = 0; p < PORT_INPUT_PADS; p++) {
        PortPad *pad = &g_pads[p];
        int i = g_players[p];
        memset(pad, 0, sizeof *pad);
        if (i < 0)
            continue;
        Device *d = &g_devices[i];
        pad->connected = 1;
        pad->wheel = d->wheel;
        snprintf(pad->name, sizeof pad->name, "%s", d->name);
        snprintf(pad->guid, sizeof pad->guid, "%s", d->guid);
        if (d->pad) read_gamepad(d, pad); else read_joystick(d, pad);
        if (!pad->wheel) {
            shape_stick(&pad->lx, &pad->ly, g_set.stick_deadzone, g_set.stick_sensitivity);
            shape_stick(&pad->rx, &pad->ry, g_set.stick_deadzone, g_set.stick_sensitivity);
        }
        pad->lt = shape_trigger(pad->lt);
        pad->rt = shape_trigger(pad->rt);
        if (pad->lt > 0.25f) pad->buttons |= PORT_BTN_LT;
        if (pad->rt > 0.25f) pad->buttons |= PORT_BTN_RT;
    }
    apply_rumble(now);
#endif
}

void port_input_pad(int index, PortPad *out)
{
#if defined(_WIN32)
    if (index >= 0 && index < PORT_INPUT_PADS && g_running) {
        *out = g_pads[index];
        return;
    }
#endif
    memset(out, 0, sizeof *out);
}

void port_input_rumble(int index, float low, float high)
{
#if defined(_WIN32)
    if (index < 0 || index >= PORT_INPUT_PADS)
        return;
    float k = g_set.vibration_strength / 100.0f;
    float l = low * k, h = high * k;
    uint32_t lo = (uint32_t)((l < 0 ? 0 : l > 1 ? 1 : l) * 65535.0f), hi = (uint32_t)((h < 0 ? 0 : h > 1 ? 1 : h) * 65535.0f);
    InterlockedExchange(&g_rumble[index], (LONG)(lo << 16 | hi));
#else
    (void)index; (void)low; (void)high;
#endif
}

int port_input_count(void)
{
#if defined(_WIN32)
    return g_running ? g_device_count : 0;
#else
    return 0;
#endif
}

const char *port_input_device_name(int i)
{
#if defined(_WIN32)
    return g_running && i >= 0 && i < g_device_count ? g_devices[i].name : "";
#else
    (void)i;
    return "";
#endif
}

const char *port_input_device_guid(int i)
{
#if defined(_WIN32)
    return g_running && i >= 0 && i < g_device_count ? g_devices[i].guid : "";
#else
    (void)i;
    return "";
#endif
}
