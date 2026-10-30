/**
 * @file ecs.h
 * @brief Entity Component System: generational entity handles and sparse-set component pools.
 *
 * Model:
 * - An entity is a 32-bit handle: low WPE_ECS_ENTITY_INDEX_BITS bits are a slot index, the high
 *   bits a generation counter. Despawning bumps the generation, so stale handles are detected.
 *   Generation 0 is never issued, which makes WPE_ENTITY_NULL (0) permanently invalid. A slot whose
 *   generation is exhausted is retired rather than reused, so handles never alias.
 * - Component types are registered once per world with a fixed id (WPE_ComponentType) and a POD
 *   layout. Each type is stored in its own pool: a dense, contiguous array of component data plus
 *   a parallel entity array, with a sparse index for O(1) lookup. Removal swaps the last element in.
 * - Queries iterate the smallest pool among the required types and filter by per-entity masks.
 *
 * Structural-change rules (adding/removing components, spawning/despawning entities):
 * - Any add or spawn may reallocate pools: pointers previously obtained from
 *   wpe_ecs_get_component()/wpe_ecs_iter_get() become invalid. Re-fetch after structural changes.
 * - While a query is iterating, adding components and spawning entities is allowed (new entries are
 *   not visited by the running iteration). Removing components or despawning is NOT allowed; use
 *   wpe_ecs_despawn_deferred() and wpe_ecs_flush_deferred(). Debug builds assert on violations and
 *   the iteration stops early in all builds.
 * - Component hooks may read and write component data but must not make structural changes.
 *   When an entity is despawned (or the world destroyed), on_remove hooks run in descending
 *   component type id order before any storage is released, so hooks of higher ids (e.g. scripts)
 *   still see lower-id components (transform, hierarchy) unmodified.
 *
 * - Dependencies: core
 * - Thread affinity: a world is not internally synchronized. Concurrent read-only access (get,
 *   has, iteration) from multiple threads is safe only while no thread mutates the world.
 *
 * @author WillPowerEngine Team
 */
#ifndef WPE_ECS_ECS_H
#define WPE_ECS_ECS_H

#include <stddef.h>
#include <stdint.h>

#include "core/result.h"

/** Entity handle. Treat as opaque; compare with == and test against WPE_ENTITY_NULL. */
typedef uint32_t WPE_Entity;

#define WPE_ENTITY_NULL ((WPE_Entity)0)
#define WPE_ECS_ENTITY_INDEX_BITS 20u
#define WPE_ECS_ENTITY_GENERATION_BITS (32u - WPE_ECS_ENTITY_INDEX_BITS)
/** Maximum number of simultaneously alive entities (and of slots ever created). */
#define WPE_ECS_MAX_ENTITIES (1u << WPE_ECS_ENTITY_INDEX_BITS)
/** Highest generation a slot can reach before it is retired. */
#define WPE_ECS_MAX_GENERATION ((1u << WPE_ECS_ENTITY_GENERATION_BITS) - 1u)

/** Maximum number of distinct component type ids (masks are 64-bit). */
#define WPE_ECS_MAX_COMPONENT_TYPES 64u
/** Maximum component type name length, including the terminator. */
#define WPE_ECS_COMPONENT_NAME_MAX 32u

/**
 * Component type id. An integer rather than an enum so games can define their own id enums
 * (starting at WPE_COMP_USER_FIRST) without enum-conversion warnings.
 */
typedef uint32_t WPE_ComponentType;

/**
 * Built-in component type ids. Values are stable (they appear in serialized scenes), so new
 * built-ins are only ever appended before WPE_COMP_USER_FIRST. Ids from WPE_COMP_USER_FIRST up to
 * WPE_ECS_MAX_COMPONENT_TYPES - 1 are free for game- or tool-defined components.
 */
