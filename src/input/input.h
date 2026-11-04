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
    WPE_KEY_HOME = 74,
    WPE_KEY_DELETE = 76,
    WPE_KEY_END = 77,
    WPE_KEY_RIGHT = 79,
    WPE_KEY_LEFT = 80,
    WPE_KEY_DOWN = 81,
    WPE_KEY_UP = 82,
    WPE_KEY_KP_ENTER = 88,
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

/* ---- Rebinding ---------------------------------------------------------------------------------- */

/**
 * Replaces the bindings of one action (the player's choice on a controls screen). The action keeps its name,
 * handle and state; the bindings it was given by wpe_input_set_actions() stay its defaults.
 *
 * @return WPE_RESULT_OK; WPE_RESULT_ERROR_NULL_ARG; WPE_RESULT_ERROR_NOT_FOUND (invalid handle);
 *         WPE_RESULT_ERROR_INVALID_ARG (more than WPE_INPUT_MAX_BINDINGS, or an invalid binding; logged).
 *
 * @thread_safety Main thread only
 */
WPE_Result wpe_input_set_bindings(WPE_Input* input, WPE_InputAction action, const WPE_InputBinding* bindings,
                                  uint32_t count);

/** Puts back the default bindings of `action` (WPE_INPUT_ACTION_NONE: of every action). @thread_safety Main
 * thread only */
void wpe_input_reset_bindings(WPE_Input* input, WPE_InputAction action);

/** @return Action `index` (< count) as wpe_input_set_actions() gave it, its default bindings (borrowed until
 * the list changes), or NULL. @thread_safety Main thread only */
const WPE_InputActionDesc* wpe_input_action_default(const WPE_Input* input, uint32_t index);

/** @return Whether two bindings are the same input read the same way (source, code, scale with 0 meaning 1,
 * and range). @thread_safety Any thread */
bool wpe_input_binding_equal(const WPE_InputBinding* a, const WPE_InputBinding* b);

/* Devices a listen accepts (bit flags; 0 means both). */
#define WPE_INPUT_LISTEN_KEYBOARD 0x1u /* Keys and mouse buttons (when the player owns them). */
#define WPE_INPUT_LISTEN_GAMEPAD 0x2u  /* Pad buttons, triggers and stick directions. */

/**
 * Listens for the input to bind: the first key, mouse button, pad button, trigger or stick direction (pushed
 * past halfway) pressed from the next wpe_input_update() on. Whatever is held when listening starts must be
 * let go first. The input heard is swallowed: neither it nor its release reaches the key, mouse, pad and
 * action queries, so the key a player picks doesn't also press a menu button (a stick or trigger stays silent
 * until it is let go). Listening again replaces the previous listen.
 *
 * @param devices WPE_INPUT_LISTEN_* flags (0: both).
 * @param player A slot (its pad, plus the keyboard and mouse if it owns them) or WPE_INPUT_ANY_PLAYER.
 *
 * @thread_safety Main thread only
 */
void wpe_input_listen(WPE_Input* input, uint32_t devices, uint32_t player);

/** Stops listening (a heard input not yet taken is dropped). @thread_safety Main thread only */
void wpe_input_stop_listening(WPE_Input* input);

/** @return Whether a listen is still waiting for an input. @thread_safety Main thread only */
bool wpe_input_listening(const WPE_Input* input);

/**
 * Takes the input a listen heard, once: a key, mouse or pad button binding; a trigger as a pad axis; a stick
 * direction as a pad axis with WPE_AXIS_POSITIVE or WPE_AXIS_NEGATIVE range. Scales are 1.
 *
 * @return true (filling out_binding) the first time after an input was heard; false while waiting or not
 *         listening.
 *
 * @thread_safety Main thread only
 */
bool wpe_input_take_heard(WPE_Input* input, WPE_InputBinding* out_binding);

/* ---- Keyboard and mouse --------------------------------------------------------------------------- */

bool wpe_input_key_down(const WPE_Input* input, WPE_Key key);
bool wpe_input_key_pressed(const WPE_Input* input, WPE_Key key);
bool wpe_input_key_released(const WPE_Input* input, WPE_Key key);

/* ---- Typing (text fields; the window must have text input on, wpe_window_set_text_input()) ---------- */

/** Most bytes of text one frame collects (more is dropped, whole characters at a time). */
#define WPE_INPUT_TEXT_MAX 256u

