/**
 * @file asset_manager.h
 * @brief Shared, reference-counted assets loaded by path through a VFS.
 *
 * An asset is identified by its type and asset path. Loading an asset that is already loaded
 * returns the same handle with one more reference; releasing the last reference destroys it.
 * Components that reference assets (mesh renderers, animators, colliders, sound sources) hold one
 * reference each for as long as they exist, so an asset lives exactly as long as something uses it.
 *
 * Handles are 32-bit generational values (WPE_ASSET_NONE = 0 is never valid), so a stale handle
 * to a destroyed asset is detected instead of aliasing a newer one.
 *
 * Asset types: models (glTF) and fonts are built in; other modules register loaders for theirs
 * (the audio module registers sounds). Assets created in code (procedural meshes) are added with
 * wpe_asset_add() under a unique name and then behave like loaded ones.
 *
 * - Dependencies: core, vfs, assets (model, font), jobs (optional texture decoding)
 * - Thread affinity: owning thread (loaders may use the job system internally)
 *
 * @author WillPowerEngine Team
 */
#ifndef WPE_ASSETS_ASSET_MANAGER_H
#define WPE_ASSETS_ASSET_MANAGER_H

#include <stdint.h>

#include "assets/font.h"
#include "assets/model.h"
#include "core/result.h"
#include "jobs/jobs.h"
#include "vfs/vfs.h"

typedef uint32_t WPE_AssetHandle;
#define WPE_ASSET_NONE ((WPE_AssetHandle)0)

/** Asset type id. Built-in ids are stable; modules and games use ids from WPE_ASSET_USER_FIRST. */
typedef uint32_t WPE_AssetType;
enum WPE_BuiltinAssetType {
    WPE_ASSET_TYPE_NONE = 0,
    WPE_ASSET_MODEL = 1, /* WPE_Model (glTF / GLB) */
    WPE_ASSET_FONT = 2,  /* WPE_BitmapFont (PNG / JPEG glyph sheet) */
    WPE_ASSET_SOUND = 3, /* Registered by the audio module. */
    WPE_ASSET_USER_FIRST = 16,
};
#define WPE_ASSET_MAX_TYPES 32u

typedef struct WPE_AssetManager WPE_AssetManager;

/** Loader for one asset type. */
typedef struct WPE_AssetLoader {
    const char* name; /* For log messages, e.g. "sound". Borrowed for the registration's lifetime. */
    /* Loads `path` (a valid asset path) through wpe_asset_manager_vfs(); errors are logged. */
    WPE_Result (*load)(WPE_AssetManager* assets, const char* path, void* user_data, void** out_asset);
    /* Frees an asset produced by load (or handed to wpe_asset_add()). */
    void (*destroy)(void* asset, void* user_data);
    void* user_data;
} WPE_AssetLoader;

typedef struct WPE_AssetManagerDesc {
    const WPE_Vfs* vfs;        /* Required, borrowed: where assets are read from. */
    WPE_JobSystem* jobs;       /* Optional: parallel texture decoding for models. */
    uint32_t max_texture_size; /* Model texture size cap (0 = original size). */
} WPE_AssetManagerDesc;

typedef struct WPE_AssetStats {
    uint32_t assets;     /* Live assets. */
    uint32_t references; /* Sum of their reference counts. */
    uint64_t loads;      /* Loads from the VFS since creation (cache hits excluded). */
    uint64_t hits;       /* wpe_asset_load() calls served by an already loaded asset. */
} WPE_AssetStats;

/**
 * Creates a manager with the model and font loaders registered.
 *
 * @return WPE_RESULT_OK, WPE_RESULT_ERROR_NULL_ARG or WPE_RESULT_ERROR_OUT_OF_MEMORY.
 *
 * @thread_safety Any thread (the manager then belongs to that thread)
 */
[[nodiscard]] WPE_Result wpe_asset_manager_create(const WPE_AssetManagerDesc* desc,
                                                  WPE_AssetManager** out_assets);

/**
 * Destroys every remaining asset and the manager (ownership consumed); NULL is a no-op. Assets
 * that still have references are reported as leaks (warning). Destroy everything that holds
 * handles (worlds, script hosts, audio) first.
 *
 * @thread_safety Owning thread
 */
void wpe_asset_manager_destroy(WPE_AssetManager* assets);

/** @return The VFS assets are read from (borrowed). @thread_safety Any thread */
const WPE_Vfs* wpe_asset_manager_vfs(const WPE_AssetManager* assets);

/** @return The job system given at creation (may be NULL). @thread_safety Owning thread */
WPE_JobSystem* wpe_asset_manager_jobs(const WPE_AssetManager* assets);

/**
 * Registers the loader for an asset type.
 *
 * @return WPE_RESULT_OK; WPE_RESULT_ERROR_NULL_ARG; WPE_RESULT_ERROR_INVALID_ARG (type out of range or
 *         loader without load/destroy); WPE_RESULT_ERROR_ALREADY_EXISTS.
 *
 * @thread_safety Owning thread
 */
WPE_Result wpe_asset_manager_register_type(WPE_AssetManager* assets, WPE_AssetType type,
                                           const WPE_AssetLoader* loader);

