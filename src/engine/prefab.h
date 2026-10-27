/**
 * @file prefab.h
 * @brief Prefabs: reusable entity trees saved as ".prefab" files, placed in scenes as linked
 *        instances (format in docs/scenes.md).
 *
 * A prefab file holds entities in the scene-file schema, exactly one of them the root. An instance
 * is a copy of those entities whose members remember where they came from (WPE_COMP_PREFAB): a scene
 * stores the instance as a reference to the prefab plus its overrides, the components its members
 * changed (or removed), so editing the prefab updates every instance where it was not overridden.
 * The instance's own name, parent and transform belong to the instance, not to the prefab.
 *
 * Overrides are found by comparing the members with the prefab as instantiated, so every change
 * counts: the editor's, an agent's, or a script's.
 *
 * - Dependencies: engine (scene files), ecs, scene, vfs
 * - Thread affinity: thread owning the world
 *
 * @author WillPowerEngine Team
 */
#ifndef WPE_ENGINE_PREFAB_H
#define WPE_ENGINE_PREFAB_H

#include "engine/scene_file.h"

/** Format version of prefab files written by this engine. */
#define WPE_PREFAB_FORMAT_VERSION 1u

/**
 * Registers WPE_COMP_PREFAB, which marks the members of prefab instances. Needed before loading
 * scenes with prefab instances.
 *
 * @return WPE_RESULT_OK, WPE_RESULT_ERROR_NULL_ARG or an ECS registration error.
 *
 * @thread_safety Owning thread
 */
[[nodiscard]] WPE_Result wpe_prefab_register(WPE_Ecs* ecs);

/** Creates a cache of parsed prefab files (each is read again when its file changes). @return WPE_RESULT_OK,
 * WPE_RESULT_ERROR_NULL_ARG or WPE_RESULT_ERROR_OUT_OF_MEMORY. @thread_safety Any thread */
[[nodiscard]] WPE_Result wpe_prefab_cache_create(WPE_PrefabCache** out_cache);

/** Destroys a cache; NULL is a no-op. Instances made from it keep working. @thread_safety Owning thread */
void wpe_prefab_cache_destroy(WPE_PrefabCache* cache);

/** @return Whether an asset path names a prefab file (it ends in ".prefab"). @thread_safety Any thread */
bool wpe_prefab_is_path(const char* path);

/**
 * Creates an instance of a prefab: its entities, the root a new root of the world.
 *
 * @param path Asset path of the prefab file.
 * @param out_root Receives the instance root.
 * @return WPE_RESULT_OK, WPE_RESULT_ERROR_NULL_ARG, WPE_RESULT_ERROR_NOT_FOUND (no such file),
 *         WPE_RESULT_ERROR_PARSE (a malformed prefab; logged with the line) or another loading error.
 *
 * @thread_safety Owning thread
 */
WPE_Result wpe_prefab_instantiate(const WPE_SceneContext* context, const char* path, WPE_Entity* out_root);

/** @return The root of the prefab instance `entity` belongs to (itself for a root), or WPE_ENTITY_NULL.
 * @thread_safety Owning thread */
WPE_Entity wpe_prefab_root(const WPE_Ecs* ecs, WPE_Entity entity);

/** @return The asset path of the prefab `entity`'s instance was made from (borrowed while the instance
 * lives), or NULL. @thread_safety Owning thread */
const char* wpe_prefab_path(const WPE_Ecs* ecs, WPE_Entity entity);

/**
 * Saves an entity and its descendants as a prefab file and makes them an instance of it.
 * - A plain entity tree becomes a new prefab. Its root is stored at the origin, keeping its rotation
 *   and scale.
 * - An instance root writes its instance into the prefab it came from ("apply"). Its members keep
 *   their prefab ids, its added children become members, and the prefab root keeps its name and
 *   transform.
 *
 * Other instances of the prefab are not changed; reload the scene to update them.
 *
 * @param path Asset path the instance refers to (the prefab written).
 * @param os_path Where to write the file.
 * @return WPE_RESULT_OK, WPE_RESULT_ERROR_NULL_ARG, WPE_RESULT_ERROR_NOT_FOUND (dead entity),
 *         WPE_RESULT_ERROR_INVALID_ARG (an instance member that is not the root, or a tree holding other
 *         instances; logged), WPE_RESULT_ERROR_IO or WPE_RESULT_ERROR_OUT_OF_MEMORY.
 *
 * @thread_safety Owning thread
 */
WPE_Result wpe_prefab_save_file(const WPE_SceneContext* context, WPE_Entity root, const char* path,
                                const char* os_path);

/**
 * Reverts an instance to its prefab.
 *
 * @param entity A member of the instance.
 * @param key A component key of `entity` (or "name") to revert just that, or NULL to rebuild the whole
 *            instance from the prefab (a new root, with the old one's name, parent and transform; added
 *            children are dropped).
 * @param out_root Optional; receives the instance root (new when the instance was rebuilt).
 * @return WPE_RESULT_OK, WPE_RESULT_ERROR_NULL_ARG, WPE_RESULT_ERROR_NOT_FOUND (not an instance member)
 *         or a loading error.
 *
 * @thread_safety Owning thread
 */
WPE_Result wpe_prefab_revert(const WPE_SceneContext* context, WPE_Entity entity, const char* key,
                             WPE_Entity* out_root);

/** Turns the instance `entity` belongs to into plain entities (the link to the prefab is dropped).
 * @return WPE_RESULT_OK, WPE_RESULT_ERROR_NULL_ARG or WPE_RESULT_ERROR_NOT_FOUND. @thread_safety Owning
 * thread */
WPE_Result wpe_prefab_unpack(WPE_Ecs* ecs, WPE_Entity entity);

/**
 * @param key A component key (or "name"); NULL asks whether anything of the member differs.
 * @return Whether an instance member differs from its prefab (false for other entities). The instance
 *         root's own name and transform never count.
 *
 * @thread_safety Owning thread
 */
bool wpe_prefab_is_overridden(const WPE_SceneContext* context, WPE_Entity entity, const char* key);

#endif /* WPE_ENGINE_PREFAB_H */
