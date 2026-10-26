#include "core/fixed_step.h"

#include <math.h>

#include "core/debug.h"

void wpe_fixed_step_init(WPE_FixedStep* clock, double step_seconds, uint32_t max_steps) {
    if (!wpe_check(clock != NULL)) return;
    *clock = (WPE_FixedStep){
        .step = (step_seconds > 0.0 && isfinite(step_seconds)) ? step_seconds : 1.0 / 60.0,
        .max_steps = max_steps ? max_steps : 1,
    };
}

uint32_t wpe_fixed_step_advance(WPE_FixedStep* clock, double frame_seconds) {
    if (!wpe_check(clock != NULL)) return 0;
    if (frame_seconds > 0.0 && isfinite(frame_seconds)) clock->accumulator += frame_seconds;

    const double available = floor(clock->accumulator / clock->step);
    uint32_t steps = available > (double)clock->max_steps ? clock->max_steps : (uint32_t)available;
    clock->accumulator -= (double)steps * clock->step;
    if (clock->accumulator >= clock->step) {
        /* Over budget: drop whole steps we will never catch up on, keep the fractional part. */
        const double excess = floor(clock->accumulator / clock->step);
        clock->dropped += (uint64_t)excess;
        clock->accumulator -= excess * clock->step;
    }
    if (clock->accumulator < 0.0) clock->accumulator = 0.0;
    return steps;
}

double wpe_fixed_step_alpha(const WPE_FixedStep* clock) {
    if (!clock || !(clock->step > 0.0)) return 0.0;
    const double alpha = clock->accumulator / clock->step;
    return alpha < 0.0 ? 0.0 : (alpha >= 1.0 ? 0.0 : alpha);
}
