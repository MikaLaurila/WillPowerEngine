#define WPE_LOG_MODULE "script"
#include <luacode.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "core/alloc.h"
#include "core/debug.h"
#include "core/log.h"
#include "graphics/anim_states.h"
#include "graphics/graphics.h"
#include "platform/platform.h"
#include "scene/scene.h"
#include "script/script_internal.h"

#define WPE__DEFAULT_MEMORY_LIMIT ((size_t)64 * 1024 * 1024)
#define WPE__DEFAULT_TIME_LIMIT_MS 250.0
#define WPE__ERROR_MESSAGE_MAX 2048u

/* Globals removed before sandboxing: dynamic code loading and environment manipulation. */
static const char* const wpe__unsafe_globals[] = {"loadstring", "load",    "dofile",
                                                  "loadfile",   "getfenv", "setfenv"};

/* ---- VM plumbing ---------------------------------------------------------------------------------- */

WPE_ScriptHost* wpe__script_host_from(lua_State* L) {
    return lua_callbacks(L)->userdata;
}

/* Allocator enforcing the VM memory limit; all VM memory goes through the tracked allocator. */
static void* wpe__lua_alloc(void* ud, void* ptr, size_t osize, size_t nsize) {
    WPE_ScriptHost* host = ud;
    if (!ptr) osize = 0;
    if (nsize == 0) {
        if (ptr) {
            wpe_free(ptr);
            host->memory_used -= osize;
        }
        return NULL;
    }
    if (nsize > osize && host->memory_used - osize + nsize > host->memory_limit) return NULL;
    void* block = wpe_realloc(ptr, nsize);
    if (!block) return NULL;
    host->memory_used = host->memory_used - osize + nsize;
    return block;
}

/* Interrupts scripts that exceed their time budget (infinite loops, runaway recursion). */
static void wpe__interrupt(lua_State* L, int gc) {
    if (gc >= 0) return; /* GC safepoints are not script progress. */
    WPE_ScriptHost* host = wpe__script_host_from(L);
    if (host->deadline > 0.0 && lua_clock() > host->deadline) {
        host->deadline = 0.0; /* Allow the error to unwind without re-triggering. */
        luaL_errorL(L, "script exceeded its time budget of %.0f ms", host->time_limit_seconds * 1000.0);
    }
}

static int wpe__traceback(lua_State* L) {
    const char* message = lua_tostring(L, 1);
    luaL_traceback(L, L, message ? message : "(error object is not a string)", 1);
    return 1;
}

/* Starts a callback's time budget, returning the previous deadline (nested invocations). */
static double wpe__begin_budget(WPE_ScriptHost* host) {
    const double previous = host->deadline;
    host->deadline = lua_clock() + host->time_limit_seconds;
    return previous;
}

/* ---- Script files --------------------------------------------------------------------------------- */

typedef struct wpe__LoadOp {
    WPE_ScriptHost* host;
    const char* chunk_name;
    const char* bytecode;
    size_t bytecode_size;
    int existing_meta_ref; /* LUA_NOREF for a new file; otherwise the meta table to retarget. */
    bool require_table;    /* Script files must return their class table; ad-hoc chunks need not. */
    int class_ref;         /* out */
    int meta_ref;          /* out */
    int thread_ref;        /* out */
} wpe__LoadOp;

/* Protected: loads bytecode into a fresh sandbox thread, runs it and captures the class table. */
static int wpe__op_load(lua_State* L) {
    wpe__LoadOp* op = lua_touserdata(L, 1);
    lua_State* T = lua_newthread(L); /* Kept on L's stack (alive) until referenced below. */
    luaL_sandboxthread(T);

    if (luau_load(T, op->chunk_name, op->bytecode, op->bytecode_size, 0) != 0) {
        luaL_errorL(L, "%s", lua_tostring(T, -1) ? lua_tostring(T, -1) : "invalid bytecode");
    }
    lua_pushcfunction(T, wpe__traceback, "traceback");
    lua_insert(T, -2);
    const double previous = wpe__begin_budget(op->host);
    const int status = lua_pcall(T, 0, 1, -2);
    op->host->deadline = previous;
    if (status != 0) luaL_errorL(L, "%s", lua_tostring(T, -1) ? lua_tostring(T, -1) : "error while loading");
    if (!op->require_table) {
        op->class_ref = op->meta_ref = op->thread_ref = LUA_NOREF;
        return 0;
    }
    if (!lua_istable(T, -1))
        luaL_errorL(L, "script must return a table of callbacks (got %s)", luaL_typename(T, -1));

    lua_xmove(T, L, 1); /* L: [op, T, class] */
    if (op->existing_meta_ref == LUA_NOREF) {
        lua_createtable(L, 0, 1);
        lua_pushvalue(L, -2);
        lua_setfield(L, -2, "__index");
        op->meta_ref = lua_ref(L, -1);
        lua_pop(L, 1);
    } else {
        /* Hot reload: every instance shares this metatable, so retargeting __index swaps code for all. */
        lua_getref(L, op->existing_meta_ref);
        lua_pushvalue(L, -2);
        lua_setfield(L, -2, "__index");
        lua_pop(L, 1);
        op->meta_ref = op->existing_meta_ref;
    }
    op->class_ref = lua_ref(L, -1);
    op->thread_ref = lua_ref(L, -2);
    return 0;
}

/* Reads and compiles a script; on success returns owned bytecode (free with free()). */
static WPE_Result wpe__compile_file(WPE_ScriptHost* host, const char* path, char** out_bytecode,
                                    size_t* out_size, int64_t* out_time) {
    const WPE_Vfs* vfs = wpe_asset_manager_vfs(host->assets);
    void* source = NULL;
    size_t source_size = 0;
    WPE_Result result = wpe_vfs_read_all(vfs, path, &source, &source_size);
    if (result == WPE_RESULT_ERROR_NOT_FOUND) wpe_log_error("script '%s' not found", path);
    if (result != WPE_RESULT_OK) return result;
    if (out_time && wpe_vfs_modified_time(vfs, path, out_time) != WPE_RESULT_OK) *out_time = 0;

    lua_CompileOptions options = {.optimizationLevel = 1, .debugLevel = 1};
    size_t bytecode_size = 0;
    char* bytecode = luau_compile(source, source_size, &options, &bytecode_size);
    wpe_free(source);
    if (!bytecode) return WPE_RESULT_ERROR_OUT_OF_MEMORY;
    if (bytecode_size == 0 || bytecode[0] == 0) {
        /* Luau encodes compile errors as a zero version byte followed by the message. */
        wpe_log_error("compile error in '%s': %.*s", path, (int)(bytecode_size > 1 ? bytecode_size - 1 : 0),
                      bytecode + 1);
        free(bytecode); /* luau_compile allocates with malloc. */
        ++host->errors;
        return WPE_RESULT_ERROR_PARSE;
    }
    *out_bytecode = bytecode;
    *out_size = bytecode_size;
    return WPE_RESULT_OK;
}

bool wpe_script_check_syntax(const char* source, size_t size, uint32_t* out_line, char* out_message,
                             size_t message_size) {
    if (out_line) *out_line = 0;
    if (out_message && message_size) out_message[0] = '\0';
    lua_CompileOptions options = {.optimizationLevel = 0, .debugLevel = 1};
    size_t bytecode_size = 0;
    char* bytecode = luau_compile(source ? source : "", source ? size : 0, &options, &bytecode_size);
    if (!bytecode) {
        if (out_message && message_size) snprintf(out_message, message_size, "out of memory");
        return false;
    }
    const bool ok = bytecode_size > 0 && bytecode[0] != 0;
    if (!ok) {
        /* The message after the zero byte reads ":LINE: text". */
        const char* message = bytecode_size > 1 ? bytecode + 1 : "";
        const size_t length = bytecode_size > 1 ? bytecode_size - 1 : 0;
        size_t at = 0;
        uint32_t line = 0;
        if (at < length && message[at] == ':') {
            size_t digits = at + 1;
            while (digits < length && message[digits] >= '0' && message[digits] <= '9') {
                line = line * 10u + (uint32_t)(message[digits] - '0');
                ++digits;
            }
            if (digits > at + 1 && digits < length && message[digits] == ':') {
                at = digits + 1;
                while (at < length && message[at] == ' ')
                    ++at;
            } else {
                line = 0;
            }
        }
        if (out_line) *out_line = line;
        if (out_message && message_size)
            snprintf(out_message, message_size, "%.*s", (int)(length - at), message + at);
    }
    free(bytecode); /* luau_compile allocates with malloc. */
    return ok;
}

typedef struct wpe__ListOp {
    void (*fn)(const char* name, bool function, void* user_data);
    void* user_data;
    uint32_t count; /* out */
} wpe__ListOp;

static int wpe__compare_api_names(const void* a, const void* b) {
    return strcmp(*(const char* const*)a, *(const char* const*)b);
}

/* Protected: collects the wpe table's string keys, sorts them and hands them out. */
static int wpe__op_list_api(lua_State* L) {
    wpe__ListOp* op = lua_touserdata(L, 1);
    lua_getglobal(L, "wpe");
    if (!lua_istable(L, -1)) return 0;
    const int table = lua_gettop(L);
    int count = 0;
    lua_pushnil(L);
    while (lua_next(L, table)) {
        lua_pop(L, 1);
        count += lua_type(L, -1) == LUA_TSTRING;
    }
    /* The names stay alive as the table's keys while they are sorted and handed out. */
    const char** names = lua_newuserdata(L, (size_t)(count ? count : 1) * sizeof *names);
    int n = 0;
    lua_pushnil(L);
    while (lua_next(L, table)) {
        lua_pop(L, 1);
        if (lua_type(L, -1) == LUA_TSTRING && n < count) names[n++] = lua_tostring(L, -1);
    }
    qsort(names, (size_t)n, sizeof *names, wpe__compare_api_names);
    for (int i = 0; i < n; ++i) {
        lua_getfield(L, table, names[i]);
        const bool function = lua_isfunction(L, -1);
        lua_pop(L, 1);
        op->fn(names[i], function, op->user_data);
    }
    op->count = (uint32_t)n;
    return 0;
}

uint32_t wpe_script_host_list_api(WPE_ScriptHost* host,
                                  void (*fn)(const char* name, bool function, void* user_data),
                                  void* user_data) {
    if (!host || !fn) return 0;
    wpe__ListOp op = {.fn = fn, .user_data = user_data};
    if (lua_cpcall(host->L, wpe__op_list_api, &op) != 0) lua_pop(host->L, 1);
    return op.count;
}

