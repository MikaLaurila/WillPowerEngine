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
 * During a crossfade it is the blend of both animations.
 *
 * @thread_safety Thread owning the world
 */
void wpe_animation_apply(WPE_Ecs* ecs, const WPE_Animator* animator);

/**
 * Starts an animation from its beginning. With `fade_seconds` > 0 it crossfades: the animation that
 * was playing carries on underneath (at its own time and speed) while the new one's weight rises
 * from 0 to 1 over that time. Nodes the new animation doesn't move keep the old one's pose. Starting
 * another fade during one fades from the newer animation (the older one is dropped).
 *
 * @param animation Index into the model's animations.
 * @return WPE_RESULT_OK, WPE_RESULT_ERROR_NULL_ARG or WPE_RESULT_ERROR_INVALID_ARG (no such
 *         animation, or a negative or non-finite fade).
 *
 * @thread_safety Thread owning the world
 */
WPE_Result wpe_animator_play(WPE_Animator* animator, uint32_t animation, float fade_seconds, bool loop);

/** @return The index of the model's animation called `name`, or WPE_MODEL_NONE. @thread_safety Any thread */
uint32_t wpe_model_find_animation(const WPE_Model* model, const char* name);

#endif /* WPE_GRAPHICS_ANIMATION_H */
