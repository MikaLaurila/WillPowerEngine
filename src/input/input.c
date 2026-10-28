#define WPE_LOG_MODULE "input"
#include "input/input.h"

#include <SDL3/SDL.h>
#include <math.h>
#include <string.h>

#include "core/alloc.h"
#include "core/debug.h"
#include "core/log.h"

/* The engine's codes are SDL's (scancodes are USB HID usages); the names below are the engine's own. */
static_assert(WPE_KEY_COUNT == SDL_SCANCODE_COUNT, "key codes mirror SDL scancodes");
#define WPE__SAME(a, b) ((int)(a) == (int)(b))
static_assert(WPE__SAME(WPE_KEY_A, SDL_SCANCODE_A) && WPE__SAME(WPE_KEY_SPACE, SDL_SCANCODE_SPACE) &&
                  WPE__SAME(WPE_KEY_UP, SDL_SCANCODE_UP) && WPE__SAME(WPE_KEY_RALT, SDL_SCANCODE_RALT),
              "key codes mirror SDL scancodes");
static_assert(WPE__SAME(WPE_MOUSE_LEFT, SDL_BUTTON_LEFT) && WPE__SAME(WPE_MOUSE_RIGHT, SDL_BUTTON_RIGHT) &&
                  WPE__SAME(WPE_MOUSE_X2, SDL_BUTTON_X2),
              "mouse buttons mirror SDL");
static_assert(WPE__SAME(WPE_PAD_SOUTH, SDL_GAMEPAD_BUTTON_SOUTH) &&
                  WPE__SAME(WPE_PAD_DPAD_RIGHT, SDL_GAMEPAD_BUTTON_DPAD_RIGHT) &&
                  WPE__SAME(WPE_PAD_TOUCHPAD, SDL_GAMEPAD_BUTTON_TOUCHPAD),
              "pad buttons mirror SDL");
static_assert(WPE__SAME(WPE_PAD_LEFT_X, SDL_GAMEPAD_AXIS_LEFTX) &&
                  WPE__SAME(WPE_PAD_RIGHT_TRIGGER, SDL_GAMEPAD_AXIS_RIGHT_TRIGGER) &&
                  WPE__SAME(WPE_PAD_AXIS_COUNT, SDL_GAMEPAD_AXIS_COUNT),
              "pad axes mirror SDL");

/* Per-key / per-button state bits. */
#define WPE__DOWN 0x1u
#define WPE__PRESSED 0x2u
#define WPE__RELEASED 0x4u

/* Action state slots: one per player plus "any player". */
#define WPE__SLOTS (WPE_INPUT_MAX_PLAYERS + 1u)
#define WPE__ANY_SLOT WPE_INPUT_MAX_PLAYERS

typedef struct wpe__Gamepad {
    SDL_Gamepad* pad; /* NULL: slot free. */
    SDL_JoystickID id;
    uint32_t down, pressed, released; /* Button bit masks. */
    float raw[WPE_PAD_AXIS_COUNT];    /* -1..1 sticks, 0..1 triggers, before dead zones. */
    float axis[WPE_PAD_AXIS_COUNT];   /* Dead-zoned. */
} wpe__Gamepad;

typedef struct wpe__ActionState {
    float value[WPE__SLOTS];
    uint8_t flags[WPE__SLOTS];
} wpe__ActionState;

struct WPE_Input {
    WPE_InputSettings settings;
    bool gamepads_enabled;

    uint8_t keys[WPE_KEY_COUNT];
    uint8_t mouse_buttons[WPE_MOUSE_BUTTON_MAX + 1];
    float mouse_x, mouse_y, mouse_dx, mouse_dy, wheel_x, wheel_y;
    bool mouse_captured;

    wpe__Gamepad pads[WPE_INPUT_MAX_PLAYERS];

    WPE_InputActionDesc actions[WPE_INPUT_MAX_ACTIONS];
    wpe__ActionState action_states[WPE_INPUT_MAX_ACTIONS];
    uint32_t action_count;
};

/* ---- Names ------------------------------------------------------------------------------------- */

typedef struct wpe__Name {
    const char* name;
    uint32_t code;
} wpe__Name;

