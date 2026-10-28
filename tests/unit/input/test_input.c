/*
 * Input tests driven by real SDL events: keyboard and mouse events are pushed into SDL's queue and
 * gamepads are SDL virtual joysticks, so the whole path from SDL to actions runs without hardware.
 */
#include <SDL3/SDL.h>
#include <stdio.h>

#include "input/input.h"
#include "wpe_test.h"

static void on_event(const WPE_PlatformEvent* event, void* user_data) {
    wpe_input_handle_event(user_data, event);
}

/* Starts a frame, lets `push` queue events, then delivers them and evaluates actions. */
static void run_frame(WPE_Input* input) {
    wpe_platform_poll_events(on_event, input);
    wpe_input_update(input);
}

static WPE_Input* create_input(const WPE_InputSettings* settings) {
    WPE_Input* input = NULL;
    WPE_InputDesc desc = {0};
    if (settings) desc.settings = *settings;
    WPE_TEST_ASSERT_OK(wpe_input_create(&desc, &input));
    /* Drain events from earlier tests (e.g. pads detached at the end of the previous case). */
    wpe_platform_poll_events(NULL, NULL);
    return input;
}

static void push_key(SDL_Scancode key, bool down, bool repeat) {
    SDL_Event e;
    SDL_zero(e);
    e.type = down ? SDL_EVENT_KEY_DOWN : SDL_EVENT_KEY_UP;
    e.key.scancode = key;
    e.key.down = down;
    e.key.repeat = repeat;
    WPE_TEST_ASSERT(SDL_PushEvent(&e));
}

static void push_mouse_button(Uint8 button, bool down, float x, float y) {
    SDL_Event e;
    SDL_zero(e);
    e.type = down ? SDL_EVENT_MOUSE_BUTTON_DOWN : SDL_EVENT_MOUSE_BUTTON_UP;
    e.button.button = button;
    e.button.down = down;
    e.button.x = x;
    e.button.y = y;
    WPE_TEST_ASSERT(SDL_PushEvent(&e));
}

static void test_input_names_round_trip(void) {
    for (WPE_Key key = 0; key < WPE_KEY_COUNT; ++key) {
        const char* name = wpe_input_key_name(key);
        if (!name) continue;
        WPE_Key parsed = 0;
        WPE_TEST_ASSERT(wpe_input_key_from_name(name, &parsed));
        WPE_TEST_ASSERT_EQ_UINT(key, parsed);
    }
    WPE_Key key = 0;
    WPE_TEST_ASSERT(wpe_input_key_from_name("w", &key));
    WPE_TEST_ASSERT_EQ_UINT(WPE_KEY_A + ('w' - 'a'), key);
    WPE_TEST_ASSERT(wpe_input_key_from_name("f12", &key) && key == WPE_KEY_F12);
    WPE_TEST_ASSERT(!wpe_input_key_from_name("W", &key)); /* Case-sensitive. */
    WPE_TEST_ASSERT(!wpe_input_key_from_name("nope", &key));
    WPE_TEST_ASSERT(wpe_input_key_name(WPE_KEY_UNKNOWN) == NULL);

    for (uint32_t b = 0; b < WPE_PAD_BUTTON_COUNT; ++b) {
        const char* name = wpe_input_pad_button_name((WPE_GamepadButton)b);
        WPE_TEST_ASSERT(name != NULL);
        WPE_GamepadButton parsed;
        WPE_TEST_ASSERT(wpe_input_pad_button_from_name(name, &parsed) && parsed == b);
    }
    WPE_GamepadButton button;
    WPE_TEST_ASSERT(wpe_input_pad_button_from_name("a", &button) && button == WPE_PAD_SOUTH);
    WPE_TEST_ASSERT(wpe_input_pad_button_from_name("y", &button) && button == WPE_PAD_NORTH);
    WPE_TEST_ASSERT_EQ_STR("south", wpe_input_pad_button_name(WPE_PAD_SOUTH)); /* Canonical name. */
    for (uint32_t a = 0; a < WPE_PAD_AXIS_COUNT; ++a) {
        WPE_GamepadAxis parsed;
        WPE_TEST_ASSERT(wpe_input_pad_axis_from_name(wpe_input_pad_axis_name((WPE_GamepadAxis)a), &parsed));
        WPE_TEST_ASSERT_EQ_UINT(a, parsed);
    }
    for (uint32_t b = 1; b <= WPE_MOUSE_BUTTON_MAX; ++b) {
        WPE_MouseButton parsed;
        WPE_TEST_ASSERT(
            wpe_input_mouse_button_from_name(wpe_input_mouse_button_name((WPE_MouseButton)b), &parsed));
        WPE_TEST_ASSERT_EQ_UINT(b, parsed);
    }
}

