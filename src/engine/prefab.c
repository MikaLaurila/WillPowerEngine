/*
 * Prefabs: prefab files, linked instances and their overrides (format in docs/scenes.md).
 *
 * Every instance keeps a baseline: its members as the prefab instantiates them, written in the
 * scene-file schema with prefab-local ids. Overrides are the differences between the members and the
 * baseline, so they are found however the members were changed. Instances made from the same version
 * of a file share one baseline through the cache.
 */
#define WPE_LOG_MODULE "prefab"
#include "engine/prefab.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "core/alloc.h"
#include "core/debug.h"
#include "core/log.h"
#include "engine/scene_file_internal.h"
#include "scene/scene.h"
#include "vfs/vfs.h"

/* ---- Baselines ------------------------------------------------------------------------------------- */

typedef struct wpe__Baseline {
    uint32_t refs;
    WPE_JsonDocument* doc; /* {"<prefab id>": entity, ...} */
} wpe__Baseline;

static wpe__Baseline* wpe__baseline_retain(wpe__Baseline* baseline) {
    if (baseline) ++baseline->refs;
    return baseline;
}

static void wpe__baseline_release(wpe__Baseline* baseline) {
    if (!baseline || --baseline->refs > 0) return;
    wpe_json_free(baseline->doc);
    wpe_free(baseline);
}

static const WPE_JsonValue* wpe__baseline_member(const wpe__Baseline* baseline, uint32_t id) {
    char key[16];
    snprintf(key, sizeof key, "%u", id);
    return wpe_json_get(wpe_json_root(baseline->doc), key);
}

/* Takes ownership of `doc` (also on failure). */
static wpe__Baseline* wpe__baseline_create(WPE_JsonDocument* doc) {
    wpe__Baseline* baseline = doc ? wpe_alloc(sizeof *baseline) : NULL;
    if (!baseline) {
        wpe_json_free(doc);
        return NULL;
    }
    *baseline = (wpe__Baseline){.refs = 1, .doc = doc};
    return baseline;
}

/* ---- Membership ------------------------------------------------------------------------------------ */

typedef struct wpe__PrefabLink {
    WPE_Entity root;         /* The instance root (the root itself for the root). */
    uint32_t local_id;       /* The entity's id in the prefab file. */
    char* path;              /* Root only: the prefab's asset path (owned). */
    wpe__Baseline* baseline; /* Root only: a reference. */
} wpe__PrefabLink;

static void wpe__link_on_remove(WPE_Ecs* ecs, WPE_Entity entity, void* component, void* user_data) {
    (void)ecs;
    (void)entity;
    (void)user_data;
    wpe__PrefabLink* link = component;
    wpe_free(link->path);
    wpe__baseline_release(link->baseline);
}

WPE_Result wpe_prefab_register(WPE_Ecs* ecs) {
    if (!ecs) return WPE_RESULT_ERROR_NULL_ARG;
    return wpe_ecs_register_component(ecs, WPE_COMP_PREFAB,
                                      &(WPE_ComponentDesc){.name = "PrefabLink",
                                                           .size = sizeof(wpe__PrefabLink),
                                                           .alignment = alignof(wpe__PrefabLink),
                                                           .on_remove = wpe__link_on_remove});
}

static const wpe__PrefabLink* wpe__link(const WPE_Ecs* ecs, WPE_Entity entity) {
    if (!ecs || !wpe_ecs_is_registered(ecs, WPE_COMP_PREFAB) || !wpe_ecs_is_alive(ecs, entity)) return NULL;
    return wpe_ecs_get_component(ecs, entity, WPE_COMP_PREFAB);
}

/* An instance member's id in its prefab (0 for other entities). */
static uint32_t wpe__local_id(const WPE_Ecs* ecs, WPE_Entity entity) {
    const wpe__PrefabLink* link = wpe__link(ecs, entity);
    return link ? link->local_id : 0;
}

WPE_Entity wpe__prefab_root(const WPE_Ecs* ecs, WPE_Entity e) {
    const wpe__PrefabLink* link = wpe__link(ecs, e);
    return link ? link->root : WPE_ENTITY_NULL;
}

WPE_Entity wpe_prefab_root(const WPE_Ecs* ecs, WPE_Entity entity) {
    return wpe__prefab_root(ecs, entity);
}

const char* wpe_prefab_path(const WPE_Ecs* ecs, WPE_Entity entity) {
    const wpe__PrefabLink* root = wpe__link(ecs, wpe__prefab_root(ecs, entity));
    return root ? root->path : NULL;
}

bool wpe_prefab_is_path(const char* path) {
    const size_t length = path ? strlen(path) : 0;
    return length > 7 && strcmp(path + length - 7, ".prefab") == 0;
}

/* `root` and its descendants in tree order (root first), honouring the context's include filter;
 * *out is wpe_alloc'd. With `only_members`, just the members of the instance rooted at `root`. */
static uint32_t wpe__tree(const WPE_SceneContext* context, WPE_Entity root, bool only_members,
                          WPE_Entity** out) {
    WPE_Ecs* ecs = context->ecs;
    const uint32_t capacity = wpe_ecs_entity_count(ecs);
    WPE_Entity* entities = wpe_alloc((size_t)capacity * sizeof *entities);
    WPE_Entity* stack = wpe_alloc((size_t)capacity * sizeof *stack);
    uint32_t count = 0, top = 0;
    if (entities && stack) stack[top++] = root;
    while (top > 0) {
        const WPE_Entity e = stack[--top];
        const wpe__PrefabLink* link = wpe__link(ecs, e);
        if (!only_members || (link && link->root == root)) entities[count++] = e;
        const WPE_Hierarchy* h = wpe_ecs_get_component(ecs, e, WPE_COMP_HIERARCHY);
        /* Children pushed in reverse, so the first child comes next. */
        const uint32_t first = top;
        for (WPE_Entity c = h ? h->first_child : WPE_ENTITY_NULL; c != WPE_ENTITY_NULL;) {
            if (top < capacity && (!context->include || context->include(c, context->include_user_data)))
                stack[top++] = c;
            const WPE_Hierarchy* ch = wpe_ecs_get_component(ecs, c, WPE_COMP_HIERARCHY);
            c = ch ? ch->next_sibling : WPE_ENTITY_NULL;
        }
        for (uint32_t i = first, j = top ? top - 1 : 0; i < j; ++i, --j) {
            const WPE_Entity swap = stack[i];
            stack[i] = stack[j];
            stack[j] = swap;
        }
    }
    wpe_free(stack);
    if (!entities) count = 0;
    *out = entities;
    return count;
}

