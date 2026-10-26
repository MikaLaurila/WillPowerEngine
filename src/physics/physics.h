/**
 * @file physics.h
 * @brief Rigid body physics (Box3D) driven by ECS components and stepped at a fixed rate.
 *
 * Components registered by wpe_physics_create():
 * - WPE_COMP_PHYSICS_RIGIDBODY (WPE_RigidBody): makes the entity a static, kinematic or dynamic
 *   body. Its colliders are the WPE_Collider on the same entity.
 * - WPE_COMP_PHYSICS_COLLIDER (WPE_Collider): shape, material and trigger flag. A collider on an
 *   entity without a rigid body gets an implicit static body (level geometry).
 *
 * One collider per entity. Colliders do not combine across the hierarchy: a collider on a child
 * of a rigid body entity gets its own body (an implicit static one unless the child also has a
 * rigid body).
 *
 * Synchronization each wpe_physics_step():
 * 1. Bodies/shapes for new components are created from the entity's current world transform
 *    (world scale is baked into the collider size). Component settings are read at creation only;
 *    call wpe_physics_refresh() after changing them or the entity's scale.
 * 2. When game code moved an entity since the last step, a kinematic body moves to the new pose
 *    with velocity (so it pushes dynamic bodies); any other body is teleported.
 * 3. The world is stepped.
 * 4. Dynamic and kinematic bodies write their pose back into the entity's Transform (relative to
 *    its parent; the local scale is kept).
 * 5. Contact and trigger begin/end events of this step become available via wpe_physics_events().
 *
 * NOTE(limitation): the solver runs single-threaded. Box3D's task API needs asynchronous tasks,
 * which the engine job system (blocking parallel_for) does not provide yet.
 *
 * - Dependencies: core, math, ecs, scene, assets (mesh colliders), Box3D (private)
 * - Thread affinity: thread owning the world
 *
 * @author WillPowerEngine Team
 */
#ifndef WPE_PHYSICS_PHYSICS_H
#define WPE_PHYSICS_PHYSICS_H

#include <stdint.h>

#include "assets/model.h"
#include "core/result.h"
#include "ecs/ecs.h"
#include "math/wpe_math.h"

typedef enum WPE_BodyType {
    WPE_BODY_STATIC = 0,
    WPE_BODY_KINEMATIC,
    WPE_BODY_DYNAMIC,
} WPE_BodyType;

typedef struct WPE_RigidBody {
    WPE_BodyType type;
    float linear_damping;
    float angular_damping;
    float gravity_scale;
    bool lock_rotation; /* Dynamic bodies only rotate when false (e.g. character capsules lock it). */
    bool continuous;    /* Continuous collision for fast-moving dynamic bodies. */

    /* Private (managed by the physics world). */
    uint64_t wpe__body;
    uint8_t wpe__state;    /* Pending, created or failed (not retried until wpe_physics_refresh()). */
    bool wpe__target_move; /* Kinematic body moved toward its entity's pose by the last step. */
} WPE_RigidBody;

typedef enum WPE_ColliderShape {
    WPE_COLLIDER_BOX = 0, /* size = full extents */
    WPE_COLLIDER_SPHERE,  /* size.x = radius */
    WPE_COLLIDER_CAPSULE, /* size.x = radius, size.y = total height along local Y (>= 2 * radius) */
    WPE_COLLIDER_CONVEX,  /* convex hull of `model`'s mesh `mesh` vertices (max 64 hull vertices) */
    WPE_COLLIDER_MESH,    /* triangle mesh of `model`'s mesh `mesh`; static or kinematic bodies only */
} WPE_ColliderShape;

typedef struct WPE_Collider {
    WPE_ColliderShape shape;
    WPE_Vec3 size;
    WPE_Vec3 offset;        /* Local-space center offset. */
    const WPE_Model* model; /* CONVEX / MESH: borrowed; only read when the shape is (re)built. */
    uint32_t mesh;
    float density;
    float friction;
    float restitution;
    bool is_trigger; /* Reports overlaps (trigger events) instead of colliding. */

    /* Private (managed by the physics world). */
    uint64_t wpe__shape;
    uint64_t wpe__static_body; /* Implicit static body when the entity has no rigid body. */
    void* wpe__mesh;           /* Owned collision mesh data (WPE_COLLIDER_MESH). */
    uint8_t wpe__state;        /* Pending, created or failed (not retried until wpe_physics_refresh()). */
} WPE_Collider;

typedef enum WPE_PhysicsEventType {
    WPE_PHYSICS_CONTACT_BEGIN = 0,
    WPE_PHYSICS_CONTACT_END,
    WPE_PHYSICS_TRIGGER_ENTER, /* entity_a is the trigger, entity_b the visitor */
    WPE_PHYSICS_TRIGGER_EXIT,
} WPE_PhysicsEventType;

typedef struct WPE_PhysicsEvent {
    WPE_PhysicsEventType type;
    WPE_Entity entity_a;
    WPE_Entity entity_b;
} WPE_PhysicsEvent;