enum WPE_BuiltinComponent {
    WPE_COMP_NONE = 0, /* Reserved; never registered. */
    WPE_COMP_TRANSFORM = 1,
    WPE_COMP_HIERARCHY = 2,
    WPE_COMP_NAME = 3,
    WPE_COMP_MESH = 4,
    WPE_COMP_MATERIAL = 5,
    WPE_COMP_LIGHT = 6,
    WPE_COMP_CAMERA = 7,
    WPE_COMP_SCRIPT = 8,
    WPE_COMP_PHYSICS_RIGIDBODY = 9,
    WPE_COMP_PHYSICS_COLLIDER = 10,
    WPE_COMP_AUDIO_SOURCE = 11,
    WPE_COMP_ANIMATOR = 12,
    WPE_COMP_PREFAB = 13, /* Prefab instance membership (engine/prefab.h). */
    WPE_COMP_UI = 14,     /* UI elements (ui/ui.h): rectangle, sprite, text, button. */
    WPE_COMP_SPRITE = 15,
    WPE_COMP_TEXT = 16,
    WPE_COMP_BUTTON = 17,
    WPE_COMP_BILLBOARD = 18, /* 3D sprites (graphics/graphics.h). */
    WPE_COMP_PARTICLES = 19, /* Particle emitters (graphics/graphics.h). */
    WPE_COMP_CHARACTER = 20, /* Character controllers (physics/physics.h). */
    WPE_COMP_TOGGLE = 21,    /* UI toggles and sliders (ui/ui.h). */
    WPE_COMP_SLIDER = 22,
    WPE_COMP_TRAIL = 23, /* Weapon trails (graphics/graphics.h). */
    WPE_COMP_USER_FIRST = 32,
};

/** Bit for a component type in a WPE_EcsQuery mask. */
#define WPE_ECS_MASK(type) (UINT64_C(1) << (unsigned)(type))

typedef struct WPE_Ecs WPE_Ecs;

/**
 * Lifecycle hook for a component type.
 *
 * @param ecs The world. Hooks must not make structural changes (see file comment).
 * @param entity The entity owning the component.
 * @param component Pointer to the component data; valid only during the call.
 * @param user_data The pointer given in WPE_ComponentDesc.
 */
typedef void (*WPE_ComponentHookFn)(WPE_Ecs* ecs, WPE_Entity entity, void* component, void* user_data);

/** Describes a component type for registration. */
typedef struct WPE_ComponentDesc {
    const char* name;              /* Required; copied (truncated to WPE_ECS_COMPONENT_NAME_MAX - 1). */
    uint32_t size;                 /* sizeof the component struct; must be > 0. */
    uint32_t alignment;            /* alignof the component struct; power of two <= alignof(max_align_t). */
    WPE_ComponentHookFn on_add;    /* Optional; called after the component is added and filled. */
    WPE_ComponentHookFn on_remove; /* Optional; called before removal, despawn or world destroy. */
    void* hook_user_data;          /* Passed to hooks; caller retains ownership. */
} WPE_ComponentDesc;

/** Query filter: entities having all of `all` and none of `none` (masks built with WPE_ECS_MASK). */
typedef struct WPE_EcsQuery {
    uint64_t all;
    uint64_t none;
} WPE_EcsQuery;

/** Query iterator. Fields other than `entity` are private to the ECS. */
typedef struct WPE_EcsIter {
    WPE_Entity entity; /* Current entity after wpe_ecs_iter_next() returns true. */

    WPE_Ecs* wpe__ecs;
    WPE_EcsQuery wpe__query;
    uint32_t wpe__driver; /* Pool driving iteration, or WPE_COMP_NONE to scan entity slots. */
    uint32_t wpe__cursor;
    uint32_t wpe__end;
    uint32_t wpe__dense_index; /* Dense index of `entity` in the driver pool. */
    uint64_t wpe__removal_version;
    uint64_t wpe__spawn_serial_end; /* Slot scans skip entities spawned after the query began. */
} WPE_EcsIter;

/** Read-only view of one pool's contiguous storage. Invalidated by any structural change. */
typedef struct WPE_EcsPoolView {
    const WPE_Entity* entities; /* entities[i] owns the component at data + i * stride. */
    void* data;
    uint32_t count;
    uint32_t stride;
} WPE_EcsPoolView;

/**
 * Creates an empty world with no component types registered.
 *
 * @param out_ecs Receives the world; must not be NULL. The caller owns it and releases it with
 *                wpe_ecs_destroy().
 * @return WPE_RESULT_OK, WPE_RESULT_ERROR_NULL_ARG or WPE_RESULT_ERROR_OUT_OF_MEMORY.
 *
 * @thread_safety Any thread
 */
[[nodiscard]] WPE_Result wpe_ecs_create(WPE_Ecs** out_ecs);

/**
 * Destroys a world. on_remove hooks run for every remaining component.
 *
 * @param ecs World to destroy (ownership consumed), or NULL (no-op).
 *
 * @thread_safety Thread owning the world
 */
void wpe_ecs_destroy(WPE_Ecs* ecs);