static WPE_Result wpe__load_bytecode(WPE_ScriptHost* host, const char* path, const char* bytecode,
                                     size_t size, int existing_meta_ref, wpe__LoadOp* out_op) {
    char chunk_name[WPE__SCRIPT_PATH_MAX + 1];
    snprintf(chunk_name, sizeof chunk_name, "@%s", path);
    *out_op = (wpe__LoadOp){.host = host,
                            .chunk_name = chunk_name,
                            .bytecode = bytecode,
                            .bytecode_size = size,
                            .existing_meta_ref = existing_meta_ref,
                            .require_table = true};
    if (lua_cpcall(host->L, wpe__op_load, out_op) != 0) {
        wpe_log_error("failed to load '%s': %s", path,
                      lua_tostring(host->L, -1) ? lua_tostring(host->L, -1) : "unknown error");
        lua_pop(host->L, 1);
        ++host->errors;
        return WPE_RESULT_ERROR_PARSE;
    }
    return WPE_RESULT_OK;
}

/* Returns the index of a loaded script file, loading it on first use. */
static WPE_Result wpe__script_file(WPE_ScriptHost* host, const char* path, uint32_t* out_index) {
    for (uint32_t i = 0; i < host->file_count; ++i) {
        if (strcmp(host->files[i].path, path) == 0) {
            *out_index = i;
            return WPE_RESULT_OK;
        }
    }
    if (host->file_count == host->file_capacity) {
        const uint32_t capacity = host->file_capacity ? host->file_capacity * 2 : 16;
        wpe__ScriptFile* files = wpe_realloc(host->files, (size_t)capacity * sizeof *files);
        if (!files) return WPE_RESULT_ERROR_OUT_OF_MEMORY;
        host->files = files;
        host->file_capacity = capacity;
    }

    char* bytecode = NULL;
    size_t size = 0;
    int64_t modified = 0;
    WPE_Result result = wpe__compile_file(host, path, &bytecode, &size, &modified);
    if (result != WPE_RESULT_OK) return result;
    wpe__LoadOp op;
    result = wpe__load_bytecode(host, path, bytecode, size, LUA_NOREF, &op);
    free(bytecode);
    if (result != WPE_RESULT_OK) return result;

    wpe__ScriptFile* file = &host->files[host->file_count];
    *file = (wpe__ScriptFile){.modified_time = modified,
                              .class_ref = op.class_ref,
                              .meta_ref = op.meta_ref,
                              .thread_ref = op.thread_ref};
    snprintf(file->path, sizeof file->path, "%s", path);
    *out_index = host->file_count++;
    return WPE_RESULT_OK;
}

/* ---- Instances and callbacks ---------------------------------------------------------------------- */

typedef struct wpe__InstanceOp {
    int meta_ref;
    WPE_Entity entity;
    int instance_ref; /* out */
} wpe__InstanceOp;

/* ---- Declared properties: the `properties` table of a script's class ------------------------------- */

static bool wpe__property_name_valid(const char* name);

/* Whether a Lua value can be a script property (number, boolean, string or vector). */
static bool wpe__property_value(lua_State* L, int index) {
    const int type = lua_type(L, index);
    return type == LUA_TNUMBER || type == LUA_TBOOLEAN || type == LUA_TSTRING || type == LUA_TVECTOR;
}

/* Pushes the declared-properties table of the class behind an instance's metatable; false (nothing
 * pushed) when the script declares none. */
static bool wpe__push_declared(lua_State* L, int instance) {
    if (!lua_getmetatable(L, instance)) return false;
    lua_pushstring(L, "__index");
    lua_rawget(L, -2);
    if (!lua_istable(L, -1)) {
        lua_pop(L, 2);
        return false;
    }
    lua_pushstring(L, "properties");
    lua_rawget(L, -2);
    lua_remove(L, -2); /* class */
    lua_remove(L, -2); /* metatable */
    if (lua_istable(L, -1)) return true;
    lua_pop(L, 1);
    return false;
}

/* Gives the instance at `instance` the declared defaults of the properties it doesn't hold yet. */
static void wpe__fill_defaults(lua_State* L, int instance) {
    if (!wpe__push_declared(L, instance)) return;
    const int declared = lua_gettop(L);
    lua_pushnil(L);
    while (lua_next(L, declared)) {
        if (lua_type(L, -2) == LUA_TSTRING && wpe__property_value(L, -1) &&
            wpe__property_name_valid(lua_tostring(L, -2))) {
            lua_pushvalue(L, -2);
            lua_rawget(L, instance);
            const bool missing = lua_isnil(L, -1);
            lua_pop(L, 1);
            if (missing) {
                lua_pushvalue(L, -2);
                lua_pushvalue(L, -2);
                lua_rawset(L, instance);
            }
        }
        lua_pop(L, 1);
    }
    lua_pop(L, 1);
}

static int wpe__op_create_instance(lua_State* L) {
    wpe__InstanceOp* op = lua_touserdata(L, 1);
    lua_createtable(L, 0, 4);
    lua_pushnumber(L, (double)op->entity);
    lua_setfield(L, -2, "entity");
    lua_getref(L, op->meta_ref);
    lua_setmetatable(L, -2);
    wpe__fill_defaults(L, lua_gettop(L));
    op->instance_ref = lua_ref(L, -1);
    lua_pop(L, 1);
    return 0;
}

/* Protected: wpe__fill_defaults for an instance (after hot reload declared new properties). */
static int wpe__op_fill_defaults(lua_State* L) {
    const int* instance_ref = lua_touserdata(L, 1);
    lua_getref(L, *instance_ref);
    wpe__fill_defaults(L, lua_gettop(L));
    return 0;
}

typedef struct wpe__CallOp {
    WPE_ScriptHost* host;
    int instance_ref;
    const char* method;
    bool has_arg;
    double arg;
    const WPE_ScriptValue* value;         /* Optional argument instead of `arg` (on_change). */
    bool failed;                          /* out */
    char message[WPE__ERROR_MESSAGE_MAX]; /* out */
} wpe__CallOp;

/* Pushes a script value (nil for NIL). */
static void wpe__push_script_value(lua_State* L, const WPE_ScriptValue* value) {
    switch (value->type) {
    case WPE_SCRIPT_VALUE_NUMBER:
        lua_pushnumber(L, value->number);
        return;
    case WPE_SCRIPT_VALUE_BOOL:
        lua_pushboolean(L, value->boolean);
        return;
    case WPE_SCRIPT_VALUE_STRING:
        lua_pushstring(L, value->string ? value->string : "");
        return;
    case WPE_SCRIPT_VALUE_VECTOR:
        lua_pushvector(L, value->vector.x, value->vector.y, value->vector.z);
        return;
    case WPE_SCRIPT_VALUE_NIL:
        break;
    }
    lua_pushnil(L);
}

/* Protected: calls instance:method(arg) if the method exists. Script errors are captured in op. */
static int wpe__op_call(lua_State* L) {
    wpe__CallOp* op = lua_touserdata(L, 1);
    lua_pushcfunction(L, wpe__traceback, "traceback");
    const int handler = lua_gettop(L);
    lua_getref(L, op->instance_ref);
    if (!lua_istable(L, -1)) return 0;
    lua_getfield(L, -1, op->method);
    if (!lua_isfunction(L, -1)) return 0;
    lua_insert(L, -2); /* [handler, fn, instance] */
    int args = 1;
    if (op->value) {
        wpe__push_script_value(L, op->value);
        ++args;
    } else if (op->has_arg) {
        lua_pushnumber(L, op->arg);
        ++args;
    }
    const double previous = wpe__begin_budget(op->host);
    const int status = lua_pcall(L, args, 0, handler);
    op->host->deadline = previous;
    if (status != 0) {
        op->failed = true;
        snprintf(op->message, sizeof op->message, "%s",
                 lua_tostring(L, -1) ? lua_tostring(L, -1) : "unknown error");
    }
    return 0;
}

/* Logs and counts a failed method of an instance and disables it until reload. */
static void wpe__instance_failed(WPE_ScriptHost* host, WPE_Entity entity, int instance_ref, uint32_t script,
                                 const char* method, const char* message) {
    ++host->errors;
    /*
     * Luau does not collect garbage when an allocation fails, so a memory error leaves the failed
     * script's garbage in place; collect now so other scripts are not starved by it.
     */
    lua_gc(host->L, LUA_GCCOLLECT, 0);
    wpe_log_error("%s failed in '%s' (entity 0x%08x); instance disabled until reload:\n%s", method,
                  script < host->file_count ? host->files[script].path : "?", entity, message);
    /* The component may have moved or been replaced during the call; only flag the same instance. */
    WPE_Script* s = wpe_ecs_get_component(host->ecs, entity, WPE_COMP_SCRIPT);
    if (s && s->instance_ref == instance_ref) s->state |= WPE__SCRIPT_FAILED;
}

static bool wpe__invoke(WPE_ScriptHost* host, WPE_Entity entity, uint32_t script, wpe__CallOp* op);

/* Calls a lifecycle method; on error logs, counts and marks the instance failed. Returns false on error. */
static bool wpe__call_method(WPE_ScriptHost* host, WPE_Entity entity, int instance_ref, uint32_t script,
                             const char* method, bool has_arg, double arg) {
    wpe__CallOp op = {
        .host = host, .instance_ref = instance_ref, .method = method, .has_arg = has_arg, .arg = arg};
    return wpe__invoke(host, entity, script, &op);
}

/* Runs a prepared call (see wpe__call_method). */
static bool wpe__invoke(WPE_ScriptHost* host, WPE_Entity entity, uint32_t script, wpe__CallOp* op) {
    const int previous_instance = host->current_instance;
    const WPE_Entity previous_entity = host->current_entity;
    host->current_instance = op->instance_ref;
    host->current_entity = entity;
    if (lua_cpcall(host->L, wpe__op_call, op) != 0) {
        op->failed = true;
        snprintf(op->message, sizeof op->message, "%s",
                 lua_tostring(host->L, -1) ? lua_tostring(host->L, -1) : "VM error");
        lua_pop(host->L, 1);
    }
    host->current_instance = previous_instance;
    host->current_entity = previous_entity;
    /* No lua_settop here: this may run nested inside a binding whose stack frame must survive. */
    if (!op->failed) return true;
    wpe__instance_failed(host, entity, op->instance_ref, script, op->method, op->message);
    return false;
}

static void wpe__kill_tween(WPE_ScriptHost* host, wpe__Tween* tween); /* With the tweens, below. */