static void test_input_keyboard_edges(void) {
    WPE_Input* input = create_input(NULL);
    wpe_input_begin_frame(input);
    push_key(SDL_SCANCODE_W, true, false);
    run_frame(input);
    WPE_TEST_ASSERT(wpe_input_key_down(input, WPE_KEY_A + ('w' - 'a')));
    WPE_TEST_ASSERT(wpe_input_key_pressed(input, WPE_KEY_A + ('w' - 'a')));
    WPE_TEST_ASSERT(!wpe_input_key_released(input, WPE_KEY_A + ('w' - 'a')));

    /* Held: no new edge; OS key repeats are ignored. */
    wpe_input_begin_frame(input);
    push_key(SDL_SCANCODE_W, true, true);
    run_frame(input);
    WPE_TEST_ASSERT(wpe_input_key_down(input, WPE_KEY_A + ('w' - 'a')));
    WPE_TEST_ASSERT(!wpe_input_key_pressed(input, WPE_KEY_A + ('w' - 'a')));

    wpe_input_begin_frame(input);
    push_key(SDL_SCANCODE_W, false, false);
    run_frame(input);
    WPE_TEST_ASSERT(!wpe_input_key_down(input, WPE_KEY_A + ('w' - 'a')));
    WPE_TEST_ASSERT(wpe_input_key_released(input, WPE_KEY_A + ('w' - 'a')));

    /* A tap within one frame reports both edges. */
    wpe_input_begin_frame(input);
    push_key(SDL_SCANCODE_SPACE, true, false);
    push_key(SDL_SCANCODE_SPACE, false, false);
    run_frame(input);
    WPE_TEST_ASSERT(!wpe_input_key_down(input, WPE_KEY_SPACE));
    WPE_TEST_ASSERT(wpe_input_key_pressed(input, WPE_KEY_SPACE));
    WPE_TEST_ASSERT(wpe_input_key_released(input, WPE_KEY_SPACE));

    /* Losing focus releases held keys. */
    wpe_input_begin_frame(input);
    push_key(SDL_SCANCODE_LSHIFT, true, false);
    run_frame(input);
    wpe_input_begin_frame(input);
    SDL_Event focus;
    SDL_zero(focus);
    focus.type = SDL_EVENT_WINDOW_FOCUS_LOST;
    WPE_TEST_ASSERT(SDL_PushEvent(&focus));
    run_frame(input);
    WPE_TEST_ASSERT(!wpe_input_key_down(input, WPE_KEY_LSHIFT));
    WPE_TEST_ASSERT(wpe_input_key_released(input, WPE_KEY_LSHIFT));

    /* A host that stops delivering events (the editor stopping a play test) releases everything held. */
    wpe_input_begin_frame(input);
    push_key(SDL_SCANCODE_W, true, false);
    push_mouse_button(SDL_BUTTON_LEFT, true, 1.0f, 1.0f);
    run_frame(input);
    WPE_TEST_ASSERT(wpe_input_key_down(input, WPE_KEY_A + ('w' - 'a')) &&
                    wpe_input_mouse_down(input, WPE_MOUSE_LEFT));
    wpe_input_begin_frame(input);
    wpe_input_release_all(input);
    wpe_input_update(input);
    WPE_TEST_ASSERT(!wpe_input_key_down(input, WPE_KEY_A + ('w' - 'a')) &&
                    !wpe_input_mouse_down(input, WPE_MOUSE_LEFT));
    WPE_TEST_ASSERT(wpe_input_key_released(input, WPE_KEY_A + ('w' - 'a')));
    WPE_TEST_ASSERT(wpe_input_mouse_released(input, WPE_MOUSE_LEFT));
    /* A late release event then changes nothing. */
    wpe_input_begin_frame(input);
    push_key(SDL_SCANCODE_W, false, false);
    run_frame(input);
    WPE_TEST_ASSERT(!wpe_input_key_released(input, WPE_KEY_A + ('w' - 'a')));

    /* Out-of-range queries are harmless. */
    WPE_TEST_ASSERT(!wpe_input_key_down(input, WPE_KEY_COUNT));
    WPE_TEST_ASSERT(!wpe_input_key_down(NULL, WPE_KEY_SPACE));
    wpe_input_destroy(input);
}

