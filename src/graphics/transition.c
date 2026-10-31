#include "graphics/transition.h"

#include <math.h>
#include <string.h>

static const char* const wpe__transition_names[] = {"none", "fade", "wipe", "iris", "dissolve"};
static_assert(sizeof wpe__transition_names / sizeof wpe__transition_names[0] == WPE_TRANSITION_KIND_COUNT,
              "every transition has a name");

WPE_Transition wpe_transition_default(WPE_TransitionKind kind) {
    return (WPE_Transition){
        .kind = kind, .duration = 0.4f, .color = wpe_vec3(0, 0, 0), .center_x = 0.5f, .center_y = 0.5f};
}

const char* wpe_transition_name(WPE_TransitionKind kind) {
    return (unsigned)kind < WPE_TRANSITION_KIND_COUNT ? wpe__transition_names[kind] : NULL;
}

bool wpe_transition_from_name(const char* name, WPE_TransitionKind* out_kind) {
    for (unsigned i = 0; name && i < WPE_TRANSITION_KIND_COUNT; ++i) {
        if (strcmp(wpe__transition_names[i], name) == 0) {
            *out_kind = (WPE_TransitionKind)i;
            return true;
        }
    }
    return false;
}

/* The 4x4 ordered-dither thresholds (0..15). */
static const uint8_t wpe__bayer[4][4] = {{0, 8, 2, 10}, {12, 4, 14, 6}, {3, 11, 1, 9}, {15, 7, 13, 5}};

static uint32_t wpe__blend(uint32_t pixel, uint32_t color, uint32_t amount /* 0..256 */) {
    uint32_t out = pixel & 0xFF000000u;
    for (int shift = 0; shift < 24; shift += 8) {
        const uint32_t a = (pixel >> shift) & 0xFFu, b = (color >> shift) & 0xFFu;
        out |= ((a * (256u - amount) + b * amount) >> 8) << shift;
    }
    return out;
}

void wpe_transition_cover(WPE_Framebuffer* fb, const WPE_Transition* transition, float coverage,
                          bool opening) {
    if (!fb || !transition || transition->kind == WPE_TRANSITION_NONE) return;
    if (!(coverage > 0.0f)) return;
    if (coverage > 1.0f) coverage = 1.0f;
    const WPE_Vec3 c = transition->color;
    const WPE_Color color = wpe_color_from_vec4(wpe_vec4(c.x, c.y, c.z, 1.0f));
    const uint32_t width = fb->width, height = fb->height;
    switch (transition->kind) {
    case WPE_TRANSITION_FADE: {
        const uint32_t amount = (uint32_t)(coverage * 256.0f + 0.5f);
        for (size_t i = 0; i < (size_t)width * height; ++i)
            fb->color[i] = wpe__blend(fb->color[i], color, amount);
        break;
    }
    case WPE_TRANSITION_WIPE: {
        /* Closing, the color comes in from the left; opening, it leaves to the right. */
        const float edge = (opening ? 1.0f - coverage : coverage) * (float)width;
        for (uint32_t y = 0; y < height; ++y) {
            for (uint32_t x = 0; x < width; ++x) {
                const bool covered = opening ? (float)x + 0.5f >= edge : (float)x + 0.5f < edge;
                if (covered) fb->color[(size_t)y * width + x] = color;
            }
        }
        break;
    }
    case WPE_TRANSITION_IRIS: {
        const float cx = transition->center_x * (float)width, cy = transition->center_y * (float)height;
        /* Fully open, the circle reaches the farthest corner. */
        const float fx = fmaxf(cx, (float)width - cx), fy = fmaxf(cy, (float)height - cy);
        const float radius = (1.0f - coverage) * sqrtf(fx * fx + fy * fy);
        for (uint32_t y = 0; y < height; ++y) {
            for (uint32_t x = 0; x < width; ++x) {
                const float dx = (float)x + 0.5f - cx, dy = (float)y + 0.5f - cy;
                if (dx * dx + dy * dy >= radius * radius) fb->color[(size_t)y * width + x] = color;
            }
        }
        break;
    }
    case WPE_TRANSITION_DISSOLVE: {
        const float level = coverage * 16.0f;
        for (uint32_t y = 0; y < height; ++y) {
            for (uint32_t x = 0; x < width; ++x) {
                if ((float)wpe__bayer[y & 3u][x & 3u] < level) fb->color[(size_t)y * width + x] = color;
            }
        }
        break;
    }
    case WPE_TRANSITION_NONE:
    default:
        break;
    }
}
