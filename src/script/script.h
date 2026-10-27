/**
 * @file script.h
 * @brief Luau scripting: per-entity behavior scripts with lifecycle callbacks, a sandboxed VM,
 *        error isolation and hot reload.
 *
 * A script file returns a table of methods (its "class"):
 *
 * @code
 * local Spinner = {}
 * function Spinner:on_start() self.speed = 1.5 end
 * function Spinner:on_update(dt) wpe.rotate(self.entity, vector.create(0, 1, 0), self.speed * dt) end
 * function Spinner:on_fixed_update(dt) end
 * function Spinner:on_destroy() end
 * function Spinner:on_reload() end -- optional: called after hot reload, state is preserved
 * function Spinner:on_collision_enter(other) end -- with physics (see below); also
 * function Spinner:on_collision_exit(other) end  -- on_trigger_enter(other) / on_trigger_exit(other)
 * return Spinner
 * @endcode
 *
 * Each entity with a WPE_COMP_SCRIPT component gets its own instance table (`self`), whose
 * `entity` field holds the entity handle and whose other fields persist across hot reloads.
 * Lifecycle:
 * - on_start: once, at the first wpe_script_host_update() after the script was attached.
 * - on_update(dt): every wpe_script_host_update(). on_fixed_update(dt): every fixed step.
 * - on_destroy: when the component is removed, its entity despawned, or the host destroyed.
 *   Inside on_destroy, scripts may read/modify component data and call wpe.destroy() (always
 *   deferred) but may not add/remove components or spawn entities. The entity's physics body is
 *   already gone when on_destroy runs on despawn.
 * - on_collision_enter/exit(other), on_trigger_enter/exit(other): from
 *   wpe_script_host_dispatch_physics(), for both entities of each event of the last physics step.
 *
 * Fixed-step order per step: wpe_script_host_fixed_update(), wpe_physics_step(),
 * wpe_script_host_dispatch_physics(). Destroy order: script host, audio, physics, ECS world,
 * asset manager.
 *
 * Isolation:
 * - Sandbox: only safe standard libraries (no io, no loadstring, no getfenv/setfenv; os has
 *   time/clock/date only); globals and libraries are read-only; each script file gets its own
 *   global environment. Prefab and script paths are asset paths in the asset manager's VFS, so
 *   scripts can only read what the game ships.
 * - Errors in a callback are logged with a traceback and disable that instance until the script
 *   is reloaded; other scripts and the engine keep running.
 * - Every callback has a time budget (infinite loops are interrupted) and the VM a memory limit.
 *
 * Hot reload: wpe_script_host_reload_changed() recompiles modified script files. On success
 * every instance switches to the new code with its state intact (and on_reload is called); on
 * compile errors the previous version keeps running.
 *
 * The Luau-side API (the read-only global `wpe`) is documented in docs/scripting.md.
 *
 * - Dependencies: core, math, ecs, scene, graphics, assets (asset manager, VFS), physics, audio, Luau
 *   (private)
 * - Thread affinity: main thread only (one VM per host)
 *
 * @author WillPowerEngine Team
 */
#ifndef WPE_SCRIPT_SCRIPT_H
#define WPE_SCRIPT_SCRIPT_H

#include <stddef.h>
#include <stdint.h>

#include "assets/asset_manager.h"
#include "audio/audio.h"
#include "core/result.h"
#include "ecs/ecs.h"
#include "physics/physics.h"

/** Script component data. Managed by the host; do not modify. */
typedef struct WPE_Script {
    uint32_t script;  /* Index of the loaded script file in the host. */
    int instance_ref; /* Registry reference of the instance table. */
    uint8_t state;    /* Private lifecycle state. */
} WPE_Script;

typedef struct WPE_ScriptHost WPE_ScriptHost;

typedef struct WPE_ScriptHostDesc {
    WPE_Ecs* ecs;             /* Borrowed. Scene and graphics components must be registered. */
    WPE_AssetManager* assets; /* Borrowed: scripts and prefabs are asset paths in its VFS. */
    WPE_Physics* physics;     /* Optional, borrowed: enables the physics API and collision callbacks. */
    WPE_Audio* audio;         /* Optional, borrowed: enables the audio API. */
    size_t memory_limit;      /* VM heap limit in bytes (0 = 64 MiB). */
    double time_limit_ms;     /* Budget per callback invocation (0 = 250 ms). */
} WPE_ScriptHostDesc;

typedef struct WPE_ScriptStats {
    uint32_t scripts;   /* Loaded script files. */
    uint32_t instances; /* Live script components. */
    uint32_t failed;    /* Instances disabled by an error. */
    uint64_t errors;    /* Errors since creation (compile and runtime). */
    size_t memory;      /* Current VM heap usage in bytes. */
} WPE_ScriptStats;

