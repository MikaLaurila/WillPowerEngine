/**
 * @file fixed_step.h
 * @brief Fixed-timestep accumulator for simulation (physics, on_fixed_update).
 *
 * Each frame, wpe_fixed_step_advance() adds the frame time and returns how many fixed steps to
 * run. If a frame took so long that more than `max_steps` would be needed, the excess time is
 * dropped instead of being simulated later, which prevents the "spiral of death" where slow
 * frames cause ever more simulation work.
 *
 * - Dependencies: none
 * - Thread affinity: not synchronized; one owner
 *
 * @author WillPowerEngine Team
 */
#ifndef WPE_CORE_FIXED_STEP_H
#define WPE_CORE_FIXED_STEP_H

#include <stdint.h>

typedef struct WPE_FixedStep {
    double step;        /* Seconds per fixed step (> 0). */
    double accumulator; /* Unsimulated time, in [0, step) after advance. */
    uint32_t max_steps; /* Upper bound on steps per advance (>= 1). */
    uint64_t dropped;   /* Total number of steps skipped by the max_steps clamp. */
} WPE_FixedStep;

/**
 * Initializes a fixed-step clock.
 *
 * @param step_seconds Fixed step length; values <= 0 or non-finite fall back to 1/60.
 * @param max_steps Steps allowed per advance; 0 is treated as 1.
 *
 * @thread_safety Owning thread
 */
void wpe_fixed_step_init(WPE_FixedStep* clock, double step_seconds, uint32_t max_steps);

/**
 * Adds frame time and returns the number of fixed steps to simulate now.
 *
 * @param frame_seconds Elapsed real time; negative or non-finite values count as 0.
 *
 * @thread_safety Owning thread
 */
uint32_t wpe_fixed_step_advance(WPE_FixedStep* clock, double frame_seconds);

/** @return accumulator / step in [0, 1): how far between the last two fixed states to interpolate.
 * @thread_safety Owning thread */
double wpe_fixed_step_alpha(const WPE_FixedStep* clock);

#endif /* WPE_CORE_FIXED_STEP_H */
