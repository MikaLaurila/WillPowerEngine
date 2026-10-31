/**
 * @file easing.h
 * @brief Easing curves for tweens and transitions (after Robert Penner's), by name for scripts.
 *
 * Each maps progress t in [0, 1] to eased progress, 0 at t = 0 and 1 at t = 1. `back` and `elastic` overshoot
 * (go below 0 or above 1 on the way); the others stay within [0, 1]. "in" eases start slowly, "out" ones end
 * slowly, "in_out" ones both.
 *
 * - Dependencies: none (C standard library)
 * - Thread affinity: any thread (pure functions)
 *
 * @author WillPowerEngine Team
 */
#ifndef WPE_MATH_EASING_H
#define WPE_MATH_EASING_H

typedef enum WPE_Ease {
    WPE_EASE_LINEAR = 0,
    WPE_EASE_SMOOTH, /* Smoothstep: gentle at both ends. */
    WPE_EASE_IN_QUAD,
    WPE_EASE_OUT_QUAD,
    WPE_EASE_IN_OUT_QUAD,
    WPE_EASE_IN_CUBIC,
    WPE_EASE_OUT_CUBIC,
    WPE_EASE_IN_OUT_CUBIC,
    WPE_EASE_IN_SINE,
    WPE_EASE_OUT_SINE,
    WPE_EASE_IN_OUT_SINE,
    WPE_EASE_IN_EXPO,
    WPE_EASE_OUT_EXPO,
    WPE_EASE_IN_OUT_EXPO,
    WPE_EASE_IN_BACK,  /* Pulls back before going. */
    WPE_EASE_OUT_BACK, /* Overshoots and settles. */
    WPE_EASE_IN_OUT_BACK,
    WPE_EASE_IN_ELASTIC,
    WPE_EASE_OUT_ELASTIC, /* Springs past the end and wobbles to rest. */
    WPE_EASE_IN_OUT_ELASTIC,
    WPE_EASE_IN_BOUNCE,
    WPE_EASE_OUT_BOUNCE, /* Lands and bounces. */
    WPE_EASE_IN_OUT_BOUNCE,
} WPE_Ease;

#define WPE_EASE_COUNT (WPE_EASE_IN_OUT_BOUNCE + 1)

/** @return Eased progress for t (clamped to [0, 1]; NaN counts as 0). @thread_safety Any thread */
float wpe_ease(WPE_Ease ease, float t);

/** @return The ease's name ("linear", "smooth", "in_quad", "out_back", ...), or NULL for an invalid value.
 * @thread_safety Any thread */
const char* wpe_ease_name(WPE_Ease ease);

/** Looks up an ease by name. @return false for an unknown name. @thread_safety Any thread */
bool wpe_ease_from_name(const char* name, WPE_Ease* out_ease);

#endif /* WPE_MATH_EASING_H */