/* ECS remove hook: runs on_destroy in restricted mode and releases the instance. */
static void wpe__script_on_remove(WPE_Ecs* ecs, WPE_Entity entity, void* component, void* user_data) {
    (void)ecs;
    WPE_ScriptHost* host = user_data;
    const WPE_Script s =
        *(const WPE_Script*)component; /* Copy: the call may not touch this slot, but be safe. */
    if ((s.state & WPE__SCRIPT_STARTED) && !(s.state & WPE__SCRIPT_FAILED)) {
        ++host->restricted_depth;
        wpe__call_method(host, entity, s.instance_ref, s.script, "on_destroy", false, 0.0);
        --host->restricted_depth;
    }
    /* Its timers go with it. */
    for (uint32_t i = 0; i < host->timer_count; ++i) {
        wpe__Timer* timer = &host->timers[i];
        if (timer->owner_instance != s.instance_ref || timer->fn_ref == LUA_NOREF) continue;
        lua_unref(host->L, timer->fn_ref);
        timer->fn_ref = LUA_NOREF;
    }
    /* So do its tweens. */
    for (uint32_t i = 0; i < host->tween_count; ++i) {
        if (host->tweens[i].live && host->tweens[i].owner_instance == s.instance_ref)
            wpe__kill_tween(host, &host->tweens[i]);
    }
    lua_unref(host->L, s.instance_ref);
    if (s.property_ref != LUA_NOREF) lua_unref(host->L, s.property_ref);
    if (s.script < host->file_count && host->files[s.script].instances > 0) --host->files[s.script].instances;
}

/* Copies the current script entities so callbacks may change the world freely. */
static uint32_t wpe__snapshot(WPE_ScriptHost* host) {
    WPE_EcsPoolView view;
    if (wpe_ecs_pool_view(host->ecs, WPE_COMP_SCRIPT, &view) != WPE_RESULT_OK || view.count == 0) return 0;
    if (view.count > host->snapshot_capacity) {
        WPE_Entity* snapshot = wpe_realloc(host->snapshot, (size_t)view.count * sizeof *snapshot);
        if (!snapshot) return 0;
        host->snapshot = snapshot;
        host->snapshot_capacity = view.count;
    }
    memcpy(host->snapshot, view.entities, (size_t)view.count * sizeof *host->snapshot);
    return view.count;
}

WPE_Result wpe__script_queue_destroy(WPE_ScriptHost* host, WPE_Entity entity) {
    if (host->pending_count == host->pending_capacity) {
        const uint32_t capacity = host->pending_capacity ? host->pending_capacity * 2 : 32;
        WPE_Entity* pending = wpe_realloc(host->pending_destroy, (size_t)capacity * sizeof *pending);
        if (!pending) return WPE_RESULT_ERROR_OUT_OF_MEMORY;
        host->pending_destroy = pending;
        host->pending_capacity = capacity;
    }
    host->pending_destroy[host->pending_count++] = entity;
    return WPE_RESULT_OK;
}

static void wpe__flush_destroys(WPE_ScriptHost* host) {
    /* on_destroy handlers may queue more; the count is re-read every iteration. */
    for (uint32_t i = 0; i < host->pending_count; ++i) {
        const WPE_Entity entity = host->pending_destroy[i];
        if (wpe_ecs_is_alive(host->ecs, entity)) wpe_scene_destroy_recursive(host->ecs, entity);
    }
    host->pending_count = 0;
}

/* ---- Timers --------------------------------------------------------------------------------------- */

uint32_t wpe__script_add_timer(WPE_ScriptHost* host, double delay, double interval, int fn_ref) {
    if (host->timer_count == host->timer_capacity) {
        const uint32_t capacity = host->timer_capacity ? host->timer_capacity * 2 : 16;
        wpe__Timer* timers = wpe_realloc(host->timers, (size_t)capacity * sizeof *timers);
        if (!timers) return 0;
        host->timers = timers;
        host->timer_capacity = capacity;
    }
    const uint32_t id = host->next_timer_id++;
    if (host->next_timer_id == 0) host->next_timer_id = 1; /* 0 means "none". */
    host->timers[host->timer_count++] = (wpe__Timer){.id = id,
                                                     .owner_instance = host->current_instance,
                                                     .owner = host->current_entity,
                                                     .due = host->time + delay,
                                                     .interval = interval,
                                                     .fn_ref = fn_ref};
    return id;
}

bool wpe__script_cancel_timer(WPE_ScriptHost* host, uint32_t id) {
    for (uint32_t i = 0; i < host->timer_count; ++i) {
        wpe__Timer* timer = &host->timers[i];
        if (timer->id != id || timer->fn_ref == LUA_NOREF) continue;
        lua_unref(host->L, timer->fn_ref);
        timer->fn_ref = LUA_NOREF; /* Removed by the next wpe__fire_timers. */
        return true;
    }
    return false;
}

typedef struct wpe__TimerOp {
    WPE_ScriptHost* host;
    int fn_ref;
    bool failed;                          /* out */
    char message[WPE__ERROR_MESSAGE_MAX]; /* out */
} wpe__TimerOp;

/* Protected: calls a timer's callback. */
static int wpe__op_call_timer(lua_State* L) {
    wpe__TimerOp* op = lua_touserdata(L, 1);
    lua_pushcfunction(L, wpe__traceback, "traceback");
    const int handler = lua_gettop(L);
    lua_getref(L, op->fn_ref);
    const double previous = wpe__begin_budget(op->host);
    const int status = lua_pcall(L, 0, 0, handler);
    op->host->deadline = previous;
    if (status != 0) {
        op->failed = true;
        snprintf(op->message, sizeof op->message, "%s",
                 lua_tostring(L, -1) ? lua_tostring(L, -1) : "unknown error");
    }
    return 0;
}

/* Fires the timers due by the current game time (each at most once per update), then drops the
 * finished and cancelled ones. Timers of a disabled (failed) script wait for its reload. */
static void wpe__fire_timers(WPE_ScriptHost* host) {
    const uint32_t count = host->timer_count; /* Timers made by callbacks wait for the next update. */
    for (uint32_t i = 0; i < count; ++i) {
        const wpe__Timer timer = host->timers[i];
        if (timer.fn_ref == LUA_NOREF || timer.due > host->time) continue;
        if (timer.owner_instance != LUA_NOREF) {
            const WPE_Script* s = wpe_ecs_get_component(host->ecs, timer.owner, WPE_COMP_SCRIPT);
            if (s && s->instance_ref == timer.owner_instance && (s->state & WPE__SCRIPT_FAILED)) continue;
        }
        if (timer.interval > 0.0) {
            /* After a long frame it fires once and keeps its rhythm from now, rather than catching up. */
            const double next = timer.due + timer.interval;
            host->timers[i].due = next > host->time ? next : host->time + timer.interval;
        } else {
            host->timers[i].fn_ref = LUA_NOREF; /* Done once it has run; the callback may not cancel it. */
        }
        wpe__TimerOp op = {.host = host, .fn_ref = timer.fn_ref};
        const int previous_instance = host->current_instance;
        const WPE_Entity previous_entity = host->current_entity;
        host->current_instance = timer.owner_instance;
        host->current_entity = timer.owner;
        if (lua_cpcall(host->L, wpe__op_call_timer, &op) != 0) {
            op.failed = true;
            snprintf(op.message, sizeof op.message, "%s",
                     lua_tostring(host->L, -1) ? lua_tostring(host->L, -1) : "VM error");
            lua_pop(host->L, 1);
        }
        host->current_instance = previous_instance;
        host->current_entity = previous_entity;
        if (timer.interval <= 0.0) lua_unref(host->L, timer.fn_ref);
        if (op.failed) {
            ++host->errors;
            wpe_log_error("timer %u (entity 0x%08x) failed and was cancelled:\n%s", timer.id, timer.owner,
                          op.message);
            if (timer.interval > 0.0) wpe__script_cancel_timer(host, timer.id);
        }
    }
    uint32_t kept = 0;
    for (uint32_t i = 0; i < host->timer_count; ++i)
        if (host->timers[i].fn_ref != LUA_NOREF) host->timers[kept++] = host->timers[i];
    host->timer_count = kept;
}

/* ---- Tweens --------------------------------------------------------------------------------------- */

/* Ends a tween now: it is dropped at the end of the update and its callbacks are released. */
static void wpe__kill_tween(WPE_ScriptHost* host, wpe__Tween* tween) {
    tween->live = false;
    if (tween->fn_ref != LUA_NOREF) lua_unref(host->L, tween->fn_ref);
    if (tween->done_ref != LUA_NOREF) lua_unref(host->L, tween->done_ref);
    tween->fn_ref = tween->done_ref = LUA_NOREF;
}

uint32_t wpe__script_add_tween(WPE_ScriptHost* host, const wpe__Tween* tween) {
    if (host->tween_count == host->tween_capacity) {
        const uint32_t capacity = host->tween_capacity ? host->tween_capacity * 2 : 16;
        wpe__Tween* tweens = wpe_realloc(host->tweens, (size_t)capacity * sizeof *tweens);
        if (!tweens) return 0;
        host->tweens = tweens;
        host->tween_capacity = capacity;
    }
    /* The newest tween of a property wins: older ones of the entity let go of it. */
    if (tween->target != WPE_ENTITY_NULL) {
        for (uint32_t i = 0; i < host->tween_count; ++i) {
            wpe__Tween* old = &host->tweens[i];
            if (!old->live || old->target != tween->target || !(old->props & tween->props)) continue;
            old->props &= ~tween->props;
            if (old->props == 0) wpe__kill_tween(host, old);
        }
    }
    const uint32_t id = host->next_tween_id++;
    if (host->next_tween_id == 0) host->next_tween_id = 1;
    wpe__Tween* added = &host->tweens[host->tween_count++];
    *added = *tween;
    added->id = id;
    added->owner_instance = host->current_instance;
    added->owner = host->current_entity;
    added->live = true;
    return id;
}

bool wpe__script_cancel_tween(WPE_ScriptHost* host, uint32_t id) {
    for (uint32_t i = 0; i < host->tween_count; ++i) {
        if (host->tweens[i].id != id || !host->tweens[i].live) continue;
        wpe__kill_tween(host, &host->tweens[i]);
        return true;
    }
    return false;
}

uint32_t wpe__script_stop_tweens(WPE_ScriptHost* host, WPE_Entity entity) {
    uint32_t stopped = 0;
    for (uint32_t i = 0; i < host->tween_count; ++i) {
        if (!host->tweens[i].live || host->tweens[i].target != entity) continue;
        wpe__kill_tween(host, &host->tweens[i]);
        ++stopped;
    }
    return stopped;
}

