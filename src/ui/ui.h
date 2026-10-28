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
 * - WPE_COMP_TEXT (WPE_Text): bitmap-font text aligned in the rectangle, optionally word-wrapped.
 * - WPE_COMP_BUTTON (WPE_Button): makes the element focusable and clickable. The button's state
 *   tints everything it and its descendants draw.
 *
 * Order: elements are drawn parent first, then children in hierarchy order; top-level elements in
 * the order they were created (later ones on top). A scene without a camera is a pure 2D scene.
 *
 * Interaction (wpe_ui_update()): one button at a time has the focus. The mouse focuses the button
 * under it when it moves and clicks with the left button (press and release on the same button).
 * Arrow keys, the D-pad and the left stick move the focus to the nearest button in that direction
 * (holding repeats), and Return, Space or the pad's south button click the focused one. When
 * nothing has the focus, the first navigation input focuses the first button. Projects can rebind
 * navigation with input actions named "ui_up", "ui_down", "ui_left", "ui_right" and "ui_submit";
 * an action with one of those names replaces that input's defaults. Clicks are collected for the
 * application (wpe_ui_clicks()); the engine passes them to scripts as on_click.
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
} WPE_Text;

typedef struct WPE_Button {
    bool interactable; /* false: shown with disabled_tint, and never focused or clicked. */
    /* Multipliers (0..2 per channel) for everything the button and its descendants draw. */
    WPE_Vec4 normal_tint, focused_tint, pressed_tint, disabled_tint;
} WPE_Button;

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
WPE_Text wpe_text_default(void);      /* Empty, white, scale 1, top-left, no wrapping. */
WPE_Button wpe_button_default(void);  /* Interactable; focus brightens, press darkens, disabled dims. */

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
 * The topmost element drawn at a point (framebuffer pixels) by the last layout, counting elements
 * with a sprite or a button (text alone does not catch the mouse).
 *
 * @return The element, or WPE_ENTITY_NULL.
 *
 * @thread_safety Main thread
 */
WPE_Entity wpe_ui_pick(const WPE_Ui* ui, float x, float y);

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
