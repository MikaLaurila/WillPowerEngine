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

#include "math/easing.h"
#include "script/script.h"

#define WPE__SCRIPT_PATH_MAX 256u
#define WPE__SCRIPT_PREFAB_NAME_MAX 128u

/* WPE_Script::state bits. */
#define WPE__SCRIPT_STARTED 0x1u
#define WPE__SCRIPT_FAILED 0x2u

typedef struct wpe__ScriptFile {
    char path[WPE__SCRIPT_PATH_MAX]; /* Asset path. */
    int64_t modified_time;
    int class_ref;  /* Registry ref: the table returned by the chunk. */
    int meta_ref;   /* Registry ref: shared instance metatable {__index = class}. */
    int thread_ref; /* Registry ref: sandbox thread holding the file's global environment. */
    uint32_t instances;
} wpe__ScriptFile;

typedef struct wpe__Prefab {
    char name[WPE__SCRIPT_PREFAB_NAME_MAX];
    WPE_AssetHandle model; /* The host holds a reference. */
} wpe__Prefab;

/* A module loaded by require(), cached until one of the modules changes on disk. */
typedef struct wpe__Module {
    char path[WPE__SCRIPT_PATH_MAX];
    int64_t modified_time;
    int value_ref;  /* Registry ref: what the chunk returned (true for nothing). */
    int thread_ref; /* Registry ref: sandbox thread holding the module's globals. */
    bool loading;   /* Running its chunk now: requiring it again is circular. */
} wpe__Module;

/* A wpe.after / wpe.every callback. */
typedef struct wpe__Timer {
    uint32_t id;
    int owner_instance; /* Instance ref of the script that made it (LUA_NOREF: none); it goes with that
                           script. */
    WPE_Entity owner;
    double due;      /* Game time it fires at. */
    double interval; /* Repeat interval (0: once). */
    int fn_ref;      /* Registry ref of the callback; LUA_NOREF once cancelled. */
} wpe__Timer;

/* What a tween moves (wpe__Tween::props bits). */
#define WPE__TWEEN_POSITION 0x1u
#define WPE__TWEEN_ROTATION 0x2u
#define WPE__TWEEN_SCALE 0x4u
#define WPE__TWEEN_VALUE 0x8u /* A number handed to `fn_ref` (wpe.tween_value). */

typedef enum wpe__TweenLoop {
    WPE__TWEEN_ONCE = 0,
    WPE__TWEEN_REPEAT, /* Starts over from the beginning. */
    WPE__TWEEN_YOYO,   /* Goes back and forth. */
} wpe__TweenLoop;

/* A wpe.tween / wpe.tween_value animation, advanced after the timers in each update. */
typedef struct wpe__Tween {
    uint32_t id;
    int owner_instance; /* As for timers: it goes with the script that made it. */
    WPE_Entity owner;
    WPE_Entity target; /* Entity tweens: whose transform moves (WPE_ENTITY_NULL for value tweens). */
    uint32_t props;    /* WPE__TWEEN_* bits still animated (a newer tween of a property takes it over). */
    /* Targets as given; the starting values are taken when it starts (after its delay), and relative
     * targets are added to them then. */
    WPE_Vec3 to_position, to_scale, from_position, from_scale;
    WPE_Quat to_rotation, from_rotation;
    double from_value, to_value;
    float delay;    /* Seconds left before it starts. */
    float duration; /* Seconds per play. */
    float elapsed;  /* Seconds into the current play. */
    uint32_t times; /* Plays in all for loops (0: forever); counts down. */
    WPE_Ease ease;
    wpe__TweenLoop loop;
    bool relative;  /* Targets are offsets from the starting values. */
    bool real_time; /* Runs on real time (through slow motion and hit-stops). */
    bool started;
    bool reversed; /* A yoyo on its way back. */
    bool live;     /* False once finished or cancelled (dropped at the end of the update). */
    int fn_ref;    /* Value tweens: called with the value each update. LUA_NOREF otherwise. */
    int done_ref;  /* Called once it finishes (not when cancelled). LUA_NOREF for none. */
} wpe__Tween;