bool wpe__script_is_tweening(const WPE_ScriptHost* host, WPE_Entity entity) {
    for (uint32_t i = 0; i < host->tween_count; ++i) {
        if (host->tweens[i].live && host->tweens[i].target == entity) return true;
    }
    return false;
}

/* The rotation `p` of the way from `from` to `to` along the shorter arc (p may leave [0, 1] for overshooting
 * eases). */
static WPE_Quat wpe__turn(WPE_Quat from, WPE_Quat to, float p) {
    const WPE_Quat inverse = {.x = -from.x, .y = -from.y, .z = -from.z, .w = from.w};
    WPE_Quat delta = wpe_quat_normalize(wpe_quat_mul(inverse, to));
    if (delta.w < 0.0f) delta = (WPE_Quat){.x = -delta.x, .y = -delta.y, .z = -delta.z, .w = -delta.w};
    const float half = acosf(delta.w > 1.0f ? 1.0f : delta.w);
    const float s = sinf(half);
    if (s < 1e-6f) return to;
    const WPE_Vec3 axis = wpe_vec3(delta.x / s, delta.y / s, delta.z / s);
    return wpe_quat_normalize(wpe_quat_mul(from, wpe_quat_from_axis_angle(axis, 2.0f * half * p)));
}

typedef struct wpe__TweenCallOp {
    WPE_ScriptHost* host;
    int fn_ref;
    bool with_value;
    double value;
    bool failed;                          /* out */
    char message[WPE__ERROR_MESSAGE_MAX]; /* out */
} wpe__TweenCallOp;

/* Protected: calls a tween callback, with the value for value tweens. */
static int wpe__op_call_tween(lua_State* L) {
    wpe__TweenCallOp* op = lua_touserdata(L, 1);
    lua_pushcfunction(L, wpe__traceback, "traceback");
    const int handler = lua_gettop(L);
    lua_getref(L, op->fn_ref);
    if (op->with_value) lua_pushnumber(L, op->value);
    const double previous = wpe__begin_budget(op->host);
    const int status = lua_pcall(L, op->with_value ? 1 : 0, 0, handler);
    op->host->deadline = previous;
    if (status != 0) {
        op->failed = true;
        snprintf(op->message, sizeof op->message, "%s",
                 lua_tostring(L, -1) ? lua_tostring(L, -1) : "unknown error");
    }
    return 0;
}

/* Runs a tween's callback as its owner; false (logged) when it failed. */
static bool wpe__call_tween(WPE_ScriptHost* host, const wpe__Tween* tween, int fn_ref, bool with_value,
                            double value) {
    wpe__TweenCallOp op = {.host = host, .fn_ref = fn_ref, .with_value = with_value, .value = value};
    const int previous_instance = host->current_instance;
    const WPE_Entity previous_entity = host->current_entity;
    host->current_instance = tween->owner_instance;
    host->current_entity = tween->owner;
    if (lua_cpcall(host->L, wpe__op_call_tween, &op) != 0) {
        op.failed = true;
        snprintf(op.message, sizeof op.message, "%s",
                 lua_tostring(host->L, -1) ? lua_tostring(host->L, -1) : "VM error");
        lua_pop(host->L, 1);
    }
    host->current_instance = previous_instance;
    host->current_entity = previous_entity;
    if (op.failed) {
        ++host->errors;
        wpe_log_error("tween %u (entity 0x%08x) failed and was cancelled:\n%s", tween->id, tween->owner,
                      op.message);
    }
    return !op.failed;
}

/* Advances one tween's clock by dt; returns false while it waits for its delay. Sets *done at its end. */
static bool wpe__tween_clock(wpe__Tween* tween, float dt, bool* done) {
    *done = false;
    if (tween->delay > 0.0f) {
        tween->delay -= dt;
        if (tween->delay > 0.0f) return false;
        dt = -tween->delay; /* What is left of the frame. */
        tween->delay = 0.0f;
    }
    if (!(tween->duration > 0.0f)) {
        *done = true;
        return true;
    }
    tween->elapsed += dt;
    while (tween->elapsed >= tween->duration) {
        if (tween->loop == WPE__TWEEN_ONCE || tween->times == 1) {
            tween->elapsed = tween->duration;
            *done = true;
            break;
        }
        if (tween->times > 1) --tween->times;
        tween->elapsed -= tween->duration;
        if (tween->loop == WPE__TWEEN_YOYO) tween->reversed = !tween->reversed;
    }
    return true;
}

/* Advances the tweens (each on game or real time) and applies them, then drops the finished ones. Tweens of a
 * disabled (failed) script wait for its reload; tweens made by callbacks start in the next update. */
static void wpe__run_tweens(WPE_ScriptHost* host, float game_dt) {
    float real_dt = game_dt;
    if (host->app.game_feel) real_dt = host->app.game_feel(host->app.user_data)->real_dt;
    const uint32_t count = host->tween_count;
    for (uint32_t i = 0; i < count; ++i) {
        wpe__Tween tween = host->tweens[i];
        if (!tween.live) continue;
        if (tween.owner_instance != LUA_NOREF) {
            const WPE_Script* s = wpe_ecs_get_component(host->ecs, tween.owner, WPE_COMP_SCRIPT);
            if (s && s->instance_ref == tween.owner_instance && (s->state & WPE__SCRIPT_FAILED)) continue;
        }
        WPE_Transform* t = NULL;
        if (tween.target != WPE_ENTITY_NULL) {
            t = wpe_ecs_get_component(host->ecs, tween.target, WPE_COMP_TRANSFORM);
            if (!t) { /* Its entity (or transform) is gone. */
                wpe__kill_tween(host, &host->tweens[i]);
                continue;
            }
        }
        bool done = false;
        if (!wpe__tween_clock(&tween, tween.real_time ? real_dt : game_dt, &done)) {
            host->tweens[i] = tween;
            continue;
        }
        if (!tween.started) {
            tween.started = true;
            if (t) {
                tween.from_position = t->position;
                tween.from_rotation = t->rotation;
                tween.from_scale = t->scale;
                if (tween.relative) {
                    tween.to_position = wpe_vec3_add(t->position, tween.to_position);
                    tween.to_rotation = wpe_quat_normalize(wpe_quat_mul(t->rotation, tween.to_rotation));
                    tween.to_scale = wpe_vec3_add(t->scale, tween.to_scale);
                }
            } else if (tween.relative) {
                tween.to_value += tween.from_value;
            }
        }
        float progress = tween.duration > 0.0f ? tween.elapsed / tween.duration : 1.0f;
        if (tween.reversed) progress = 1.0f - progress;
        const float p = wpe_ease(tween.ease, progress);
        if (t) {
            if (tween.props & WPE__TWEEN_POSITION)
                t->position = wpe_vec3_lerp(tween.from_position, tween.to_position, p);
            if (tween.props & WPE__TWEEN_ROTATION)
                t->rotation = wpe__turn(tween.from_rotation, tween.to_rotation, p);
            if (tween.props & WPE__TWEEN_SCALE) t->scale = wpe_vec3_lerp(tween.from_scale, tween.to_scale, p);
        }
        /* Its state is stored before any callback runs: callbacks may cancel it or start others. */
        if (done) tween.live = false;
        host->tweens[i] = tween;
        if ((tween.props & WPE__TWEEN_VALUE) && tween.fn_ref != LUA_NOREF) {
            const double value = tween.from_value + (tween.to_value - tween.from_value) * (double)p;
            if (!wpe__call_tween(host, &tween, tween.fn_ref, true, value)) {
                wpe__kill_tween(host, &host->tweens[i]);
                continue;
            }
        }
        if (done) {
            /* Finished: on_done runs, then the references are released. */
            if (tween.done_ref != LUA_NOREF) (void)wpe__call_tween(host, &tween, tween.done_ref, false, 0.0);
            wpe__kill_tween(host, &host->tweens[i]);
        }
    }
    uint32_t kept = 0;
    for (uint32_t i = 0; i < host->tween_count; ++i)
        if (host->tweens[i].live) host->tweens[kept++] = host->tweens[i];
    host->tween_count = kept;
}

/* ---- Modules (require) ---------------------------------------------------------------------------- */

typedef struct wpe__ModuleOp {
    WPE_ScriptHost* host;
    const char* chunk_name;
    const char* bytecode;
    size_t bytecode_size;
    int value_ref;                        /* out */
    int thread_ref;                       /* out */
    bool failed;                          /* out */
    char message[WPE__ERROR_MESSAGE_MAX]; /* out */
} wpe__ModuleOp;

/* Protected: runs a module's chunk in a fresh sandbox thread and keeps what it returns. */
static int wpe__op_load_module(lua_State* L) {
    wpe__ModuleOp* op = lua_touserdata(L, 1);
    lua_State* T = lua_newthread(L);
    luaL_sandboxthread(T);
    if (luau_load(T, op->chunk_name, op->bytecode, op->bytecode_size, 0) != 0) {
        op->failed = true;
        snprintf(op->message, sizeof op->message, "%s",
                 lua_tostring(T, -1) ? lua_tostring(T, -1) : "invalid bytecode");
        return 0;
    }
    lua_pushcfunction(T, wpe__traceback, "traceback");
    lua_insert(T, -2);
    if (lua_pcall(T, 0, 1, -2) != 0) {
        op->failed = true;
        snprintf(op->message, sizeof op->message, "%s",
                 lua_tostring(T, -1) ? lua_tostring(T, -1) : "unknown error");
        return 0;
    }
    if (lua_isnil(T, -1)) {
        lua_pop(T, 1);
        lua_pushboolean(T, 1); /* A module returning nothing is still loaded. */
    }
    lua_xmove(T, L, 1); /* L: [op, T, value] */
    op->value_ref = lua_ref(L, -1);
    op->thread_ref = lua_ref(L, -2);
    return 0;
}