static const wpe__Name wpe__key_names[] = {
    {"a", 4},           {"b", 5},          {"c", 6},
    {"d", 7},           {"e", 8},          {"f", 9},
    {"g", 10},          {"h", 11},         {"i", 12},
    {"j", 13},          {"k", 14},         {"l", 15},
    {"m", 16},          {"n", 17},         {"o", 18},
    {"p", 19},          {"q", 20},         {"r", 21},
    {"s", 22},          {"t", 23},         {"u", 24},
    {"v", 25},          {"w", 26},         {"x", 27},
    {"y", 28},          {"z", 29},         {"1", 30},
    {"2", 31},          {"3", 32},         {"4", 33},
    {"5", 34},          {"6", 35},         {"7", 36},
    {"8", 37},          {"9", 38},         {"0", 39},
    {"return", 40},     {"escape", 41},    {"backspace", 42},
    {"tab", 43},        {"space", 44},     {"minus", 45},
    {"equals", 46},     {"lbracket", 47},  {"rbracket", 48},
    {"backslash", 49},  {"semicolon", 51}, {"apostrophe", 52},
    {"grave", 53},      {"comma", 54},     {"period", 55},
    {"slash", 56},      {"capslock", 57},  {"f1", 58},
    {"f2", 59},         {"f3", 60},        {"f4", 61},
    {"f5", 62},         {"f6", 63},        {"f7", 64},
    {"f8", 65},         {"f9", 66},        {"f10", 67},
    {"f11", 68},        {"f12", 69},       {"printscreen", 70},
    {"scrolllock", 71}, {"pause", 72},     {"insert", 73},
    {"home", 74},       {"pageup", 75},    {"delete", 76},
    {"end", 77},        {"pagedown", 78},  {"right", 79},
    {"left", 80},       {"down", 81},      {"up", 82},
    {"numlock", 83},    {"kp_divide", 84}, {"kp_multiply", 85},
    {"kp_minus", 86},   {"kp_plus", 87},   {"kp_enter", 88},
    {"kp_1", 89},       {"kp_2", 90},      {"kp_3", 91},
    {"kp_4", 92},       {"kp_5", 93},      {"kp_6", 94},
    {"kp_7", 95},       {"kp_8", 96},      {"kp_9", 97},
    {"kp_0", 98},       {"kp_period", 99}, {"lctrl", 224},
    {"lshift", 225},    {"lalt", 226},     {"lgui", 227},
    {"rctrl", 228},     {"rshift", 229},   {"ralt", 230},
    {"rgui", 231},
};

static const wpe__Name wpe__mouse_names[] = {
    {"left", WPE_MOUSE_LEFT}, {"middle", WPE_MOUSE_MIDDLE}, {"right", WPE_MOUSE_RIGHT},
    {"x1", WPE_MOUSE_X1},     {"x2", WPE_MOUSE_X2},
};

/* Canonical names first; aliases after (name lookups by code return the first match). */
static const wpe__Name wpe__pad_button_names[] = {
    {"south", WPE_PAD_SOUTH},
    {"east", WPE_PAD_EAST},
    {"west", WPE_PAD_WEST},
    {"north", WPE_PAD_NORTH},
    {"back", WPE_PAD_BACK},
    {"guide", WPE_PAD_GUIDE},
    {"start", WPE_PAD_START},
    {"left_stick", WPE_PAD_LEFT_STICK},
    {"right_stick", WPE_PAD_RIGHT_STICK},
    {"left_shoulder", WPE_PAD_LEFT_SHOULDER},
    {"right_shoulder", WPE_PAD_RIGHT_SHOULDER},
    {"dpad_up", WPE_PAD_DPAD_UP},
    {"dpad_down", WPE_PAD_DPAD_DOWN},
    {"dpad_left", WPE_PAD_DPAD_LEFT},
    {"dpad_right", WPE_PAD_DPAD_RIGHT},
    {"misc1", WPE_PAD_MISC1},
    {"right_paddle1", WPE_PAD_RIGHT_PADDLE1},
    {"left_paddle1", WPE_PAD_LEFT_PADDLE1},
    {"right_paddle2", WPE_PAD_RIGHT_PADDLE2},
    {"left_paddle2", WPE_PAD_LEFT_PADDLE2},
    {"touchpad", WPE_PAD_TOUCHPAD},
    {"a", WPE_PAD_SOUTH},
    {"b", WPE_PAD_EAST},
    {"x", WPE_PAD_WEST},
    {"y", WPE_PAD_NORTH},
};

static const wpe__Name wpe__pad_axis_names[] = {
    {"left_x", WPE_PAD_LEFT_X},
    {"left_y", WPE_PAD_LEFT_Y},
    {"right_x", WPE_PAD_RIGHT_X},
    {"right_y", WPE_PAD_RIGHT_Y},
    {"left_trigger", WPE_PAD_LEFT_TRIGGER},
    {"right_trigger", WPE_PAD_RIGHT_TRIGGER},
};

#define WPE__COUNT(array) ((uint32_t)(sizeof(array) / sizeof((array)[0])))

static bool wpe__lookup_name(const wpe__Name* names, uint32_t count, const char* name, uint32_t* out_code) {
    if (!name) return false;
    for (uint32_t i = 0; i < count; ++i) {
        if (strcmp(names[i].name, name) == 0) {
            if (out_code) *out_code = names[i].code;
            return true;
        }
    }
    return false;
}

static const char* wpe__lookup_code(const wpe__Name* names, uint32_t count, uint32_t code) {
    for (uint32_t i = 0; i < count; ++i) {
        if (names[i].code == code) return names[i].name;
    }
    return NULL;
}

bool wpe_input_key_from_name(const char* name, WPE_Key* out_key) {
    return wpe__lookup_name(wpe__key_names, WPE__COUNT(wpe__key_names), name, out_key);
}

