#define WPE_LOG_MODULE "assets"
#include "assets/asset_manager.h"

#include "assets/procedural.h"
#include <string.h>

#include "assets/image.h"
#include "core/alloc.h"
#include "core/debug.h"
#include "core/log.h"

/* Handle layout: low 20 bits slot index + 1, high 12 bits generation (never 0). */
#define WPE__ASSET_INDEX_BITS 20u
#define WPE__ASSET_INDEX_MASK ((1u << WPE__ASSET_INDEX_BITS) - 1u)
#define WPE__ASSET_MAX_SLOTS (WPE__ASSET_INDEX_MASK - 1u)
#define WPE__ASSET_MAX_GENERATION ((1u << (32u - WPE__ASSET_INDEX_BITS)) - 1u)

typedef struct wpe__AssetSlot {
    void* asset; /* NULL when the slot is free. */
    char* path;
    WPE_AssetType type;
    uint32_t refs;
    uint32_t generation;
    uint32_t next_free; /* Free-list link (slot index), UINT32_MAX ends the list. */
} wpe__AssetSlot;

typedef struct wpe__TypeEntry {
    WPE_AssetLoader loader;
    bool registered;
} wpe__TypeEntry;

struct WPE_AssetManager {
    const WPE_Vfs* vfs;
    WPE_JobSystem* jobs;
    uint32_t max_texture_size;
    wpe__TypeEntry types[WPE_ASSET_MAX_TYPES];
    wpe__AssetSlot* slots;
    uint32_t slot_count, slot_capacity;
    uint32_t free_head;
    uint32_t live;
    uint64_t loads, hits;
};

/* ---- Built-in loaders ---------------------------------------------------------------------------- */

static WPE_Result wpe__load_model(WPE_AssetManager* assets, const char* path, void* user_data,
                                  void** out_asset) {
    (void)user_data;
    /* Primitive descriptions (".prim") build procedural models; everything else is glTF. */
    const size_t length = strlen(path);
    if (length > 5 && strcmp(path + length - 5, ".prim") == 0) {
        void* data = NULL;
        size_t size = 0;
        WPE_Result result = wpe_vfs_read_all(assets->vfs, path, &data, &size);
        if (result == WPE_RESULT_ERROR_NOT_FOUND) wpe_log_error("model '%s' not found", path);
        if (result == WPE_RESULT_OK)
            result = wpe_model_parse_primitive(data, size, path, (WPE_Model**)out_asset);
        wpe_free(data);
        return result;
    }
    const WPE_ModelLoadOptions options = {
        .jobs = assets->jobs, .max_texture_size = assets->max_texture_size, .vfs = assets->vfs};
    return wpe_model_load(path, &options, (WPE_Model**)out_asset);
}

static void wpe__destroy_model(void* asset, void* user_data) {
    (void)user_data;
    wpe_model_destroy(asset);
}

static WPE_Result wpe__load_font(WPE_AssetManager* assets, const char* path, void* user_data,
                                 void** out_asset) {
    (void)user_data;
    void* data = NULL;
    size_t size = 0;
    WPE_Result result = wpe_vfs_read_all(assets->vfs, path, &data, &size);
    if (result == WPE_RESULT_OK) result = wpe_font_load_memory(data, size, path, (WPE_BitmapFont**)out_asset);
    wpe_free(data);
    return result;
}

static void wpe__destroy_font(void* asset, void* user_data) {
    (void)user_data;
    wpe_font_destroy(asset);
}

/* Images keep their full size and no mips: 2D drawing samples level 0 with nearest filtering. */
static WPE_Result wpe__load_image(WPE_AssetManager* assets, const char* path, void* user_data,
                                  void** out_asset) {
    (void)user_data;
    void* data = NULL;
    size_t size = 0;
    WPE_Result result = wpe_vfs_read_all(assets->vfs, path, &data, &size);
    if (result == WPE_RESULT_ERROR_NOT_FOUND) wpe_log_error("image '%s' not found", path);
    WPE_Image image = {0};
    if (result == WPE_RESULT_OK) result = wpe_image_decode(data, size, path, &image);
    wpe_free(data);
    if (result != WPE_RESULT_OK) return result;
    result = wpe_texture_create(
        &(WPE_TextureDesc){.width = image.width, .height = image.height, .texels = image.pixels},
        (WPE_Texture**)out_asset);
    if (result != WPE_RESULT_OK)
        wpe_log_error("image '%s' cannot be used: %s", path, wpe_result_to_string(result));
    wpe_image_free(&image);
    return result;
}

static void wpe__destroy_image(void* asset, void* user_data) {
    (void)user_data;
    wpe_texture_destroy(asset);
}

