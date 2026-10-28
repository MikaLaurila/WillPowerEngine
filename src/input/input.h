/**
 * @file input.h
 * @brief Keyboard, mouse and gamepad input with named actions bound across devices.
 *
 * Devices:
 * - Keyboard keys are physical positions (USB HID usage codes, layout-independent): WPE_KEY_W is
 *   the key labelled W on a US layout wherever the user's layout puts that letter.
 * - The mouse position is in framebuffer pixels when events come from a WPE_Window
 *   (wpe_window_poll_events() converts them); deltas and wheel movement accumulate per frame.
 * - Gamepads use the standard layout (Xbox-style positions: SOUTH is A on Xbox, Cross on
 *   PlayStation). Each connected pad occupies a player slot (0 .. WPE_INPUT_MAX_PLAYERS - 1): a new
 *   pad takes the lowest free slot, a disconnected pad frees its slot, so a reconnecting pad
 *   normally gets its slot back. Sticks get a radial dead zone and triggers a linear one;
 *   stick Y axes are positive downwards. Pads beyond the slot count are ignored until a slot frees.
 *
 * Actions: named inputs ("jump", "move_x") with up to WPE_INPUT_MAX_BINDINGS bindings each, mixing
 * keys, mouse buttons, pad buttons and pad axes. An action's value is the sum of its bindings'
 * values (a digital binding contributes `scale` while held, an axis its dead-zoned value times
 * `scale`), clamped to [-1, 1]; it is "down" while |value| >= press_threshold. Queries take a
 * player: the keyboard and mouse belong to settings.keyboard_player, each pad to its slot, and
 * WPE_INPUT_ANY_PLAYER combines every device.
 *
 * Per-frame protocol (main thread):
 *   wpe_input_begin_frame() → wpe_input_handle_event() for every OS event → wpe_input_update()
 * then query. "Pressed" and "released" are edges of the current frame; a press and release within
 * one frame still report both edges.
 *
 * - Dependencies: core, platform (WPE_PlatformEvent), SDL3 (private)
 * - Thread affinity: main thread only
 *
 * @author WillPowerEngine Team
 */
#ifndef WPE_INPUT_INPUT_H
#define WPE_INPUT_INPUT_H

#include <stdint.h>

#include "core/result.h"
#include "platform/platform.h"

#define WPE_INPUT_MAX_PLAYERS 4u
#define WPE_INPUT_MAX_ACTIONS 64u
#define WPE_INPUT_MAX_BINDINGS 8u
#define WPE_INPUT_ACTION_NAME_MAX 32u /* Including the terminator. */
/** Player argument meaning "every device". */
#define WPE_INPUT_ANY_PLAYER UINT32_MAX

/** Physical key (USB HID usage code). Values are stable; names are listed by wpe_input_key_name(). */
typedef uint32_t WPE_Key;
#define WPE_KEY_COUNT 512u
enum WPE_KeyCode {
    WPE_KEY_UNKNOWN = 0,
    WPE_KEY_A = 4, /* WPE_KEY_A + n is the n-th letter. */
    WPE_KEY_Z = 29,
    WPE_KEY_1 = 30, /* 1..9, then 0. */
    WPE_KEY_0 = 39,
    WPE_KEY_RETURN = 40,
    WPE_KEY_ESCAPE = 41,
    WPE_KEY_BACKSPACE = 42,
    WPE_KEY_TAB = 43,
    WPE_KEY_SPACE = 44,
    WPE_KEY_F1 = 58, /* F1..F12 are consecutive. */
    WPE_KEY_F12 = 69,
    WPE_KEY_RIGHT = 79,
    WPE_KEY_LEFT = 80,
    WPE_KEY_DOWN = 81,
    WPE_KEY_UP = 82,
    WPE_KEY_LCTRL = 224,
    WPE_KEY_LSHIFT = 225,
    WPE_KEY_LALT = 226,
    WPE_KEY_RCTRL = 228,
    WPE_KEY_RSHIFT = 229,
    WPE_KEY_RALT = 230,
};

typedef enum WPE_MouseButton {
    WPE_MOUSE_LEFT = 1,
    WPE_MOUSE_MIDDLE = 2,
    WPE_MOUSE_RIGHT = 3,
    WPE_MOUSE_X1 = 4,
    WPE_MOUSE_X2 = 5,
} WPE_MouseButton;
#define WPE_MOUSE_BUTTON_MAX 5u