const char* wpe_input_key_name(WPE_Key key) {
    return wpe__lookup_code(wpe__key_names, WPE__COUNT(wpe__key_names), key);
}

bool wpe_input_mouse_button_from_name(const char* name, WPE_MouseButton* out_button) {
    uint32_t code = 0;
    if (!wpe__lookup_name(wpe__mouse_names, WPE__COUNT(wpe__mouse_names), name, &code)) return false;
    if (out_button) *out_button = (WPE_MouseButton)code;
    return true;
}

const char* wpe_input_mouse_button_name(WPE_MouseButton button) {
    return wpe__lookup_code(wpe__mouse_names, WPE__COUNT(wpe__mouse_names), (uint32_t)button);
}

bool wpe_input_pad_button_from_name(const char* name, WPE_GamepadButton* out_button) {
    uint32_t code = 0;
    if (!wpe__lookup_name(wpe__pad_button_names, WPE__COUNT(wpe__pad_button_names), name, &code))
        return false;
    if (out_button) *out_button = (WPE_GamepadButton)code;
    return true;
}

const char* wpe_input_pad_button_name(WPE_GamepadButton button) {
    return wpe__lookup_code(wpe__pad_button_names, WPE__COUNT(wpe__pad_button_names), (uint32_t)button);
}

bool wpe_input_pad_axis_from_name(const char* name, WPE_GamepadAxis* out_axis) {
    uint32_t code = 0;
    if (!wpe__lookup_name(wpe__pad_axis_names, WPE__COUNT(wpe__pad_axis_names), name, &code)) return false;
    if (out_axis) *out_axis = (WPE_GamepadAxis)code;
    return true;
}

const char* wpe_input_pad_axis_name(WPE_GamepadAxis axis) {
    return wpe__lookup_code(wpe__pad_axis_names, WPE__COUNT(wpe__pad_axis_names), (uint32_t)axis);
}

/* ---- Settings ------------------------------------------------------------------------------------ */

WPE_InputSettings wpe_input_settings_default(void) {
    return (WPE_InputSettings){
        .stick_deadzone = 0.2f, .stick_outer = 0.95f, .trigger_deadzone = 0.1f, .press_threshold = 0.5f};
}

/* Fills zero fields with defaults and validates; false (logged) when out of range. */
static bool wpe__resolve_settings(const WPE_InputSettings* in, WPE_InputSettings* out) {
    const WPE_InputSettings defaults = wpe_input_settings_default();
    *out = *in;
    if (out->stick_deadzone == 0.0f) out->stick_deadzone = defaults.stick_deadzone;
    if (out->stick_outer == 0.0f) out->stick_outer = defaults.stick_outer;
    if (out->trigger_deadzone == 0.0f) out->trigger_deadzone = defaults.trigger_deadzone;
    if (out->press_threshold == 0.0f) out->press_threshold = defaults.press_threshold;
    if (!(out->stick_deadzone > 0.0f && out->stick_deadzone <= 0.9f) ||
        !(out->stick_outer > out->stick_deadzone && out->stick_outer <= 1.0f) ||
        !(out->trigger_deadzone > 0.0f && out->trigger_deadzone <= 0.9f) ||
        !(out->press_threshold >= 0.05f && out->press_threshold <= 1.0f) ||
        out->keyboard_player >= WPE_INPUT_MAX_PLAYERS) {
        wpe_log_error("input settings out of range (dead zones 0..0.9, outer > dead zone and <= 1, threshold "
                      "0.05..1, keyboard player < %u)",
                      WPE_INPUT_MAX_PLAYERS);
        return false;
    }
    return true;
}

WPE_InputSettings wpe_input_settings(const WPE_Input* input) {
    return input ? input->settings : wpe_input_settings_default();
}

WPE_Result wpe_input_set_settings(WPE_Input* input, const WPE_InputSettings* settings) {
    if (!input || !settings) return WPE_RESULT_ERROR_NULL_ARG;
    WPE_InputSettings resolved;
    if (!wpe__resolve_settings(settings, &resolved)) return WPE_RESULT_ERROR_INVALID_ARG;
    input->settings = resolved;
    return WPE_RESULT_OK;
}

/* ---- Gamepads ------------------------------------------------------------------------------------ */

static wpe__Gamepad* wpe__pad_by_id(WPE_Input* input, SDL_JoystickID id) {
    for (uint32_t i = 0; i < WPE_INPUT_MAX_PLAYERS; ++i) {
        if (input->pads[i].pad && input->pads[i].id == id) return &input->pads[i];
    }
    return NULL;
}