/**
 * Registers a component type under a fixed id.
 *
 * @param ecs World; must not be NULL.
 * @param type Id in [1, WPE_ECS_MAX_COMPONENT_TYPES).
 * @param desc Layout and hooks; must not be NULL. Contents are copied.
 * @return WPE_RESULT_OK; WPE_RESULT_ERROR_NULL_ARG; WPE_RESULT_ERROR_INVALID_ARG for a bad id,
 *         size, alignment or missing name; WPE_RESULT_ERROR_ALREADY_EXISTS if the id is taken.
 *
 * @thread_safety Thread owning the world
 */
[[nodiscard]] WPE_Result wpe_ecs_register_component(WPE_Ecs* ecs, WPE_ComponentType type,
                                                    const WPE_ComponentDesc* desc);

/**
 * Unregisters a component type so its id can be registered again (e.g. when a subsystem that
 * owns the type's hooks shuts down). Only allowed when no entity has the component.
 *
 * @return WPE_RESULT_OK, WPE_RESULT_ERROR_NULL_ARG, WPE_RESULT_ERROR_NOT_FOUND (not registered) or
 *         WPE_RESULT_ERROR_INVALID_STATE (components of this type still exist, or called from a hook).
 *
 * @thread_safety Thread owning the world
 */
WPE_Result wpe_ecs_unregister_component(WPE_Ecs* ecs, WPE_ComponentType type);

/** @return Whether `type` is registered in `ecs`. @thread_safety Thread owning the world */
bool wpe_ecs_is_registered(const WPE_Ecs* ecs, WPE_ComponentType type);

/** @return The registered name of `type`, or NULL if unregistered. @thread_safety Thread owning the world */
const char* wpe_ecs_component_name(const WPE_Ecs* ecs, WPE_ComponentType type);

/** @return The registered size in bytes of `type`, or 0 if unregistered. @thread_safety Thread owning the
 * world */
uint32_t wpe_ecs_component_size(const WPE_Ecs* ecs, WPE_ComponentType type);

/**
 * Creates an entity with no components.
 *
 * @return The new entity, or WPE_ENTITY_NULL if the entity limit is reached or memory is
 *         exhausted (an ERROR is logged).
 *
 * @thread_safety Thread owning the world
 */
WPE_Entity wpe_ecs_spawn(WPE_Ecs* ecs);

/**
 * Destroys an entity immediately, removing all its components (on_remove hooks run first).
 * Must not be called while a query is iterating; use wpe_ecs_despawn_deferred() there.
 *
 * @return WPE_RESULT_OK, WPE_RESULT_ERROR_NULL_ARG, WPE_RESULT_ERROR_NOT_FOUND for a dead or
 *         stale handle, or WPE_RESULT_ERROR_INVALID_STATE when called from a hook.
 *
 * @thread_safety Thread owning the world
 */
WPE_Result wpe_ecs_despawn(WPE_Ecs* ecs, WPE_Entity entity);

/**
 * Queues an entity for destruction at the next wpe_ecs_flush_deferred(). Safe during iteration.
 * Queuing the same entity more than once is harmless.
 *
 * @return WPE_RESULT_OK, WPE_RESULT_ERROR_NULL_ARG, WPE_RESULT_ERROR_NOT_FOUND or
 *         WPE_RESULT_ERROR_OUT_OF_MEMORY.
 *
 * @thread_safety Thread owning the world
 */
WPE_Result wpe_ecs_despawn_deferred(WPE_Ecs* ecs, WPE_Entity entity);

/**
 * Despawns every queued entity that is still alive. Must not be called while iterating.
 *
 * @return Number of entities despawned.
 *
 * @thread_safety Thread owning the world
 */
uint32_t wpe_ecs_flush_deferred(WPE_Ecs* ecs);

/** @return Whether `entity` refers to a currently alive entity. @thread_safety See file comment */
bool wpe_ecs_is_alive(const WPE_Ecs* ecs, WPE_Entity entity);

/**
 * @return A number that orders alive entities by creation (later spawns have larger values; never
 *         reused within a world), or 0 for dead handles. Slot indices do not give that order, since
 *         freed slots are reused.
 *
 * @thread_safety See file comment
 */
uint64_t wpe_ecs_spawn_serial(const WPE_Ecs* ecs, WPE_Entity entity);

/** @return Number of alive entities. @thread_safety See file comment */
uint32_t wpe_ecs_entity_count(const WPE_Ecs* ecs);

