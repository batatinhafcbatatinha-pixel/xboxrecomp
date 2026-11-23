/* port_input: controllers for every recompiler's runtime, the same way (C, for C and C++ runtimes).
 *
 * SDL3.dll next to the game (loaded at run time: no SDL header or import library is needed to build) reads every
 * controller SDL knows: Xbox 360 / One / Series pads, DualShock 3 / 4, DualSense, Switch pads, USB adapters for
 * PS1 / PS2 / original Xbox / GameCube pads, generic pads (gamecontrollerdb.txt next to the game names their
 * buttons) and wheels. Each is read as one Xbox-layout pad (PortPad), so a runtime maps it to its console's pad by
 * position the way it already does.
 *
 * Settings: %APPDATA%\port-launcher\input.ini (every game), then input.ini in the game's folder (that game only,
 * key by key over the first), section [input]; the launcher's Controllers page writes them:
 *   controller          = auto | <SDL GUID>   which controller is player 1 (the others follow, as connected)
 *   vibration           = 1 | 0
 *   vibration_strength  = 0..200 (%)
 *   stick_deadzone      = 0..50 (%)          stick_sensitivity  = 10..300 (%)
 *   trigger_deadzone    = 0..50 (%)
 *   mouse_sensitivity   = 10..500 (%)        for runtimes with mouse look (port_input_settings)
 *   wheel_sensitivity   = 10..300 (%)        wheel_deadzone     = 0..30 (%)
 *   wheel_steering_axis = 0   wheel_throttle_axis = 2   wheel_brake_axis = 3   (the wheel's raw axes)
 *   wheel_pedals_full_at_min = 1   (pedals that report -32768 pressed, as most do)
 *   wheel_buttons       = a,b,x,y,lb,rb,back,start,ls,rs   (the wheel's buttons 0, 1, 2... as pad buttons)
 *   mapping.<GUID>      = <SDL mapping string>   (a remapped controller: SDL's own format)
 * Without SDL3.dll every call reports nothing connected: the runtime keeps its own input. */
#ifndef PORT_INPUT_H
#define PORT_INPUT_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define PORT_INPUT_PADS 4

enum {
    PORT_BTN_A = 1u << 0,      /* bottom face button (Cross, Nintendo B) */
    PORT_BTN_B = 1u << 1,      /* right (Circle) */
    PORT_BTN_X = 1u << 2,      /* left (Square) */
    PORT_BTN_Y = 1u << 3,      /* top (Triangle) */
    PORT_BTN_LB = 1u << 4,
    PORT_BTN_RB = 1u << 5,
    PORT_BTN_BACK = 1u << 6,   /* Back / Select / Share / - */
    PORT_BTN_START = 1u << 7,  /* Start / Options / + */
    PORT_BTN_LS = 1u << 8,     /* stick presses */
    PORT_BTN_RS = 1u << 9,
    PORT_BTN_UP = 1u << 10,
    PORT_BTN_DOWN = 1u << 11,
    PORT_BTN_LEFT = 1u << 12,
    PORT_BTN_RIGHT = 1u << 13,
    PORT_BTN_GUIDE = 1u << 14,
    PORT_BTN_LT = 1u << 15,    /* triggers past a quarter, for runtimes with digital triggers */
    PORT_BTN_RT = 1u << 16,
};

typedef struct PortPad {
    int connected;
    int wheel;                 /* a wheel: lx is the steering, rt the throttle, lt the brake */
    uint32_t buttons;          /* PORT_BTN_* */
    float lx, ly, rx, ry;      /* -1..1 after dead zone and sensitivity; up is negative, as in SDL */
    float lt, rt;              /* 0..1 */
    char name[96];
    char guid[40];
} PortPad;

typedef struct PortInputSettings {
    int vibration;             /* 0 / 1 */
    int vibration_strength;    /* % */
    int stick_deadzone, stick_sensitivity, trigger_deadzone;   /* % */
    int mouse_sensitivity;     /* % */
    int wheel_sensitivity, wheel_deadzone;                      /* % */
} PortInputSettings;

/* Once, before anything else: exe_dir holds SDL3.dll and gamecontrollerdb.txt; game_dir the game's input.ini
 * (NULL: the PORT_INPUT_GAME_DIR environment variable, when set). 1 when SDL3 runs. */
int port_input_start(const char *exe_dir, const char *game_dir);
/* Once per frame, on the thread that reads the pads. */
void port_input_update(void);
/* Player index's controller as it stands after the last update. */
void port_input_pad(int index, PortPad *out);
/* Vibration, 0..1 per motor (low frequency, high frequency); any thread. Lasts until changed. */
void port_input_rumble(int index, float low, float high);
const PortInputSettings *port_input_settings(void);
/* Every controller connected now, for listings: count, then name / GUID of each (index < count). */
int port_input_count(void);
const char *port_input_device_name(int i);
const char *port_input_device_guid(int i);

#ifdef __cplusplus
}
#endif
#endif