static void wpe__open_pad(WPE_Input* input, SDL_JoystickID id) {
    if (wpe__pad_by_id(input, id)) return; /* Already open (enumerated at creation, then announced). */
    uint32_t slot = 0;
    while (slot < WPE_INPUT_MAX_PLAYERS && input->pads[slot].pad)
        ++slot;
    if (slot == WPE_INPUT_MAX_PLAYERS) {
        wpe_log_warn("gamepad ignored: all %u player slots are taken", WPE_INPUT_MAX_PLAYERS);
        return;
    }
    SDL_Gamepad* pad = SDL_OpenGamepad(id);
    if (!pad) {
        wpe_log_warn("cannot open gamepad: %s", SDL_GetError());
        return;
    }
    input->pads[slot] = (wpe__Gamepad){.pad = pad, .id = id};
    SDL_SetGamepadPlayerIndex(pad, (int)slot); /* Lights the player LED where the pad has one. */
    const char* name = SDL_GetGamepadName(pad);
    wpe_log_info("gamepad connected as player %u: %s", slot + 1, name ? name : "(unnamed)");
}

static void wpe__close_pad(WPE_Input* input, SDL_JoystickID id) {
    wpe__Gamepad* pad = wpe__pad_by_id(input, id);
    if (!pad) return;
    wpe_log_info("gamepad of player %u disconnected", (uint32_t)(pad - input->pads) + 1);
    SDL_CloseGamepad(pad->pad);
    *pad = (wpe__Gamepad){0};
}

/* Radial dead zone for a stick, rescaled so motion starts smoothly at the dead zone edge. */
static void wpe__stick_deadzone(const WPE_InputSettings* s, float x, float y, float* out_x, float* out_y) {
    const float magnitude = sqrtf(x * x + y * y);
    if (magnitude <= s->stick_deadzone) {
        *out_x = *out_y = 0.0f;
        return;
    }
    float scaled = (magnitude - s->stick_deadzone) / (s->stick_outer - s->stick_deadzone);
    if (scaled > 1.0f) scaled = 1.0f;
    *out_x = x / magnitude * scaled;
    *out_y = y / magnitude * scaled;
}

static float wpe__trigger_deadzone(const WPE_InputSettings* s, float value) {
    if (value <= s->trigger_deadzone) return 0.0f;
    const float scaled = (value - s->trigger_deadzone) / (1.0f - s->trigger_deadzone);
    return scaled > 1.0f ? 1.0f : scaled;
}

static void wpe__apply_deadzones(const WPE_InputSettings* s, wpe__Gamepad* pad) {
    wpe__stick_deadzone(s, pad->raw[WPE_PAD_LEFT_X], pad->raw[WPE_PAD_LEFT_Y], &pad->axis[WPE_PAD_LEFT_X],
                        &pad->axis[WPE_PAD_LEFT_Y]);
    wpe__stick_deadzone(s, pad->raw[WPE_PAD_RIGHT_X], pad->raw[WPE_PAD_RIGHT_Y], &pad->axis[WPE_PAD_RIGHT_X],
                        &pad->axis[WPE_PAD_RIGHT_Y]);
    pad->axis[WPE_PAD_LEFT_TRIGGER] = wpe__trigger_deadzone(s, pad->raw[WPE_PAD_LEFT_TRIGGER]);
    pad->axis[WPE_PAD_RIGHT_TRIGGER] = wpe__trigger_deadzone(s, pad->raw[WPE_PAD_RIGHT_TRIGGER]);
}

/* ---- Lifecycle and events -------------------------------------------------------------------------- */

WPE_Result wpe_input_create(const WPE_InputDesc* desc, WPE_Input** out_input) {
    if (!out_input) return WPE_RESULT_ERROR_NULL_ARG;
    *out_input = NULL;
    const WPE_InputDesc defaults = {0};
    if (!desc) desc = &defaults;
    WPE_InputSettings settings;
    if (!wpe__resolve_settings(&desc->settings, &settings)) return WPE_RESULT_ERROR_INVALID_ARG;

    WPE_Input* input = wpe_alloc_zeroed(1, sizeof *input);
    if (!input) return WPE_RESULT_ERROR_OUT_OF_MEMORY;
    input->settings = settings;
    if (!desc->no_gamepads) {
        /* Reference-counted by SDL; also initializes the event queue so headless programs get pad events. */
        if (!SDL_InitSubSystem(SDL_INIT_GAMEPAD)) {
            wpe_log_error("gamepad initialization failed: %s", SDL_GetError());
            wpe_free(input);
            return WPE_RESULT_ERROR;
        }
        input->gamepads_enabled = true;
        int count = 0;
        SDL_JoystickID* ids = SDL_GetGamepads(&count);
        for (int i = 0; ids && i < count; ++i)
            wpe__open_pad(input, ids[i]);
        SDL_free(ids);
    }
    *out_input = input;
    return WPE_RESULT_OK;
}

void wpe_input_destroy(WPE_Input* input) {
    if (!input) return;
    for (uint32_t i = 0; i < WPE_INPUT_MAX_PLAYERS; ++i) {
        if (input->pads[i].pad) SDL_CloseGamepad(input->pads[i].pad);
    }
    if (input->gamepads_enabled) SDL_QuitSubSystem(SDL_INIT_GAMEPAD);
    wpe_free(input);
}