/* Prefab-local ids of an instance's members (both lookup directions). */
static bool wpe__member_ids(const WPE_Ecs* ecs, const WPE_Entity* members, uint32_t count,
                            WPE_SceneIds* out) {
    *out = (WPE_SceneIds){0};
    out->by_entity = wpe_alloc((count ? count : 1) * sizeof *out->by_entity);
    out->by_id = wpe_alloc((count ? count : 1) * sizeof *out->by_id);
    if (!out->by_entity || !out->by_id) {
        wpe_free(out->by_entity);
        wpe_free(out->by_id);
        *out = (WPE_SceneIds){0};
        return false;
    }
    for (uint32_t i = 0; i < count; ++i)
        out->by_entity[i] = (wpe__IdPair){.id = wpe__local_id(ecs, members[i]), .entity = members[i]};
    memcpy(out->by_id, out->by_entity, count * sizeof *out->by_id);
    qsort(out->by_entity, count, sizeof *out->by_entity, wpe__compare_by_entity);
    qsort(out->by_id, count, sizeof *out->by_id, wpe__compare_by_id);
    out->count = count;
    return true;
}

static void wpe__free_ids(WPE_SceneIds* ids) {
    wpe_free(ids->by_entity);
    wpe_free(ids->by_id);
    *ids = (WPE_SceneIds){0};
}

/* Entities written as {"<id>": entity, ...} with `ids` (the baseline form); NULL on failure (logged). */
static WPE_JsonDocument* wpe__write_members(const WPE_SceneContext* context, const WPE_Entity* entities,
                                            uint32_t count, const WPE_SceneIds* ids) {
    WPE_JsonWriter* w = NULL;
    if (wpe_json_writer_create(&w) != WPE_RESULT_OK) return NULL;
    wpe__Saver s = {.context = context, .w = w, .ids = *ids};
    wpe_json_write_object_begin(w);
    for (uint32_t i = 0; i < count && s.result == WPE_RESULT_OK; ++i) {
        char key[16];
        snprintf(key, sizeof key, "%u", wpe_scene_ids_to_file(ids, entities[i]));
        wpe_json_write_key(w, key);
        wpe__save_entity(&s, entities[i]);
    }
    wpe_json_write_object_end(w);
    const char* text = NULL;
    size_t size = 0;
    WPE_JsonDocument* doc = NULL;
    if (s.result == WPE_RESULT_OK && wpe_json_writer_finish(w, &text, &size) == WPE_RESULT_OK)
        (void)wpe_json_parse(text, size, &doc, NULL);
    wpe_json_writer_destroy(w);
    return doc;
}

/* ---- Overrides ------------------------------------------------------------------------------------- */

/* Whether a key is compared with the prefab: not ids and links, nor the root's own name and transform. */
static bool wpe__compared(const char* key, bool root) {
    if (strcmp(key, "id") == 0 || strcmp(key, "parent") == 0 || strcmp(key, "prefab") == 0) return false;
    return !root || (strcmp(key, "name") != 0 && strcmp(key, "transform") != 0);
}

/* Counts, and writes when `w` is set, the keys in which a member differs from its baseline: the
 * member's value, or null where it lacks one. `only` limits it to one key. */
static uint32_t wpe__member_overrides(WPE_JsonWriter* w, const WPE_JsonValue* base, const WPE_JsonValue* live,
                                      bool root, const char* only) {
    uint32_t count = 0;
    for (int pass = 0; pass < 2; ++pass) {
        /* Pass 0: the baseline's keys; pass 1: keys only the member has. */
        const WPE_JsonValue* from = pass == 0 ? base : live;
        for (uint32_t i = 0; i < wpe_json_count(from); ++i) {
            const char* key = NULL;
            wpe_json_member(from, i, &key);
            if (!wpe__compared(key, root) || (only && strcmp(key, only) != 0)) continue;
            if (pass == 1 && wpe_json_get(base, key)) continue;
            const WPE_JsonValue* value = wpe_json_get(live, key);
            if (wpe_json_equal(wpe_json_get(base, key), value)) continue;
            ++count;
            if (!w) continue;
            wpe_json_write_key(w, key);
            if (value) {
                wpe_json_write_value(w, value);
            } else {
                wpe_json_write_null(w);
            }
        }
    }
    return count;
}

/* Counts, and writes when `w` is set, an instance's overrides: per prefab id, the changed keys, or
 * null for a removed member. */
static uint32_t wpe__instance_overrides(WPE_JsonWriter* w, const wpe__PrefabLink* root,
                                        const WPE_JsonValue* live) {
    const WPE_JsonValue* baseline = wpe_json_root(root->baseline->doc);
    uint32_t count = 0;
    for (uint32_t i = 0; i < wpe_json_count(baseline); ++i) {
        const char* key = NULL;
        const WPE_JsonValue* base = wpe_json_member(baseline, i, &key);
        const WPE_JsonValue* member = wpe_json_get(live, key);
        const bool is_root = (uint32_t)strtoul(key, NULL, 10) == root->local_id;
        if (!member) {
            ++count;
            if (w) {
                wpe_json_write_key(w, key);
                wpe_json_write_null(w);
            }
            continue;
        }
        if (!wpe_json_equal(wpe_json_get(base, "parent"), wpe_json_get(member, "parent")))
            wpe_log_warn(
                "entity %s of a '%s' instance was moved within it; prefab instances keep their structure "
                "(unpack the instance to restructure it)",
                key, root->path);
        if (wpe__member_overrides(NULL, base, member, is_root, NULL) == 0) continue;
        ++count;
        if (!w) continue;
        wpe_json_write_key(w, key);
        wpe_json_write_object_begin(w);
        wpe__member_overrides(w, base, member, is_root, NULL);
        wpe_json_write_object_end(w);
    }
    return count;
}