/**
 * Creates a VM and registers WPE_COMP_SCRIPT in the world. The host must be destroyed before the
 * world (it removes all script components, running their on_destroy).
 *
 * @return WPE_RESULT_OK; WPE_RESULT_ERROR_NULL_ARG; WPE_RESULT_ERROR_ALREADY_EXISTS (the world
 *         already has a script host); WPE_RESULT_ERROR_OUT_OF_MEMORY; WPE_RESULT_ERROR.
 *
 * @thread_safety Main thread
 */
[[nodiscard]] WPE_Result wpe_script_host_create(const WPE_ScriptHostDesc* desc, WPE_ScriptHost** out_host);

/** Removes all script components (on_destroy runs), then closes the VM. NULL is a no-op. @thread_safety Main
 * thread */
void wpe_script_host_destroy(WPE_ScriptHost* host);

/**
 * Attaches a script to an entity (replacing any existing one). The file is loaded and compiled
 * on first use and cached; on_start runs at the next wpe_script_host_update().
 *
 * @param path Asset path of the script (see wpe_vfs_path_is_valid()).
 * @return WPE_RESULT_OK; WPE_RESULT_ERROR_NULL_ARG; WPE_RESULT_ERROR_NOT_FOUND (dead entity or no
 *         such script file);
 *         WPE_RESULT_ERROR_INVALID_ARG (invalid asset path); WPE_RESULT_ERROR_IO;
 *         WPE_RESULT_ERROR_PARSE (compile error or the chunk did not return a table; logged);
 *         WPE_RESULT_ERROR_INVALID_STATE (called from inside on_destroy).
 *
 * @thread_safety Main thread
 */
WPE_Result wpe_script_attach(WPE_ScriptHost* host, WPE_Entity entity, const char* path);

/**
 * Runs on_start for new instances, then on_update(dt) for all, then applies deferred destroys.
 * Scripts may spawn, destroy and modify entities freely during the update.
 *
 * @return WPE_RESULT_OK or WPE_RESULT_ERROR_NULL_ARG. Script errors are not reported here; they
 *         are logged and counted in wpe_script_host_stats().
 *
 * @thread_safety Main thread
 */
WPE_Result wpe_script_host_update(WPE_ScriptHost* host, float dt);

/** Runs on_fixed_update(dt) for started instances, then applies deferred destroys. @thread_safety Main thread
 */
WPE_Result wpe_script_host_fixed_update(WPE_ScriptHost* host, float dt);

/**
 * Calls the collision and trigger callbacks for the events of the most recent wpe_physics_step()
 * (on started instances only), then applies deferred destroys. Each event notifies both entities;
 * `other` is the entity on the opposite side. A no-op without physics.
 *
 * @return WPE_RESULT_OK or WPE_RESULT_ERROR_NULL_ARG.
 *
 * @thread_safety Main thread
 */
WPE_Result wpe_script_host_dispatch_physics(WPE_ScriptHost* host);

/**
 * Recompiles every script file whose modification time changed since it was loaded.
 *
 * @return Number of scripts successfully reloaded.
 *
 * @thread_safety Main thread
 */
uint32_t wpe_script_host_reload_changed(WPE_ScriptHost* host);

/**
 * Recompiles one script file now (whether or not it changed).
 *
 * @return WPE_RESULT_OK, WPE_RESULT_ERROR_NULL_ARG, WPE_RESULT_ERROR_NOT_FOUND (not loaded),
 *         WPE_RESULT_ERROR_IO or WPE_RESULT_ERROR_PARSE (previous version kept).
 *
 * @thread_safety Main thread
 */
WPE_Result wpe_script_host_reload(WPE_ScriptHost* host, const char* path);

/**
 * Registers a named prefab that scripts can instantiate with wpe.instantiate(name). The host takes
 * its own reference to the model (released when replaced or when the host is destroyed). Names are
 * case-sensitive; re-registering replaces. Model asset paths work as prefab names without
 * registration (loaded on first use).
 *
 * @param model A WPE_ASSET_MODEL handle.
 * @return WPE_RESULT_OK, WPE_RESULT_ERROR_NULL_ARG, WPE_RESULT_ERROR_INVALID_ARG (empty or too long
 *         name, or not a model handle) or WPE_RESULT_ERROR_OUT_OF_MEMORY.
 *
 * @thread_safety Main thread
 */
WPE_Result wpe_script_host_register_prefab(WPE_ScriptHost* host, const char* name, WPE_AssetHandle model);

/** @return Current statistics. @thread_safety Main thread */
WPE_ScriptStats wpe_script_host_stats(const WPE_ScriptHost* host);

/**
 * Compiles and runs a chunk of Luau code in a fresh sandboxed environment (tools, consoles,
 * tests). Errors are logged.
 *
 * @param chunk_name Name used in error messages.
 * @return WPE_RESULT_OK, WPE_RESULT_ERROR_NULL_ARG, WPE_RESULT_ERROR_PARSE (compile error) or
 *         WPE_RESULT_ERROR (runtime error).
 *
 * @thread_safety Main thread
 */
WPE_Result wpe_script_host_run(WPE_ScriptHost* host, const char* chunk_name, const char* source);

#endif /* WPE_SCRIPT_SCRIPT_H */