static void test_input_mouse(void) {
    WPE_Input* input = create_input(NULL);
    wpe_input_begin_frame(input);
    SDL_Event motion;
    SDL_zero(motion);
    motion.type = SDL_EVENT_MOUSE_MOTION;
    motion.motion.x = 10.0f;
    motion.motion.y = 20.0f;
    motion.motion.xrel = 3.0f;
    motion.motion.yrel = -1.0f;
    WPE_TEST_ASSERT(SDL_PushEvent(&motion));
    motion.motion.x = 12.0f;
    motion.motion.xrel = 2.0f;
    motion.motion.yrel = -1.0f;
    WPE_TEST_ASSERT(SDL_PushEvent(&motion));
    SDL_Event wheel;
    SDL_zero(wheel);
    wheel.type = SDL_EVENT_MOUSE_WHEEL;
    wheel.wheel.y = 1.0f;
    WPE_TEST_ASSERT(SDL_PushEvent(&wheel));
    wheel.wheel.direction = SDL_MOUSEWHEEL_FLIPPED; /* Natural scrolling reports inverted values. */
    wheel.wheel.y = -2.0f;
    WPE_TEST_ASSERT(SDL_PushEvent(&wheel));
    push_mouse_button(SDL_BUTTON_LEFT, true, 12.0f, 20.0f);
    run_frame(input);
    float x, y;
    wpe_input_mouse_position(input, &x, &y);
    WPE_TEST_ASSERT_NEAR(12.0, x, 0.0);
    WPE_TEST_ASSERT_NEAR(20.0, y, 0.0);
    wpe_input_mouse_delta(input, &x, &y);
    WPE_TEST_ASSERT_NEAR(5.0, x, 0.0);
    WPE_TEST_ASSERT_NEAR(-2.0, y, 0.0);
    wpe_input_mouse_wheel(input, &x, &y);
    WPE_TEST_ASSERT_NEAR(3.0, y, 0.0);
    WPE_TEST_ASSERT(wpe_input_mouse_down(input, WPE_MOUSE_LEFT));
    WPE_TEST_ASSERT(wpe_input_mouse_pressed(input, WPE_MOUSE_LEFT));
    WPE_TEST_ASSERT(!wpe_input_mouse_down(input, WPE_MOUSE_RIGHT));

    /* Deltas and wheel are per frame; the position persists. */
    wpe_input_begin_frame(input);
    push_mouse_button(SDL_BUTTON_LEFT, false, 12.0f, 20.0f);
    run_frame(input);
    wpe_input_mouse_delta(input, &x, &y);
    WPE_TEST_ASSERT(x == 0.0f && y == 0.0f);
    wpe_input_mouse_position(input, &x, &y);
    WPE_TEST_ASSERT_NEAR(12.0, x, 0.0);
    WPE_TEST_ASSERT(wpe_input_mouse_released(input, WPE_MOUSE_LEFT));

    WPE_TEST_ASSERT(!wpe_input_mouse_captured(input));
    wpe_input_set_mouse_captured(input, true);
    WPE_TEST_ASSERT(wpe_input_mouse_captured(input));
    wpe_input_destroy(input);
}

static WPE_InputActionDesc make_action(const char* name) {
    WPE_InputActionDesc action = {0};
    snprintf(action.name, sizeof action.name, "%s", name);
    return action;
}

static void add_binding(WPE_InputActionDesc* action, WPE_InputSource source, uint32_t code, float scale,
                        WPE_AxisRange range) {
    action->bindings[action->binding_count++] =
        (WPE_InputBinding){.source = source, .code = code, .scale = scale, .range = range};
}