void wpe__prefab_save_reference(wpe__Saver* s, WPE_Entity root, bool ids) {
    WPE_Ecs* ecs = s->context->ecs;
    const wpe__PrefabLink* link = wpe__link(ecs, root);
    WPE_Entity* members = NULL;
    const uint32_t count = wpe__tree(s->context, root, true, &members);
    WPE_SceneIds member_ids;
    WPE_JsonDocument* live = members && wpe__member_ids(ecs, members, count, &member_ids)
                                 ? wpe__write_members(s->context, members, count, &member_ids)
                                 : NULL;
    if (!live) {
        wpe_log_error("cannot describe the '%s' instance 0x%08x", link->path, root);
        if (s->result == WPE_RESULT_OK) s->result = WPE_RESULT_ERROR_OUT_OF_MEMORY;
    }
    wpe_json_write_object_begin(s->w);
    wpe_json_write_key(s->w, "path");
    wpe_json_write_string(s->w, link->path);
    if (ids && members && count > 1) {
        /* Scene ids of the members, so other entities can refer to them. */
        wpe_json_write_key(s->w, "ids");
        wpe_json_write_object_begin(s->w);
        for (uint32_t i = 1; i < count; ++i) {
            const uint32_t id = wpe_scene_ids_to_file(&s->ids, members[i]);
            if (id == 0) continue;
            char key[16];
            snprintf(key, sizeof key, "%u", wpe__local_id(ecs, members[i]));
            wpe_json_write_key(s->w, key);
            wpe_json_write_integer(s->w, id);
        }
        wpe_json_write_object_end(s->w);
    }
    if (live && wpe__instance_overrides(NULL, link, wpe_json_root(live)) > 0) {
        wpe_json_write_key(s->w, "overrides");
        wpe_json_write_object_begin(s->w);
        wpe__instance_overrides(s->w, link, wpe_json_root(live));
        wpe_json_write_object_end(s->w);
    }
    wpe_json_write_object_end(s->w);
    wpe_json_free(live);
    if (members) wpe__free_ids(&member_ids);
    wpe_free(members);
}

/* Applies an instance's overrides; `o` loads with the instance's prefab ids. */
static bool wpe__apply_overrides(wpe__Loader* o, WPE_Entity root, const WPE_JsonValue* overrides) {
    if (!overrides) return true;
    if (wpe_json_type(overrides) != WPE_JSON_OBJECT)
        return wpe__json_fail(&o->r, overrides, WPE_RESULT_ERROR_PARSE,
                              "\"overrides\" must map prefab entity ids to their changes");
    WPE_Ecs* ecs = o->context->ecs;
    for (uint32_t i = 0; i < wpe_json_count(overrides); ++i) {
        const char* id_text = NULL;
        const WPE_JsonValue* changes = wpe_json_member(overrides, i, &id_text);
        char* end = NULL;
        const unsigned long id = strtoul(id_text, &end, 10);
        const WPE_Entity e = *end == '\0' ? wpe_scene_ids_to_entity(&o->ids, (uint32_t)id) : WPE_ENTITY_NULL;
        if (e == WPE_ENTITY_NULL)
            return wpe__json_fail(&o->r, changes, WPE_RESULT_ERROR_PARSE, "the prefab has no entity \"%s\"",
                                  id_text);
        if (wpe_json_type(changes) == WPE_JSON_NULL) {
            if (e == root)
                return wpe__json_fail(&o->r, changes, WPE_RESULT_ERROR_PARSE, "an instance keeps its root");
            continue; /* Removed below, after every change. */
        }
        if (wpe_json_type(changes) != WPE_JSON_OBJECT)
            return wpe__json_fail(&o->r, changes, WPE_RESULT_ERROR_PARSE,
                                  "the changes of entity %s must be an object, or null to remove it",
                                  id_text);
        for (uint32_t k = 0; k < wpe_json_count(changes); ++k) {
            const char* key = NULL;
            const WPE_JsonValue* value = wpe_json_member(changes, k, &key);
            if (!wpe__compared(key, e == root) || strcmp(key, "instance") == 0)
                return wpe__json_fail(&o->r, value, WPE_RESULT_ERROR_PARSE,
                                      e == root && wpe__compared(key, false)
                                          ? "\"%s\" of the instance root is set on the instance itself"
                                          : "\"%s\" cannot be overridden",
                                      key);
            if (strcmp(key, "name") == 0) {
                if (wpe_json_type(value) == WPE_JSON_NULL) {
                    if (wpe_ecs_has_component(ecs, e, WPE_COMP_NAME))
                        wpe_ecs_remove_component(ecs, e, WPE_COMP_NAME);
                } else if (!wpe__apply(o, e, key, value)) {
                    return false;
                }
                continue;
            }
            const WPE_ComponentType type = wpe__edit_type(o->context, key);
            if (type == WPE_COMP_NONE)
                return wpe__json_fail(&o->r, value, WPE_RESULT_ERROR_PARSE, "unknown component \"%s\"", key);
            if (wpe_ecs_has_component(ecs, e, type)) wpe_ecs_remove_component(ecs, e, type);
            if (wpe_json_type(value) != WPE_JSON_NULL && !wpe__apply(o, e, key, value)) return false;
        }
    }
    for (uint32_t i = 0; i < wpe_json_count(overrides); ++i) {
        const char* id_text = NULL;
        if (wpe_json_type(wpe_json_member(overrides, i, &id_text)) != WPE_JSON_NULL) continue;
        const WPE_Entity e = wpe_scene_ids_to_entity(&o->ids, (uint32_t)strtoul(id_text, NULL, 10));
        if (wpe_ecs_is_alive(ecs, e)) wpe_scene_destroy_recursive(ecs, e);
    }
    return true;
}

