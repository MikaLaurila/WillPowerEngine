/**
 * @file ui.h
 * @brief Retained 2D user interface: rectangles anchored to their parent or the screen, sprites,
 *        text and buttons, laid out and drawn over the scene, with mouse, keyboard and gamepad
 *        navigation.
 *
 * Components registered by wpe_ui_create():
 * - WPE_COMP_UI (WPE_UiRect): makes the entity a UI element and places its rectangle. Each corner
 *   is anchored to a point of the parent's rectangle (fractions, (0, 0) top-left) and offset from
 *   it in pixels: anchors at the same point keep a fixed size, anchors apart stretch with the
 *   parent. Elements whose parent is not a UI element are placed on the screen (the framebuffer).
 * - WPE_COMP_SPRITE (WPE_Sprite): fills the element's rectangle with an image (optionally a region
 *   of it, and nine-sliced so borders keep their size) or a plain color.
 * - WPE_COMP_TEXT (WPE_Text): bitmap-font text aligned in the rectangle, optionally word-wrapped, with
 *   an optional drop shadow and outline (text fields draw their text without them).
 * - WPE_COMP_BUTTON (WPE_Button): makes the element focusable and clickable. The button's state
 *   tints everything it and its descendants draw.
 * - WPE_COMP_TOGGLE (WPE_Toggle) and WPE_COMP_SLIDER (WPE_Slider): with a button on the same entity,
 *   an on/off switch and a value dragged along a track. Their changes are collected like clicks
 *   (wpe_ui_changes()); the engine passes them to scripts as on_change(value). Without a button they
 *   are drawn but don't react.
 * - WPE_COMP_TEXT_INPUT (WPE_TextInput): with a button on the same entity, a one-line text field the
 *   player types into (a name, an address). A WPE_Text on the entity gives its font, scale, color and
 *   vertical alignment (its own string isn't shown). Each edit is a change (wpe_ui_changes(); the
 *   engine passes on_change(value)), and Return a submit (wpe_ui_submits(); on_submit(value)).
 *
 * Order: elements are drawn parent first, then children in hierarchy order; top-level elements in
 * the order they were created (later ones on top). A scene without a camera is a pure 2D scene.
 *
 * Interaction (wpe_ui_update()): one button at a time has the focus. The mouse focuses the button
 * under it when it moves and clicks with the left button (press and release on the same button).
 * Arrow keys, the D-pad and the left stick move the focus to the nearest button wholly past the focused
 * one's edge in that direction, those in line with it first (holding repeats); with none, it stays, and
 * Return, Space or the pad's south button click the focused one. When nothing has the focus, the first
 * navigation input focuses the first button. Projects can rebind navigation with input actions named "ui_up",
 * "ui_down", "ui_left", "ui_right" and "ui_submit"; an action with one of those names replaces that input's
 * defaults. Clicks are collected for the application (wpe_ui_clicks()); the engine passes them to scripts as
 * on_click. Focus changes are reported too (wpe_ui_focus_change(); on_blur and on_focus).
 *
 * Text fields: clicking one, or submitting it (Return, Space, the pad's south button), starts editing it.
 * While it edits, the keyboard types: printable characters (filtered as the field says), Backspace and
 * Delete, Left/Right (Shift selects, Ctrl moves by words), Home/End, Ctrl+A/C/X/V (clipboard). Return
 * submits and stops editing; Escape stops and restores the text it had when editing began; Tab, Up and Down
 * stop and move the focus on. Clicking elsewhere stops editing. Only one field edits at a time
 * (wpe_ui_editing()); the application turns the keyboard's actions off meanwhile (the engine does).
 * NOTE(limitation): text is ASCII (the bitmap fonts' 95 glyphs); other characters typed are dropped.
 * There is no on-screen keyboard for gamepads.
 *
 * Asset lifetime: sprites and texts hold a reference to their image and font from their add hook
 * to their remove hook, like mesh renderers. A text's string is copied by its add hook, so the
 * caller keeps ownership of what it passed; change it later with wpe_ui_set_text().
 *
 * NOTE(limitation): UI elements ignore their Transform (no rotation or scale), and children are not
 * clipped to their parent's rectangle.
 *
 * - Dependencies: core, math, ecs, scene, renderer, assets, graphics (2D drawing), input (optional)
 * - Thread affinity: main thread, same as the owning world
 *
 * @author WillPowerEngine Team
 */
