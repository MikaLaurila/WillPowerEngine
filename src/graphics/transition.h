/**
 * @file transition.h
 * @brief Screen transitions between scenes: how a finished frame is covered as the screen closes and opens.
 *
 * A transition covers the frame with a color by `coverage` (0: untouched, 1: all color): `fade` blends every
 * pixel, `wipe` slides an edge across the screen, `iris` closes a circle on a point, and `dissolve` turns
 * pixels over in an ordered-dither pattern (the retro way to fade without blending). Opening runs the same
 * shape onward (a wipe keeps going the same way rather than sliding back).
 *
 * - Dependencies: renderer
 * - Thread affinity: any thread (the framebuffer must not be recording)
 *
 * @author WillPowerEngine Team
 */
#ifndef WPE_GRAPHICS_TRANSITION_H
#define WPE_GRAPHICS_TRANSITION_H

#include "renderer/renderer.h"

typedef enum WPE_TransitionKind {
    WPE_TRANSITION_NONE = 0, /* A cut. */
    WPE_TRANSITION_FADE,
    WPE_TRANSITION_WIPE,
    WPE_TRANSITION_IRIS,
    WPE_TRANSITION_DISSOLVE,
} WPE_TransitionKind;

#define WPE_TRANSITION_KIND_COUNT (WPE_TRANSITION_DISSOLVE + 1)

typedef struct WPE_Transition {
    WPE_TransitionKind kind;
    float duration; /* Seconds to close, and again to open. */
    WPE_Vec3 color; /* What covers the screen (0..1). */
    float center_x; /* Iris: where the circle closes, as fractions of the screen (0.5, 0.5: the middle). */
    float center_y;
} WPE_Transition;

/** @return A black fade of 0.4 s each way, centered. @thread_safety Any thread */
WPE_Transition wpe_transition_default(WPE_TransitionKind kind);

/** @return The kind's name ("none", "fade", "wipe", "iris", "dissolve"), or NULL. @thread_safety Any thread
 */
const char* wpe_transition_name(WPE_TransitionKind kind);

/** Looks up a kind by name. @return false for an unknown name. @thread_safety Any thread */
bool wpe_transition_from_name(const char* name, WPE_TransitionKind* out_kind);

/**
 * Covers a frame by `coverage` (clamped to 0..1) with the transition's color.
 *
 * @param opening The screen is opening (coverage falls): the shape carries on rather than reversing.
 * @thread_safety Any thread
 */
void wpe_transition_cover(WPE_Framebuffer* framebuffer, const WPE_Transition* transition, float coverage,
                          bool opening);

#endif /* WPE_GRAPHICS_TRANSITION_H */