typedef enum WPE_GamepadButton {
    WPE_PAD_SOUTH = 0, /* A (Xbox), Cross (PlayStation), B (Nintendo) */
    WPE_PAD_EAST,
    WPE_PAD_WEST,
    WPE_PAD_NORTH,
    WPE_PAD_BACK,
    WPE_PAD_GUIDE,
    WPE_PAD_START,
    WPE_PAD_LEFT_STICK,
    WPE_PAD_RIGHT_STICK,
    WPE_PAD_LEFT_SHOULDER,
    WPE_PAD_RIGHT_SHOULDER,
    WPE_PAD_DPAD_UP,
    WPE_PAD_DPAD_DOWN,
    WPE_PAD_DPAD_LEFT,
    WPE_PAD_DPAD_RIGHT,
    WPE_PAD_MISC1, /* Share / capture / mic button, where present. */
    WPE_PAD_RIGHT_PADDLE1,
    WPE_PAD_LEFT_PADDLE1,
    WPE_PAD_RIGHT_PADDLE2,
    WPE_PAD_LEFT_PADDLE2,
    WPE_PAD_TOUCHPAD,
} WPE_GamepadButton;
#define WPE_PAD_BUTTON_COUNT 21u

typedef enum WPE_GamepadAxis {
    WPE_PAD_LEFT_X = 0, /* Sticks: -1..1, Y positive downwards. */
    WPE_PAD_LEFT_Y,
    WPE_PAD_RIGHT_X,
    WPE_PAD_RIGHT_Y,
    WPE_PAD_LEFT_TRIGGER, /* Triggers: 0..1. */
    WPE_PAD_RIGHT_TRIGGER,
} WPE_GamepadAxis;
#define WPE_PAD_AXIS_COUNT 6u

typedef enum WPE_InputSource {
    WPE_INPUT_SOURCE_KEY = 0,      /* code: WPE_Key */
    WPE_INPUT_SOURCE_MOUSE_BUTTON, /* code: WPE_MouseButton */
    WPE_INPUT_SOURCE_PAD_BUTTON,   /* code: WPE_GamepadButton */
    WPE_INPUT_SOURCE_PAD_AXIS,     /* code: WPE_GamepadAxis */
} WPE_InputSource;

/** Which part of an axis a binding reads. */
typedef enum WPE_AxisRange {
    WPE_AXIS_FULL = 0, /* The signed value. */
    WPE_AXIS_POSITIVE, /* max(value, 0): e.g. stick right as a button. */
    WPE_AXIS_NEGATIVE, /* max(-value, 0): e.g. stick up as a button. */
} WPE_AxisRange;

typedef struct WPE_InputBinding {
    WPE_InputSource source;
    uint32_t code;
    float scale;         /* Multiplies the binding's value (0 is treated as 1; -1 inverts). */
    WPE_AxisRange range; /* Pad axes only. */
} WPE_InputBinding;

typedef struct WPE_InputActionDesc {
    char name[WPE_INPUT_ACTION_NAME_MAX]; /* Unique, non-empty. */
    WPE_InputBinding bindings[WPE_INPUT_MAX_BINDINGS];
    uint32_t binding_count;
} WPE_InputActionDesc;

typedef struct WPE_InputSettings {
    float stick_deadzone;     /* Radial inner dead zone of sticks (0..0.9, default 0.2). */
    float stick_outer;        /* Stick magnitude treated as full deflection (deadzone..1, default 0.95). */
    float trigger_deadzone;   /* Trigger dead zone (0..0.9, default 0.1). */
    float press_threshold;    /* |action value| at which an action is down (0.05..1, default 0.5). */
    uint32_t keyboard_player; /* Player slot owning keyboard and mouse (default 0). */
} WPE_InputSettings;

/** Action handle: 1 + index in the action list; WPE_INPUT_ACTION_NONE (0) is never valid. */
typedef uint32_t WPE_InputAction;
#define WPE_INPUT_ACTION_NONE ((WPE_InputAction)0)

typedef struct WPE_InputDesc {
    WPE_InputSettings settings; /* Zeroed fields take their defaults. */
    bool no_gamepads;           /* Do not initialize gamepad support (keyboard and mouse only). */
} WPE_InputDesc;

typedef struct WPE_Input WPE_Input;

/** @return The default settings (see WPE_InputSettings). @thread_safety Any thread */
WPE_InputSettings wpe_input_settings_default(void);