#ifndef WPE_UI_UI_H
#define WPE_UI_UI_H

#include <stdint.h>

#include "assets/asset_manager.h"
#include "core/result.h"
#include "ecs/ecs.h"
#include "graphics/draw2d.h"
#include "input/input.h"
#include "math/wpe_math.h"
#include "renderer/renderer.h"

typedef struct WPE_UiRect {
    WPE_Vec2 anchor_min; /* Top-left corner's anchor in the parent's rectangle ((0, 0) top-left). */
    WPE_Vec2 anchor_max; /* Bottom-right corner's anchor ((1, 1) bottom-right). */
    WPE_Vec2 offset_min; /* Pixels from the top-left anchor to the top-left corner. */
    WPE_Vec2 offset_max; /* Pixels from the bottom-right anchor to the bottom-right corner. */
    bool visible;        /* false hides the element and everything below it. */
    /* Results of the last layout (read-only): the rectangle in framebuffer pixels, and whether the
     * element was shown (it and all its UI ancestors visible). */
    WPE_Rect rect;
    bool shown;
} WPE_UiRect;

typedef struct WPE_Sprite {
    WPE_AssetHandle image;      /* WPE_ASSET_IMAGE, or WPE_ASSET_NONE for a plain rectangle. */
    const WPE_Texture* texture; /* Resolved from `image` by the add hook (read-only). */
    WPE_Vec4 color;             /* RGBA (0..1): multiplies the image, or fills without one. */
    WPE_Rect source;            /* Region of the image in texels; zero width or height = all of it. */
    WPE_Vec4 slice;             /* Nine-slice borders in texels (left, top, right, bottom); 0 = none. */
} WPE_Sprite;

typedef enum WPE_UiAlign {
    WPE_UI_ALIGN_START = 0, /* Left, or top. */
    WPE_UI_ALIGN_CENTER,
    WPE_UI_ALIGN_END, /* Right, or bottom. */
} WPE_UiAlign;

typedef struct WPE_Text {
    char* text;                   /* Copied by the add hook (the copy is owned and freed by the component). */
    WPE_AssetHandle font;         /* WPE_ASSET_FONT, or WPE_ASSET_NONE for the UI's default font. */
    const WPE_BitmapFont* glyphs; /* Resolved from `font` by the add hook (read-only; NULL = default). */
    WPE_Vec4 color;               /* RGBA (0..1). */
    float scale;                  /* Glyph magnification (1 = the font's pixels). */
    WPE_UiAlign align_x, align_y;
    bool wrap; /* Break lines at spaces so they fit the rectangle's width. */
    /* A drop shadow: the text (with its outline) drawn again under it, moved by `shadow_offset` font
     * pixels (times `scale`). Off while `shadow_color`'s alpha is 0 (the default). */
    WPE_Vec4 shadow_color;
    WPE_Vec2 shadow_offset;
    /* An outline: the text drawn again under it at every offset up to `outline_width` font pixels
     * (times `scale`) in each direction. Off while `outline_color`'s alpha is 0 (the default) or the
     * width is 0. NOTE(limitation): the copies overlap, so translucent outlines and shadows of outlined
     * text come out uneven; use opaque colors for them. */
    WPE_Vec4 outline_color;
    uint32_t outline_width; /* 0..WPE_TEXT_MAX_OUTLINE. */
} WPE_Text;

#define WPE_TEXT_MAX_OUTLINE 4u

typedef struct WPE_Button {
    bool interactable; /* false: shown with disabled_tint, and never focused or clicked. */
    /* Multipliers (0..2 per channel) for everything the button and its descendants draw. */
    WPE_Vec4 normal_tint, focused_tint, pressed_tint, disabled_tint;
} WPE_Button;