/**
 * Adds a component to an entity.
 *
 * @param ecs World; must not be NULL.
 * @param entity Alive entity.
 * @param type Registered component type.
 * @param initial Data copied into the new component (size bytes), or NULL to zero-initialize.
 * @param out_component Optional; receives a pointer to the stored component (borrowed; invalidated
 *                      by structural changes).
 * @return WPE_RESULT_OK; WPE_RESULT_ERROR_NULL_ARG; WPE_RESULT_ERROR_NOT_FOUND for a dead entity;
 *         WPE_RESULT_ERROR_INVALID_ARG for an unregistered type; WPE_RESULT_ERROR_ALREADY_EXISTS
 *         if the entity already has it; WPE_RESULT_ERROR_INVALID_STATE when called from a hook;
 *         WPE_RESULT_ERROR_OUT_OF_MEMORY.
 *
 * @thread_safety Thread owning the world
 */
WPE_Result wpe_ecs_add_component(WPE_Ecs* ecs, WPE_Entity entity, WPE_ComponentType type, const void* initial,
                                 void** out_component);

/**
 * Removes a component from an entity (its on_remove hook runs first). Not allowed while a query is
 * iterating.
 *
 * @return WPE_RESULT_OK; WPE_RESULT_ERROR_NULL_ARG; WPE_RESULT_ERROR_NOT_FOUND if the entity is
 *         dead or lacks the component; WPE_RESULT_ERROR_INVALID_ARG for an unregistered type;
 *         WPE_RESULT_ERROR_INVALID_STATE when called from a hook.
 *
 * @thread_safety Thread owning the world
 */
WPE_Result wpe_ecs_remove_component(WPE_Ecs* ecs, WPE_Entity entity, WPE_ComponentType type);

/**
 * @return Borrowed pointer to the entity's component, or NULL if the entity is dead, the type is
 *         unregistered, or the entity lacks it. Invalidated by structural changes.
 *
 * @thread_safety See file comment
 */
void* wpe_ecs_get_component(const WPE_Ecs* ecs, WPE_Entity entity, WPE_ComponentType type);

/** @return Whether the alive entity has the component. @thread_safety See file comment */
bool wpe_ecs_has_component(const WPE_Ecs* ecs, WPE_Entity entity, WPE_ComponentType type);

/** @return Component mask of an alive entity (0 for dead handles). @thread_safety See file comment */
uint64_t wpe_ecs_component_mask(const WPE_Ecs* ecs, WPE_Entity entity);

/** @return Number of components of `type` (0 if unregistered). @thread_safety See file comment */
uint32_t wpe_ecs_component_count(const WPE_Ecs* ecs, WPE_ComponentType type);

/**
 * Exposes a pool's contiguous storage for batch processing.
 *
 * @param out_view Receives the view; zeroed if the type is unregistered.
 * @return WPE_RESULT_OK, WPE_RESULT_ERROR_NULL_ARG or WPE_RESULT_ERROR_INVALID_ARG.
 *
 * @thread_safety See file comment
 */
WPE_Result wpe_ecs_pool_view(const WPE_Ecs* ecs, WPE_ComponentType type, WPE_EcsPoolView* out_view);

/**
 * Starts iterating entities matching `query`. With query.all == 0 every alive entity is a candidate.
 * Required types that are unregistered match nothing.
 *
 * @code
 * WPE_EcsIter it = wpe_ecs_query(ecs, (WPE_EcsQuery){.all = WPE_ECS_MASK(WPE_COMP_TRANSFORM)});
 * while (wpe_ecs_iter_next(&it)) {
 *     WPE_Transform* t = wpe_ecs_iter_get(&it, WPE_COMP_TRANSFORM);
 * }
 * @endcode
 *
 * @thread_safety See file comment
 */
WPE_EcsIter wpe_ecs_query(WPE_Ecs* ecs, WPE_EcsQuery query);

/**
 * Advances to the next matching entity.
 *
 * @return true with it->entity set, or false when finished. Returns false early (after a debug
 *         assertion) if components were removed or entities despawned during iteration.
 *
 * @thread_safety See file comment
 */
bool wpe_ecs_iter_next(WPE_EcsIter* it);

/**
 * @return Borrowed pointer to the current entity's component of `type`, or NULL if it lacks it
 *         (only possible for types outside query.all).
 *
 * @thread_safety See file comment
 */
void* wpe_ecs_iter_get(const WPE_EcsIter* it, WPE_ComponentType type);

#endif /* WPE_ECS_ECS_H */
