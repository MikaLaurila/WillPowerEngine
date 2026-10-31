#include "math/easing.h"

#include <math.h>
#include <string.h>

static const char* const wpe__ease_names[] = {
    "linear",      "smooth",         "in_quad",   "out_quad",   "in_out_quad",  "in_cubic",
    "out_cubic",   "in_out_cubic",   "in_sine",   "out_sine",   "in_out_sine",  "in_expo",
    "out_expo",    "in_out_expo",    "in_back",   "out_back",   "in_out_back",  "in_elastic",
    "out_elastic", "in_out_elastic", "in_bounce", "out_bounce", "in_out_bounce"};
static_assert(sizeof wpe__ease_names / sizeof wpe__ease_names[0] == WPE_EASE_COUNT, "every ease has a name");

#define WPE__PI_F 3.14159265358979323846f

static float wpe__out_bounce(float t) {
    const float n = 7.5625f, d = 2.75f;
    if (t < 1.0f / d) return n * t * t;
    if (t < 2.0f / d) {
        t -= 1.5f / d;
        return n * t * t + 0.75f;
    }
    if (t < 2.5f / d) {
        t -= 2.25f / d;
        return n * t * t + 0.9375f;
    }
    t -= 2.625f / d;
    return n * t * t + 0.984375f;
}

float wpe_ease(WPE_Ease ease, float t) {
    if (!(t > 0.0f)) return 0.0f; /* Also NaN. */
    if (t >= 1.0f) return 1.0f;
    const float back = 1.70158f, back_in_out = back * 1.525f;
    const float elastic = 2.0f * WPE__PI_F / 3.0f, elastic_in_out = 2.0f * WPE__PI_F / 4.5f;
    switch (ease) {
    case WPE_EASE_SMOOTH:
        return t * t * (3.0f - 2.0f * t);
    case WPE_EASE_IN_QUAD:
        return t * t;
    case WPE_EASE_OUT_QUAD:
        return 1.0f - (1.0f - t) * (1.0f - t);
    case WPE_EASE_IN_OUT_QUAD:
        return t < 0.5f ? 2.0f * t * t : 1.0f - powf(-2.0f * t + 2.0f, 2.0f) * 0.5f;
    case WPE_EASE_IN_CUBIC:
        return t * t * t;
    case WPE_EASE_OUT_CUBIC:
        return 1.0f - powf(1.0f - t, 3.0f);
    case WPE_EASE_IN_OUT_CUBIC:
        return t < 0.5f ? 4.0f * t * t * t : 1.0f - powf(-2.0f * t + 2.0f, 3.0f) * 0.5f;
    case WPE_EASE_IN_SINE:
        return 1.0f - cosf(t * WPE__PI_F * 0.5f);
    case WPE_EASE_OUT_SINE:
        return sinf(t * WPE__PI_F * 0.5f);
    case WPE_EASE_IN_OUT_SINE:
        return -(cosf(WPE__PI_F * t) - 1.0f) * 0.5f;
    case WPE_EASE_IN_EXPO:
        return powf(2.0f, 10.0f * t - 10.0f);
    case WPE_EASE_OUT_EXPO:
        return 1.0f - powf(2.0f, -10.0f * t);
    case WPE_EASE_IN_OUT_EXPO:
        return t < 0.5f ? powf(2.0f, 20.0f * t - 10.0f) * 0.5f
                        : (2.0f - powf(2.0f, -20.0f * t + 10.0f)) * 0.5f;
    case WPE_EASE_IN_BACK:
        return (back + 1.0f) * t * t * t - back * t * t;
    case WPE_EASE_OUT_BACK:
        return 1.0f + (back + 1.0f) * powf(t - 1.0f, 3.0f) + back * powf(t - 1.0f, 2.0f);
    case WPE_EASE_IN_OUT_BACK:
        return t < 0.5f
                   ? powf(2.0f * t, 2.0f) * ((back_in_out + 1.0f) * 2.0f * t - back_in_out) * 0.5f
                   : (powf(2.0f * t - 2.0f, 2.0f) * ((back_in_out + 1.0f) * (t * 2.0f - 2.0f) + back_in_out) +
                      2.0f) *
                         0.5f;
    case WPE_EASE_IN_ELASTIC:
        return -powf(2.0f, 10.0f * t - 10.0f) * sinf((t * 10.0f - 10.75f) * elastic);
    case WPE_EASE_OUT_ELASTIC:
        return powf(2.0f, -10.0f * t) * sinf((t * 10.0f - 0.75f) * elastic) + 1.0f;
    case WPE_EASE_IN_OUT_ELASTIC:
        return t < 0.5f
                   ? -(powf(2.0f, 20.0f * t - 10.0f) * sinf((20.0f * t - 11.125f) * elastic_in_out)) * 0.5f
                   : powf(2.0f, -20.0f * t + 10.0f) * sinf((20.0f * t - 11.125f) * elastic_in_out) * 0.5f +
                         1.0f;
    case WPE_EASE_IN_BOUNCE:
        return 1.0f - wpe__out_bounce(1.0f - t);
    case WPE_EASE_OUT_BOUNCE:
        return wpe__out_bounce(t);
    case WPE_EASE_IN_OUT_BOUNCE:
        return t < 0.5f ? (1.0f - wpe__out_bounce(1.0f - 2.0f * t)) * 0.5f
                        : (1.0f + wpe__out_bounce(2.0f * t - 1.0f)) * 0.5f;
    case WPE_EASE_LINEAR:
    default:
        return t;
    }
}

const char* wpe_ease_name(WPE_Ease ease) {
    return (unsigned)ease < WPE_EASE_COUNT ? wpe__ease_names[ease] : NULL;
}

bool wpe_ease_from_name(const char* name, WPE_Ease* out_ease) {
    for (unsigned i = 0; name && i < WPE_EASE_COUNT; ++i) {
        if (strcmp(wpe__ease_names[i], name) == 0) {
            *out_ease = (WPE_Ease)i;
            return true;
        }
    }
    return false;
}