/* A button that is on or off: each click flips it. While on, a mark fills the rectangle inset by
 * mark_inset pixels (a check box); scripts can show their own instead and set the mark's alpha to 0. */
typedef struct WPE_Toggle {
    bool on;
    WPE_Vec4 mark_color; /* RGBA (0..1); tinted like the button. */
    float mark_inset;    /* Pixels from the rectangle's edge to the mark. */
} WPE_Toggle;

/* A button whose value the player drags along it, or steps with left/right while it has the focus. It
 * draws a track across the middle of its rectangle, the part up to the value filled, and a handle at
 * the value. */
typedef struct WPE_Slider {
    float value;
    float min, max;
    float step; /* Values snap to min + n * step (0: any value). Keys and the pad step by it, or by 1/20 of
                   the range without one. */
    WPE_Vec4 track_color, fill_color, handle_color; /* RGBA (0..1); tinted like the button. */
    float handle_width;                             /* Pixels (0: no handle). */
} WPE_Slider;

/* Which characters a text field accepts. */
typedef enum WPE_TextFilter {
    WPE_TEXT_FILTER_ANY = 0, /* Every printable character. */
    WPE_TEXT_FILTER_DIGITS,  /* 0-9. */
    WPE_TEXT_FILTER_NUMBER,  /* Digits, one '.' and a leading '-'. */
    WPE_TEXT_FILTER_NAME,    /* Letters, digits, space, '_', '-' and '.' (player and file names). */
    WPE_TEXT_FILTER_ADDRESS, /* Letters, digits, '.', ':', '-', '[' and ']' (host names, IPs, ports). */
} WPE_TextFilter;

typedef struct WPE_TextInput {
    char* value;         /* The text (ASCII). Copied by the add hook; owned and freed by the component. */
    char* placeholder;   /* Shown, dimmed, while the field is empty and not editing. Copied like `value`. */
    uint32_t max_length; /* Most characters (0: 256). */
    WPE_TextFilter filter;
    bool password;                                             /* Shown as '*'. */
    WPE_Vec4 placeholder_color, cursor_color, selection_color; /* RGBA (0..1). */
    float padding; /* Pixels between the rectangle's sides and the text (default 3). */

    /* State (read-only; reset by the add hook). */
    bool editing;
    uint32_t cursor, anchor; /* Character positions in `value`; the selection is between them. */
    float scroll;            /* Pixels the text is scrolled left to keep the cursor in view. */
    float blink;             /* Seconds into the cursor's blink. */
    char* wpe__before;       /* The value when editing began (Escape restores it); owned. */
} WPE_TextInput;

typedef struct WPE_Ui WPE_Ui;
typedef struct WPE_UiDesc {
    WPE_Ecs* ecs;             /* Borrowed; scene components must be registered. */
    WPE_AssetManager* assets; /* Borrowed: images and fonts. */
    WPE_Input* input;         /* Optional, borrowed: without it wpe_ui_update() only lays out. */
    WPE_AssetHandle font;     /* Default font for texts without one (WPE_ASSET_NONE: such texts are not
                                 drawn); the UI keeps a reference. */
} WPE_UiDesc;

/* Defaults (also the scene-file defaults). @thread_safety Any thread */
WPE_UiRect wpe_ui_rect_default(void); /* Fills its parent (anchors at its corners, no offsets), visible. */
WPE_Sprite wpe_sprite_default(void);  /* White, no image. */
WPE_Text wpe_text_default(void);      /* Empty, white, scale 1, top-left, no wrapping; shadow and outline
                                       * off (black, alpha 0), shadow offset (1, 1), outline width 1. */
WPE_Button wpe_button_default(void);  /* Interactable; focus brightens, press darkens, disabled dims. */
WPE_Toggle wpe_toggle_default(void);  /* Off; a white mark inset 3 pixels. */
WPE_Slider wpe_slider_default(void);  /* 0..1 at 0, no step; gray track, white fill and handle (6 px). */
WPE_TextInput wpe_text_input_default(void); /* Empty, any characters, 256 at most; gray placeholder, white
                                               cursor, blue selection, 3 px padding. */

