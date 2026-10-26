/**
 * @file script_internal.h
 * @brief Private state shared by the script host (script_host.c) and the Luau API bindings
 *        (script_api.c). Not a public API.
 *
 * Error-safety rule for every function callable from Luau: Luau errors unwind with longjmp, so a
 * binding must perform all argument checks (which may raise) before acquiring engine resources,
 * and must not hold owned engine allocations across any call that can raise (including pushes,
 * which may fail on memory).
 *
 * @author WillPowerEngine Team
 */
#ifndef WPE_SCRIPT_SCRIPT_INTERNAL_H
#define WPE_SCRIPT_SCRIPT_INTERNAL_H

#include <lua.h>
#include <lualib.h>

#include "script/script.h"

#define WPE__SCRIPT_PATH_MAX 256u
#define WPE__SCRIPT_PREFAB_NAME_MAX 128u

/* WPE_Script::state bits. */
#define WPE__SCRIPT_STARTED 0x1u
#define WPE__SCRIPT_FAILED 0x2u

typedef struct wpe__ScriptFile {
    char path[WPE__SCRIPT_PATH_MAX]; /* Relative to the asset root. */
    int64_t modified_time;
    int class_ref;  /* Registry ref: the table returned by the chunk. */
    int meta_ref;   /* Registry ref: shared instance metatable {__index = class}. */
    int thread_ref; /* Registry ref: sandbox thread holding the file's global environment. */
    uint32_t instances;
} wpe__ScriptFile;

typedef struct wpe__Prefab {
    char name[WPE__SCRIPT_PREFAB_NAME_MAX];
    const WPE_Model* model;
    WPE_Model* owned; /* Non-NULL when the host loaded the model and must free it. */
} wpe__Prefab;

struct WPE_ScriptHost {
    WPE_Ecs* ecs;
    lua_State* L;
    char* asset_root;
    WPE_JobSystem* jobs;
    uint32_t max_texture_size;

    size_t memory_limit;
    size_t memory_used;
    double time_limit_seconds;
    double deadline; /* lua_clock() time after which the running callback is interrupted (0 = none). */
    uint32_t restricted_depth; /* > 0 while on_destroy runs inside an ECS hook. */
    bool shutting_down;

    wpe__ScriptFile* files;
    uint32_t file_count;
    uint32_t file_capacity;

    wpe__Prefab* prefabs;
    uint32_t prefab_count;
    uint32_t prefab_capacity;

    WPE_Entity* pending_destroy; /* Deferred wpe.destroy() targets. */
    uint32_t pending_count;
    uint32_t pending_capacity;

    WPE_Entity* snapshot; /* Script entities iterated by the current update. */
    uint32_t snapshot_capacity;

    uint64_t errors;
    double time; /* Accumulated game time (seconds) from updates. */
    uint64_t frame;
};

/* Host services used by the bindings. */
WPE_ScriptHost* wpe__script_host_from(lua_State* L);
bool wpe__script_path_is_safe(const char* path);
/* Builds asset_root + "/" + path into `out` (size `capacity`); false if it does not fit. */
bool wpe__script_full_path(const WPE_ScriptHost* host, const char* path, char* out, size_t capacity);
WPE_Result wpe__script_queue_destroy(WPE_ScriptHost* host, WPE_Entity entity);
/* Finds or loads a prefab by registered name or asset-relative model path; NULL on failure (logged). */
const WPE_Model* wpe__script_find_prefab(WPE_ScriptHost* host, const char* name_or_path);

/* Installs the `wpe` library and `print` into the VM's globals (before sandboxing). */
void wpe__script_api_register(lua_State* L);

#endif /* WPE_SCRIPT_SCRIPT_INTERNAL_H */