static void test_input_keyboard_actions(void) {
    WPE_InputSettings settings = wpe_input_settings_default();
    settings.keyboard_player = 1;
    WPE_Input* input = create_input(&settings);
    WPE_InputActionDesc actions[2] = {make_action("move_x"), make_action("fire")};
    add_binding(&actions[0], WPE_INPUT_SOURCE_KEY, WPE_KEY_A + ('d' - 'a'), 1.0f, WPE_AXIS_FULL);
    add_binding(&actions[0], WPE_INPUT_SOURCE_KEY, WPE_KEY_A, -1.0f, WPE_AXIS_FULL);
    add_binding(&actions[1], WPE_INPUT_SOURCE_MOUSE_BUTTON, WPE_MOUSE_LEFT, 0.0f, WPE_AXIS_FULL);
    WPE_TEST_ASSERT_OK(wpe_input_set_actions(input, actions, 2));
    const WPE_InputAction move = wpe_input_find_action(input, "move_x");
    const WPE_InputAction fire = wpe_input_find_action(input, "fire");
    WPE_TEST_ASSERT(move != WPE_INPUT_ACTION_NONE && fire != WPE_INPUT_ACTION_NONE && move != fire);
    WPE_TEST_ASSERT_EQ_UINT(WPE_INPUT_ACTION_NONE, wpe_input_find_action(input, "jump"));
    WPE_TEST_ASSERT_EQ_UINT(2, wpe_input_action_count(input));
    WPE_TEST_ASSERT_EQ_STR("fire", wpe_input_action_desc(input, 1)->name);

    wpe_input_begin_frame(input);
    push_key(SDL_SCANCODE_A, true, false);
    run_frame(input);
    /* The keyboard belongs to player 1 (second slot), and to "any player". */
    WPE_TEST_ASSERT_NEAR(-1.0, wpe_input_action_value(input, move, 1), 0.0);
    WPE_TEST_ASSERT_NEAR(-1.0, wpe_input_action_value(input, move, WPE_INPUT_ANY_PLAYER), 0.0);
    WPE_TEST_ASSERT_NEAR(0.0, wpe_input_action_value(input, move, 0), 0.0);
    WPE_TEST_ASSERT(wpe_input_action_down(input, move, 1) && wpe_input_action_pressed(input, move, 1));
    WPE_TEST_ASSERT(!wpe_input_action_down(input, move, 0));

    /* Opposite keys cancel out: the action is released. */
    wpe_input_begin_frame(input);
    push_key(SDL_SCANCODE_D, true, false);
    run_frame(input);
    WPE_TEST_ASSERT_NEAR(0.0, wpe_input_action_value(input, move, 1), 0.0);
    WPE_TEST_ASSERT(!wpe_input_action_down(input, move, 1));
    WPE_TEST_ASSERT(wpe_input_action_released(input, move, 1));

    /* A click within one frame: pressed and released, never down. */
    wpe_input_begin_frame(input);
    push_mouse_button(SDL_BUTTON_LEFT, true, 0, 0);
    push_mouse_button(SDL_BUTTON_LEFT, false, 0, 0);
    run_frame(input);
    WPE_TEST_ASSERT(wpe_input_action_pressed(input, fire, WPE_INPUT_ANY_PLAYER));
    WPE_TEST_ASSERT(wpe_input_action_released(input, fire, WPE_INPUT_ANY_PLAYER));
    WPE_TEST_ASSERT(!wpe_input_action_down(input, fire, WPE_INPUT_ANY_PLAYER));

    /* Invalid handles and players read as idle. */
    WPE_TEST_ASSERT(!wpe_input_action_down(input, WPE_INPUT_ACTION_NONE, 0));
    WPE_TEST_ASSERT(!wpe_input_action_down(input, 99, 0));
    WPE_TEST_ASSERT_NEAR(0.0, wpe_input_action_value(input, move, WPE_INPUT_MAX_PLAYERS), 0.0);
    wpe_input_destroy(input);
}