/* ---- Prefab files and the cache ------------------------------------------------------------------- */

typedef struct wpe__CacheEntry {
    char path[WPE_VFS_PATH_MAX]; /* Normalized asset path. */
    int64_t modified;
    WPE_JsonDocument* doc;
    wpe__Baseline* baseline; /* Made by the first instantiation (a reference). */
} wpe__CacheEntry;

struct WPE_PrefabCache {
    wpe__CacheEntry* entries;
    uint32_t count, capacity;
};

static void wpe__entry_clear(wpe__CacheEntry* entry) {
    wpe_json_free(entry->doc);
    wpe__baseline_release(entry->baseline);
    *entry = (wpe__CacheEntry){0};
}

WPE_Result wpe_prefab_cache_create(WPE_PrefabCache** out_cache) {
    if (!out_cache) return WPE_RESULT_ERROR_NULL_ARG;
    *out_cache = wpe_alloc_zeroed(1, sizeof **out_cache);
    return *out_cache ? WPE_RESULT_OK : WPE_RESULT_ERROR_OUT_OF_MEMORY;
}

void wpe_prefab_cache_destroy(WPE_PrefabCache* cache) {
    if (!cache) return;
    for (uint32_t i = 0; i < cache->count; ++i)
        wpe__entry_clear(&cache->entries[i]);
    wpe_free(cache->entries);
    wpe_free(cache);
}

/* The cache entry of a path (a new empty one if there is none); NULL when out of memory. */
static wpe__CacheEntry* wpe__cache_slot(WPE_PrefabCache* cache, const char* path) {
    for (uint32_t i = 0; i < cache->count; ++i) {
        if (strcmp(cache->entries[i].path, path) == 0) return &cache->entries[i];
    }
    if (cache->count == cache->capacity) {
        const uint32_t capacity = cache->capacity ? cache->capacity * 2 : 8;
        wpe__CacheEntry* entries = wpe_realloc(cache->entries, (size_t)capacity * sizeof *entries);
        if (!entries) return NULL;
        cache->entries = entries;
        cache->capacity = capacity;
    }
    wpe__CacheEntry* entry = &cache->entries[cache->count++];
    *entry = (wpe__CacheEntry){0};
    snprintf(entry->path, sizeof entry->path, "%s", path);
    return entry;
}

static void wpe__cache_forget(WPE_PrefabCache* cache, const char* path) {
    char normalized[WPE_VFS_PATH_MAX];
    if (!cache || !wpe_vfs_path_normalize(path, normalized, sizeof normalized)) return;
    for (uint32_t i = 0; i < cache->count; ++i) {
        if (strcmp(cache->entries[i].path, normalized) != 0) continue;
        wpe__entry_clear(&cache->entries[i]);
        cache->entries[i] = cache->entries[--cache->count];
        return;
    }
}

/* Checks a prefab document: format, version, and entities of which exactly one is the root. */
static bool wpe__check_document(wpe__JsonReader* r, const WPE_JsonValue* root) {
    if (!WPE__JSON_CHECK_OBJECT(r, root, "prefab", "format", "version", "entities")) return false;
    const char* format = wpe_json_string(wpe_json_get(root, "format"));
    if (!format || strcmp(format, "wpe.prefab") != 0)
        return wpe__json_fail(r, root, WPE_RESULT_ERROR_PARSE,
                              "not a prefab file (\"format\" must be \"wpe.prefab\")");
    uint32_t version = 0;
    if (!wpe__json_read_uint(r, root, "version", 1, UINT32_MAX, &version)) return false;
    if (version == 0) return wpe__json_fail(r, root, WPE_RESULT_ERROR_PARSE, "\"version\" is required");
    if (version > WPE_PREFAB_FORMAT_VERSION)
        return wpe__json_fail(r, root, WPE_RESULT_ERROR_UNSUPPORTED,
                              "format version %u is newer than this engine's (%u)", version,
                              WPE_PREFAB_FORMAT_VERSION);
    const WPE_JsonValue* entities = wpe_json_get(root, "entities");
    uint32_t roots = 0;
    for (uint32_t i = 0; i < wpe_json_count(entities); ++i) {
        const WPE_JsonValue* parent = wpe_json_get(wpe_json_at(entities, i), "parent");
        roots += !parent || wpe_json_type(parent) == WPE_JSON_NULL;
    }
    if (wpe_json_type(entities) != WPE_JSON_ARRAY || roots != 1)
        return wpe__json_fail(
            r, entities ? entities : root, WPE_RESULT_ERROR_PARSE,
            "\"entities\" must hold the prefab's entities, exactly one of them without a parent");
    return true;
}

/* The checked document of a prefab file, through the cache when the context has one: *out_entry is the
 * cached entry, or `scratch` (which the caller clears). */
