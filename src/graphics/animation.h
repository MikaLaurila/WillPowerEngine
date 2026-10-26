/**
 * @file animation.h
 * @brief Keyframe sampling and the animator system that drives instantiated model nodes.
 *
 * - Dependencies: graphics, assets (animation data), scene
 * - Thread affinity: sampling is pure (any thread); the system runs on the world's thread
 *
 * @author WillPowerEngine Team
 */
#ifndef WPE_GRAPHICS_ANIMATION_H
#define WPE_GRAPHICS_ANIMATION_H

#include "graphics/graphics.h"

/**
 * Samples a channel at `time` (seconds). Times before the first key clamp to the first value and
 * after the last key to the last value. Rotations are returned normalized.
 *
 * @param out_value Receives 3 floats (translation/scale) or 4 (rotation quaternion xyzw).
 *
 * @thread_safety Any thread
 */
void wpe_animation_sample_channel(const WPE_AnimationChannel* channel, float time, float out_value[4]);

/**
 * Advances every playing animator by `dt` seconds and writes the sampled poses into the local
 * transforms of the animated node entities. Call before wpe_scene_update_transforms().
 * Looping animators wrap; non-looping ones clamp at the end (or start, when playing backwards)
 * and stop playing.
 *
 * @return WPE_RESULT_OK or WPE_RESULT_ERROR_NULL_ARG.
 *
 * @thread_safety Thread owning the world
 */
WPE_Result wpe_animation_update(WPE_Ecs* ecs, float dt);

/**
 * Applies an animator's pose at its current time without advancing it (useful after seeking).
 *
 * @thread_safety Thread owning the world
 */
void wpe_animation_apply(WPE_Ecs* ecs, const WPE_Animator* animator);

#endif /* WPE_GRAPHICS_ANIMATION_H */