int wpe__script_require(lua_State* L) {
    WPE_ScriptHost* host = wpe__script_host_from(L);
    const char* name = luaL_checkstring(L, 1);
    char path[WPE__SCRIPT_PATH_MAX];
    const size_t length = strlen(name);
    const bool extension = length >= 5 && strcmp(name + length - 5, ".luau") == 0;
    if (length == 0 || length + (extension ? 0 : 5) >= sizeof path)
        luaL_errorL(L, "require: '%s' is not a module path", name);
    snprintf(path, sizeof path, extension ? "%s" : "%s.luau", name);
    if (!wpe_vfs_path_is_valid(path)) luaL_errorL(L, "require: '%s' is not a safe asset-relative path", path);
    /* Listed modules are loaded (returned at once), loading (a circular require) or stale: dropped by a
     * reload, or failed. Stale ones stay listed so hot reload watches them, and load again here. */
    uint32_t index = host->module_count;
    for (uint32_t i = 0; i < host->module_count && index == host->module_count; ++i)
        if (strcmp(host->modules[i].path, path) == 0) index = i;
    if (index < host->module_count) {
        if (host->modules[index].loading)
            luaL_errorL(L, "require: '%s' requires itself (a circular require)", path);
        if (host->modules[index].value_ref != LUA_NOREF) {
            lua_getref(L, host->modules[index].value_ref);
            return 1;
        }
    } else {
        if (host->module_count == host->module_capacity) {
            const uint32_t capacity = host->module_capacity ? host->module_capacity * 2 : 8;
            wpe__Module* modules = wpe_realloc(host->modules, (size_t)capacity * sizeof *modules);
            if (!modules) luaL_errorL(L, "require: out of memory");
            host->modules = modules;
            host->module_capacity = capacity;
        }
        host->modules[index] = (wpe__Module){.value_ref = LUA_NOREF, .thread_ref = LUA_NOREF};
        snprintf(host->modules[index].path, sizeof host->modules[index].path, "%s", path);
        ++host->module_count;
    }
    char* bytecode = NULL; /* malloc'd by luau_compile: freed before anything below can raise. */
    size_t size = 0;
    int64_t modified = 0;
    const WPE_Result compiled = wpe__compile_file(host, path, &bytecode, &size, &modified);
    if (modified == 0) wpe_vfs_modified_time(wpe_asset_manager_vfs(host->assets), path, &modified);
    host->modules[index].modified_time = modified; /* Watched from now on, even when broken. */
    if (compiled != WPE_RESULT_OK) luaL_errorL(L, "require: cannot load '%s' (see the log)", path);

    /* Marked while its chunk runs, so a circular require is caught (nested ones may grow the list). */
    host->modules[index].loading = true;
    char chunk_name[WPE__SCRIPT_PATH_MAX + 1];
    snprintf(chunk_name, sizeof chunk_name, "@%s", path);
    wpe__ModuleOp op = {.host = host, .chunk_name = chunk_name, .bytecode = bytecode, .bytecode_size = size};
    if (lua_cpcall(L, wpe__op_load_module, &op) != 0) {
        op.failed = true;
        snprintf(op.message, sizeof op.message, "%s", lua_tostring(L, -1) ? lua_tostring(L, -1) : "VM error");
        lua_pop(L, 1);
    }
    free(bytecode);
    host->modules[index].loading = false;
    if (op.failed) {
        ++host->errors;
        luaL_errorL(L, "require: '%s' failed:\n%s", path, op.message);
    }
    host->modules[index].value_ref = op.value_ref;
    host->modules[index].thread_ref = op.thread_ref;
    lua_getref(L, op.value_ref);
    return 1;
}

/* Makes every module stale, so the next require of each loads it again; they stay watched, with
 * their current modification times. */
static void wpe__drop_modules(WPE_ScriptHost* host) {
    const WPE_Vfs* vfs = wpe_asset_manager_vfs(host->assets);
    for (uint32_t i = 0; i < host->module_count; ++i) {
        wpe__Module* module = &host->modules[i];
        if (module->value_ref != LUA_NOREF) lua_unref(host->L, module->value_ref);
        if (module->thread_ref != LUA_NOREF) lua_unref(host->L, module->thread_ref);
        module->value_ref = module->thread_ref = LUA_NOREF;
        int64_t modified = 0;
        if (wpe_vfs_modified_time(vfs, module->path, &modified) == WPE_RESULT_OK)
            module->modified_time = modified;
    }
}

/* ---- Messages ------------------------------------------------------------------------------------- */

int wpe__script_broadcast(lua_State* L) {
    WPE_ScriptHost* host = wpe__script_host_from(L);
    const char* method = luaL_checkstring(L, 1);
    const int arguments = lua_gettop(L) - 1;
    /* The receivers as they are now (a userdata copy: handlers may change the world, and the snapshot
     * belongs to the running update). */
    WPE_EcsPoolView view;
    if (wpe_ecs_pool_view(host->ecs, WPE_COMP_SCRIPT, &view) != WPE_RESULT_OK || view.count == 0) {
        lua_pushinteger(L, 0);
        return 1;
    }
    const uint32_t count = view.count;
    WPE_Entity* receivers = lua_newuserdata(L, (size_t)count * sizeof *receivers);
    memcpy(receivers, view.entities, (size_t)count * sizeof *receivers);
    lua_pushcfunction(L, wpe__traceback, "traceback");
    const int handler = lua_gettop(L);
    int called = 0;
    for (uint32_t i = 0; i < count; ++i) {
        const WPE_Entity entity = receivers[i];
        const WPE_Script* s = wpe_ecs_get_component(host->ecs, entity, WPE_COMP_SCRIPT);
        if (!s || !(s->state & WPE__SCRIPT_STARTED) || (s->state & WPE__SCRIPT_FAILED)) continue;
        const int instance_ref = s->instance_ref;
        const uint32_t script = s->script;
        lua_getref(L, instance_ref);
        lua_getfield(L, -1, method);
        if (!lua_isfunction(L, -1)) {
            lua_pop(L, 2);
            continue;
        }
        lua_insert(L, -2); /* [fn, instance] */
        for (int a = 0; a < arguments; ++a)
            lua_pushvalue(L, 2 + a);
        const int previous_instance = host->current_instance;
        const WPE_Entity previous_entity = host->current_entity;
        host->current_instance = instance_ref;
        host->current_entity = entity;
        const int status = lua_pcall(L, arguments + 1, 0, handler);
        host->current_instance = previous_instance;
        host->current_entity = previous_entity;
        if (status != 0) {
            /* A receiver's error disables that receiver, not the sender. */
            wpe__instance_failed(host, entity, instance_ref, script, method,
                                 lua_tostring(L, -1) ? lua_tostring(L, -1) : "unknown error");
            lua_pop(L, 1);
            continue;
        }
        ++called;
    }
    lua_pushinteger(L, called);
    return 1;
}

/* ---- Public API ----------------------------------------------------------------------------------- */

static int wpe__op_init(lua_State* L) {
    luaL_openlibs(L);
    for (size_t i = 0; i < sizeof wpe__unsafe_globals / sizeof wpe__unsafe_globals[0]; ++i) {
        lua_pushnil(L);
        lua_setglobal(L, wpe__unsafe_globals[i]);
    }
    wpe__script_api_register(L);
    luaL_sandbox(L); /* Libraries and globals become read-only. */
    return 0;
}

WPE_Result wpe_script_host_create(const WPE_ScriptHostDesc* desc, WPE_ScriptHost** out_host) {
    if (!desc || !desc->ecs || !desc->assets || !out_host) return WPE_RESULT_ERROR_NULL_ARG;
    *out_host = NULL;
    if (wpe_ecs_is_registered(desc->ecs, WPE_COMP_SCRIPT)) {
        wpe_log_error("the world already has a script host");
        return WPE_RESULT_ERROR_ALREADY_EXISTS;
    }

    WPE_ScriptHost* host = wpe_alloc_zeroed(1, sizeof *host);
    if (!host) return WPE_RESULT_ERROR_OUT_OF_MEMORY;
    host->ecs = desc->ecs;
    host->assets = desc->assets;
    host->physics = desc->physics;
    host->audio = desc->audio;
    host->input = desc->input;
    host->draw2d = desc->draw2d;
    host->ui = desc->ui;
    host->current_instance = LUA_NOREF;
    host->next_timer_id = 1;
    host->next_tween_id = 1;
    if (desc->app) host->app = *desc->app;
    if (desc->font != WPE_ASSET_NONE) {
        if (!wpe_asset_font(desc->assets, desc->font)) {
            wpe_log_error("the script host's font handle is not a font");
            wpe_free(host);
            return WPE_RESULT_ERROR_INVALID_ARG;
        }
        wpe_asset_retain(desc->assets, desc->font);
        host->font = desc->font;
    }
    host->memory_limit = desc->memory_limit ? desc->memory_limit : WPE__DEFAULT_MEMORY_LIMIT;
    host->time_limit_seconds =
        (desc->time_limit_ms > 0.0 ? desc->time_limit_ms : WPE__DEFAULT_TIME_LIMIT_MS) / 1000.0;

    host->L = lua_newstate(wpe__lua_alloc, host);
    if (!host->L) {
        wpe_free(host);
        return WPE_RESULT_ERROR_OUT_OF_MEMORY;
    }
    lua_callbacks(host->L)->userdata = host;
    lua_callbacks(host->L)->interrupt = wpe__interrupt;
    if (lua_cpcall(host->L, wpe__op_init, NULL) != 0) {
        wpe_log_error("failed to initialize the Luau VM: %s", lua_tostring(host->L, -1));
        wpe_script_host_destroy(host);
        return WPE_RESULT_ERROR;
    }

    const WPE_Result result =
        wpe_ecs_register_component(host->ecs, WPE_COMP_SCRIPT,
                                   &(WPE_ComponentDesc){.name = "Script",
                                                        .size = sizeof(WPE_Script),
                                                        .alignment = alignof(WPE_Script),
                                                        .on_remove = wpe__script_on_remove,
                                                        .hook_user_data = host});
    if (result != WPE_RESULT_OK) {
        lua_close(host->L);
        host->L = NULL;
        wpe_script_host_destroy(host);
        return result;
    }
    *out_host = host;
    return WPE_RESULT_OK;
}

void wpe_script_host_destroy(WPE_ScriptHost* host) {
    if (!host) return;
    host->shutting_down = true;
    if (host->L && wpe_ecs_is_registered(host->ecs, WPE_COMP_SCRIPT)) {
        const uint32_t count = wpe__snapshot(host);
        for (uint32_t i = 0; i < count; ++i)
            wpe_ecs_remove_component(host->ecs, host->snapshot[i], WPE_COMP_SCRIPT);
        wpe__flush_destroys(host);
        /* Destroys may have removed further scripts; anything left is removed without callbacks' world
         * access. */
        const uint32_t remaining = wpe__snapshot(host);
        for (uint32_t i = 0; i < remaining; ++i)
            wpe_ecs_remove_component(host->ecs, host->snapshot[i], WPE_COMP_SCRIPT);
        wpe_ecs_unregister_component(host->ecs, WPE_COMP_SCRIPT);
    }
    if (host->L) lua_close(host->L);
    wpe_assert(host->memory_used == 0);
    for (uint32_t i = 0; i < host->prefab_count; ++i)
        wpe_asset_release(host->assets, host->prefabs[i].model);
    wpe_free(host->prefabs);
    for (uint32_t i = 0; i < host->sound_count; ++i)
        wpe_asset_release(host->assets, host->sounds[i].sound);
    wpe_free(host->sounds);
    wpe_asset_release(host->assets, host->font);
    wpe_free(host->files);
    wpe_free(host->modules); /* Their references went with the VM. */
    wpe_free(host->timers);
    wpe_free(host->tweens); /* Their references went with the VM. */
    wpe_free(host->pending_destroy);
    wpe_free(host->snapshot);
    wpe_free(host);
}