/**
 * Creates the input state. Unless desc->no_gamepads, initializes SDL's gamepad support and opens
 * every gamepad already connected.
 *
 * @param desc Optional (NULL = defaults with gamepads).
 * @return WPE_RESULT_OK; WPE_RESULT_ERROR_NULL_ARG; WPE_RESULT_ERROR_INVALID_ARG (settings out of
 *         range; logged); WPE_RESULT_ERROR_OUT_OF_MEMORY; WPE_RESULT_ERROR (SDL failure; logged).
 *
 * @thread_safety Main thread only
 */
[[nodiscard]] WPE_Result wpe_input_create(const WPE_InputDesc* desc, WPE_Input** out_input);

/** Closes all gamepads and frees the input state; NULL is a no-op. @thread_safety Main thread only */
void wpe_input_destroy(WPE_Input* input);

/** @return The current settings. @thread_safety Main thread only */
WPE_InputSettings wpe_input_settings(const WPE_Input* input);

/** Replaces the settings (zeroed fields take defaults). @return WPE_RESULT_OK, WPE_RESULT_ERROR_NULL_ARG or
 * WPE_RESULT_ERROR_INVALID_ARG (logged). @thread_safety Main thread only */
WPE_Result wpe_input_set_settings(WPE_Input* input, const WPE_InputSettings* settings);

/** Starts a frame: clears the previous frame's edges, deltas and wheel. @thread_safety Main thread only */
void wpe_input_begin_frame(WPE_Input* input);

/**
 * Applies one OS event (from wpe_window_poll_events() or wpe_platform_poll_events()). Events that
 * are not input are ignored.
 *
 * @thread_safety Main thread only
 */
void wpe_input_handle_event(WPE_Input* input, const WPE_PlatformEvent* event);

/** Evaluates the actions from the device state after this frame's events. @thread_safety Main thread only */
void wpe_input_update(WPE_Input* input);

/**
 * Releases every held key and mouse button (with release edges), as losing window focus does: for hosts
 * that stop delivering events (the editor between play tests), so nothing stays held. Gamepads are not
 * affected; they report their own state.
 *
 * @thread_safety Main thread only
 */
void wpe_input_release_all(WPE_Input* input);

/* ---- Actions ------------------------------------------------------------------------------------ */

/**
 * Replaces the action list (handles of the previous list become invalid).
 *
 * @return WPE_RESULT_OK; WPE_RESULT_ERROR_NULL_ARG; WPE_RESULT_ERROR_CAPACITY (more than
 *         WPE_INPUT_MAX_ACTIONS); WPE_RESULT_ERROR_INVALID_ARG (empty, unterminated or duplicate name,
 *         too many bindings, or a binding with an unknown source or code; logged).
 *
 * @thread_safety Main thread only
 */
WPE_Result wpe_input_set_actions(WPE_Input* input, const WPE_InputActionDesc* actions, uint32_t count);

/** @return Number of actions. @thread_safety Main thread only */
uint32_t wpe_input_action_count(const WPE_Input* input);

/** @return The description of action `index` (< count; borrowed until the list changes), or NULL.
 * @thread_safety Main thread only */
const WPE_InputActionDesc* wpe_input_action_desc(const WPE_Input* input, uint32_t index);

/** @return The action named `name`, or WPE_INPUT_ACTION_NONE. Look actions up once and keep the handle.
 * @thread_safety Main thread only */
WPE_InputAction wpe_input_find_action(const WPE_Input* input, const char* name);

/* Action state for `player` (a slot or WPE_INPUT_ANY_PLAYER). Invalid handles or players read as
 * released / 0. */
bool wpe_input_action_down(const WPE_Input* input, WPE_InputAction action, uint32_t player);
bool wpe_input_action_pressed(const WPE_Input* input, WPE_InputAction action, uint32_t player);
bool wpe_input_action_released(const WPE_Input* input, WPE_InputAction action, uint32_t player);
float wpe_input_action_value(const WPE_Input* input, WPE_InputAction action, uint32_t player);

/* ---- Keyboard and mouse --------------------------------------------------------------------------- */

bool wpe_input_key_down(const WPE_Input* input, WPE_Key key);
bool wpe_input_key_pressed(const WPE_Input* input, WPE_Key key);
bool wpe_input_key_released(const WPE_Input* input, WPE_Key key);

bool wpe_input_mouse_down(const WPE_Input* input, WPE_MouseButton button);
bool wpe_input_mouse_pressed(const WPE_Input* input, WPE_MouseButton button);
bool wpe_input_mouse_released(const WPE_Input* input, WPE_MouseButton button);