static void test_input_rejects_invalid_configuration(void) {
    wpe_log_set_stderr_enabled(false);
    WPE_Input* input = create_input(NULL);
    WPE_InputActionDesc actions[2] = {make_action("jump"), make_action("jump")};
    WPE_TEST_ASSERT_EQ_INT(WPE_RESULT_ERROR_INVALID_ARG, wpe_input_set_actions(input, actions, 2));
    actions[1] = make_action("");
    WPE_TEST_ASSERT_EQ_INT(WPE_RESULT_ERROR_INVALID_ARG, wpe_input_set_actions(input, actions, 2));
    actions[1] = make_action("bad");
    add_binding(&actions[1], WPE_INPUT_SOURCE_PAD_AXIS, WPE_PAD_AXIS_COUNT, 1.0f, WPE_AXIS_FULL);
    WPE_TEST_ASSERT_EQ_INT(WPE_RESULT_ERROR_INVALID_ARG, wpe_input_set_actions(input, actions, 2));
    actions[1] = make_action("bad");
    add_binding(&actions[1], WPE_INPUT_SOURCE_MOUSE_BUTTON, 0, 1.0f, WPE_AXIS_FULL);
    WPE_TEST_ASSERT_EQ_INT(WPE_RESULT_ERROR_INVALID_ARG, wpe_input_set_actions(input, actions, 2));
    actions[1] = make_action("bad");
    actions[1].binding_count = WPE_INPUT_MAX_BINDINGS + 1;
    WPE_TEST_ASSERT_EQ_INT(WPE_RESULT_ERROR_INVALID_ARG, wpe_input_set_actions(input, actions, 2));
    WPE_TEST_ASSERT_EQ_INT(WPE_RESULT_ERROR_CAPACITY,
                           wpe_input_set_actions(input, actions, WPE_INPUT_MAX_ACTIONS + 1));
    WPE_TEST_ASSERT_EQ_UINT(0, wpe_input_action_count(input)); /* Failed calls change nothing. */
    WPE_TEST_ASSERT_OK(wpe_input_set_actions(input, NULL, 0));

    WPE_InputSettings settings = wpe_input_settings_default();
    settings.stick_outer = 0.1f; /* Below the dead zone. */
    WPE_TEST_ASSERT_EQ_INT(WPE_RESULT_ERROR_INVALID_ARG, wpe_input_set_settings(input, &settings));
    settings = (WPE_InputSettings){.keyboard_player = WPE_INPUT_MAX_PLAYERS};
    WPE_TEST_ASSERT_EQ_INT(WPE_RESULT_ERROR_INVALID_ARG, wpe_input_set_settings(input, &settings));
    settings = (WPE_InputSettings){.press_threshold = 2.0f};
    WPE_Input* bad = NULL;
    WPE_TEST_ASSERT_EQ_INT(WPE_RESULT_ERROR_INVALID_ARG,
                           wpe_input_create(&(WPE_InputDesc){.settings = settings}, &bad));
    /* Zeroed fields mean defaults. */
    WPE_TEST_ASSERT_OK(wpe_input_set_settings(input, &(WPE_InputSettings){0}));
    WPE_TEST_ASSERT_NEAR(0.2, wpe_input_settings(input).stick_deadzone, 1e-6);
    wpe_input_destroy(input);
    wpe_log_set_stderr_enabled(true);
}

/* ---- Virtual gamepads ------------------------------------------------------------------------------ */

typedef struct VirtualPad {
    SDL_JoystickID id;
    SDL_Joystick* joystick;
    Uint16 rumble_low, rumble_high;
    int rumble_calls;
} VirtualPad;

static bool SDLCALL record_rumble(void* user_data, Uint16 low, Uint16 high) {
    VirtualPad* pad = user_data;
    pad->rumble_low = low;
    pad->rumble_high = high;
    ++pad->rumble_calls;
    return true;
}

static void attach_pad(VirtualPad* pad, const char* name) {
    *pad = (VirtualPad){0};
    SDL_VirtualJoystickDesc desc;
    SDL_INIT_INTERFACE(&desc);
    desc.type = SDL_JOYSTICK_TYPE_GAMEPAD;
    desc.naxes = SDL_GAMEPAD_AXIS_COUNT;
    desc.nbuttons = SDL_GAMEPAD_BUTTON_COUNT;
    desc.name = name;
    desc.Rumble = record_rumble;
    desc.userdata = pad;
    pad->id = SDL_AttachVirtualJoystick(&desc);
    WPE_TEST_ASSERT_MSG(pad->id != 0, "virtual joystick: %s", SDL_GetError());
    pad->joystick = SDL_OpenJoystick(pad->id);
    WPE_TEST_ASSERT_MSG(pad->joystick != NULL, "open virtual joystick: %s", SDL_GetError());
}