WPE_Result wpe_script_attach(WPE_ScriptHost* host, WPE_Entity entity, const char* path) {
    if (!host || !path) return WPE_RESULT_ERROR_NULL_ARG;
    if (host->restricted_depth > 0 || host->shutting_down) {
        wpe_log_error("scripts cannot be attached from on_destroy");
        return WPE_RESULT_ERROR_INVALID_STATE;
    }
    if (!wpe_ecs_is_alive(host->ecs, entity)) return WPE_RESULT_ERROR_NOT_FOUND;
    if (!wpe_vfs_path_is_valid(path)) {
        wpe_log_error("script path '%s' is not a safe asset-relative path", path);
        return WPE_RESULT_ERROR_INVALID_ARG;
    }

    uint32_t index = 0;
    WPE_Result result = wpe__script_file(host, path, &index);
    if (result != WPE_RESULT_OK) return result;
    if (wpe_ecs_has_component(host->ecs, entity, WPE_COMP_SCRIPT)) {
        result = wpe_ecs_remove_component(host->ecs, entity, WPE_COMP_SCRIPT);
        if (result != WPE_RESULT_OK) return result;
        if (!wpe_ecs_is_alive(host->ecs, entity)) return WPE_RESULT_ERROR_NOT_FOUND;
    }

    wpe__InstanceOp op = {.meta_ref = host->files[index].meta_ref, .entity = entity};
    if (lua_cpcall(host->L, wpe__op_create_instance, &op) != 0) {
        wpe_log_error("failed to create script instance: %s", lua_tostring(host->L, -1));
        lua_pop(host->L, 1);
        return WPE_RESULT_ERROR_OUT_OF_MEMORY;
    }
    const WPE_Script component = {
        .script = index, .instance_ref = op.instance_ref, .property_ref = LUA_NOREF};
    result = wpe_ecs_add_component(host->ecs, entity, WPE_COMP_SCRIPT, &component, NULL);
    if (result != WPE_RESULT_OK) {
        lua_unref(host->L, op.instance_ref);
        return result;
    }
    ++host->files[index].instances;
    return WPE_RESULT_OK;
}

static WPE_Result wpe__run_callbacks(WPE_ScriptHost* host, float dt, bool fixed) {
    const uint32_t count = wpe__snapshot(host);
    for (uint32_t i = 0; i < count; ++i) {
        const WPE_Entity entity = host->snapshot[i];
        WPE_Script* s = wpe_ecs_get_component(host->ecs, entity, WPE_COMP_SCRIPT);
        if (!s || (s->state & WPE__SCRIPT_FAILED)) continue;
        if (!(s->state & WPE__SCRIPT_STARTED)) {
            if (fixed) continue; /* Fixed updates begin after on_start has run in a regular update. */
            s->state |= WPE__SCRIPT_STARTED;
            const int ref = s->instance_ref;
            const uint32_t script = s->script;
            if (!wpe__call_method(host, entity, ref, script, "on_start", false, 0.0)) continue;
            s = wpe_ecs_get_component(host->ecs, entity, WPE_COMP_SCRIPT);
            if (!s || s->instance_ref != ref || (s->state & WPE__SCRIPT_FAILED)) continue;
        }
        wpe__call_method(host, entity, s->instance_ref, s->script, fixed ? "on_fixed_update" : "on_update",
                         true, dt);
    }
    wpe__flush_destroys(host);
    return WPE_RESULT_OK;
}

WPE_Result wpe_script_host_update(WPE_ScriptHost* host, float dt) {
    if (!host) return WPE_RESULT_ERROR_NULL_ARG;
    const WPE_Result result = wpe__run_callbacks(host, dt, false);
    host->time += dt;
    ++host->frame;
    wpe__fire_timers(host);
    wpe__run_tweens(host, dt);
    wpe__flush_destroys(host);
    return result;
}

WPE_Result wpe_script_host_fixed_update(WPE_ScriptHost* host, float dt) {
    if (!host) return WPE_RESULT_ERROR_NULL_ARG;
    return wpe__run_callbacks(host, dt, true);
}

/* Calls instance:method(other) on a started, healthy script of `entity`, if any. */
static void wpe__notify(WPE_ScriptHost* host, WPE_Entity entity, const char* method, WPE_Entity other) {
    const WPE_Script* s = wpe_ecs_get_component(host->ecs, entity, WPE_COMP_SCRIPT);
    if (!s || !(s->state & WPE__SCRIPT_STARTED) || (s->state & WPE__SCRIPT_FAILED)) return;
    wpe__call_method(host, entity, s->instance_ref, s->script, method, true, (double)other);
}

WPE_Result wpe_script_host_dispatch_physics(WPE_ScriptHost* host) {
    if (!host) return WPE_RESULT_ERROR_NULL_ARG;
    if (!host->physics) return WPE_RESULT_OK;
    static const char* const methods[] = {
        [WPE_PHYSICS_CONTACT_BEGIN] = "on_collision_enter",
        [WPE_PHYSICS_CONTACT_END] = "on_collision_exit",
        [WPE_PHYSICS_TRIGGER_ENTER] = "on_trigger_enter",
        [WPE_PHYSICS_TRIGGER_EXIT] = "on_trigger_exit",
    };
    /* Callbacks cannot step physics, so the event array stays valid throughout. */
    uint32_t count = 0;
    const WPE_PhysicsEvent* events = wpe_physics_events(host->physics, &count);
    for (uint32_t i = 0; i < count; ++i) {
        const WPE_PhysicsEvent event = events[i];
        wpe__notify(host, event.entity_a, methods[event.type], event.entity_b);
        wpe__notify(host, event.entity_b, methods[event.type], event.entity_a);
    }
    wpe__flush_destroys(host);
    return WPE_RESULT_OK;
}

WPE_Result wpe_script_host_dispatch_animation(WPE_ScriptHost* host) {
    if (!host) return WPE_RESULT_ERROR_NULL_ARG;
    if (!wpe_ecs_is_registered(host->ecs, WPE_COMP_ANIM_STATES)) return WPE_RESULT_OK;
    /* Collected first (names copied): callbacks may change or remove state machines. */
    typedef struct wpe__AnimCall {
        WPE_Entity listener;
        bool event;
        char name[WPE_ANIM_NAME_MAX];
    } wpe__AnimCall;
    wpe__AnimCall calls[64];
    for (;;) {
        uint32_t count = 0;
        WPE_EcsIter it = wpe_ecs_query(host->ecs, (WPE_EcsQuery){.all = WPE_ECS_MASK(WPE_COMP_ANIM_STATES)});
        while (wpe_ecs_iter_next(&it) && count < 64) {
            WPE_AnimStates* st = wpe_ecs_iter_get(&it, WPE_COMP_ANIM_STATES);
            uint32_t taken = 0;
            for (; taken < st->queue_count && count < 64; ++taken) {
                const WPE_AnimNotice notice = st->queue[taken];
                const bool event = notice.kind == WPE_ANIM_NOTICE_EVENT;
                const char* name =
                    event ? st->graph->events[notice.index].name : st->graph->states[notice.index].name;
                calls[count] = (wpe__AnimCall){.listener = st->listener, .event = event};
                memcpy(calls[count].name, name, sizeof calls[count].name);
                ++count;
            }
            memmove(st->queue, st->queue + taken, (st->queue_count - taken) * sizeof *st->queue);
            st->queue_count -= taken;
        }
        if (count == 0) break;
        for (uint32_t i = 0; i < count; ++i) {
            const WPE_ScriptValue name = {.type = WPE_SCRIPT_VALUE_STRING, .string = calls[i].name};
            if (calls[i].listener != WPE_ENTITY_NULL)
                wpe_script_host_notify_value(host, calls[i].listener,
                                             calls[i].event ? "on_animation_event" : "on_animation_state",
                                             &name);
        }
    }
    return WPE_RESULT_OK;
}

WPE_Result wpe_script_host_notify(WPE_ScriptHost* host, WPE_Entity entity, const char* method) {
    return wpe_script_host_notify_value(host, entity, method, NULL);
}

WPE_Result wpe_script_host_notify_value(WPE_ScriptHost* host, WPE_Entity entity, const char* method,
                                        const WPE_ScriptValue* value) {
    if (!host || !method) return WPE_RESULT_ERROR_NULL_ARG;
    const WPE_Script* s = wpe_ecs_get_component(host->ecs, entity, WPE_COMP_SCRIPT);
    if (s && (s->state & WPE__SCRIPT_STARTED) && !(s->state & WPE__SCRIPT_FAILED)) {
        wpe__CallOp op = {.host = host, .instance_ref = s->instance_ref, .method = method, .value = value};
        wpe__invoke(host, entity, s->script, &op);
    }
    wpe__flush_destroys(host);
    return WPE_RESULT_OK;
}