void wpe_input_begin_frame(WPE_Input* input) {
    if (!wpe_check(input != NULL)) return;
    for (uint32_t i = 0; i < WPE_KEY_COUNT; ++i)
        input->keys[i] &= WPE__DOWN;
    for (uint32_t i = 0; i <= WPE_MOUSE_BUTTON_MAX; ++i)
        input->mouse_buttons[i] &= WPE__DOWN;
    input->mouse_dx = input->mouse_dy = input->wheel_x = input->wheel_y = 0.0f;
    for (uint32_t i = 0; i < WPE_INPUT_MAX_PLAYERS; ++i)
        input->pads[i].pressed = input->pads[i].released = 0;
}

static void wpe__set_button(uint8_t* state, bool down) {
    if (down && !(*state & WPE__DOWN)) {
        *state |= WPE__DOWN | WPE__PRESSED;
    } else if (!down && (*state & WPE__DOWN)) {
        *state = (uint8_t)((*state & ~WPE__DOWN) | WPE__RELEASED);
    }
}

/* Focus loss: nothing held can be trusted to receive its release event. */
void wpe_input_release_all(WPE_Input* input) {
    if (!wpe_check(input != NULL)) return;
    for (uint32_t i = 0; i < WPE_KEY_COUNT; ++i)
        wpe__set_button(&input->keys[i], false);
    for (uint32_t i = 0; i <= WPE_MOUSE_BUTTON_MAX; ++i)
        wpe__set_button(&input->mouse_buttons[i], false);
}

void wpe_input_handle_event(WPE_Input* input, const WPE_PlatformEvent* platform_event) {
    if (!wpe_check(input != NULL && platform_event != NULL)) return;
    const SDL_Event* event = (const SDL_Event*)platform_event;
    switch (event->type) {
    case SDL_EVENT_KEY_DOWN:
    case SDL_EVENT_KEY_UP:
        if (!event->key.repeat && (uint32_t)event->key.scancode < WPE_KEY_COUNT)
            wpe__set_button(&input->keys[event->key.scancode], event->key.down);
        break;
    case SDL_EVENT_MOUSE_BUTTON_DOWN:
    case SDL_EVENT_MOUSE_BUTTON_UP:
        if (event->button.button >= 1 && event->button.button <= WPE_MOUSE_BUTTON_MAX)
            wpe__set_button(&input->mouse_buttons[event->button.button], event->button.down);
        input->mouse_x = event->button.x;
        input->mouse_y = event->button.y;
        break;
    case SDL_EVENT_MOUSE_MOTION:
        input->mouse_x = event->motion.x;
        input->mouse_y = event->motion.y;
        input->mouse_dx += event->motion.xrel;
        input->mouse_dy += event->motion.yrel;
        break;
    case SDL_EVENT_MOUSE_WHEEL: {
        const float sign = event->wheel.direction == SDL_MOUSEWHEEL_FLIPPED ? -1.0f : 1.0f;
        input->wheel_x += event->wheel.x * sign;
        input->wheel_y += event->wheel.y * sign;
        break;
    }
    case SDL_EVENT_WINDOW_FOCUS_LOST:
        wpe_input_release_all(input);
        break;
    case SDL_EVENT_GAMEPAD_ADDED:
        if (input->gamepads_enabled) wpe__open_pad(input, event->gdevice.which);
        break;
    case SDL_EVENT_GAMEPAD_REMOVED:
        wpe__close_pad(input, event->gdevice.which);
        break;
    case SDL_EVENT_GAMEPAD_BUTTON_DOWN:
    case SDL_EVENT_GAMEPAD_BUTTON_UP: {
        wpe__Gamepad* pad = wpe__pad_by_id(input, event->gbutton.which);
        if (!pad || event->gbutton.button >= WPE_PAD_BUTTON_COUNT) break;
        const uint32_t bit = 1u << event->gbutton.button;
        if (event->gbutton.down && !(pad->down & bit)) {
            pad->down |= bit;
            pad->pressed |= bit;
        } else if (!event->gbutton.down && (pad->down & bit)) {
            pad->down &= ~bit;
            pad->released |= bit;
        }
        break;
    }
    case SDL_EVENT_GAMEPAD_AXIS_MOTION: {
        wpe__Gamepad* pad = wpe__pad_by_id(input, event->gaxis.which);
        if (!pad || event->gaxis.axis >= WPE_PAD_AXIS_COUNT) break;
        float value = (float)event->gaxis.value / 32767.0f; /* -32768 maps just below -1: clamp. */
        if (value < -1.0f) value = -1.0f;
        pad->raw[event->gaxis.axis] = value;
        break;
    }
    default:
        break;
    }
}

/* ---- Actions ------------------------------------------------------------------------------------- */