/* Sounds used by scripts stay loaded (one reference each) until the host is destroyed. */
typedef struct wpe__CachedSound {
    char path[WPE__SCRIPT_PATH_MAX];
    WPE_AssetHandle sound;
} wpe__CachedSound;

struct WPE_ScriptHost {
    WPE_Ecs* ecs;
    WPE_AssetManager* assets;
    WPE_Physics* physics; /* Optional. */
    WPE_Audio* audio;     /* Optional. */
    WPE_Input* input;     /* Optional. */
    WPE_Draw2D* draw2d;   /* Optional. */
    WPE_AssetHandle font; /* Optional; the host holds a reference. */
    WPE_Ui* ui;           /* Optional. */
    WPE_ScriptApp app;    /* Zeroed when the application provides no services. */
    lua_State* L;

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

    wpe__CachedSound* sounds;
    uint32_t sound_count;
    uint32_t sound_capacity;

    WPE_Entity* pending_destroy; /* Deferred wpe.destroy() targets. */
    uint32_t pending_count;
    uint32_t pending_capacity;

    WPE_Entity* snapshot; /* Script entities iterated by the current update. */
    uint32_t snapshot_capacity;

    wpe__Module* modules;
    uint32_t module_count;
    uint32_t module_capacity;

    wpe__Timer* timers;
    uint32_t timer_count;
    uint32_t timer_capacity;
    uint32_t next_timer_id;

    wpe__Tween* tweens;
    uint32_t tween_count;
    uint32_t tween_capacity;
    uint32_t next_tween_id;

    /* The script instance whose code runs now (timers it makes belong to it); LUA_NOREF/NULL outside. */
    int current_instance;
    WPE_Entity current_entity;

    uint64_t errors;
    double time; /* Accumulated game time (seconds) from updates. */
    uint64_t frame;
};

/* Host services used by the bindings. */
WPE_ScriptHost* wpe__script_host_from(lua_State* L);
WPE_Result wpe__script_queue_destroy(WPE_ScriptHost* host, WPE_Entity entity);
/* Finds or loads a prefab by registered name or model asset path; WPE_ASSET_NONE on failure (logged).
 * The prefab table keeps the returned model loaded. */
WPE_AssetHandle wpe__script_find_prefab(WPE_ScriptHost* host, const char* name_or_path);
/* Finds or loads a sound by asset path (kept loaded by the host); WPE_ASSET_NONE on failure (logged). */
WPE_AssetHandle wpe__script_find_sound(WPE_ScriptHost* host, const char* path);

/* Adds a timer owned by the running script (if any); returns its id (0 when out of memory). */
uint32_t wpe__script_add_timer(WPE_ScriptHost* host, double delay, double interval, int fn_ref);
/* Cancels a timer; false when no such timer is pending. */
bool wpe__script_cancel_timer(WPE_ScriptHost* host, uint32_t id);

/* Adds a tween owned by the running script (if any), taking its targets' properties over from older tweens of
 * the same entity; fills in id and owner. Returns its id (0 when out of memory: the caller drops its refs).
 */
uint32_t wpe__script_add_tween(WPE_ScriptHost* host, const wpe__Tween* tween);
/* Stops a tween where it is (no on_done); false when no such tween runs. */
bool wpe__script_cancel_tween(WPE_ScriptHost* host, uint32_t id);
/* Stops every tween of an entity; returns how many. */
uint32_t wpe__script_stop_tweens(WPE_ScriptHost* host, WPE_Entity entity);
/* Whether a tween of the entity runs (or waits for its delay). */
bool wpe__script_is_tweening(const WPE_ScriptHost* host, WPE_Entity entity);

/* Bindings that need the host's internals: require(name), wpe.broadcast(method, ...). */
int wpe__script_require(lua_State* L);
int wpe__script_broadcast(lua_State* L);

/* Installs the `wpe` library and `print` into the VM's globals (before sandboxing). */
void wpe__script_api_register(lua_State* L);

#endif /* WPE_SCRIPT_SCRIPT_INTERNAL_H */