static void detach_pad(VirtualPad* pad) {
    SDL_CloseJoystick(pad->joystick);
    SDL_DetachVirtualJoystick(pad->id);
    pad->joystick = NULL;
}

static void set_pad_axis(VirtualPad* pad, SDL_GamepadAxis axis, float value) {
    WPE_TEST_ASSERT(SDL_SetJoystickVirtualAxis(pad->joystick, axis, (Sint16)(value * 32767.0f)));
}

static void test_input_gamepad_slots_and_hot_plug(void) {
    WPE_Input* input = create_input(NULL);
    WPE_TEST_ASSERT_EQ_UINT(0, wpe_input_gamepad_count(input));
    VirtualPad first, second;
    attach_pad(&first, "First Pad");
    attach_pad(&second, "Second Pad");
    wpe_input_begin_frame(input);
    run_frame(input);
    WPE_TEST_ASSERT_EQ_UINT(2, wpe_input_gamepad_count(input));
    WPE_TEST_ASSERT(wpe_input_gamepad_connected(input, 0) && wpe_input_gamepad_connected(input, 1));
    WPE_TEST_ASSERT_EQ_STR("First Pad", wpe_input_gamepad_name(input, 0));
    WPE_TEST_ASSERT_EQ_STR("Second Pad", wpe_input_gamepad_name(input, 1));

    /* Unplugging frees the slot; a new pad takes the lowest free one. */
    detach_pad(&first);
    wpe_input_begin_frame(input);
    run_frame(input);
    WPE_TEST_ASSERT_EQ_UINT(1, wpe_input_gamepad_count(input));
    WPE_TEST_ASSERT(!wpe_input_gamepad_connected(input, 0));
    WPE_TEST_ASSERT(wpe_input_gamepad_name(input, 0) == NULL);
    WPE_TEST_ASSERT_EQ_STR("Second Pad", wpe_input_gamepad_name(input, 1));
    attach_pad(&first, "Third Pad");
    wpe_input_begin_frame(input);
    run_frame(input);
    WPE_TEST_ASSERT_EQ_STR("Third Pad", wpe_input_gamepad_name(input, 0));
    detach_pad(&first);
    detach_pad(&second);
    wpe_input_begin_frame(input);
    run_frame(input);
    WPE_TEST_ASSERT_EQ_UINT(0, wpe_input_gamepad_count(input));
    wpe_input_destroy(input);
}