/** The value a slider gets when set to `value`: clamped to its range and snapped to its step.
 * @thread_safety Any thread */
float wpe_slider_constrain(const WPE_Slider* slider, float value);

/**
 * Creates the UI of a world and registers its components there. One UI per world; destroy it before
 * the world's asset manager.
 *
 * @return WPE_RESULT_OK, WPE_RESULT_ERROR_NULL_ARG, WPE_RESULT_ERROR_OUT_OF_MEMORY or a registration
 *         error (WPE_RESULT_ERROR_ALREADY_EXISTS: the world already has a UI).
 *
 * @thread_safety Main thread
 */
[[nodiscard]] WPE_Result wpe_ui_create(const WPE_UiDesc* desc, WPE_Ui** out_ui);

/** Releases the default font and frees the UI; NULL is a no-op. @thread_safety Main thread */
void wpe_ui_destroy(WPE_Ui* ui);

/**
 * Computes every element's rectangle for a screen of the given size (WPE_UiRect.rect and .shown).
 *
 * @return WPE_RESULT_OK, WPE_RESULT_ERROR_NULL_ARG or WPE_RESULT_ERROR_OUT_OF_MEMORY.
 *
 * @thread_safety Main thread
 */
WPE_Result wpe_ui_layout(WPE_Ui* ui, uint32_t width, uint32_t height);

/**
 * Where a sprite element would sit at its image's own size: the image's (or the sprite's `source` region's)
 * width and height times `scale`, centered where the element is now (laid out on a screen of the given size).
 * Both anchors move to one point, the middle of the old ones, so the element keeps that size whatever the
 * screen; corners land on whole pixels, so the image stays crisp. Nothing changes: see wpe_ui_fit_image().
 *
 * @param scale Pixels per texel (1 = the image's own size; 2, 3 for small pixel art); > 0.
 * @param out_rect The element's rectangle with the new anchors and offsets (visible as it was).
 * @return WPE_RESULT_OK; WPE_RESULT_ERROR_NULL_ARG; WPE_RESULT_ERROR_INVALID_ARG (scale not > 0);
 *         WPE_RESULT_ERROR_NOT_FOUND (not a UI element, no sprite, or a sprite without an image; logged);
 *         WPE_RESULT_ERROR_OUT_OF_MEMORY (the layout).
 *
 * @thread_safety Main thread
 */
WPE_Result wpe_ui_fit_image_rect(WPE_Ui* ui, WPE_Entity entity, float scale, uint32_t width, uint32_t height,
                                 WPE_UiRect* out_rect);

/** wpe_ui_fit_image_rect(), applied to the element. @return The same results. @thread_safety Main thread */
WPE_Result wpe_ui_fit_image(WPE_Ui* ui, WPE_Entity entity, float scale, uint32_t width, uint32_t height);

/**
 * Lays out the UI and runs one frame of interaction from the input's current frame (after
 * wpe_input_update()): focus, navigation, button states and clicks.
 *
 * @param dt Seconds since the last update (for navigation repeat).
 * @return Same as wpe_ui_layout().
 *
 * @thread_safety Main thread
 */
WPE_Result wpe_ui_update(WPE_Ui* ui, uint32_t width, uint32_t height, float dt);

/** @return The buttons clicked by the last wpe_ui_update(), in click order (borrowed until the next
 * update); *out_count receives their number. @thread_safety Main thread */
const WPE_Entity* wpe_ui_clicks(const WPE_Ui* ui, uint32_t* out_count);

/** @return The toggles and sliders whose value the player changed in the last wpe_ui_update(), each
 * once, in order (borrowed until the next update). @thread_safety Main thread */
const WPE_Entity* wpe_ui_changes(const WPE_Ui* ui, uint32_t* out_count);