static WPE_Result wpe__reload_file(WPE_ScriptHost* host, uint32_t index) {
    wpe__ScriptFile* file = &host->files[index];
    char* bytecode = NULL;
    size_t size = 0;
    int64_t modified = 0;
    WPE_Result result = wpe__compile_file(host, file->path, &bytecode, &size, &modified);
    if (result == WPE_RESULT_OK) {
        wpe__LoadOp op;
        result = wpe__load_bytecode(host, file->path, bytecode, size, file->meta_ref, &op);
        free(bytecode);
        if (result == WPE_RESULT_OK) {
            lua_unref(host->L, file->class_ref);
            lua_unref(host->L, file->thread_ref);
            file->class_ref = op.class_ref;
            file->thread_ref = op.thread_ref;
        }
    }
    /* Record the timestamp even on failure so a broken file is not recompiled every poll. */
    if (modified != 0) file->modified_time = modified;
    if (result != WPE_RESULT_OK) {
        wpe_log_warn("reload of '%s' failed; the previous version keeps running", file->path);
        return result;
    }

    wpe_log_info("reloaded '%s'", file->path);
    const uint32_t count = wpe__snapshot(host);
    for (uint32_t i = 0; i < count; ++i) {
        WPE_Script* s = wpe_ecs_get_component(host->ecs, host->snapshot[i], WPE_COMP_SCRIPT);
        if (!s || s->script != index) continue;
        s->state &= (uint8_t)~WPE__SCRIPT_FAILED; /* New code gets a fresh chance. */
        /* Properties the new code declares start at their defaults; existing values stay. */
        int instance_ref = s->instance_ref;
        if (lua_cpcall(host->L, wpe__op_fill_defaults, &instance_ref) != 0) {
            wpe_log_error("declared properties of '%s': %s", file->path, lua_tostring(host->L, -1));
            lua_pop(host->L, 1);
        }
        if (s->state & WPE__SCRIPT_STARTED) {
            wpe__call_method(host, host->snapshot[i], s->instance_ref, index, "on_reload", false, 0.0);
        }
    }
    wpe__flush_destroys(host);
    return WPE_RESULT_OK;
}

/* A module changed: scripts hold on to what they required, so every script file loads again (and
 * requires fresh modules). Returns how many reloaded. */
static uint32_t wpe__reload_all(WPE_ScriptHost* host) {
    wpe_log_info("a module changed; reloading every script");
    wpe__drop_modules(host);
    uint32_t reloaded = 0;
    for (uint32_t i = 0; i < host->file_count; ++i)
        if (wpe__reload_file(host, i) == WPE_RESULT_OK) ++reloaded;
    return reloaded;
}

uint32_t wpe_script_host_reload_changed(WPE_ScriptHost* host) {
    if (!host) return 0;
    const WPE_Vfs* vfs = wpe_asset_manager_vfs(host->assets);
    for (uint32_t i = 0; i < host->module_count; ++i) {
        int64_t modified = 0;
        if (wpe_vfs_modified_time(vfs, host->modules[i].path, &modified) == WPE_RESULT_OK &&
            modified != host->modules[i].modified_time)
            return wpe__reload_all(host);
    }
    uint32_t reloaded = 0;
    for (uint32_t i = 0; i < host->file_count; ++i) {
        int64_t modified = 0;
        if (wpe_vfs_modified_time(wpe_asset_manager_vfs(host->assets), host->files[i].path, &modified) !=
                WPE_RESULT_OK ||
            modified == host->files[i].modified_time) {
            continue;
        }
        if (wpe__reload_file(host, i) == WPE_RESULT_OK) ++reloaded;
    }
    return reloaded;
}

WPE_Result wpe_script_host_reload(WPE_ScriptHost* host, const char* path) {
    if (!host || !path) return WPE_RESULT_ERROR_NULL_ARG;
    for (uint32_t i = 0; i < host->file_count; ++i) {
        if (strcmp(host->files[i].path, path) == 0) return wpe__reload_file(host, i);
    }
    for (uint32_t i = 0; i < host->module_count; ++i) {
        if (strcmp(host->modules[i].path, path) != 0) continue;
        wpe__reload_all(host);
        return WPE_RESULT_OK;
    }
    return WPE_RESULT_ERROR_NOT_FOUND;
}

WPE_Result wpe_script_host_register_prefab(WPE_ScriptHost* host, const char* name, WPE_AssetHandle model) {
    if (!host || !name) return WPE_RESULT_ERROR_NULL_ARG;
    if (!name[0] || strlen(name) >= WPE__SCRIPT_PREFAB_NAME_MAX || !wpe_asset_model(host->assets, model))
        return WPE_RESULT_ERROR_INVALID_ARG;
    for (uint32_t i = 0; i < host->prefab_count; ++i) {
        if (strcmp(host->prefabs[i].name, name) == 0) {
            wpe_asset_retain(host->assets,
                             model); /* Before releasing: the same model may be re-registered. */
            wpe_asset_release(host->assets, host->prefabs[i].model);
            host->prefabs[i].model = model;
            return WPE_RESULT_OK;
        }
    }
    if (host->prefab_count == host->prefab_capacity) {
        const uint32_t capacity = host->prefab_capacity ? host->prefab_capacity * 2 : 16;
        wpe__Prefab* prefabs = wpe_realloc(host->prefabs, (size_t)capacity * sizeof *prefabs);
        if (!prefabs) return WPE_RESULT_ERROR_OUT_OF_MEMORY;
        host->prefabs = prefabs;
        host->prefab_capacity = capacity;
    }
    wpe_asset_retain(host->assets, model);
    wpe__Prefab* prefab = &host->prefabs[host->prefab_count++];
    *prefab = (wpe__Prefab){.model = model};
    snprintf(prefab->name, sizeof prefab->name, "%s", name);
    return WPE_RESULT_OK;
}

WPE_AssetHandle wpe__script_find_prefab(WPE_ScriptHost* host, const char* name_or_path) {
    for (uint32_t i = 0; i < host->prefab_count; ++i) {
        if (strcmp(host->prefabs[i].name, name_or_path) == 0) return host->prefabs[i].model;
    }
    if (!wpe_vfs_path_is_valid(name_or_path) || strlen(name_or_path) >= WPE__SCRIPT_PREFAB_NAME_MAX) {
        wpe_log_error("prefab '%s' is not registered and is not a valid asset path", name_or_path);
        return WPE_ASSET_NONE;
    }
    /* Path-based prefabs are cached under their path (the prefab table keeps them loaded). */
    WPE_AssetHandle model = WPE_ASSET_NONE;
    if (wpe_asset_load(host->assets, WPE_ASSET_MODEL, name_or_path, &model) != WPE_RESULT_OK)
        return WPE_ASSET_NONE;
    const WPE_Result result = wpe_script_host_register_prefab(host, name_or_path, model);
    wpe_asset_release(host->assets, model); /* The prefab table holds its own reference. */
    return result == WPE_RESULT_OK ? model : WPE_ASSET_NONE;
}

WPE_AssetHandle wpe__script_find_sound(WPE_ScriptHost* host, const char* path) {
    char normalized[WPE_VFS_PATH_MAX];
    if (!wpe_vfs_path_normalize(path, normalized, sizeof normalized)) {
        wpe_log_error("'%s' is not a valid asset path", path);
        return WPE_ASSET_NONE;
    }
    for (uint32_t i = 0; i < host->sound_count; ++i) {
        if (strcmp(host->sounds[i].path, normalized) == 0) return host->sounds[i].sound;
    }
    if (host->sound_count == host->sound_capacity) {
        const uint32_t capacity = host->sound_capacity ? host->sound_capacity * 2 : 16;
        wpe__CachedSound* sounds = wpe_realloc(host->sounds, (size_t)capacity * sizeof *sounds);
        if (!sounds) return WPE_ASSET_NONE;
        host->sounds = sounds;
        host->sound_capacity = capacity;
    }
    WPE_AssetHandle sound = WPE_ASSET_NONE;
    if (wpe_asset_load(host->assets, WPE_ASSET_SOUND, normalized, &sound) != WPE_RESULT_OK)
        return WPE_ASSET_NONE;
    wpe__CachedSound* entry = &host->sounds[host->sound_count++];
    snprintf(entry->path, sizeof entry->path, "%s", normalized);
    entry->sound = sound;
    return sound;
}

WPE_ScriptStats wpe_script_host_stats(const WPE_ScriptHost* host) {
    WPE_ScriptStats stats = {0};
    if (!host) return stats;
    stats.scripts = host->file_count;
    stats.errors = host->errors;
    stats.memory = host->memory_used;
    WPE_EcsPoolView view;
    if (wpe_ecs_pool_view(host->ecs, WPE_COMP_SCRIPT, &view) == WPE_RESULT_OK) {
        stats.instances = view.count;
        for (uint32_t i = 0; i < view.count; ++i) {
            const WPE_Script* s =
                (const WPE_Script*)((const unsigned char*)view.data + (size_t)i * view.stride);
            stats.failed += (s->state & WPE__SCRIPT_FAILED) != 0;
        }
    }
    return stats;
}

WPE_Result wpe_script_host_run(WPE_ScriptHost* host, const char* chunk_name, const char* source) {
    if (!host || !chunk_name || !source) return WPE_RESULT_ERROR_NULL_ARG;
    lua_CompileOptions options = {.optimizationLevel = 1, .debugLevel = 1};
    size_t size = 0;
    char* bytecode = luau_compile(source, strlen(source), &options, &size);
    if (!bytecode) return WPE_RESULT_ERROR_OUT_OF_MEMORY;
    if (size == 0 || bytecode[0] == 0) {
        wpe_log_error("compile error in '%s': %.*s", chunk_name, (int)(size > 1 ? size - 1 : 0),
                      bytecode + 1);
        free(bytecode);
        ++host->errors;
        return WPE_RESULT_ERROR_PARSE;
    }
    /* Ad-hoc chunks run in their own sandbox thread like script files but may return anything. */
    char chunk[WPE__SCRIPT_PATH_MAX + 1];
    snprintf(chunk, sizeof chunk, "=%s", chunk_name);
    wpe__LoadOp op = {.host = host,
                      .chunk_name = chunk,
                      .bytecode = bytecode,
                      .bytecode_size = size,
                      .existing_meta_ref = LUA_NOREF,
                      .require_table = false};
    const int status = lua_cpcall(host->L, wpe__op_load, &op);
    free(bytecode);
    if (status != 0) {
        wpe_log_error("error in '%s': %s", chunk_name,
                      lua_tostring(host->L, -1) ? lua_tostring(host->L, -1) : "unknown error");
        lua_pop(host->L, 1);
        ++host->errors;
        return WPE_RESULT_ERROR;
    }
    return WPE_RESULT_OK;
}

/* ---- Script properties ------------------------------------------------------------------------------ */

const char* wpe_script_path(const WPE_ScriptHost* host, WPE_Entity entity) {
    if (!host) return NULL;
    const WPE_Script* s = wpe_ecs_get_component(host->ecs, entity, WPE_COMP_SCRIPT);
    return s && s->script < host->file_count ? host->files[s->script].path : NULL;
}

static bool wpe__property_name_valid(const char* name) {
    const size_t length = strlen(name);
    if (length == 0 || length > 63 || strcmp(name, "entity") == 0) return false;
    for (size_t i = 0; i < length; ++i) {
        const char c = name[i];
        const bool letter = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_';
        if (!letter && !(i > 0 && c >= '0' && c <= '9')) return false;
    }
    return true;
}