/** Mouse position (framebuffer pixels), this frame's motion and wheel movement (up/right positive).
 * @thread_safety Main thread only */
void wpe_input_mouse_position(const WPE_Input* input, float* out_x, float* out_y);
void wpe_input_mouse_delta(const WPE_Input* input, float* out_dx, float* out_dy);
void wpe_input_mouse_wheel(const WPE_Input* input, float* out_x, float* out_y);

/** Requests mouse capture (hidden cursor, relative motion for mouse look); the owner of the window
 * applies it with wpe_window_set_relative_mouse(). @thread_safety Main thread only */
void wpe_input_set_mouse_captured(WPE_Input* input, bool captured);
bool wpe_input_mouse_captured(const WPE_Input* input);

/* ---- Gamepads ----------------------------------------------------------------------------------- */

/** @return Number of connected gamepads occupying player slots. @thread_safety Main thread only */
uint32_t wpe_input_gamepad_count(const WPE_Input* input);

/** @return Whether a gamepad occupies player slot `player`. @thread_safety Main thread only */
bool wpe_input_gamepad_connected(const WPE_Input* input, uint32_t player);

/** @return The product name of the pad in slot `player` (borrowed until it disconnects), or NULL.
 * @thread_safety Main thread only */
const char* wpe_input_gamepad_name(const WPE_Input* input, uint32_t player);

/* Pad state by player slot (or WPE_INPUT_ANY_PLAYER for any pad); disconnected slots read as
 * released / 0. Axis values are dead-zoned. */
bool wpe_input_gamepad_down(const WPE_Input* input, uint32_t player, WPE_GamepadButton button);
bool wpe_input_gamepad_pressed(const WPE_Input* input, uint32_t player, WPE_GamepadButton button);
bool wpe_input_gamepad_released(const WPE_Input* input, uint32_t player, WPE_GamepadButton button);
float wpe_input_gamepad_axis(const WPE_Input* input, uint32_t player, WPE_GamepadAxis axis);

/**
 * Starts (or replaces) rumble on a pad.
 *
 * @param low,high Low- and high-frequency motor strength (0..1, clamped).
 * @param seconds Duration (clamped to 0..10; 0 stops).
 * @return WPE_RESULT_OK; WPE_RESULT_ERROR_NULL_ARG; WPE_RESULT_ERROR_NOT_FOUND (no pad in the slot);
 *         WPE_RESULT_ERROR_UNSUPPORTED (the pad cannot rumble).
 *
 * @thread_safety Main thread only
 */
WPE_Result wpe_input_rumble(WPE_Input* input, uint32_t player, float low, float high, float seconds);

/* ---- Names (for files and scripts) -------------------------------------------------------------- */

/* Lowercase names: keys "a".."z", "0".."9", "f1".."f12", "space", "return", "escape", "tab",
 * "backspace", "up", "down", "left", "right", "lshift", "rctrl", "kp_5", ... (see input.c);
 * mouse buttons "left", "middle", "right", "x1", "x2"; pad buttons "south", "east", "west",
 * "north" (aliases "a", "b", "x", "y"), "back", "guide", "start", "left_stick", "right_stick",
 * "left_shoulder", "right_shoulder", "dpad_up", "dpad_down", "dpad_left", "dpad_right", "misc1",
 * "right_paddle1", "left_paddle1", "right_paddle2", "left_paddle2", "touchpad"; pad axes "left_x",
 * "left_y", "right_x", "right_y", "left_trigger", "right_trigger". Lookups are case-sensitive.
 * The name functions return NULL for codes without a name; the parsers return false for unknown
 * names. */
bool wpe_input_key_from_name(const char* name, WPE_Key* out_key);
const char* wpe_input_key_name(WPE_Key key);
bool wpe_input_mouse_button_from_name(const char* name, WPE_MouseButton* out_button);
const char* wpe_input_mouse_button_name(WPE_MouseButton button);
bool wpe_input_pad_button_from_name(const char* name, WPE_GamepadButton* out_button);
const char* wpe_input_pad_button_name(WPE_GamepadButton button);
bool wpe_input_pad_axis_from_name(const char* name, WPE_GamepadAxis* out_axis);
const char* wpe_input_pad_axis_name(WPE_GamepadAxis axis);

#endif /* WPE_INPUT_INPUT_H */