/**
 * How the focus moved since the update before the last wpe_ui_update(), whatever moved it: the player,
 * wpe_ui_focus() (reported by the next update), or the focused button being removed, hidden or disabled.
 * The engine passes it to scripts as on_blur on the button that lost it, then on_focus on the one that
 * gained it.
 *
 * @param out_lost Receives the button that lost the focus (WPE_ENTITY_NULL: none, or no change); it may have
 *        been despawned since.
 * @param out_gained Receives the button that has it now (WPE_ENTITY_NULL: none, or no change).
 * @return Whether the focus changed.
 *
 * @thread_safety Main thread
 */
bool wpe_ui_focus_change(const WPE_Ui* ui, WPE_Entity* out_lost, WPE_Entity* out_gained);

/** @return The text fields submitted (Return) in the last wpe_ui_update(), in order (borrowed until the next
 * update). @thread_safety Main thread */
const WPE_Entity* wpe_ui_submits(const WPE_Ui* ui, uint32_t* out_count);

/** @return The text field being edited, or WPE_ENTITY_NULL. @thread_safety Main thread */
WPE_Entity wpe_ui_editing(const WPE_Ui* ui);

/**
 * Starts editing a text field (focusing it, with the cursor at the end), or stops editing with
 * WPE_ENTITY_NULL (keeping the text).
 *
 * @return false when the entity isn't a focusable text field.
 *
 * @thread_safety Main thread
 */
bool wpe_ui_edit(WPE_Ui* ui, WPE_Entity field);

/**
 * Replaces a text field's text (cut to its max_length; the filter doesn't apply to text set by code); the
 * cursor goes to the end and the selection is cleared.
 *
 * @return WPE_RESULT_OK, WPE_RESULT_ERROR_NULL_ARG, WPE_RESULT_ERROR_NOT_FOUND (no text field) or
 *         WPE_RESULT_ERROR_OUT_OF_MEMORY (the old text is kept).
 *
 * @thread_safety Main thread
 */
WPE_Result wpe_ui_set_input_text(WPE_Ecs* ecs, WPE_Entity entity, const char* text);

/** @return A text field's text (borrowed until it changes), or NULL without one. @thread_safety Main thread
 */
const char* wpe_ui_input_text(const WPE_Ecs* ecs, WPE_Entity entity);

/**
 * Lays out the UI for the renderer's framebuffer and draws it into the recording frame (between
 * wpe_renderer_begin_frame() and wpe_renderer_end_frame()), over what was drawn before.
 *
 * @return WPE_RESULT_OK, WPE_RESULT_ERROR_NULL_ARG, WPE_RESULT_ERROR_OUT_OF_MEMORY or a renderer
 *         error.
 *
 * @thread_safety Main thread
 */
WPE_Result wpe_ui_draw(WPE_Ui* ui, WPE_Renderer* renderer);

/** Gives the focus to a shown, interactable button (WPE_ENTITY_NULL clears it). @return false if the entity
 * cannot take the focus. @thread_safety Main thread */
bool wpe_ui_focus(WPE_Ui* ui, WPE_Entity button);

/** @return The focused button, or WPE_ENTITY_NULL. @thread_safety Main thread */
WPE_Entity wpe_ui_focused(const WPE_Ui* ui);

/**
 * The topmost shown element at a point (framebuffer pixels) in the last layout.
 *
 * @param any_element true: every element counts (editors); false: only elements with a sprite or a button,
 *        the ones that catch the mouse (text alone does not).
 * @return The element, or WPE_ENTITY_NULL.
 *
 * @thread_safety Main thread
 */
WPE_Entity wpe_ui_pick(const WPE_Ui* ui, float x, float y, bool any_element);

/**
 * Replaces the string of an entity's text.
 *
 * @return WPE_RESULT_OK, WPE_RESULT_ERROR_NULL_ARG, WPE_RESULT_ERROR_NOT_FOUND (no text) or
 *         WPE_RESULT_ERROR_OUT_OF_MEMORY (the old string is kept).
 *
 * @thread_safety Main thread
 */
WPE_Result wpe_ui_set_text(WPE_Ecs* ecs, WPE_Entity entity, const char* text);

#endif /* WPE_UI_UI_H */