static bool wpe__binding_valid(const WPE_InputBinding* b) {
    switch (b->source) {
    case WPE_INPUT_SOURCE_KEY:
        return b->code > 0 && b->code < WPE_KEY_COUNT;
    case WPE_INPUT_SOURCE_MOUSE_BUTTON:
        return b->code >= 1 && b->code <= WPE_MOUSE_BUTTON_MAX;
    case WPE_INPUT_SOURCE_PAD_BUTTON:
        return b->code < WPE_PAD_BUTTON_COUNT;
    case WPE_INPUT_SOURCE_PAD_AXIS:
        return b->code < WPE_PAD_AXIS_COUNT &&
               (b->range == WPE_AXIS_FULL || b->range == WPE_AXIS_POSITIVE || b->range == WPE_AXIS_NEGATIVE);
    default:
        return false;
    }
}

WPE_Result wpe_input_set_actions(WPE_Input* input, const WPE_InputActionDesc* actions, uint32_t count) {
    if (!input || (!actions && count > 0)) return WPE_RESULT_ERROR_NULL_ARG;
    if (count > WPE_INPUT_MAX_ACTIONS) {
        wpe_log_error("%u input actions exceed the limit of %u", count, WPE_INPUT_MAX_ACTIONS);
        return WPE_RESULT_ERROR_CAPACITY;
    }
    for (uint32_t i = 0; i < count; ++i) {
        const WPE_InputActionDesc* a = &actions[i];
        if (a->name[0] == '\0' || !memchr(a->name, '\0', sizeof a->name)) {
            wpe_log_error("input action %u has an empty or overlong name", i);
            return WPE_RESULT_ERROR_INVALID_ARG;
        }
        for (uint32_t j = 0; j < i; ++j) {
            if (strcmp(actions[j].name, a->name) == 0) {
                wpe_log_error("input action '%s' is defined twice", a->name);
                return WPE_RESULT_ERROR_INVALID_ARG;
            }
        }
        if (a->binding_count > WPE_INPUT_MAX_BINDINGS) {
            wpe_log_error("input action '%s' has more than %u bindings", a->name, WPE_INPUT_MAX_BINDINGS);
            return WPE_RESULT_ERROR_INVALID_ARG;
        }
        for (uint32_t b = 0; b < a->binding_count; ++b) {
            if (!wpe__binding_valid(&a->bindings[b]) || !isfinite(a->bindings[b].scale)) {
                wpe_log_error("input action '%s': binding %u is invalid", a->name, b);
                return WPE_RESULT_ERROR_INVALID_ARG;
            }
        }
    }
    if (count > 0) memcpy(input->actions, actions, (size_t)count * sizeof *actions);
    memset(input->action_states, 0, sizeof input->action_states);
    input->action_count = count;
    return WPE_RESULT_OK;
}

uint32_t wpe_input_action_count(const WPE_Input* input) {
    return input ? input->action_count : 0;
}

const WPE_InputActionDesc* wpe_input_action_desc(const WPE_Input* input, uint32_t index) {
    return input && index < input->action_count ? &input->actions[index] : NULL;
}

WPE_InputAction wpe_input_find_action(const WPE_Input* input, const char* name) {
    if (!input || !name) return WPE_INPUT_ACTION_NONE;
    for (uint32_t i = 0; i < input->action_count; ++i) {
        if (strcmp(input->actions[i].name, name) == 0) return i + 1;
    }
    return WPE_INPUT_ACTION_NONE;
}

/* Value and press edge of one binding for one player slot (WPE__ANY_SLOT = every device). */
static float wpe__binding_value(const WPE_Input* input, const WPE_InputBinding* b, uint32_t slot,
                                bool* io_pressed) {
    const float scale = b->scale != 0.0f ? b->scale : 1.0f;
    const bool keyboard = slot == WPE__ANY_SLOT || slot == input->settings.keyboard_player;
    switch (b->source) {
    case WPE_INPUT_SOURCE_KEY:
    case WPE_INPUT_SOURCE_MOUSE_BUTTON: {
        if (!keyboard) return 0.0f;
        const uint8_t state =
            b->source == WPE_INPUT_SOURCE_KEY ? input->keys[b->code] : input->mouse_buttons[b->code];
        if (state & WPE__PRESSED) *io_pressed = true;
        return (state & WPE__DOWN) ? scale : 0.0f;
    }
    case WPE_INPUT_SOURCE_PAD_BUTTON:
    case WPE_INPUT_SOURCE_PAD_AXIS:
        break;
    }
    float sum = 0.0f;
    for (uint32_t p = 0; p < WPE_INPUT_MAX_PLAYERS; ++p) {
        const wpe__Gamepad* pad = &input->pads[p];
        if (!pad->pad || (slot != WPE__ANY_SLOT && slot != p)) continue;
        if (b->source == WPE_INPUT_SOURCE_PAD_BUTTON) {
            const uint32_t bit = 1u << b->code;
            if (pad->pressed & bit) *io_pressed = true;
            if (pad->down & bit) sum += scale;
            continue;
        }
        float value = pad->axis[b->code];
        if (b->range == WPE_AXIS_POSITIVE) value = value > 0.0f ? value : 0.0f;
        if (b->range == WPE_AXIS_NEGATIVE) value = value < 0.0f ? -value : 0.0f;
        sum += value * scale;
    }
    return sum;
}

