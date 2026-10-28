/**
 * @file scene_file.h
 * @brief Scene files: JSON documents holding a scene's entities, their components and the
 *        environment (render settings, post-processing, gravity, music).
 *
 * The format (".scene", documented in docs/scenes.md) is meant to be written by tools and by hand
 * (or by AI agents): JSON with named fields, assets referenced by path, angles in degrees, and
 * entity references as file-local ids. Loading is strict: unknown keys, wrong types, out-of-range
 * values and dangling references are errors naming the line, so mistakes never pass silently.
 *
 * Saving writes every alive entity: roots in entity order, each followed by its descendants in
 * child order, so loading recreates the same hierarchy. Saved state is the authored state of each
 * component (simulation internals such as physics bodies are recreated on load). Script
 * components save their path and the properties set through wpe_script_set_property() with their
 * current values.
 *
 * Game-defined components take part through WPE_ComponentSerializer entries, stored under the
 * entity's "components" object by name.
 *
 * - Dependencies: core (JSON), ecs, scene, graphics, physics, audio, script, assets
 * - Thread affinity: thread owning the world
 *
 * @author WillPowerEngine Team
 */
#ifndef WPE_ENGINE_SCENE_FILE_H
#define WPE_ENGINE_SCENE_FILE_H

#include <stdint.h>

#include "assets/asset_manager.h"
#include "core/json.h"
#include "core/result.h"
#include "ecs/ecs.h"
#include "graphics/postfx.h"
#include "graphics/scene_render.h"
#include "script/script.h"

/** Scene file format version written by this engine (files with a newer version are rejected). */
#define WPE_SCENE_FORMAT_VERSION 1u
/** Maximum number of post-processing effects in a scene environment. */
#define WPE_SCENE_MAX_POSTFX 8u

typedef enum WPE_PostFxKind {
    WPE_POSTFX_SSAO = 0,
    WPE_POSTFX_CHROMATIC_ABERRATION,
    WPE_POSTFX_PS1,
    WPE_POSTFX_VIGNETTE,
    WPE_POSTFX_COLOR_ADJUST,
} WPE_PostFxKind;

/** One configured post-processing effect; only the parameters matching `kind` are used. */
typedef struct WPE_PostFxConfig {
    WPE_PostFxKind kind;
    bool enabled;
    WPE_SsaoParams ssao;
    WPE_ChromaticAberrationParams chromatic_aberration;
    WPE_Ps1Params ps1;
    WPE_VignetteParams vignette;
    WPE_ColorAdjustParams color_adjust;
} WPE_PostFxConfig;

/** Scene-wide settings stored with the entities. */
typedef struct WPE_SceneEnvironment {
    WPE_Vec3 clear_color; /* Background (sRGB, 0..1) where nothing is drawn. */
    WPE_SceneRenderSettings render;
    WPE_PostFxConfig postfx[WPE_SCENE_MAX_POSTFX]; /* Applied in order. */
    uint32_t postfx_count;
    WPE_Vec3 gravity;             /* Physics gravity (m/s^2). */
    char music[WPE_VFS_PATH_MAX]; /* Music track started when the scene loads ("" = none). */
    float music_fade;             /* Crossfade into the track, seconds. */
} WPE_SceneEnvironment;

/** Maps entities to file ids while saving and file ids to entities while loading. Opaque. */
typedef struct WPE_SceneIds WPE_SceneIds;

/** @return The file id of `entity` while saving (0 for WPE_ENTITY_NULL or entities not saved). @thread_safety
 * Owning thread */
uint32_t wpe_scene_ids_to_file(const WPE_SceneIds* ids, WPE_Entity entity);

/** @return The entity created for file id `id` while loading (WPE_ENTITY_NULL for 0 or unknown ids).
 * @thread_safety Owning thread */
WPE_Entity wpe_scene_ids_to_entity(const WPE_SceneIds* ids, uint32_t id);

typedef struct WPE_SceneContext WPE_SceneContext;

/** Parsed prefab files, reused while they are unchanged on disk (engine/prefab.h). */
typedef struct WPE_PrefabCache WPE_PrefabCache;