/**
 * Unregisters a type so its module can shut down. Only allowed when no asset of the type exists.
 *
 * @return WPE_RESULT_OK, WPE_RESULT_ERROR_NULL_ARG, WPE_RESULT_ERROR_NOT_FOUND or
 *         WPE_RESULT_ERROR_INVALID_STATE (assets of the type are still loaded; logged).
 *
 * @thread_safety Owning thread
 */
WPE_Result wpe_asset_manager_unregister_type(WPE_AssetManager* assets, WPE_AssetType type);

/**
 * Loads an asset, or returns the already loaded one, adding a reference either way.
 *
 * @param path Asset path; "." and ".." segments are resolved, so equivalent spellings share one asset.
 * @param out_handle Receives the handle; release it with wpe_asset_release().
 * @return WPE_RESULT_OK; WPE_RESULT_ERROR_NULL_ARG; WPE_RESULT_ERROR_INVALID_ARG (invalid path; logged);
 *         WPE_RESULT_ERROR_NOT_FOUND (unregistered type or missing file; logged);
 *         WPE_RESULT_ERROR_CAPACITY (too many assets); WPE_RESULT_ERROR_OUT_OF_MEMORY; or the
 *         loader's error.
 *
 * @thread_safety Owning thread
 */
WPE_Result wpe_asset_load(WPE_AssetManager* assets, WPE_AssetType type, const char* path,
                          WPE_AssetHandle* out_handle);

/**
 * Adds an asset created in code under a name, transferring ownership to the manager (it is freed
 * with the type's loader destroy function when the last reference goes). On failure the caller
 * keeps ownership.
 *
 * @param name Asset path syntax; must not name an asset of the same type that is already loaded.
 * @return WPE_RESULT_OK; WPE_RESULT_ERROR_NULL_ARG; WPE_RESULT_ERROR_INVALID_ARG (invalid name);
 *         WPE_RESULT_ERROR_NOT_FOUND (unregistered type); WPE_RESULT_ERROR_ALREADY_EXISTS;
 *         WPE_RESULT_ERROR_CAPACITY; WPE_RESULT_ERROR_OUT_OF_MEMORY.
 *
 * @thread_safety Owning thread
 */
WPE_Result wpe_asset_add(WPE_AssetManager* assets, WPE_AssetType type, const char* name, void* asset,
                         WPE_AssetHandle* out_handle);

/** Adds a reference (no-op for invalid handles, which are reported in debug builds). @thread_safety Owning
 * thread */
void wpe_asset_retain(WPE_AssetManager* assets, WPE_AssetHandle handle);

/** Removes a reference, destroying the asset with the last one. WPE_ASSET_NONE is a no-op. @thread_safety
 * Owning thread */
void wpe_asset_release(WPE_AssetManager* assets, WPE_AssetHandle handle);

/** @return Whether the handle refers to a live asset. @thread_safety Owning thread */
bool wpe_asset_is_valid(const WPE_AssetManager* assets, WPE_AssetHandle handle);

/** @return The asset's type, or WPE_ASSET_TYPE_NONE for invalid handles. @thread_safety Owning thread */
WPE_AssetType wpe_asset_type(const WPE_AssetManager* assets, WPE_AssetHandle handle);

/** @return The asset's path or name (borrowed while the asset lives), or NULL. @thread_safety Owning thread
 */
const char* wpe_asset_path(const WPE_AssetManager* assets, WPE_AssetHandle handle);

/** @return The reference count (0 for invalid handles). @thread_safety Owning thread */
uint32_t wpe_asset_ref_count(const WPE_AssetManager* assets, WPE_AssetHandle handle);

/**
 * @return The asset data if the handle is valid and of `type`, else NULL. Borrowed: valid while the
 *         caller (or anything else) holds a reference.
 *
 * @thread_safety Owning thread; the returned data may be read from any thread while referenced
 */
void* wpe_asset_get(const WPE_AssetManager* assets, WPE_AssetHandle handle, WPE_AssetType type);

/** @return The handle of a loaded asset without adding a reference, or WPE_ASSET_NONE. @thread_safety Owning
 * thread */
WPE_AssetHandle wpe_asset_find(const WPE_AssetManager* assets, WPE_AssetType type, const char* path);

/** @return Current statistics. @thread_safety Owning thread */
WPE_AssetStats wpe_asset_manager_stats(const WPE_AssetManager* assets);

/** @return The model behind a WPE_ASSET_MODEL handle, or NULL. @thread_safety See wpe_asset_get() */
static inline const WPE_Model* wpe_asset_model(const WPE_AssetManager* assets, WPE_AssetHandle handle) {
    return (const WPE_Model*)wpe_asset_get(assets, handle, WPE_ASSET_MODEL);
}

/** @return The font behind a WPE_ASSET_FONT handle, or NULL. @thread_safety See wpe_asset_get() */
static inline const WPE_BitmapFont* wpe_asset_font(const WPE_AssetManager* assets, WPE_AssetHandle handle) {
    return (const WPE_BitmapFont*)wpe_asset_get(assets, handle, WPE_ASSET_FONT);
}

#endif /* WPE_ASSETS_ASSET_MANAGER_H */