void wpe_input_update(WPE_Input* input) {
    if (!wpe_check(input != NULL)) return;
    for (uint32_t p = 0; p < WPE_INPUT_MAX_PLAYERS; ++p) {
        if (input->pads[p].pad) wpe__apply_deadzones(&input->settings, &input->pads[p]);
    }
    for (uint32_t a = 0; a < input->action_count; ++a) {
        const WPE_InputActionDesc* action = &input->actions[a];
        wpe__ActionState* state = &input->action_states[a];
        for (uint32_t slot = 0; slot < WPE__SLOTS; ++slot) {
            float value = 0.0f;
            bool digital_press = false;
            for (uint32_t b = 0; b < action->binding_count; ++b)
                value += wpe__binding_value(input, &action->bindings[b], slot, &digital_press);
            value = value > 1.0f ? 1.0f : value < -1.0f ? -1.0f : value;
            const bool was_down = (state->flags[slot] & WPE__DOWN) != 0;
            const bool down = fabsf(value) >= input->settings.press_threshold;
            /* A binding pressed and released within the frame still counts as a press (and a release). */
            const bool pressed = !was_down && (down || digital_press);
            const bool released = (was_down || pressed) && !down;
            state->value[slot] = value;
            state->flags[slot] = (uint8_t)((down ? WPE__DOWN : 0u) | (pressed ? WPE__PRESSED : 0u) |
                                           (released ? WPE__RELEASED : 0u));
        }
    }
}

static const wpe__ActionState* wpe__action(const WPE_Input* input, WPE_InputAction action, uint32_t player,
                                           uint32_t* out_slot) {
    if (!input || action == WPE_INPUT_ACTION_NONE || action > input->action_count) return NULL;
    if (player == WPE_INPUT_ANY_PLAYER) {
        *out_slot = WPE__ANY_SLOT;
    } else if (player < WPE_INPUT_MAX_PLAYERS) {
        *out_slot = player;
    } else {
        return NULL;
    }
    return &input->action_states[action - 1];
}

static bool wpe__action_flag(const WPE_Input* input, WPE_InputAction action, uint32_t player, uint8_t flag) {
    uint32_t slot = 0;
    const wpe__ActionState* state = wpe__action(input, action, player, &slot);
    return state && (state->flags[slot] & flag);
}

bool wpe_input_action_down(const WPE_Input* input, WPE_InputAction action, uint32_t player) {
    return wpe__action_flag(input, action, player, WPE__DOWN);
}

bool wpe_input_action_pressed(const WPE_Input* input, WPE_InputAction action, uint32_t player) {
    return wpe__action_flag(input, action, player, WPE__PRESSED);
}

bool wpe_input_action_released(const WPE_Input* input, WPE_InputAction action, uint32_t player) {
    return wpe__action_flag(input, action, player, WPE__RELEASED);
}

float wpe_input_action_value(const WPE_Input* input, WPE_InputAction action, uint32_t player) {
    uint32_t slot = 0;
    const wpe__ActionState* state = wpe__action(input, action, player, &slot);
    return state ? state->value[slot] : 0.0f;
}

/* ---- Keyboard and mouse queries ---------------------------------------------------------------------- */

static bool wpe__key_flag(const WPE_Input* input, WPE_Key key, uint8_t flag) {
    return input && key < WPE_KEY_COUNT && (input->keys[key] & flag);
}

bool wpe_input_key_down(const WPE_Input* input, WPE_Key key) {
    return wpe__key_flag(input, key, WPE__DOWN);
}

bool wpe_input_key_pressed(const WPE_Input* input, WPE_Key key) {
    return wpe__key_flag(input, key, WPE__PRESSED);
}

bool wpe_input_key_released(const WPE_Input* input, WPE_Key key) {
    return wpe__key_flag(input, key, WPE__RELEASED);
}

static bool wpe__mouse_flag(const WPE_Input* input, WPE_MouseButton button, uint8_t flag) {
    return input && button >= 1 && (uint32_t)button <= WPE_MOUSE_BUTTON_MAX &&
           (input->mouse_buttons[button] & flag);
}

bool wpe_input_mouse_down(const WPE_Input* input, WPE_MouseButton button) {
    return wpe__mouse_flag(input, button, WPE__DOWN);
}

bool wpe_input_mouse_pressed(const WPE_Input* input, WPE_MouseButton button) {
    return wpe__mouse_flag(input, button, WPE__PRESSED);
}

bool wpe_input_mouse_released(const WPE_Input* input, WPE_MouseButton button) {
    return wpe__mouse_flag(input, button, WPE__RELEASED);
}

void wpe_input_mouse_position(const WPE_Input* input, float* out_x, float* out_y) {
    if (out_x) *out_x = input ? input->mouse_x : 0.0f;
    if (out_y) *out_y = input ? input->mouse_y : 0.0f;
}