/** @return What was typed this frame, as UTF-8 (as the keyboard layout composes it: "A" with Shift, "ä" on
 * a Finnish keyboard); "" when nothing was. Borrowed until the next frame. @thread_safety Main thread */
const char* wpe_input_text(const WPE_Input* input);

/** @return How many times a key went down this frame, the operating system's key repeats included (hold
 * Backspace to delete several characters). @thread_safety Main thread */
uint32_t wpe_input_key_typed(const WPE_Input* input, WPE_Key key);

/**
 * Whether keys drive actions (default true). While a text field takes the keyboard, the engine turns it
 * off, so typing "w" in a name doesn't also walk forward; mouse buttons and gamepads keep working, and the
 * key functions above still report every key. Turning it back on leaves keys that are still held (the
 * Return that ended the typing) silent until they are released. Takes effect at the next wpe_input_update.
 *
 * @thread_safety Main thread
 */
void wpe_input_set_keyboard_actions(WPE_Input* input, bool enabled);
bool wpe_input_keyboard_actions(const WPE_Input* input);

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

/* ---- Devices and prompts -------------------------------------------------------------------------- */

typedef enum WPE_InputDevice {
    WPE_INPUT_DEVICE_KEYBOARD = 0, /* The keyboard and mouse. */
    WPE_INPUT_DEVICE_GAMEPAD,
} WPE_InputDevice;

/** How a pad's buttons are labelled. */
typedef enum WPE_GamepadStyle {
    WPE_PAD_STYLE_GENERIC = 0, /* Unknown pads: Xbox-like letters, "Back" and "Start". */
    WPE_PAD_STYLE_XBOX,
    WPE_PAD_STYLE_PLAYSTATION,
    WPE_PAD_STYLE_NINTENDO, /* Positions differ from the letters: SOUTH is B, EAST is A. */
} WPE_GamepadStyle;
#define WPE_PAD_STYLE_COUNT 4u

/**
 * The device `player` used last, for showing the right prompts: key presses, mouse buttons, the wheel and
 * moving the mouse count for the keyboard (when the player owns it); button presses and sticks or triggers
 * pushed past halfway for the player's pad. WPE_INPUT_ANY_PLAYER: whatever anyone used last. Until the player
 * has used either (and after their last-used pad disconnects): the gamepad if they have one, else the
 * keyboard.
 *
 * @thread_safety Main thread only
 */
WPE_InputDevice wpe_input_last_device(const WPE_Input* input, uint32_t player);

/** @return The style of the pad in `player`'s slot (WPE_INPUT_ANY_PLAYER: of the pad used last, else of the
 * first connected), WPE_PAD_STYLE_GENERIC without one. @thread_safety Main thread only */
WPE_GamepadStyle wpe_input_gamepad_style(const WPE_Input* input, uint32_t player);

/**
 * The binding of `action` to show `player`: the first one on the device they used last (keys and mouse
 * buttons for the keyboard, pad buttons and axes for a gamepad).
 *
 * @param direction 0: any binding. +1 / -1: one that pushes the action that way, by the sign of its scale (a
 *        stick half reads 0..1 on its side, so {left_x, NEGATIVE, scale -1} pushes -1); a stick axis read
 * whole counts for both.
 * @return The binding (borrowed until the action's bindings change), or NULL when the action has none there.
 *
 * @thread_safety Main thread only
 */
const WPE_InputBinding* wpe_input_action_prompt(const WPE_Input* input, WPE_InputAction action,
                                                uint32_t player, int direction);

/**
 * Writes the label of a binding for prompts, in ASCII: keys as the player's keyboard layout prints them ("W",
 * "Space", "Left Shift"; keys whose label isn't ASCII use the US one), "Left Mouse", pad buttons as `style`
 * labels them ("A" / "Cross" / "B" for SOUTH, "RB" / "R1" / "R"), "L Stick Up", "RT" / "R2" / "ZR".
 *
 * @return The label's length (it is cut to fit `capacity`, terminator included); 0 for an invalid binding.
 *
 * @thread_safety Main thread only (reads SDL's keymap)
 */
size_t wpe_input_binding_label(const WPE_InputBinding* binding, WPE_GamepadStyle style, char* out,
                               size_t capacity);

/** Style names: "generic", "xbox", "playstation", "nintendo". @thread_safety Any thread */
const char* wpe_input_gamepad_style_name(WPE_GamepadStyle style);
bool wpe_input_gamepad_style_from_name(const char* name, WPE_GamepadStyle* out_style);

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
