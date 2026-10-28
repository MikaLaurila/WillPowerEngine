/**
 * @file scene_file_internal.h
 * @brief Private: the scene-file loader and saver internals shared by scene_file.c and prefab.c.
 *        Not a public API.
 */
#ifndef WPE_ENGINE_SCENE_FILE_INTERNAL_H
#define WPE_ENGINE_SCENE_FILE_INTERNAL_H

#include "engine/json_read.h"
#include "engine/scene_file.h"

#define WPE__MAX_FILE_ID 0x7FFFFFFFu

typedef struct wpe__IdPair {
    uint32_t id;
    WPE_Entity entity;
} wpe__IdPair;

struct WPE_SceneIds {
    wpe__IdPair* by_entity; /* Sorted by entity (saving). */
    wpe__IdPair* by_id;     /* Sorted by id (loading). */
    uint32_t count;
    const WPE_Ecs* identity; /* Set: ids are live entity handles of this world (editing commands). */
};

int wpe__compare_by_entity(const void* a, const void* b);
int wpe__compare_by_id(const void* a, const void* b);

typedef struct wpe__Saver {
    const WPE_SceneContext* context;
    WPE_JsonWriter* w;
    WPE_SceneIds ids;
    WPE_Result result;
    /* Prefab instance roots also describe their prefab ("prefab": path and overrides); off for the
     * plain component state of prefab files and their baselines. */
    bool describe_prefabs;
} wpe__Saver;

/* Writes one entity object: "id", "name", "parent" and every component. */
void wpe__save_entity(wpe__Saver* s, WPE_Entity e);

typedef struct wpe__Loader {
    wpe__JsonReader r;
    const WPE_SceneContext* context;
    WPE_SceneIds ids;
    bool in_prefab; /* Loading a prefab file: instances of other prefabs are not allowed (yet). */
} wpe__Loader;

/* The ids.by_id entries needed to load an entity array: one per entity plus prefab members' scene ids. */
uint32_t wpe__entity_id_capacity(const WPE_JsonValue* entities);

/* Loads an entity array (every stage); `created` receives one entity per array entry. ids.by_id must
 * have room for every entry plus the scene ids of prefab members ("ids" of instance entries). */
bool wpe__load_entities(wpe__Loader* l, const WPE_JsonValue* entities, WPE_Entity* created,
                        uint32_t* io_count);

/* Loads one part of an entity by its scene-file key ("name", "parent", a component...). */
bool wpe__apply(wpe__Loader* l, WPE_Entity e, const char* key, const WPE_JsonValue* value);

/* The component type stored under a scene-file key (WPE_COMP_NONE for unknown keys). */
WPE_ComponentType wpe__edit_type(const WPE_SceneContext* context, const char* key);

/* Writes an asset reference by path (null for no asset). */
void wpe__write_asset(wpe__Saver* s, const char* key, WPE_AssetHandle handle);

/* Optional asset path under `key`; loads it (the caller owns the reference). null in the file means none. */
bool wpe__read_asset(wpe__Loader* l, const WPE_JsonValue* object, const char* key, WPE_AssetType type,
                     WPE_AssetHandle* out);

/* Adds a component loaded from `at` (errors are reported there; `what` names it in messages). */
bool wpe__add(wpe__Loader* l, const WPE_JsonValue* at, WPE_Entity e, WPE_ComponentType type, const void* data,
              const char* what);

/* ---- UI components (scene_file_ui.c) --------------------------------------------------------------- */

/* Writes the entity's "ui", "sprite", "text" and "button" (those it has). */
void wpe__save_ui_components(wpe__Saver* s, WPE_Entity e);

bool wpe__load_ui(wpe__Loader* l, WPE_Entity e, const WPE_JsonValue* v);
bool wpe__load_sprite(wpe__Loader* l, WPE_Entity e, const WPE_JsonValue* v);
bool wpe__load_text(wpe__Loader* l, WPE_Entity e, const WPE_JsonValue* v);
bool wpe__load_button(wpe__Loader* l, WPE_Entity e, const WPE_JsonValue* v);
bool wpe__load_toggle(wpe__Loader* l, WPE_Entity e, const WPE_JsonValue* v);
bool wpe__load_slider(wpe__Loader* l, WPE_Entity e, const WPE_JsonValue* v);

/* "billboard" (3D sprites; scene_file_ui.c). */
void wpe__save_billboard(wpe__Saver* s, WPE_Entity e);
bool wpe__load_billboard(wpe__Loader* l, WPE_Entity e, const WPE_JsonValue* v);

/* "particles" (scene_file_ui.c). */
void wpe__save_particles(wpe__Saver* s, WPE_Entity e);
bool wpe__load_particles(wpe__Loader* l, WPE_Entity e, const WPE_JsonValue* v);

/* Writes a whole file at an OS path through a temporary file, so it is replaced completely or not at all. */
WPE_Result wpe__write_file_atomic(const char* os_path, const char* text, size_t size);

/* ---- Prefabs (prefab.c) ---------------------------------------------------------------------------- */

/* Instantiates a prefab file and applies its overrides (prefab-local ids). On success *out_root is the
 * instance root and `out_members` (wpe_alloc'd, owned by the caller) pairs each prefab-local id with its
 * entity. Errors go to `l` (reported at `at`) and nothing is left in the world. */
bool wpe__prefab_load(wpe__Loader* l, const WPE_JsonValue* at, const char* path,
                      const WPE_JsonValue* overrides, WPE_Entity* out_root, wpe__IdPair** out_members,
                      uint32_t* out_member_count);

/* Writes the "prefab" object of an instance root: path, (with `ids`) the scene ids of its members, and
 * the overrides against the prefab. */
void wpe__prefab_save_reference(wpe__Saver* s, WPE_Entity root, bool ids);

/* The instance root `e` belongs to (itself for a root), or WPE_ENTITY_NULL. */
WPE_Entity wpe__prefab_root(const WPE_Ecs* ecs, WPE_Entity e);

#endif /* WPE_ENGINE_SCENE_FILE_INTERNAL_H */