static void test_input_gamepad_buttons_axes_and_actions(void) {
    WPE_Input* input = create_input(NULL);
    WPE_InputActionDesc actions[3] = {make_action("jump"), make_action("move_y"), make_action("menu_up")};
    add_binding(&actions[0], WPE_INPUT_SOURCE_PAD_BUTTON, WPE_PAD_SOUTH, 1.0f, WPE_AXIS_FULL);
    add_binding(&actions[0], WPE_INPUT_SOURCE_KEY, WPE_KEY_SPACE, 1.0f, WPE_AXIS_FULL);
    add_binding(&actions[1], WPE_INPUT_SOURCE_PAD_AXIS, WPE_PAD_LEFT_Y, -1.0f, WPE_AXIS_FULL);
    add_binding(&actions[2], WPE_INPUT_SOURCE_PAD_AXIS, WPE_PAD_LEFT_Y, 1.0f, WPE_AXIS_NEGATIVE);
    WPE_TEST_ASSERT_OK(wpe_input_set_actions(input, actions, 3));
    const WPE_InputAction jump = wpe_input_find_action(input, "jump");
    const WPE_InputAction move_y = wpe_input_find_action(input, "move_y");
    const WPE_InputAction menu_up = wpe_input_find_action(input, "menu_up");

    VirtualPad pad, other;
    attach_pad(&pad, "Pad");
    attach_pad(&other, "Other");
    wpe_input_begin_frame(input);
    run_frame(input);

    /* Button edges. */
    wpe_input_begin_frame(input);
    WPE_TEST_ASSERT(SDL_SetJoystickVirtualButton(pad.joystick, SDL_GAMEPAD_BUTTON_SOUTH, true));
    run_frame(input);
    WPE_TEST_ASSERT(wpe_input_gamepad_down(input, 0, WPE_PAD_SOUTH));
    WPE_TEST_ASSERT(wpe_input_gamepad_pressed(input, 0, WPE_PAD_SOUTH));
    WPE_TEST_ASSERT(wpe_input_gamepad_pressed(input, WPE_INPUT_ANY_PLAYER, WPE_PAD_SOUTH));
    WPE_TEST_ASSERT(!wpe_input_gamepad_down(input, 1, WPE_PAD_SOUTH));
    WPE_TEST_ASSERT(wpe_input_action_pressed(input, jump, 0));
    WPE_TEST_ASSERT(wpe_input_action_down(input, jump, WPE_INPUT_ANY_PLAYER));
    WPE_TEST_ASSERT(!wpe_input_action_down(input, jump, 1)); /* The other pad's player. */

    wpe_input_begin_frame(input);
    WPE_TEST_ASSERT(SDL_SetJoystickVirtualButton(pad.joystick, SDL_GAMEPAD_BUTTON_SOUTH, false));
    run_frame(input);
    WPE_TEST_ASSERT(!wpe_input_gamepad_down(input, 0, WPE_PAD_SOUTH));
    WPE_TEST_ASSERT(wpe_input_gamepad_released(input, 0, WPE_PAD_SOUTH));
    WPE_TEST_ASSERT(wpe_input_action_released(input, jump, 0));

    /* Stick dead zone: small deflections read as zero, larger ones are rescaled from the edge. */
    wpe_input_begin_frame(input);
    set_pad_axis(&pad, SDL_GAMEPAD_AXIS_LEFTX, 0.1f);
    set_pad_axis(&pad, SDL_GAMEPAD_AXIS_LEFTY, 0.1f);
    run_frame(input);
    WPE_TEST_ASSERT_NEAR(0.0, wpe_input_gamepad_axis(input, 0, WPE_PAD_LEFT_X), 0.0);
    WPE_TEST_ASSERT_NEAR(0.0, wpe_input_gamepad_axis(input, 0, WPE_PAD_LEFT_Y), 0.0);
    wpe_input_begin_frame(input);
    set_pad_axis(&pad, SDL_GAMEPAD_AXIS_LEFTX, 0.0f);
    set_pad_axis(&pad, SDL_GAMEPAD_AXIS_LEFTY, -0.5f); /* Stick pushed up. */
    run_frame(input);
    WPE_TEST_ASSERT_NEAR(-(0.5 - 0.2) / (0.95 - 0.2), wpe_input_gamepad_axis(input, 0, WPE_PAD_LEFT_Y), 1e-3);
    WPE_TEST_ASSERT_NEAR((0.5 - 0.2) / (0.95 - 0.2), wpe_input_action_value(input, move_y, 0), 1e-3);
    WPE_TEST_ASSERT(!wpe_input_action_down(input, menu_up, 0)); /* 0.4 < press threshold 0.5. */
    wpe_input_begin_frame(input);
    set_pad_axis(&pad, SDL_GAMEPAD_AXIS_LEFTY, -1.0f);
    run_frame(input);
    WPE_TEST_ASSERT_NEAR(-1.0, wpe_input_gamepad_axis(input, 0, WPE_PAD_LEFT_Y), 1e-6);
    WPE_TEST_ASSERT_NEAR(1.0, wpe_input_action_value(input, move_y, 0), 1e-6);
    WPE_TEST_ASSERT(wpe_input_action_pressed(input, menu_up, 0));
    WPE_TEST_ASSERT_NEAR(1.0, wpe_input_action_value(input, menu_up, WPE_INPUT_ANY_PLAYER), 1e-6);
    /* Pushing down does not count for the "negative half" binding. */
    wpe_input_begin_frame(input);
    set_pad_axis(&pad, SDL_GAMEPAD_AXIS_LEFTY, 1.0f);
    run_frame(input);
    WPE_TEST_ASSERT_NEAR(0.0, wpe_input_action_value(input, menu_up, 0), 0.0);
    WPE_TEST_ASSERT(wpe_input_action_released(input, menu_up, 0));

    /* Triggers: 0..1 with a linear dead zone. */
    wpe_input_begin_frame(input);
    WPE_TEST_ASSERT(
        SDL_SetJoystickVirtualAxis(pad.joystick, SDL_GAMEPAD_AXIS_RIGHT_TRIGGER, SDL_JOYSTICK_AXIS_MAX));
    run_frame(input);
    WPE_TEST_ASSERT_NEAR(1.0, wpe_input_gamepad_axis(input, 0, WPE_PAD_RIGHT_TRIGGER), 1e-6);
    WPE_TEST_ASSERT_NEAR(1.0, wpe_input_gamepad_axis(input, WPE_INPUT_ANY_PLAYER, WPE_PAD_RIGHT_TRIGGER),
                         1e-6);
    WPE_TEST_ASSERT_NEAR(0.0, wpe_input_gamepad_axis(input, 1, WPE_PAD_RIGHT_TRIGGER), 0.0);
    wpe_input_begin_frame(input);
    WPE_TEST_ASSERT(
        SDL_SetJoystickVirtualAxis(pad.joystick, SDL_GAMEPAD_AXIS_RIGHT_TRIGGER, SDL_JOYSTICK_AXIS_MIN));
    run_frame(input);
    WPE_TEST_ASSERT_NEAR(0.0, wpe_input_gamepad_axis(input, 0, WPE_PAD_RIGHT_TRIGGER), 0.0);

    /* Rumble reaches the device (scaled to 16 bits). */
    WPE_TEST_ASSERT_OK(wpe_input_rumble(input, 0, 0.5f, 2.0f, 0.25f));
    WPE_TEST_ASSERT_EQ_INT(1, pad.rumble_calls);
    WPE_TEST_ASSERT_EQ_UINT(32767, pad.rumble_low);
    WPE_TEST_ASSERT_EQ_UINT(65535, pad.rumble_high); /* Clamped to full strength. */
    WPE_TEST_ASSERT_EQ_INT(WPE_RESULT_ERROR_NOT_FOUND, wpe_input_rumble(input, 3, 1, 1, 1));

    /* A pad that disconnects while a button is held stops contributing. */
    wpe_input_begin_frame(input);
    WPE_TEST_ASSERT(SDL_SetJoystickVirtualButton(other.joystick, SDL_GAMEPAD_BUTTON_SOUTH, true));
    run_frame(input);
    WPE_TEST_ASSERT(wpe_input_action_down(input, jump, 1));
    detach_pad(&other);
    wpe_input_begin_frame(input);
    run_frame(input);
    WPE_TEST_ASSERT(!wpe_input_action_down(input, jump, 1));
    WPE_TEST_ASSERT(wpe_input_action_released(input, jump, 1));

    detach_pad(&pad);
    wpe_input_begin_frame(input);
    run_frame(input);
    wpe_input_destroy(input);
}