static bool wpe__document(wpe__Loader* l, const WPE_JsonValue* at, const char* path, wpe__CacheEntry* scratch,
                          wpe__CacheEntry** out_entry) {
    char normalized[WPE_VFS_PATH_MAX];
    if (!wpe_vfs_path_normalize(path, normalized, sizeof normalized))
        return wpe__json_fail(&l->r, at, WPE_RESULT_ERROR_PARSE, "'%s' is not an asset path", path);
    const WPE_Vfs* vfs = wpe_asset_manager_vfs(l->context->assets);
    int64_t modified = 0;
    if (wpe_vfs_modified_time(vfs, normalized, &modified) != WPE_RESULT_OK)
        return wpe__json_fail(&l->r, at, WPE_RESULT_ERROR_NOT_FOUND, "prefab '%s' not found", normalized);
    wpe__CacheEntry* entry = scratch;
    if (l->context->prefabs) {
        entry = wpe__cache_slot(l->context->prefabs, normalized);
        if (!entry) return wpe__json_fail(&l->r, at, WPE_RESULT_ERROR_OUT_OF_MEMORY, "out of memory");
        if (entry->doc && entry->modified == modified) {
            *out_entry = entry;
            return true;
        }
        wpe__entry_clear(entry);
    }
    snprintf(entry->path, sizeof entry->path, "%s", normalized);
    entry->modified = modified;
    void* data = NULL;
    size_t size = 0;
    WPE_Result result = wpe_vfs_read_all(vfs, normalized, &data, &size);
    WPE_JsonError error = {0};
    if (result == WPE_RESULT_OK) result = wpe_json_parse(data, size, &entry->doc, &error);
    if (result == WPE_RESULT_ERROR_PARSE)
        wpe_log_error("prefab '%s', line %u, column %u: %s", normalized, error.line, error.column,
                      error.message);
    wpe_free(data);
    wpe__JsonReader r = {.name = entry->path, .kind = "prefab"};
    if (result == WPE_RESULT_OK && !wpe__check_document(&r, wpe_json_root(entry->doc))) result = r.result;
    if (result != WPE_RESULT_OK) {
        wpe__entry_clear(entry);
        if (entry != scratch) entry->path[0] = '\0'; /* Kept as an empty slot; read again next time. */
        return wpe__json_fail(&l->r, at, result, "cannot use prefab '%s'", normalized);
    }
    *out_entry = entry;
    return true;
}

bool wpe__prefab_load(wpe__Loader* l, const WPE_JsonValue* at, const char* path,
                      const WPE_JsonValue* overrides, WPE_Entity* out_root, wpe__IdPair** out_members,
                      uint32_t* out_member_count) {
    *out_root = WPE_ENTITY_NULL;
    *out_members = NULL;
    *out_member_count = 0;
    WPE_Ecs* ecs = l->context->ecs;
    if (!wpe_ecs_is_registered(ecs, WPE_COMP_PREFAB))
        return wpe__json_fail(&l->r, at, WPE_RESULT_ERROR_UNSUPPORTED,
                              "prefab instances need their component registered (wpe_prefab_register)");
    wpe__CacheEntry scratch = {0};
    wpe__CacheEntry* entry = NULL;
    if (!wpe__document(l, at, path, &scratch, &entry) || !entry) return false;

    const WPE_JsonValue* entities = wpe_json_get(wpe_json_root(entry->doc), "entities");
    const uint32_t count = wpe_json_count(entities);
    wpe__Loader p = {.r = {.name = entry->path, .kind = "prefab"}, .context = l->context, .in_prefab = true};
    WPE_Entity* created = wpe_alloc_zeroed(count, sizeof *created);
    p.ids.by_id = wpe_alloc_zeroed(count, sizeof *p.ids.by_id);
    uint32_t created_count = 0;
    bool ok = created && p.ids.by_id;
    if (!ok) wpe__json_fail(&l->r, at, WPE_RESULT_ERROR_OUT_OF_MEMORY, "out of memory");
    if (ok && !wpe__load_entities(&p, entities, created, &created_count))
        ok = wpe__json_fail(&l->r, at, p.r.result, "prefab '%s' cannot be instantiated", entry->path);
    WPE_Entity root = WPE_ENTITY_NULL;
    for (uint32_t i = 0; ok && i < count; ++i) {
        const WPE_JsonValue* parent = wpe_json_get(wpe_json_at(entities, i), "parent");
        if (!parent || wpe_json_type(parent) == WPE_JSON_NULL) root = created[i];
    }
    /* The baseline is the prefab as instantiated, before this instance's overrides. */
    if (ok && !entry->baseline) {
        WPE_SceneIds ids = {.by_entity = wpe_alloc((size_t)count * sizeof(wpe__IdPair)), .count = count};
        if (ids.by_entity) {
            memcpy(ids.by_entity, p.ids.by_id, (size_t)count * sizeof(wpe__IdPair));
            qsort(ids.by_entity, count, sizeof(wpe__IdPair), wpe__compare_by_entity);
            entry->baseline = wpe__baseline_create(wpe__write_members(l->context, created, count, &ids));
        }
        wpe_free(ids.by_entity);
        if (!entry->baseline) ok = wpe__json_fail(&l->r, at, WPE_RESULT_ERROR_OUT_OF_MEMORY, "out of memory");
    }
    for (uint32_t i = 0; ok && i < count; ++i) {
        wpe__PrefabLink link = {
            .root = root,
            .local_id = (uint32_t)wpe_json_number(wpe_json_get(wpe_json_at(entities, i), "id"), 0)};
        if (created[i] == root) {
            const size_t length = strlen(entry->path) + 1;
            link.path = wpe_alloc(length);
            if (link.path) memcpy(link.path, entry->path, length);
            link.baseline = wpe__baseline_retain(entry->baseline);
        }
        const WPE_Result result = created[i] == root && !link.path
                                      ? WPE_RESULT_ERROR_OUT_OF_MEMORY
                                      : wpe_ecs_add_component(ecs, created[i], WPE_COMP_PREFAB, &link, NULL);
        if (result != WPE_RESULT_OK) {
            wpe_free(link.path);
            wpe__baseline_release(link.baseline);
            ok = wpe__json_fail(&l->r, at, result, "cannot link the prefab instance: %s",
                                wpe_result_to_string(result));
        }
    }
    if (ok) {
        /* Overrides use the prefab's ids; messages name the file they are in. */
        wpe__Loader o = {.r = l->r, .context = l->context, .ids = p.ids};
        if (!wpe__apply_overrides(&o, root, overrides)) {
            l->r.result = o.r.result;
            ok = false;
        }
    }
    if (ok) {
        wpe__IdPair* members = wpe_alloc((count ? count : 1) * sizeof *members);
        if (members) {
            for (uint32_t i = 0; i < p.ids.count; ++i) {
                if (wpe_ecs_is_alive(ecs, p.ids.by_id[i].entity))
                    members[(*out_member_count)++] = p.ids.by_id[i];
            }
            *out_members = members;
        } else {
            ok = false;
            wpe__json_fail(&l->r, at, WPE_RESULT_ERROR_OUT_OF_MEMORY, "out of memory");
        }
    }
    if (ok) {
        *out_root = root;
    } else {
        for (uint32_t i = created_count; i-- > 0;) {
            if (wpe_ecs_is_alive(ecs, created[i])) wpe_scene_destroy_recursive(ecs, created[i]);
        }
    }
    wpe_free(created);
    wpe_free(p.ids.by_id);
    if (entry == &scratch) wpe__entry_clear(&scratch);
    return ok;
}