/* ---- Slots --------------------------------------------------------------------------------------- */

static WPE_AssetHandle wpe__handle(const WPE_AssetManager* assets, uint32_t index) {
    return (assets->slots[index].generation << WPE__ASSET_INDEX_BITS) | (index + 1u);
}

/* Slot of a live asset, or NULL for stale/invalid handles. */
static wpe__AssetSlot* wpe__slot(const WPE_AssetManager* assets, WPE_AssetHandle handle) {
    if (!assets || handle == WPE_ASSET_NONE) return NULL;
    const uint32_t index = (handle & WPE__ASSET_INDEX_MASK) - 1u;
    if (index >= assets->slot_count) return NULL;
    wpe__AssetSlot* slot = &assets->slots[index];
    if (!slot->asset || slot->generation != handle >> WPE__ASSET_INDEX_BITS) return NULL;
    return slot;
}

/* Stores a new asset with one reference; takes ownership of `path` only on success. */
static WPE_Result wpe__insert(WPE_AssetManager* assets, WPE_AssetType type, char* path, void* asset,
                              WPE_AssetHandle* out_handle) {
    uint32_t index = assets->free_head;
    if (index != UINT32_MAX) {
        assets->free_head = assets->slots[index].next_free;
    } else {
        if (assets->slot_count == WPE__ASSET_MAX_SLOTS) return WPE_RESULT_ERROR_CAPACITY;
        if (assets->slot_count == assets->slot_capacity) {
            const uint32_t capacity = assets->slot_capacity ? assets->slot_capacity * 2 : 64;
            wpe__AssetSlot* slots = wpe_realloc(assets->slots, (size_t)capacity * sizeof *slots);
            if (!slots) return WPE_RESULT_ERROR_OUT_OF_MEMORY;
            assets->slots = slots;
            assets->slot_capacity = capacity;
        }
        index = assets->slot_count++;
        assets->slots[index] = (wpe__AssetSlot){.generation = 0};
    }
    wpe__AssetSlot* slot = &assets->slots[index];
    slot->generation = slot->generation + 1; /* Free slots never sit at the retired maximum. */
    slot->asset = asset;
    slot->path = path;
    slot->type = type;
    slot->refs = 1;
    ++assets->live;
    *out_handle = wpe__handle(assets, index);
    return WPE_RESULT_OK;
}

static void wpe__destroy_slot(WPE_AssetManager* assets, uint32_t index) {
    wpe__AssetSlot* slot = &assets->slots[index];
    const wpe__TypeEntry* type = &assets->types[slot->type];
    wpe_assert(type->registered);
    type->loader.destroy(slot->asset, type->loader.user_data);
    wpe_free(slot->path);
    slot->asset = NULL;
    slot->path = NULL;
    slot->refs = 0;
    --assets->live;
    /* A slot whose generation is exhausted is retired, so handles never alias. */
    if (slot->generation < WPE__ASSET_MAX_GENERATION) {
        slot->next_free = assets->free_head;
        assets->free_head = index;
    }
}

static char* wpe__normalized_copy(const char* path) {
    char normalized[WPE_VFS_PATH_MAX];
    if (!wpe_vfs_path_normalize(path, normalized, sizeof normalized)) return NULL;
    const size_t length = strlen(normalized);
    char* copy = wpe_alloc(length + 1);
    if (copy) memcpy(copy, normalized, length + 1);
    return copy;
}

static bool wpe__type_ok(const WPE_AssetManager* assets, WPE_AssetType type) {
    return type < WPE_ASSET_MAX_TYPES && assets->types[type].registered;
}

/* ---- Public API ---------------------------------------------------------------------------------- */

WPE_Result wpe_asset_manager_create(const WPE_AssetManagerDesc* desc, WPE_AssetManager** out_assets) {
    if (!desc || !desc->vfs || !out_assets) return WPE_RESULT_ERROR_NULL_ARG;
    *out_assets = NULL;
    WPE_AssetManager* assets = wpe_alloc_zeroed(1, sizeof *assets);
    if (!assets) return WPE_RESULT_ERROR_OUT_OF_MEMORY;
    assets->vfs = desc->vfs;
    assets->jobs = desc->jobs;
    assets->max_texture_size = desc->max_texture_size;
    assets->free_head = UINT32_MAX;
    assets->types[WPE_ASSET_MODEL] =
        (wpe__TypeEntry){.loader = {.name = "model", .load = wpe__load_model, .destroy = wpe__destroy_model},
                         .registered = true};
    assets->types[WPE_ASSET_FONT] = (wpe__TypeEntry){
        .loader = {.name = "font", .load = wpe__load_font, .destroy = wpe__destroy_font}, .registered = true};
    assets->types[WPE_ASSET_IMAGE] =
        (wpe__TypeEntry){.loader = {.name = "image", .load = wpe__load_image, .destroy = wpe__destroy_image},
                         .registered = true};
    *out_assets = assets;
    return WPE_RESULT_OK;
}

