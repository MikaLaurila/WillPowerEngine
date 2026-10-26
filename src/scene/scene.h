/**
 * @file scene.h
 * @brief Built-in scene components (transform, hierarchy, name) and their systems.
 *
 * Components registered by wpe_scene_register_components():
 * - WPE_COMP_TRANSFORM (WPE_Transform): local TRS relative to the parent, plus the world matrix
 *   computed by wpe_scene_update_transforms().
 * - WPE_COMP_HIERARCHY (WPE_Hierarchy): parent/child links forming a forest. Maintained through
 *   wpe_scene_set_parent(); never edit the links directly. If an entity with children is despawned
 *   or loses its hierarchy component, its children become roots (use wpe_scene_destroy_recursive()
 *   to delete a whole subtree instead).
 * - WPE_COMP_NAME (WPE_Name): display name for tools and debugging. Not used for runtime lookups.
 *
 * Transform propagation: world = parent_world * TRS(local). Entities in the hierarchy without a
 * transform are transparent: their children inherit the nearest transformed ancestor's world.
 *
 * - Dependencies: core, math, ecs
 * - Thread affinity: same rules as the owning WPE_Ecs world
 *
 * @author WillPowerEngine Team
 */
#ifndef WPE_SCENE_SCENE_H
#define WPE_SCENE_SCENE_H

#include "core/result.h"
#include "ecs/ecs.h"
#include "math/wpe_math.h"

/** Maximum name length, including the terminator. */
#define WPE_NAME_MAX 64u

typedef struct WPE_Transform {
    WPE_Vec3 position; /* Local, relative to parent. */
    WPE_Quat rotation; /* Local; normalized when the world matrix is built. */
    WPE_Vec3 scale;    /* Local. */
    WPE_Mat4 world;    /* Output of wpe_scene_update_transforms(); treat as read-only. */
} WPE_Transform;

typedef struct WPE_Hierarchy {
    WPE_Entity parent;      /* WPE_ENTITY_NULL for roots. */
    WPE_Entity first_child; /* Children form a doubly linked list in insertion order. */
    WPE_Entity next_sibling;
    WPE_Entity prev_sibling;
    uint32_t child_count;
} WPE_Hierarchy;

typedef struct WPE_Name {
    char value[WPE_NAME_MAX];
} WPE_Name;

/** @return A transform at the origin with identity rotation and unit scale. @thread_safety Any thread */
WPE_Transform wpe_transform_identity(void);

/**
 * Registers WPE_COMP_TRANSFORM, WPE_COMP_HIERARCHY and WPE_COMP_NAME in a world.
 *
 * @return WPE_RESULT_OK, WPE_RESULT_ERROR_NULL_ARG, or the registration error (e.g.
 *         WPE_RESULT_ERROR_ALREADY_EXISTS when called twice).
 *
 * @thread_safety Thread owning the world
 */
[[nodiscard]] WPE_Result wpe_scene_register_components(WPE_Ecs* ecs);

/**
 * Attaches `child` under `parent` (appended as the last child), or detaches it when parent is
 * WPE_ENTITY_NULL. Adds hierarchy components as needed.
 *
 * @param keep_world When true and the child has a transform, its local transform is rewritten so
 *                   its world placement is unchanged. Fails without modification if that is
 *                   impossible (singular parent matrix or degenerate scale).
 *                   NOTE(limitation): under a rotated parent with non-uniform scale the exact
 *                   local matrix contains shear, which TRS cannot represent; world position is
 *                   preserved exactly, rotation and scale best-effort.
 * @return WPE_RESULT_OK; WPE_RESULT_ERROR_NULL_ARG; WPE_RESULT_ERROR_NOT_FOUND for dead entities;
 *         WPE_RESULT_ERROR_INVALID_ARG when parenting would create a cycle (including to itself);
 *         WPE_RESULT_ERROR_INVALID_STATE when keep_world cannot be satisfied; or an ECS error.
 *
 * @thread_safety Thread owning the world
 */
WPE_Result wpe_scene_set_parent(WPE_Ecs* ecs, WPE_Entity child, WPE_Entity parent, bool keep_world);

/** @return The entity's parent, or WPE_ENTITY_NULL for roots and dead entities. @thread_safety See file
 * comment */
WPE_Entity wpe_scene_get_parent(const WPE_Ecs* ecs, WPE_Entity entity);

/** @return Whether `ancestor` is a strict ancestor of `entity`. @thread_safety See file comment */
bool wpe_scene_is_ancestor(const WPE_Ecs* ecs, WPE_Entity ancestor, WPE_Entity entity);

/**
 * Despawns an entity and all of its descendants immediately. Not allowed during query iteration.
 *
 * @return WPE_RESULT_OK, WPE_RESULT_ERROR_NULL_ARG, WPE_RESULT_ERROR_NOT_FOUND, or
 *         WPE_RESULT_ERROR_OUT_OF_MEMORY (nothing is despawned then).
 *
 * @thread_safety Thread owning the world
 */
WPE_Result wpe_scene_destroy_recursive(WPE_Ecs* ecs, WPE_Entity entity);

/**
 * Queues an entity and its current descendants for despawn at the next wpe_ecs_flush_deferred().
 * Safe during iteration. Descendants attached after this call are orphaned instead of destroyed.
 *
 * @return Same codes as wpe_scene_destroy_recursive().
 *
 * @thread_safety Thread owning the world
 */
WPE_Result wpe_scene_destroy_recursive_deferred(WPE_Ecs* ecs, WPE_Entity entity);

/**
 * Computes an entity's world matrix from the current local transforms of it and its ancestors,
 * independent of the cached WPE_Transform.world values.
 *
 * @param out_world Receives the matrix (identity for an entity without transformed ancestry).
 * @return false if the entity is dead or an argument is NULL.
 *
 * @thread_safety See file comment
 */
bool wpe_scene_compute_world_matrix(const WPE_Ecs* ecs, WPE_Entity entity, WPE_Mat4* out_world);

/**
 * Recomputes WPE_Transform.world for every transform in the world, parents before children.
 *
 * @return WPE_RESULT_OK, WPE_RESULT_ERROR_NULL_ARG or WPE_RESULT_ERROR_OUT_OF_MEMORY.
 *
 * @thread_safety Thread owning the world
 */
WPE_Result wpe_scene_update_transforms(WPE_Ecs* ecs);

/**
 * Sets (adding if needed) an entity's name. Longer names are truncated to WPE_NAME_MAX - 1 bytes.
 *
 * @return WPE_RESULT_OK, WPE_RESULT_ERROR_NULL_ARG, WPE_RESULT_ERROR_NOT_FOUND or an ECS error.
 *
 * @thread_safety Thread owning the world
 */
WPE_Result wpe_scene_set_name(WPE_Ecs* ecs, WPE_Entity entity, const char* name);

/** @return The entity's name, or NULL when it has none. Borrowed. @thread_safety See file comment */
const char* wpe_scene_get_name(const WPE_Ecs* ecs, WPE_Entity entity);

#endif /* WPE_SCENE_SCENE_H */