WPE_Result wpe_prefab_instantiate(const WPE_SceneContext* context, const char* path, WPE_Entity* out_root) {
    if (!context || !context->ecs || !context->assets || !path || !out_root) return WPE_RESULT_ERROR_NULL_ARG;
    wpe__Loader l = {
        .r = {.name = path, .kind = "prefab"}, .context = context, .ids = {.identity = context->ecs}};
    wpe__IdPair* members = NULL;
    uint32_t count = 0;
    if (!wpe__prefab_load(&l, NULL, path, NULL, out_root, &members, &count)) return l.r.result;
    wpe_free(members);
    return WPE_RESULT_OK;
}

/* ---- Saving, reverting, unpacking ------------------------------------------------------------------ */

/* Writes the prefab document of `tree` (tree[0] the root) with the given ids. For the root, `name` and
 * `transform` replace its own when set ("parent" is never written for it). */
static WPE_Result wpe__write_prefab(const WPE_SceneContext* context, const WPE_Entity* tree, uint32_t count,
                                    const WPE_SceneIds* ids, const WPE_JsonValue* name,
                                    const WPE_JsonValue* transform, bool origin, WPE_JsonWriter* w) {
    WPE_JsonDocument* entities = wpe__write_members(context, tree, count, ids);
    if (!entities) return WPE_RESULT_ERROR_OUT_OF_MEMORY;
    wpe_json_write_object_begin(w);
    wpe_json_write_key(w, "format");
    wpe_json_write_string(w, "wpe.prefab");
    wpe_json_write_key(w, "version");
    wpe_json_write_integer(w, WPE_PREFAB_FORMAT_VERSION);
    wpe_json_write_key(w, "entities");
    wpe_json_write_array_begin(w, false);
    for (uint32_t i = 0; i < count; ++i) {
        const WPE_JsonValue* entity = wpe_json_member(wpe_json_root(entities), i, NULL);
        wpe_json_write_object_begin(w);
        for (uint32_t k = 0; k < wpe_json_count(entity); ++k) {
            const char* key = NULL;
            const WPE_JsonValue* value = wpe_json_member(entity, k, &key);
            if (i == 0 && strcmp(key, "parent") == 0) continue;
            if (i == 0 && strcmp(key, "name") == 0 && name) value = name;
            if (i == 0 && strcmp(key, "transform") == 0) {
                if (transform) {
                    value = transform;
                } else if (origin) {
                    /* A new prefab's root sits at the origin; rotation and scale are kept. */
                    wpe_json_write_key(w, key);
                    wpe_json_write_object_begin(w);
                    for (uint32_t t = 0; t < wpe_json_count(value); ++t) {
                        const char* part = NULL;
                        const WPE_JsonValue* v = wpe_json_member(value, t, &part);
                        wpe_json_write_key(w, part);
                        if (strcmp(part, "position") == 0) {
                            wpe_json_write_array_begin(w, true);
                            for (int c = 0; c < 3; ++c)
                                wpe_json_write_integer(w, 0);
                            wpe_json_write_array_end(w);
                        } else {
                            wpe_json_write_value(w, v);
                        }
                    }
                    wpe_json_write_object_end(w);
                    continue;
                }
            }
            wpe_json_write_key(w, key);
            wpe_json_write_value(w, value);
        }
        wpe_json_write_object_end(w);
    }
    wpe_json_write_array_end(w);
    wpe_json_write_object_end(w);
    wpe_json_free(entities);
    return WPE_RESULT_OK;
}