typedef struct wpe__PropertyOp {
    int instance_ref;
    int property_ref; /* in/out: created on first use */
    const char* name;
    const WPE_ScriptValue* value;
} wpe__PropertyOp;

/* Protected: sets instance[name] (raw) and records the name in the property list, unless the value is the
 * script's declared default (then there is nothing to save). Removing (nil) a declared property sets it
 * back to its default. */
static int wpe__op_set_property(lua_State* L) {
    wpe__PropertyOp* op = lua_touserdata(L, 1);
    lua_getref(L, op->instance_ref);
    const int instance = lua_gettop(L);
    /* The declared default, or nil. */
    if (wpe__push_declared(L, instance)) {
        lua_getfield(L, -1, op->name);
        lua_remove(L, -2);
    } else {
        lua_pushnil(L);
    }
    const int declared_default = lua_gettop(L);
    const bool declared = wpe__property_value(L, declared_default);
    lua_pushstring(L, op->name);
    switch (op->value->type) {
    case WPE_SCRIPT_VALUE_NUMBER:
        lua_pushnumber(L, op->value->number);
        break;
    case WPE_SCRIPT_VALUE_BOOL:
        lua_pushboolean(L, op->value->boolean);
        break;
    case WPE_SCRIPT_VALUE_STRING:
        lua_pushstring(L, op->value->string);
        break;
    case WPE_SCRIPT_VALUE_VECTOR:
        lua_pushvector(L, op->value->vector.x, op->value->vector.y, op->value->vector.z);
        break;
    case WPE_SCRIPT_VALUE_NIL:
        if (declared) {
            lua_pushvalue(L, declared_default); /* Back to the declared default. */
        } else {
            lua_pushnil(L);
        }
        break;
    }
    const bool is_default = declared && lua_rawequal(L, -1, declared_default);
    lua_rawset(L, instance);
    const bool forget = op->value->type == WPE_SCRIPT_VALUE_NIL || is_default;

    if (op->property_ref == LUA_NOREF) {
        if (forget) return 0;
        lua_createtable(L, 4, 0);
        op->property_ref = lua_ref(L, -1);
    } else {
        lua_getref(L, op->property_ref);
    }
    const int list = lua_gettop(L);
    const int count = lua_objlen(L, list);
    int found = 0;
    for (int i = 1; i <= count && !found; ++i) {
        lua_rawgeti(L, list, i);
        if (strcmp(lua_tostring(L, -1), op->name) == 0) found = i;
        lua_pop(L, 1);
    }
    if (forget) {
        /* Remove the name, keeping the order of the others. */
        for (int i = found; found && i < count; ++i) {
            lua_rawgeti(L, list, i + 1);
            lua_rawseti(L, list, i);
        }
        if (found) {
            lua_pushnil(L);
            lua_rawseti(L, list, count);
        }
    } else if (!found) {
        lua_pushstring(L, op->name);
        lua_rawseti(L, list, count + 1);
    }
    return 0;
}

WPE_Result wpe_script_set_property(WPE_ScriptHost* host, WPE_Entity entity, const char* name,
                                   const WPE_ScriptValue* value) {
    if (!host || !name || !value) return WPE_RESULT_ERROR_NULL_ARG;
    WPE_Script* s = wpe_ecs_get_component(host->ecs, entity, WPE_COMP_SCRIPT);
    if (!s) return WPE_RESULT_ERROR_NOT_FOUND;
    if (!wpe__property_name_valid(name)) {
        wpe_log_error("'%s' is not a valid script property name", name);
        return WPE_RESULT_ERROR_INVALID_ARG;
    }
    const bool known_type = value->type == WPE_SCRIPT_VALUE_NIL || value->type == WPE_SCRIPT_VALUE_NUMBER ||
                            value->type == WPE_SCRIPT_VALUE_BOOL || value->type == WPE_SCRIPT_VALUE_VECTOR ||
                            (value->type == WPE_SCRIPT_VALUE_STRING && value->string);
    if (!known_type) {
        wpe_log_error("script property '%s' has an invalid value", name);
        return WPE_RESULT_ERROR_INVALID_ARG;
    }
    wpe__PropertyOp op = {
        .instance_ref = s->instance_ref, .property_ref = s->property_ref, .name = name, .value = value};
    if (lua_cpcall(host->L, wpe__op_set_property, &op) != 0) {
        lua_pop(host->L, 1);
        /* A list created before the failure is not referenced by the component; release it. */
        if (s->property_ref == LUA_NOREF && op.property_ref != LUA_NOREF) lua_unref(host->L, op.property_ref);
        return WPE_RESULT_ERROR_OUT_OF_MEMORY;
    }
    s->property_ref = op.property_ref; /* The ECS was not touched, so `s` is still valid. */
    return WPE_RESULT_OK;
}

typedef struct wpe__VisitOp {
    int instance_ref;
    int property_ref;
    WPE_ScriptPropertyFn fn;
    WPE_ScriptDeclaredFn declared_fn;
    void* user_data;
} wpe__VisitOp;

/* A Lua value as a script value (NIL for kinds a property can't hold). Strings are borrowed from the stack.
 */
static WPE_ScriptValue wpe__script_value(lua_State* L, int index) {
    switch (lua_type(L, index)) {
    case LUA_TNUMBER:
        return (WPE_ScriptValue){.type = WPE_SCRIPT_VALUE_NUMBER, .number = lua_tonumber(L, index)};
    case LUA_TBOOLEAN:
        return (WPE_ScriptValue){.type = WPE_SCRIPT_VALUE_BOOL, .boolean = lua_toboolean(L, index) != 0};
    case LUA_TSTRING:
        return (WPE_ScriptValue){.type = WPE_SCRIPT_VALUE_STRING, .string = lua_tostring(L, index)};
    case LUA_TVECTOR: {
        const float* v = lua_tovector(L, index);
        return (WPE_ScriptValue){.type = WPE_SCRIPT_VALUE_VECTOR, .vector = wpe_vec3(v[0], v[1], v[2])};
    }
    default:
        return (WPE_ScriptValue){.type = WPE_SCRIPT_VALUE_NIL};
    }
}

static int wpe__op_visit_properties(lua_State* L) {
    const wpe__VisitOp* op = lua_touserdata(L, 1);
    lua_getref(L, op->property_ref);
    const int list = lua_gettop(L);
    lua_getref(L, op->instance_ref);
    const int instance = lua_gettop(L);
    const int count = lua_objlen(L, list);
    for (int i = 1; i <= count; ++i) {
        lua_rawgeti(L, list, i);
        const char* name = lua_tostring(L, -1);
        lua_pushvalue(L, -1);
        lua_rawget(L, instance);
        const WPE_ScriptValue value = wpe__script_value(L, -1);
        if (value.type != WPE_SCRIPT_VALUE_NIL) op->fn(name, &value, op->user_data);
        lua_pop(L, 2);
    }
    return 0;
}

#define WPE__MAX_DECLARED 64u

static int wpe__compare_names(const void* a, const void* b) {
    return strcmp((const char*)a, (const char*)b);
}

static int wpe__op_visit_declared(lua_State* L) {
    const wpe__VisitOp* op = lua_touserdata(L, 1);
    lua_getref(L, op->instance_ref);
    const int instance = lua_gettop(L);
    if (!wpe__push_declared(L, instance)) return 0;
    const int declared = lua_gettop(L);
    /* Names sorted, so every listing shows them in the same order (table order is arbitrary). */
    char names[WPE__MAX_DECLARED][64];
    uint32_t count = 0;
    lua_pushnil(L);
    while (lua_next(L, declared)) {
        if (lua_type(L, -2) == LUA_TSTRING && wpe__property_value(L, -1) &&
            wpe__property_name_valid(lua_tostring(L, -2)) && count < WPE__MAX_DECLARED)
            snprintf(names[count++], sizeof names[0], "%s", lua_tostring(L, -2));
        lua_pop(L, 1);
    }
    qsort(names, count, sizeof names[0], wpe__compare_names);
    for (uint32_t i = 0; i < count; ++i) {
        lua_getfield(L, declared, names[i]);
        lua_pushstring(L, names[i]);
        lua_rawget(L, instance);
        const WPE_ScriptValue fallback = wpe__script_value(L, -2);
        WPE_ScriptValue value = wpe__script_value(L, -1);
        if (value.type == WPE_SCRIPT_VALUE_NIL)
            value = fallback; /* The script cleared it: show the default. */
        op->declared_fn(names[i], &value, &fallback, op->user_data);
        lua_pop(L, 2);
    }
    return 0;
}

WPE_Result wpe_script_visit_declared(const WPE_ScriptHost* host, WPE_Entity entity, WPE_ScriptDeclaredFn fn,
                                     void* user_data) {
    if (!host || !fn) return WPE_RESULT_ERROR_NULL_ARG;
    const WPE_Script* s = wpe_ecs_get_component(host->ecs, entity, WPE_COMP_SCRIPT);
    if (!s) return WPE_RESULT_ERROR_NOT_FOUND;
    wpe__VisitOp op = {.instance_ref = s->instance_ref, .declared_fn = fn, .user_data = user_data};
    if (lua_cpcall(host->L, wpe__op_visit_declared, &op) != 0) {
        wpe_log_error("reading declared script properties failed: %s", lua_tostring(host->L, -1));
        lua_pop(host->L, 1);
        return WPE_RESULT_ERROR_OUT_OF_MEMORY;
    }
    return WPE_RESULT_OK;
}

WPE_Result wpe_script_visit_properties(const WPE_ScriptHost* host, WPE_Entity entity, WPE_ScriptPropertyFn fn,
                                       void* user_data) {
    if (!host || !fn) return WPE_RESULT_ERROR_NULL_ARG;
    const WPE_Script* s = wpe_ecs_get_component(host->ecs, entity, WPE_COMP_SCRIPT);
    if (!s) return WPE_RESULT_ERROR_NOT_FOUND;
    if (s->property_ref == LUA_NOREF) return WPE_RESULT_OK;
    wpe__VisitOp op = {
        .instance_ref = s->instance_ref, .property_ref = s->property_ref, .fn = fn, .user_data = user_data};
    if (lua_cpcall(host->L, wpe__op_visit_properties, &op) != 0) {
        wpe_log_error("reading script properties failed: %s", lua_tostring(host->L, -1));
        lua_pop(host->L, 1);
        return WPE_RESULT_ERROR_OUT_OF_MEMORY;
    }
    return WPE_RESULT_OK;
}