void wpe_input_mouse_delta(const WPE_Input* input, float* out_dx, float* out_dy) {
    if (out_dx) *out_dx = input ? input->mouse_dx : 0.0f;
    if (out_dy) *out_dy = input ? input->mouse_dy : 0.0f;
}

void wpe_input_mouse_wheel(const WPE_Input* input, float* out_x, float* out_y) {
    if (out_x) *out_x = input ? input->wheel_x : 0.0f;
    if (out_y) *out_y = input ? input->wheel_y : 0.0f;
}

void wpe_input_set_mouse_captured(WPE_Input* input, bool captured) {
    if (wpe_check(input != NULL)) input->mouse_captured = captured;
}

bool wpe_input_mouse_captured(const WPE_Input* input) {
    return input && input->mouse_captured;
}

/* ---- Gamepad queries ----------------------------------------------------------------------------- */

uint32_t wpe_input_gamepad_count(const WPE_Input* input) {
    uint32_t count = 0;
    for (uint32_t i = 0; input && i < WPE_INPUT_MAX_PLAYERS; ++i)
        count += input->pads[i].pad != NULL;
    return count;
}

bool wpe_input_gamepad_connected(const WPE_Input* input, uint32_t player) {
    return input && player < WPE_INPUT_MAX_PLAYERS && input->pads[player].pad;
}

const char* wpe_input_gamepad_name(const WPE_Input* input, uint32_t player) {
    if (!wpe_input_gamepad_connected(input, player)) return NULL;
    return SDL_GetGamepadName(input->pads[player].pad);
}

typedef enum wpe__PadMask { WPE__PAD_DOWN, WPE__PAD_PRESSED, WPE__PAD_RELEASED } wpe__PadMask;

/* Combines one of the button masks over the selected pads. */
static uint32_t wpe__pad_mask(const WPE_Input* input, uint32_t player, wpe__PadMask which) {
    uint32_t mask = 0;
    for (uint32_t i = 0; input && i < WPE_INPUT_MAX_PLAYERS; ++i) {
        const wpe__Gamepad* pad = &input->pads[i];
        if (!pad->pad || (player != WPE_INPUT_ANY_PLAYER && player != i)) continue;
        mask |= which == WPE__PAD_DOWN ? pad->down : which == WPE__PAD_PRESSED ? pad->pressed : pad->released;
    }
    return mask;
}

bool wpe_input_gamepad_down(const WPE_Input* input, uint32_t player, WPE_GamepadButton button) {
    return (uint32_t)button < WPE_PAD_BUTTON_COUNT &&
           (wpe__pad_mask(input, player, WPE__PAD_DOWN) & (1u << button));
}

bool wpe_input_gamepad_pressed(const WPE_Input* input, uint32_t player, WPE_GamepadButton button) {
    return (uint32_t)button < WPE_PAD_BUTTON_COUNT &&
           (wpe__pad_mask(input, player, WPE__PAD_PRESSED) & (1u << button));
}

bool wpe_input_gamepad_released(const WPE_Input* input, uint32_t player, WPE_GamepadButton button) {
    return (uint32_t)button < WPE_PAD_BUTTON_COUNT &&
           (wpe__pad_mask(input, player, WPE__PAD_RELEASED) & (1u << button));
}

float wpe_input_gamepad_axis(const WPE_Input* input, uint32_t player, WPE_GamepadAxis axis) {
    if (!input || (uint32_t)axis >= WPE_PAD_AXIS_COUNT) return 0.0f;
    /* Any player: the most deflected pad. */
    float best = 0.0f;
    for (uint32_t i = 0; i < WPE_INPUT_MAX_PLAYERS; ++i) {
        const wpe__Gamepad* pad = &input->pads[i];
        if (!pad->pad || (player != WPE_INPUT_ANY_PLAYER && player != i)) continue;
        if (fabsf(pad->axis[axis]) > fabsf(best)) best = pad->axis[axis];
    }
    return best;
}

WPE_Result wpe_input_rumble(WPE_Input* input, uint32_t player, float low, float high, float seconds) {
    if (!input) return WPE_RESULT_ERROR_NULL_ARG;
    if (!wpe_input_gamepad_connected(input, player)) return WPE_RESULT_ERROR_NOT_FOUND;
    const float l = low > 0.0f ? (low < 1.0f ? low : 1.0f) : 0.0f;
    const float h = high > 0.0f ? (high < 1.0f ? high : 1.0f) : 0.0f;
    const float s = seconds > 0.0f ? (seconds < 10.0f ? seconds : 10.0f) : 0.0f;
    if (SDL_RumbleGamepad(input->pads[player].pad, (Uint16)(l * 65535.0f), (Uint16)(h * 65535.0f),
                          (Uint32)(s * 1000.0f)))
        return WPE_RESULT_OK;
    return WPE_RESULT_ERROR_UNSUPPORTED;
}
