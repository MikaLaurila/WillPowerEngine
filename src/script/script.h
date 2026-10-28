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
 * function Spinner:on_click() end -- a UI button on this entity was clicked (see ui/ui.h)
 * return Spinner
 * @endcode
 *
 * Each entity with a WPE_COMP_SCRIPT component gets its own instance table (`self`), whose
 * `entity` field holds the entity handle and whose other fields persist across hot reloads.
 *
 * Properties: a script may declare its configurable fields with defaults, `Spinner.properties = {speed
 * = 1.5}`. Every instance starts with those fields (before the scene's own values are set), editors
 * list them (wpe_script_visit_declared()), and only values that differ from a default are saved.
 * Lifecycle:
 * - on_start: once, at the first wpe_script_host_update() after the script was attached.
 * - on_update(dt): every wpe_script_host_update(). on_fixed_update(dt): every fixed step.
 * - on_destroy: when the component is removed, its entity despawned, or the host destroyed.
 *   Inside on_destroy, scripts may read/modify component data and call wpe.destroy() (always
 *   deferred) but may not add/remove components or spawn entities. The entity's physics body is
 *   already gone when on_destroy runs on despawn.
 * - on_collision_enter/exit(other), on_trigger_enter/exit(other): from
 *   wpe_script_host_dispatch_physics(), for both entities of each event of the last physics step.
 * - Events of the application (on_click from the UI) through wpe_script_host_notify().
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
 * - Dependencies: core, math, ecs, scene, graphics, assets (asset manager, VFS), physics, audio, input, ui,
 *   Luau (private)
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
#include "graphics/draw2d.h"
#include "graphics/postfx.h"
#include "input/input.h"
#include "physics/physics.h"
#include "ui/ui.h"

/** Script component data. Managed by the host; do not modify. */
typedef struct WPE_Script {
    uint32_t script;  /* Index of the loaded script file in the host. */
    int instance_ref; /* Registry reference of the instance table. */
    int property_ref; /* Registry reference of the list of property names set from C (or LUA_NOREF). */
    uint8_t state;    /* Private lifecycle state. */
} WPE_Script;

typedef struct WPE_ScriptHost WPE_ScriptHost;

/**
 * Services of the application running the scripts (the engine runtime or the editor). Every
 * function is optional; the matching Luau functions raise an error when it is missing.
 */
typedef struct WPE_ScriptApp {
    void* user_data;
    /* Requests a scene switch; the application performs it after the current frame. */
    void (*load_scene)(void* user_data, const char* path);
    /* Requests the application to exit after the current frame. */
    void (*quit)(void* user_data);
    /* Makes an entity the camera the scene is rendered from (and heard from). */
    void (*set_camera)(void* user_data, WPE_Entity camera);
    WPE_Entity (*get_camera)(void* user_data);
    /* Size of the 2D canvas (framebuffer) in pixels. */
    void (*screen_size)(void* user_data, uint32_t* out_width, uint32_t* out_height);
    /* wpe.instantiate of names the application resolves itself (prefab files): creates the instance,
     * or returns WPE_RESULT_ERROR_NOT_FOUND for names that are not its own (the host then looks for a
     * model). Other errors are raised in the script. */
    WPE_Result (*instantiate)(void* user_data, const char* name, WPE_Entity* out_root);
    /* The existing directory for wpe.save_data/load_data files (ending in a separator or not), or NULL
     * when saving isn't possible. Asked on first use, so a directory is only made for games that save. */
    const char* (*save_directory)(void* user_data);
    /* The scene's post effects, which scripts may change: the array (room for WPE_POSTFX_MAX_EFFECTS) and,
     * through out_count, its count. */
    WPE_PostFxConfig* (*postfx)(void* user_data, uint32_t** out_count);
} WPE_ScriptApp;

typedef struct WPE_ScriptHostDesc {
    WPE_Ecs* ecs;             /* Borrowed. Scene and graphics components must be registered. */
    WPE_AssetManager* assets; /* Borrowed: scripts and prefabs are asset paths in its VFS. */
    WPE_Physics* physics;     /* Optional, borrowed: enables the physics API and collision callbacks. */
    WPE_Audio* audio;         /* Optional, borrowed: enables the audio API. */
    WPE_Input* input;         /* Optional, borrowed: enables the input API. */
    /* Optional, borrowed: enables 2D drawing (the application begins and submits the batch each
     * frame). Text needs `font` too, a WPE_ASSET_FONT handle the host keeps a reference to. */
    WPE_Draw2D* draw2d;
    WPE_AssetHandle font;
    /* Optional, borrowed: enables the UI focus functions (the UI component functions only need the
     * components registered). */
    WPE_Ui* ui;
    const WPE_ScriptApp* app; /* Optional; copied. */
    size_t memory_limit;      /* VM heap limit in bytes (0 = 64 MiB). */
    double time_limit_ms;     /* Budget per callback invocation (0 = 250 ms). */
} WPE_ScriptHostDesc;

typedef enum WPE_ScriptValueType {
    WPE_SCRIPT_VALUE_NIL = 0,
    WPE_SCRIPT_VALUE_NUMBER,
    WPE_SCRIPT_VALUE_BOOL,
    WPE_SCRIPT_VALUE_STRING,
    WPE_SCRIPT_VALUE_VECTOR,
} WPE_ScriptValueType;

/** A plain value stored in a script instance field (the field matching `type` is used). */
typedef struct WPE_ScriptValue {
    WPE_ScriptValueType type;
    double number;
    bool boolean;
    const char* string; /* Borrowed. */
    WPE_Vec3 vector;
} WPE_ScriptValue;

/** Receives one property; `name` and `value->string` are valid only during the call. */
typedef void (*WPE_ScriptPropertyFn)(const char* name, const WPE_ScriptValue* value, void* user_data);

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
 * Calls `method` (an event callback without arguments, such as "on_click") on the started, healthy
 * script of an entity, if it has one and the method exists, then applies deferred destroys.
 *
 * @return WPE_RESULT_OK or WPE_RESULT_ERROR_NULL_ARG. Script errors are logged, not returned.
 *
 * @thread_safety Main thread
 */
WPE_Result wpe_script_host_notify(WPE_ScriptHost* host, WPE_Entity entity, const char* method);

/**
 * Like wpe_script_host_notify(), passing one argument: `method(self, value)` (such as on_change). NULL
 * passes nothing.
 *
 * @thread_safety Main thread
 */
WPE_Result wpe_script_host_notify_value(WPE_ScriptHost* host, WPE_Entity entity, const char* method,
                                        const WPE_ScriptValue* value);

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

/** @return The asset path of the entity's script (borrowed from the host), or NULL if it has none.
 * @thread_safety Main thread */
const char* wpe_script_path(const WPE_ScriptHost* host, WPE_Entity entity);

/**
 * Sets a property of an entity's script instance: the field `self[name]`, assigned before on_start
 * when set right after wpe_script_attach() (scene files use this to configure scripts). Properties
 * set this way are remembered so wpe_script_visit_properties() can save their current values; one set
 * to the default its script declares is not (there is nothing to save), and NIL resets a declared
 * property to its default.
 *
 * @param name An identifier ([A-Za-z_][A-Za-z0-9_]*, at most 63 bytes) other than "entity".
 * @param value Number, boolean, string or vector (NIL removes the field and the property).
 * @return WPE_RESULT_OK; WPE_RESULT_ERROR_NULL_ARG; WPE_RESULT_ERROR_NOT_FOUND (no script on the
 *         entity); WPE_RESULT_ERROR_INVALID_ARG (bad name or value type; logged);
 *         WPE_RESULT_ERROR_OUT_OF_MEMORY.
 *
 * @thread_safety Main thread
 */
WPE_Result wpe_script_set_property(WPE_ScriptHost* host, WPE_Entity entity, const char* name,
                                   const WPE_ScriptValue* value);

/**
 * Calls `fn` for each property set with wpe_script_set_property(), in the order they were first
 * set, with the field's current value (scripts may have changed it). Fields that now hold another
 * kind of value (a table, a function, nil) are skipped.
 *
 * @return WPE_RESULT_OK, WPE_RESULT_ERROR_NULL_ARG or WPE_RESULT_ERROR_NOT_FOUND (no script).
 *
 * @thread_safety Main thread; `fn` must not call into the host
 */
WPE_Result wpe_script_visit_properties(const WPE_ScriptHost* host, WPE_Entity entity, WPE_ScriptPropertyFn fn,
                                       void* user_data);

/** Receives one declared property: its current value and its default (both valid only during the call). */
typedef void (*WPE_ScriptDeclaredFn)(const char* name, const WPE_ScriptValue* value,
                                     const WPE_ScriptValue* default_value, void* user_data);

/**
 * Calls `fn` for each property the entity's script declares in its class's `properties` table (number,
 * boolean, string or vector defaults), sorted by name, with the instance's current value. Editors show
 * these so properties can be set without knowing their names.
 *
 * @return WPE_RESULT_OK (also when the script declares none), WPE_RESULT_ERROR_NULL_ARG or
 *         WPE_RESULT_ERROR_NOT_FOUND (no script).
 *
 * @thread_safety Main thread; `fn` must not call into the host
 */
WPE_Result wpe_script_visit_declared(const WPE_ScriptHost* host, WPE_Entity entity, WPE_ScriptDeclaredFn fn,
                                     void* user_data);

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