/** Saves and loads one game-defined component type. */
typedef struct WPE_ComponentSerializer {
    WPE_ComponentType type;
    const char* name; /* Key under the entity's "components" object; unique. */
    /* Writes exactly one JSON value describing `component`. Errors are logged and returned. */
    WPE_Result (*save)(const WPE_SceneContext* context, const WPE_SceneIds* ids, WPE_Entity entity,
                       const void* component, WPE_JsonWriter* writer, void* user_data);
    /* Fills `out_component` (zeroed, the component's size) from `value`; the loader adds it to the
     * entity afterwards. Errors are logged and returned (WPE_RESULT_ERROR_PARSE for bad data). */
    WPE_Result (*load)(const WPE_SceneContext* context, const WPE_SceneIds* ids, WPE_Entity entity,
                       const WPE_JsonValue* value, void* out_component, void* user_data);
    void* user_data;
} WPE_ComponentSerializer;

/** What scene files read from and write to. */
struct WPE_SceneContext {
    WPE_Ecs* ecs;             /* Scene and graphics components registered; physics and audio optional. */
    WPE_AssetManager* assets; /* Assets are loaded by path through it. */
    WPE_ScriptHost* scripts;  /* Optional: needed for scenes with scripts. */
    const WPE_ComponentSerializer* serializers; /* Game components (may be NULL). */
    uint32_t serializer_count;
    /* Optional: entities for which this returns false are not saved, nor are their descendants
     * (e.g. an editor's own camera). */
    bool (*include)(WPE_Entity entity, void* user_data);
    void* include_user_data;
    /* Optional: parsed prefab files to reuse (without it, every instance reads its prefab file). Scenes
     * with prefab instances need WPE_COMP_PREFAB registered (wpe_prefab_register()). */
    WPE_PrefabCache* prefabs;
};

/** @return The default environment: render defaults, no post-processing, gravity -9.81 Y, a dark blue
 * background, no music. @thread_safety Any thread */
WPE_SceneEnvironment wpe_scene_environment_default(void);

/**
 * Builds the WPE_PostEffect list for wpe_postfx_apply() from an environment.
 *
 * @param out_effects At least WPE_SCENE_MAX_POSTFX entries; their params point into `environment`,
 *                    which must stay alive and unchanged while they are used.
 * @return The number of effects written (disabled ones are included, flagged disabled).
 *
 * @thread_safety Any thread
 */
uint32_t wpe_scene_environment_effects(const WPE_SceneEnvironment* environment, WPE_PostEffect* out_effects);

/**
 * Writes a scene document: the environment, the camera and every alive entity.
 *
 * @param camera Entity rendered from by default (WPE_ENTITY_NULL for none).
 * @return WPE_RESULT_OK; WPE_RESULT_ERROR_NULL_ARG; WPE_RESULT_ERROR_INVALID_STATE (an entity has a
 *         script but context->scripts is NULL, a component references an asset that has no path,
 *         or a serializer failed; logged); WPE_RESULT_ERROR_OUT_OF_MEMORY.
 *
 * @thread_safety Owning thread
 */
WPE_Result wpe_scene_save(const WPE_SceneContext* context, const WPE_SceneEnvironment* environment,
                          WPE_Entity camera, WPE_JsonWriter* writer);

/** wpe_scene_save() into a file at an OS path (written completely or not at all). Also returns
 * WPE_RESULT_ERROR_IO. @thread_safety Owning thread */
WPE_Result wpe_scene_save_file(const WPE_SceneContext* context, const WPE_SceneEnvironment* environment,
                               WPE_Entity camera, const char* os_path);

/**
 * Adds the entities of a scene document to the world (existing entities are kept). On failure
 * every entity created by the call is destroyed again and the world is as before.
 *
 * @param name Used in error messages (usually the asset path).
 * @param out_environment Optional; receives the scene's environment.
 * @param out_camera Optional; receives the scene's camera entity (WPE_ENTITY_NULL if none).
 * @return WPE_RESULT_OK; WPE_RESULT_ERROR_NULL_ARG; WPE_RESULT_ERROR_PARSE (malformed document;
 *         logged with the line); WPE_RESULT_ERROR_UNSUPPORTED (newer format version);
 *         WPE_RESULT_ERROR_NOT_FOUND (a referenced asset or script is missing; logged); other asset,
 *         ECS or script errors.
 *
 * @thread_safety Owning thread
 */