WPE_Result wpe_prefab_save_file(const WPE_SceneContext* context, WPE_Entity root, const char* path,
                                const char* os_path) {
    if (!context || !context->ecs || !context->assets || !path || !os_path) return WPE_RESULT_ERROR_NULL_ARG;
    WPE_Ecs* ecs = context->ecs;
    if (!wpe_ecs_is_alive(ecs, root)) return WPE_RESULT_ERROR_NOT_FOUND;
    if (!wpe_ecs_is_registered(ecs, WPE_COMP_PREFAB)) {
        wpe_log_error("prefabs need their component registered (wpe_prefab_register)");
        return WPE_RESULT_ERROR_INVALID_STATE;
    }
    char normalized[WPE_VFS_PATH_MAX];
    if (!wpe_vfs_path_normalize(path, normalized, sizeof normalized) || !wpe_prefab_is_path(normalized)) {
        wpe_log_error("'%s' is not a prefab asset path (it ends in \".prefab\")", path);
        return WPE_RESULT_ERROR_INVALID_ARG;
    }
    const wpe__PrefabLink* root_link = wpe__link(ecs, root);
    if (root_link && root_link->root != root) {
        wpe_log_error("entity 0x%08x is inside a '%s' instance: save the instance root, or unpack it first",
                      root, wpe_prefab_path(ecs, root));
        return WPE_RESULT_ERROR_INVALID_ARG;
    }
    WPE_Entity* tree = NULL;
    const uint32_t count = wpe__tree(context, root, false, &tree);
    if (!tree) return WPE_RESULT_ERROR_OUT_OF_MEMORY;
    /* Members keep their prefab ids; everything else gets new ones. */
    uint32_t next = 1;
    for (uint32_t i = 0; i < count; ++i) {
        const wpe__PrefabLink* link = wpe__link(ecs, tree[i]);
        if (link && link->root != root) {
            wpe_log_error("the tree holds an instance of '%s'; unpack it first (prefabs cannot hold prefab "
                          "instances)",
                          wpe_prefab_path(ecs, tree[i]));
            wpe_free(tree);
            return WPE_RESULT_ERROR_INVALID_ARG;
        }
        if (link && link->local_id >= next) next = link->local_id + 1;
    }
    WPE_SceneIds ids = {.by_entity = wpe_alloc((size_t)count * sizeof(wpe__IdPair)), .count = count};
    if (!ids.by_entity) {
        wpe_free(tree);
        return WPE_RESULT_ERROR_OUT_OF_MEMORY;
    }
    for (uint32_t i = 0; i < count; ++i) {
        const wpe__PrefabLink* link = wpe__link(ecs, tree[i]);
        ids.by_entity[i] = (wpe__IdPair){.id = link ? link->local_id : next++, .entity = tree[i]};
    }
    qsort(ids.by_entity, count, sizeof(wpe__IdPair), wpe__compare_by_entity);

    /* Applying to its own prefab keeps the prefab root's name and transform (they are the instance's). */
    const bool apply = root_link && strcmp(root_link->path, normalized) == 0;
    const WPE_JsonValue* old_root =
        apply ? wpe__baseline_member(root_link->baseline, root_link->local_id) : NULL;
    WPE_JsonWriter* w = NULL;
    WPE_Result result = wpe_json_writer_create(&w);
    if (result == WPE_RESULT_OK)
        result =
            wpe__write_prefab(context, tree, count, &ids, old_root ? wpe_json_get(old_root, "name") : NULL,
                              old_root ? wpe_json_get(old_root, "transform") : NULL, !apply, w);
    const char* text = NULL;
    size_t size = 0;
    if (result == WPE_RESULT_OK) result = wpe_json_writer_finish(w, &text, &size);
    if (result == WPE_RESULT_OK) result = wpe__write_file_atomic(os_path, text, size);

    /* The tree is now an instance of the file just written: link it, with that content as baseline. */
    WPE_JsonDocument* written = NULL;
    if (result == WPE_RESULT_OK) result = wpe_json_parse(text, size, &written, NULL);
    WPE_JsonWriter* bw = NULL;
    if (result == WPE_RESULT_OK) result = wpe_json_writer_create(&bw);
    if (result == WPE_RESULT_OK) {
        const WPE_JsonValue* entities = wpe_json_get(wpe_json_root(written), "entities");
        wpe_json_write_object_begin(bw);
        for (uint32_t i = 0; i < wpe_json_count(entities); ++i) {
            char key[16];
            snprintf(key, sizeof key, "%u",
                     (uint32_t)wpe_json_number(wpe_json_get(wpe_json_at(entities, i), "id"), 0));
            wpe_json_write_key(bw, key);
            wpe_json_write_value(bw, wpe_json_at(entities, i));
        }
        wpe_json_write_object_end(bw);
    }
    const char* baseline_text = NULL;
    size_t baseline_size = 0;
    WPE_JsonDocument* baseline_doc = NULL;
    if (result == WPE_RESULT_OK) result = wpe_json_writer_finish(bw, &baseline_text, &baseline_size);
    if (result == WPE_RESULT_OK) result = wpe_json_parse(baseline_text, baseline_size, &baseline_doc, NULL);
    wpe__Baseline* baseline = result == WPE_RESULT_OK ? wpe__baseline_create(baseline_doc) : NULL;
    if (result == WPE_RESULT_OK && !baseline) result = WPE_RESULT_ERROR_OUT_OF_MEMORY;
    char* link_path = NULL;
    if (result == WPE_RESULT_OK) {
        link_path = wpe_alloc(strlen(normalized) + 1);
        if (link_path) memcpy(link_path, normalized, strlen(normalized) + 1);
        if (!link_path) result = WPE_RESULT_ERROR_OUT_OF_MEMORY;
    }
    if (result == WPE_RESULT_OK) {
        for (uint32_t i = 0; i < count; ++i) {
            if (wpe_ecs_has_component(ecs, tree[i], WPE_COMP_PREFAB))
                wpe_ecs_remove_component(ecs, tree[i], WPE_COMP_PREFAB);
        }
        for (uint32_t i = 0; i < count && result == WPE_RESULT_OK; ++i) {
            wpe__PrefabLink link = {.root = root, .local_id = wpe_scene_ids_to_file(&ids, tree[i])};
            if (i == 0) {
                link.path = link_path;
                link.baseline = baseline;
                link_path = NULL;
                baseline = NULL;
            }
            result = wpe_ecs_add_component(ecs, tree[i], WPE_COMP_PREFAB, &link, NULL);
            if (result != WPE_RESULT_OK) {
                wpe_free(link.path);
                wpe__baseline_release(link.baseline);
            }
        }
        wpe__cache_forget(context->prefabs, normalized); /* Read again by the next instantiation. */
    }
    if (result != WPE_RESULT_OK)
        wpe_log_error("saving prefab '%s' failed: %s", normalized, wpe_result_to_string(result));
    wpe_free(link_path);
    wpe__baseline_release(baseline);
    wpe_json_writer_destroy(bw);
    wpe_json_free(written);
    wpe_json_writer_destroy(w);
    wpe_free(ids.by_entity);
    wpe_free(tree);
    return result;
}