static void test_input_without_gamepads(void) {
    WPE_Input* input = NULL;
    WPE_TEST_ASSERT_OK(wpe_input_create(&(WPE_InputDesc){.no_gamepads = true}, &input));
    WPE_TEST_ASSERT_EQ_UINT(0, wpe_input_gamepad_count(input));
    WPE_TEST_ASSERT_EQ_INT(WPE_RESULT_ERROR_NOT_FOUND, wpe_input_rumble(input, 0, 1, 1, 1));
    wpe_input_begin_frame(input);
    wpe_input_update(input);
    wpe_input_destroy(input);
    wpe_input_destroy(NULL);
    WPE_TEST_ASSERT_EQ_INT(WPE_RESULT_ERROR_NULL_ARG, wpe_input_create(NULL, NULL));
}

WPE_TEST_MAIN(WPE_TEST_CASE(test_input_names_round_trip), WPE_TEST_CASE(test_input_keyboard_edges),
              WPE_TEST_CASE(test_input_mouse), WPE_TEST_CASE(test_input_keyboard_actions),
              WPE_TEST_CASE(test_input_rejects_invalid_configuration),
              WPE_TEST_CASE(test_input_gamepad_slots_and_hot_plug),
              WPE_TEST_CASE(test_input_gamepad_buttons_axes_and_actions),
              WPE_TEST_CASE(test_input_without_gamepads))