void wpe_asset_manager_destroy(WPE_AssetManager* assets) {
    if (!assets) return;
    for (uint32_t i = 0; i < assets->slot_count; ++i) {
        const wpe__AssetSlot* slot = &assets->slots[i];
        if (!slot->asset) continue;
        wpe_log_warn("asset '%s' (%s) still has %u reference(s) at shutdown", slot->path,
                     assets->types[slot->type].loader.name, slot->refs);
        wpe__destroy_slot(assets, i);
    }
    wpe_free(assets->slots);
    wpe_free(assets);
}

const WPE_Vfs* wpe_asset_manager_vfs(const WPE_AssetManager* assets) {
    return assets ? assets->vfs : NULL;
}

WPE_JobSystem* wpe_asset_manager_jobs(const WPE_AssetManager* assets) {
    return assets ? assets->jobs : NULL;
}

WPE_Result wpe_asset_manager_register_type(WPE_AssetManager* assets, WPE_AssetType type,
                                           const WPE_AssetLoader* loader) {
    if (!assets || !loader) return WPE_RESULT_ERROR_NULL_ARG;
    if (type == WPE_ASSET_TYPE_NONE || type >= WPE_ASSET_MAX_TYPES || !loader->load || !loader->destroy ||
        !loader->name)
        return WPE_RESULT_ERROR_INVALID_ARG;
    if (assets->types[type].registered) return WPE_RESULT_ERROR_ALREADY_EXISTS;
    assets->types[type] = (wpe__TypeEntry){.loader = *loader, .registered = true};
    return WPE_RESULT_OK;
}

WPE_Result wpe_asset_manager_unregister_type(WPE_AssetManager* assets, WPE_AssetType type) {
    if (!assets) return WPE_RESULT_ERROR_NULL_ARG;
    if (!wpe__type_ok(assets, type)) return WPE_RESULT_ERROR_NOT_FOUND;
    for (uint32_t i = 0; i < assets->slot_count; ++i) {
        if (assets->slots[i].asset && assets->slots[i].type == type) {
            wpe_log_error("cannot unregister asset type '%s': '%s' is still loaded",
                          assets->types[type].loader.name, assets->slots[i].path);
            return WPE_RESULT_ERROR_INVALID_STATE;
        }
    }
    assets->types[type] = (wpe__TypeEntry){0};
    return WPE_RESULT_OK;
}

WPE_AssetHandle wpe_asset_find(const WPE_AssetManager* assets, WPE_AssetType type, const char* path) {
    char normalized[WPE_VFS_PATH_MAX];
    if (!assets || !wpe_vfs_path_normalize(path, normalized, sizeof normalized)) return WPE_ASSET_NONE;
    /* NOTE(limitation): linear search; asset loads are rare compared to lookups by handle. */
    for (uint32_t i = 0; i < assets->slot_count; ++i) {
        const wpe__AssetSlot* slot = &assets->slots[i];
        if (slot->asset && slot->type == type && strcmp(slot->path, normalized) == 0)
            return wpe__handle(assets, i);
    }
    return WPE_ASSET_NONE;
}

WPE_Result wpe_asset_load(WPE_AssetManager* assets, WPE_AssetType type, const char* path,
                          WPE_AssetHandle* out_handle) {
    if (!assets || !path || !out_handle) return WPE_RESULT_ERROR_NULL_ARG;
    *out_handle = WPE_ASSET_NONE;
    if (!wpe__type_ok(assets, type)) {
        wpe_log_error("cannot load '%s': asset type %u is not registered", path, type);
        return WPE_RESULT_ERROR_NOT_FOUND;
    }
    char checked[WPE_VFS_PATH_MAX];
    if (!wpe_vfs_path_normalize(path, checked, sizeof checked)) {
        wpe_log_error("'%s' is not a valid asset path", path);
        return WPE_RESULT_ERROR_INVALID_ARG;
    }
    const WPE_AssetHandle existing = wpe_asset_find(assets, type, checked);
    wpe__AssetSlot* cached = wpe__slot(assets, existing);
    if (cached) {
        ++cached->refs;
        ++assets->hits;
        *out_handle = existing;
        return WPE_RESULT_OK;
    }
    char* normalized = wpe__normalized_copy(checked);
    if (!normalized) return WPE_RESULT_ERROR_OUT_OF_MEMORY;
    const wpe__TypeEntry* entry = &assets->types[type];
    if (!wpe_vfs_exists(assets->vfs, normalized)) {
        wpe_log_error("%s '%s' not found", entry->loader.name, normalized);
        wpe_free(normalized);
        return WPE_RESULT_ERROR_NOT_FOUND;
    }
    void* asset = NULL;
    WPE_Result result = entry->loader.load(assets, normalized, entry->loader.user_data, &asset);
    if (result == WPE_RESULT_OK && !asset) result = WPE_RESULT_ERROR;
    if (result == WPE_RESULT_OK) {
        result = wpe__insert(assets, type, normalized, asset, out_handle);
        if (result != WPE_RESULT_OK) entry->loader.destroy(asset, entry->loader.user_data);
    }
    if (result != WPE_RESULT_OK) {
        wpe_free(normalized);
        return result;
    }
    ++assets->loads;
    return WPE_RESULT_OK;
}