WPE_Result wpe_prefab_revert(const WPE_SceneContext* context, WPE_Entity entity, const char* key,
                             WPE_Entity* out_root) {
    if (!context || !context->ecs || !context->assets) return WPE_RESULT_ERROR_NULL_ARG;
    WPE_Ecs* ecs = context->ecs;
    const WPE_Entity root = wpe__prefab_root(ecs, entity);
    if (root == WPE_ENTITY_NULL) {
        wpe_log_error("entity 0x%08x is not part of a prefab instance", entity);
        return WPE_RESULT_ERROR_NOT_FOUND;
    }
    if (out_root) *out_root = root;
    const wpe__PrefabLink* root_link = wpe__link(ecs, root);
    if (key) {
        if (!wpe__compared(key, entity == root))
            return WPE_RESULT_OK; /* The instance's own: nothing to revert. */
        const WPE_JsonValue* base =
            wpe_json_get(wpe__baseline_member(root_link->baseline, wpe__local_id(ecs, entity)), key);
        WPE_Entity* members = NULL;
        const uint32_t count = wpe__tree(context, root, true, &members);
        wpe__Loader o = {.r = {.name = root_link->path, .kind = "prefab"}, .context = context};
        if (!members || !wpe__member_ids(ecs, members, count, &o.ids)) {
            wpe_free(members);
            return WPE_RESULT_ERROR_OUT_OF_MEMORY;
        }
        const WPE_ComponentType type =
            strcmp(key, "name") == 0 ? WPE_COMP_NAME : wpe__edit_type(context, key);
        if (type == WPE_COMP_NONE) {
            wpe_log_error("unknown component \"%s\"", key);
            o.r.result = WPE_RESULT_ERROR_INVALID_ARG;
        } else {
            if (wpe_ecs_has_component(ecs, entity, type)) wpe_ecs_remove_component(ecs, entity, type);
            if (base) wpe__apply(&o, entity, key, base);
        }
        wpe__free_ids(&o.ids);
        wpe_free(members);
        return o.r.result;
    }
    /* The whole instance: a fresh one in the old one's place. */
    char path[WPE_VFS_PATH_MAX];
    snprintf(path, sizeof path, "%s", root_link->path);
    const WPE_Entity parent = wpe_scene_get_parent(ecs, root);
    const WPE_Transform* transform = wpe_ecs_get_component(ecs, root, WPE_COMP_TRANSFORM);
    const WPE_Transform placement = transform ? *transform : wpe_transform_identity();
    const bool placed = transform != NULL;
    char name[256] = "";
    const char* old_name = wpe_scene_get_name(ecs, root);
    if (old_name) snprintf(name, sizeof name, "%s", old_name);
    WPE_Entity fresh = WPE_ENTITY_NULL;
    WPE_Result result = wpe_prefab_instantiate(context, path, &fresh);
    if (result != WPE_RESULT_OK) return result;
    if (old_name) result = wpe_scene_set_name(ecs, fresh, name);
    if (result == WPE_RESULT_OK && placed) {
        if (wpe_ecs_has_component(ecs, fresh, WPE_COMP_TRANSFORM))
            wpe_ecs_remove_component(ecs, fresh, WPE_COMP_TRANSFORM);
        result = wpe_ecs_add_component(ecs, fresh, WPE_COMP_TRANSFORM, &placement, NULL);
    }
    if (result == WPE_RESULT_OK && parent != WPE_ENTITY_NULL)
        result = wpe_scene_set_parent(ecs, fresh, parent, false);
    if (result != WPE_RESULT_OK) {
        wpe_scene_destroy_recursive(ecs, fresh);
        return result;
    }
    wpe_scene_destroy_recursive(ecs, root);
    if (out_root) *out_root = fresh;
    return WPE_RESULT_OK;
}

WPE_Result wpe_prefab_unpack(WPE_Ecs* ecs, WPE_Entity entity) {
    if (!ecs) return WPE_RESULT_ERROR_NULL_ARG;
    const WPE_Entity root = wpe__prefab_root(ecs, entity);
    if (root == WPE_ENTITY_NULL) return WPE_RESULT_ERROR_NOT_FOUND;
    WPE_Entity* members = NULL;
    const WPE_SceneContext context = {.ecs = ecs};
    const uint32_t count = wpe__tree(&context, root, true, &members);
    if (!members) return WPE_RESULT_ERROR_OUT_OF_MEMORY;
    for (uint32_t i = 0; i < count; ++i)
        wpe_ecs_remove_component(ecs, members[i], WPE_COMP_PREFAB);
    wpe_free(members);
    return WPE_RESULT_OK;
}

bool wpe_prefab_is_overridden(const WPE_SceneContext* context, WPE_Entity entity, const char* key) {
    if (!context || !context->ecs) return false;
    WPE_Ecs* ecs = context->ecs;
    const WPE_Entity root = wpe__prefab_root(ecs, entity);
    if (root == WPE_ENTITY_NULL) return false;
    const wpe__PrefabLink* root_link = wpe__link(ecs, root);
    const WPE_JsonValue* base = wpe__baseline_member(root_link->baseline, wpe__local_id(ecs, entity));
    WPE_Entity* members = NULL;
    const uint32_t count = wpe__tree(context, root, true, &members);
    WPE_SceneIds ids;
    bool overridden = false;
    if (members && wpe__member_ids(ecs, members, count, &ids)) {
        WPE_JsonDocument* live = wpe__write_members(context, &entity, 1, &ids);
        const WPE_JsonValue* member = wpe_json_member(wpe_json_root(live), 0, NULL);
        overridden = live && wpe__member_overrides(NULL, base, member, entity == root, key) > 0;
        wpe_json_free(live);
        wpe__free_ids(&ids);
    }
    wpe_free(members);
    return overridden;
}
