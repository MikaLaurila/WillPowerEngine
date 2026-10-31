/**
 * @file game_feel.h
 * @brief Game feel: the game clock's speed (slow motion, hit-stop) and entity shakes.
 *
 * The game clock runs at a time scale: hit-stop (0) while one lasts, else slow motion while it lasts (easing
 * back over its fade), else the base scale. wpe_game_feel_advance() turns a frame's real time into game time,
 * which drives everything simulated (fixed steps, scripts and their timers, animation, effects). Hit-stops,
 * slow motion and shakes count real time, so they end on time whatever the scale.
 *
 * A shake moves an entity (the camera, a hit enemy) by smooth noise that fades over its duration. The offset
 * is put on just before transforms are computed (wpe_game_feel_apply_shakes()) and taken off at the start of
 * the next frame (wpe_game_feel_restore_shakes()), so game code and physics only ever see the real pose.
 *
 * - Dependencies: core, math, ecs, scene
 * - Thread affinity: the thread owning the world
 *
 * @author WillPowerEngine Team
 */
#ifndef WPE_SCENE_GAME_FEEL_H
#define WPE_SCENE_GAME_FEEL_H

#include "ecs/ecs.h"
#include "math/wpe_math.h"

/** Shakes running at once; starting another replaces the oldest. */
#define WPE_MAX_SHAKES 32u

typedef struct WPE_ShakeParams {
    float amplitude; /* Meters the entity moves at most (each axis), at the start. */
    float rotation;  /* Degrees it turns at most (each axis, in its own space), at the start. */
    float duration;  /* Real seconds until it has faded out (> 0). */
    float frequency; /* About how many times a second it changes direction. */
} WPE_ShakeParams;

typedef struct WPE_Shake {
    WPE_Entity entity;
    WPE_ShakeParams params;
    float elapsed;          /* Real seconds. */
    uint32_t seed;          /* Picks the noise, so two shakes differ. */
    bool applied;           /* The offset is on the transform. */
    WPE_Vec3 base_position; /* The pose without the offset, and the pose with it as written. */
    WPE_Quat base_rotation;
    WPE_Vec3 shaken_position;
    WPE_Quat shaken_rotation;
} WPE_Shake;

typedef struct WPE_GameFeel {
    float base_scale; /* The time scale outside slow motion and hit-stops (1). */
    float slow_scale; /* Slow motion: its scale, real seconds left and the fade back at its end. */
    float slow_remaining;
    float slow_fade;
    float stop_remaining; /* Hit-stop: real seconds left. */
    float real_dt;        /* The last frame's real time (wpe_game_feel_advance). */
    WPE_Shake shakes[WPE_MAX_SHAKES];
    uint32_t shake_count;
    uint32_t next_seed;
} WPE_GameFeel;

/** @return Time scale 1, nothing running. @thread_safety Any thread */
WPE_GameFeel wpe_game_feel_default(void);

/** @return The time scale now: 0 during a hit-stop, else slow motion's (eased), else the base. @thread_safety
 * Any thread */
float wpe_game_feel_time_scale(const WPE_GameFeel* feel);

/**
 * Advances hit-stop and slow motion by a frame's real time.
 *
 * @return The frame's game time: real time at the scales that applied through it (a hit-stop ending mid-frame
 *         freezes only its share).
 * @thread_safety Any thread
 */
double wpe_game_feel_advance(WPE_GameFeel* feel, double real_dt);

/** Slows the game to `scale` (0..) for `duration` real seconds, easing back to the base scale over the last
 * `fade` of them. Replaces slow motion already running. @thread_safety Any thread */
void wpe_game_feel_slow_motion(WPE_GameFeel* feel, float scale, float duration, float fade);

/** Freezes the game for `duration` real seconds (a longer hit-stop already running wins). @thread_safety Any
 * thread */
void wpe_game_feel_hit_stop(WPE_GameFeel* feel, float duration);

/**
 * Starts shaking an entity (replacing its shake if it has one). The entity needs a Transform.
 *
 * @thread_safety Thread owning the world
 */
void wpe_game_feel_shake(WPE_GameFeel* feel, WPE_Entity entity, const WPE_ShakeParams* params);

/** Ends an entity's shake (its pose is restored at the next restore). @thread_safety Thread owning the world
 */
void wpe_game_feel_stop_shake(WPE_GameFeel* feel, WPE_Entity entity);

/** @return Whether the entity is shaking. @thread_safety Any thread */
bool wpe_game_feel_is_shaking(const WPE_GameFeel* feel, WPE_Entity entity);

/**
 * The offset of a shake `elapsed` real seconds in: position in meters and rotation in radians (X, Y, Z), each
 * within the shake's amplitude and fading to zero at its end.
 *
 * @thread_safety Any thread
 */
void wpe_shake_offset(const WPE_ShakeParams* params, uint32_t seed, float elapsed, WPE_Vec3* out_position,
                      WPE_Vec3* out_rotation);

/** Takes the shake offsets off (call before anything reads or moves transforms in a frame). Entities that
 * moved since keep their new pose. @thread_safety Thread owning the world */
void wpe_game_feel_restore_shakes(WPE_GameFeel* feel, WPE_Ecs* ecs);

/** Advances shakes by `real_dt`, drops finished ones and puts the offsets on (call just before
 * wpe_scene_update_transforms()). @thread_safety Thread owning the world */
void wpe_game_feel_apply_shakes(WPE_GameFeel* feel, WPE_Ecs* ecs, float real_dt);

/** Restores every pose and stops all shakes, slow motion and hit-stops; the base scale returns to 1. Pass a
 * NULL world when it was cleared (nothing to restore). @thread_safety Thread owning the world */
void wpe_game_feel_reset(WPE_GameFeel* feel, WPE_Ecs* ecs);

#endif /* WPE_SCENE_GAME_FEEL_H */