WPE_Result wpe_scene_load(const WPE_SceneContext* context, const char* text, size_t size, const char* name,
                          WPE_SceneEnvironment* out_environment, WPE_Entity* out_camera);

/** wpe_scene_load() of an asset path read through the context's asset manager VFS. @thread_safety Owning
 * thread */
WPE_Result wpe_scene_load_asset(const WPE_SceneContext* context, const char* path,
                                WPE_SceneEnvironment* out_environment, WPE_Entity* out_camera);

/* ---- Editing (tools, the editor's commands) --------------------------------------------------------
 * These use the scene-file schema for one entity or component, but entity references (ids,
 * "parent", joints...) are the world's live entity handles instead of file-local ids. Errors are
 * logged like loading errors and nothing is changed on failure. */

/** Writes one entity as a JSON object in the scene-file schema ("id" is its handle). Returns
 * WPE_RESULT_ERROR_NOT_FOUND for dead entities. @thread_safety Owning thread */
WPE_Result wpe_scene_save_entity(const WPE_SceneContext* context, WPE_Entity entity, WPE_JsonWriter* writer);

/**
 * Creates an entity from a description with the keys of a scene entity ("id" is ignored, so the output of
 * wpe_scene_save_entity() makes a copy): "name",
 * "parent", "transform", "instance", "mesh", ..., "script", "components".
 *
 * @return WPE_RESULT_OK, WPE_RESULT_ERROR_NULL_ARG, WPE_RESULT_ERROR_CAPACITY or a loading error.
 *
 * @thread_safety Owning thread
 */
WPE_Result wpe_scene_create_entity(const WPE_SceneContext* context, const WPE_JsonValue* description,
                                   WPE_Entity* out_entity);

/**
 * Sets one part of an entity from its scene-file description: "name", "parent", "transform",
 * "mesh", "camera", "light", "animator", "rigidbody", "collider", "character", "audio_source", "script", or a
 * game component's serializer name. An existing component is replaced as a whole (fields not given
 * take their defaults), so read the entity first to change single fields.
 *
 * @return WPE_RESULT_OK, WPE_RESULT_ERROR_NULL_ARG, WPE_RESULT_ERROR_NOT_FOUND (dead entity) or a
 *         loading error (the previous component is kept).
 *
 * @thread_safety Owning thread
 */
WPE_Result wpe_scene_set_component(const WPE_SceneContext* context, WPE_Entity entity, const char* key,
                                   const WPE_JsonValue* value);

/** Removes a component by its scene-file key. @return WPE_RESULT_OK, WPE_RESULT_ERROR_NULL_ARG,
 * WPE_RESULT_ERROR_NOT_FOUND (dead entity or no such component) or WPE_RESULT_ERROR_INVALID_ARG (unknown
 * key; logged). @thread_safety Owning thread */
WPE_Result wpe_scene_remove_component(const WPE_SceneContext* context, WPE_Entity entity, const char* key);

/** Writes an environment as the JSON object stored under a scene's "environment". @thread_safety Any thread
 */
WPE_Result wpe_scene_write_environment(const WPE_SceneEnvironment* environment, WPE_JsonWriter* writer);

/** Applies an "environment" object onto an environment (keys not given keep their values; a given
 * "postfx" list replaces the effects). @return WPE_RESULT_OK, WPE_RESULT_ERROR_NULL_ARG or
 * WPE_RESULT_ERROR_PARSE (logged; nothing changed). @thread_safety Any thread */
WPE_Result wpe_scene_read_environment(const WPE_JsonValue* value, const char* name,
                                      WPE_SceneEnvironment* io_environment);

/**
 * Despawns every entity of the world (on_remove hooks run as usual, so scripts get on_destroy).
 *
 * @return WPE_RESULT_OK, WPE_RESULT_ERROR_NULL_ARG or WPE_RESULT_ERROR_OUT_OF_MEMORY.
 *
 * @thread_safety Owning thread; not during a query
 */
WPE_Result wpe_scene_clear(WPE_Ecs* ecs);

#endif /* WPE_ENGINE_SCENE_FILE_H */