typedef struct WPE_RaycastHit {
    WPE_Entity entity;
    WPE_Vec3 point;
    WPE_Vec3 normal;
    float distance;
} WPE_RaycastHit;

typedef struct WPE_PhysicsDesc {
    WPE_Ecs* ecs;      /* Borrowed; scene components must be registered. */
    WPE_Vec3 gravity;  /* Meters per second squared, e.g. {0, -9.81f, 0}. */
    uint32_t substeps; /* Solver sub-steps per step (0 = 4). */
} WPE_PhysicsDesc;

typedef struct WPE_PhysicsStats {
    uint32_t bodies;
    uint32_t awake_bodies;
    uint32_t events;
    size_t memory; /* Bytes allocated by Box3D (all worlds). */
} WPE_PhysicsStats;

typedef struct WPE_Physics WPE_Physics;

/** Line callback for debug drawing (world-space segment, 0xRRGGBB color). */
typedef void (*WPE_PhysicsLineFn)(WPE_Vec3 a, WPE_Vec3 b, uint32_t color, void* user_data);

/** @return A body of `type` with no damping and gravity scale 1. @thread_safety Any thread */
WPE_RigidBody wpe_rigidbody_default(WPE_BodyType type);

/** @return A 1x1x1 box collider with density 1, friction 0.6, restitution 0. @thread_safety Any thread */
WPE_Collider wpe_collider_default(WPE_ColliderShape shape);

/**
 * Creates a physics world for an ECS world and registers the physics components. Destroy it
 * before the ECS world.
 *
 * @return WPE_RESULT_OK; WPE_RESULT_ERROR_NULL_ARG; WPE_RESULT_ERROR_ALREADY_EXISTS (the world
 *         already has physics); WPE_RESULT_ERROR_OUT_OF_MEMORY.
 *
 * @thread_safety Thread owning the world
 */
[[nodiscard]] WPE_Result wpe_physics_create(const WPE_PhysicsDesc* desc, WPE_Physics** out_physics);

/** Removes all physics components, destroys the simulation and unregisters the components. @thread_safety
 * Owning thread */
void wpe_physics_destroy(WPE_Physics* physics);

/**
 * Advances the simulation by one fixed step (see file comment for the synchronization rules).
 *
 * @return WPE_RESULT_OK, WPE_RESULT_ERROR_NULL_ARG, WPE_RESULT_ERROR_INVALID_ARG (dt <= 0) or
 *         WPE_RESULT_ERROR_OUT_OF_MEMORY (event buffer).
 *
 * @thread_safety Owning thread
 */
WPE_Result wpe_physics_step(WPE_Physics* physics, float dt);

/**
 * Rebuilds the body and shape of an entity from its current components and transform (after
 * changing collider/rigid body settings or the entity's scale).
 *
 * @thread_safety Owning thread
 */
WPE_Result wpe_physics_refresh(WPE_Physics* physics, WPE_Entity entity);

/** @return Events of the most recent step (borrowed until the next step). @thread_safety Owning thread */
const WPE_PhysicsEvent* wpe_physics_events(const WPE_Physics* physics, uint32_t* out_count);

/** Sets the world gravity. @thread_safety Owning thread */
void wpe_physics_set_gravity(WPE_Physics* physics, WPE_Vec3 gravity);

/** @return The world gravity. @thread_safety Owning thread */
WPE_Vec3 wpe_physics_gravity(const WPE_Physics* physics);

/**
 * Casts a ray against all non-trigger colliders and reports the closest hit.
 *
 * @param direction Need not be normalized; zero vectors never hit.
 * @return true when something was hit within max_distance.
 *
 * @thread_safety Owning thread
 */
bool wpe_physics_raycast(const WPE_Physics* physics, WPE_Vec3 origin, WPE_Vec3 direction, float max_distance,
                         WPE_RaycastHit* out_hit);

/* Body control. These fail with WPE_RESULT_ERROR_NOT_FOUND when the entity has no simulated
 * rigid body yet (bodies are created at the next step); forces apply to dynamic bodies only. */
WPE_Result wpe_physics_apply_force(WPE_Physics* physics, WPE_Entity entity, WPE_Vec3 force);
WPE_Result wpe_physics_apply_impulse(WPE_Physics* physics, WPE_Entity entity, WPE_Vec3 impulse);
WPE_Result wpe_physics_set_velocity(WPE_Physics* physics, WPE_Entity entity, WPE_Vec3 velocity);
WPE_Result wpe_physics_get_velocity(const WPE_Physics* physics, WPE_Entity entity, WPE_Vec3* out_velocity);

/** Emits wireframe lines for every collider (debug visualization). @thread_safety Owning thread */
void wpe_physics_debug_draw(const WPE_Physics* physics, WPE_PhysicsLineFn line, void* user_data);

/** @return Current statistics. @thread_safety Owning thread */
WPE_PhysicsStats wpe_physics_stats(const WPE_Physics* physics);

#endif /* WPE_PHYSICS_PHYSICS_H */