WPE_Result wpe_asset_add(WPE_AssetManager* assets, WPE_AssetType type, const char* name, void* asset,
                         WPE_AssetHandle* out_handle) {
    if (!assets || !name || !asset || !out_handle) return WPE_RESULT_ERROR_NULL_ARG;
    *out_handle = WPE_ASSET_NONE;
    if (!wpe__type_ok(assets, type)) return WPE_RESULT_ERROR_NOT_FOUND;
    char normalized[WPE_VFS_PATH_MAX];
    if (!wpe_vfs_path_normalize(name, normalized, sizeof normalized)) {
        wpe_log_error("'%s' is not a valid asset name", name);
        return WPE_RESULT_ERROR_INVALID_ARG;
    }
    if (wpe_asset_find(assets, type, normalized) != WPE_ASSET_NONE) {
        wpe_log_error("an asset named '%s' already exists", normalized);
        return WPE_RESULT_ERROR_ALREADY_EXISTS;
    }
    char* copy = wpe__normalized_copy(normalized);
    if (!copy) return WPE_RESULT_ERROR_OUT_OF_MEMORY;
    const WPE_Result result = wpe__insert(assets, type, copy, asset, out_handle);
    if (result != WPE_RESULT_OK) wpe_free(copy);
    return result;
}

void wpe_asset_retain(WPE_AssetManager* assets, WPE_AssetHandle handle) {
    wpe__AssetSlot* slot = wpe__slot(assets, handle);
    if (!wpe_check(slot != NULL)) return;
    ++slot->refs;
}

void wpe_asset_release(WPE_AssetManager* assets, WPE_AssetHandle handle) {
    if (handle == WPE_ASSET_NONE) return;
    wpe__AssetSlot* slot = wpe__slot(assets, handle);
    if (!wpe_check(slot != NULL)) return;
    if (--slot->refs == 0) wpe__destroy_slot(assets, (handle & WPE__ASSET_INDEX_MASK) - 1u);
}

bool wpe_asset_is_valid(const WPE_AssetManager* assets, WPE_AssetHandle handle) {
    return wpe__slot(assets, handle) != NULL;
}

WPE_AssetType wpe_asset_type(const WPE_AssetManager* assets, WPE_AssetHandle handle) {
    const wpe__AssetSlot* slot = wpe__slot(assets, handle);
    return slot ? slot->type : WPE_ASSET_TYPE_NONE;
}

const char* wpe_asset_path(const WPE_AssetManager* assets, WPE_AssetHandle handle) {
    const wpe__AssetSlot* slot = wpe__slot(assets, handle);
    return slot ? slot->path : NULL;
}

uint32_t wpe_asset_ref_count(const WPE_AssetManager* assets, WPE_AssetHandle handle) {
    const wpe__AssetSlot* slot = wpe__slot(assets, handle);
    return slot ? slot->refs : 0;
}

void* wpe_asset_get(const WPE_AssetManager* assets, WPE_AssetHandle handle, WPE_AssetType type) {
    const wpe__AssetSlot* slot = wpe__slot(assets, handle);
    return slot && slot->type == type ? slot->asset : NULL;
}

WPE_AssetStats wpe_asset_manager_stats(const WPE_AssetManager* assets) {
    WPE_AssetStats stats = {0};
    if (!assets) return stats;
    stats.assets = assets->live;
    stats.loads = assets->loads;
    stats.hits = assets->hits;
    for (uint32_t i = 0; i < assets->slot_count; ++i)
        if (assets->slots[i].asset) stats.references += assets->slots[i].refs;
    return stats;
}
