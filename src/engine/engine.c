#define WPE_LOG_MODULE "engine"
#include "engine/engine.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

#include "core/alloc.h"
#include "core/debug.h"
#include "core/fixed_step.h"
#include "core/log.h"
#include "engine/replication.h"
#include "graphics/anim_states.h"
#include "graphics/animation.h"
#include "graphics/debug_lines.h"
#include "graphics/draw2d.h"
#include "graphics/graphics.h"
#include "graphics/postfx.h"
#include "graphics/scene_render.h"
#include "graphics/transition.h"
#include "jobs/jobs.h"
#include "math/easing.h"
#include "platform/platform.h"
#include "scene/scene.h"

#define WPE__MAX_FRAME_SECONDS 0.25
#define WPE__GRID_HALF_LINES 50 /* Grid lines on each side of the center. */
#define WPE__GRID_PIECE 5.0f    /* Grid line pieces, in spacings (each fades on its own). */
#define WPE__AUDIO_CHUNK_FRAMES 1024u
#define WPE__CAPTURE_SLOTS 8u     /* wpe.capture_screen slots (and shatter's capture parameter). */
#define WPE__SLASH_DISTORTIONS 8u /* Slashes whose distortion a frame shows (the first ones found). */

/* A network event waiting for the scripts (see wpe__queue_script_event). */
typedef struct wpe__QueuedNetEvent {
    WPE_ScriptNetEventType type;
    WPE_PlayerId player;
    char text[32];
    const char* reason; /* Static. */
    uint8_t* value;     /* Owned. */
    size_t size;
    uint32_t generation; /* The session it belongs to. */
} wpe__QueuedNetEvent;

/* Bytes per second in and out, measured over the last whole second (the network overlay). */
typedef struct wpe__NetRates {
    double since;  /* When the current second began. */
    uint64_t sent; /* The transport's totals then. */
    uint64_t received;
    uint32_t generation; /* The session measured. */
    double up, down;     /* The last second's rates. */
} wpe__NetRates;

struct WPE_Engine {
    WPE_ProjectSettings project;
    WPE_JobSystem* jobs;
    WPE_Renderer* renderer;
    WPE_Window* window; /* NULL when headless. */
    WPE_SceneRenderer* scene_renderer;
    WPE_PostChain* postfx;
    WPE_Draw2D* draw2d;
    WPE_DebugLines* debug_lines;
    WPE_EngineDebugDraw debug_draw;
    WPE_Vfs* vfs;
    WPE_AssetManager* assets;
    WPE_AssetHandle font;
    WPE_Ecs* ecs;
    WPE_Physics* physics;
    WPE_PrefabCache* prefabs;
    WPE_Audio* audio;
    WPE_Input* input;
    WPE_Ui* ui;
    WPE_ScriptHost* scripts;

    WPE_SceneEnvironment environment;
    WPE_Entity camera;
    WPE_FixedStep clock;
    WPE_GameFeel feel;            /* Time scale (slow motion, hit-stop) and shakes. */
    char music[WPE_VFS_PATH_MAX]; /* Track started by the current scene ("" = none). */
    char pending_scene[WPE_VFS_PATH_MAX];
    /* A scene transition: closing toward `transition_scene`, then (after it loads) opening. Kind NONE: none.
     */
    WPE_Transition transition;
    char transition_scene[WPE_VFS_PATH_MAX];
    float transition_time; /* Real seconds into the current half. */
    bool transition_opening;
    bool transition_loading; /* The transition's own load is running (it doesn't cancel the transition). */
    char save_dir[1024];     /* "" until a script needs it (see wpe__app_save_directory). */
    bool save_dir_made;      /* save_dir exists (made on first use). */
    WPE_AssetHandle sky;     /* The loaded sky image (wpe__resolve_sky). */
    /* Captured frames (wpe.capture_screen), image assets "capture/1".. that the engine owns. */
    WPE_AssetHandle captures[WPE__CAPTURE_SLOTS];
    WPE_Texture* capture_textures[WPE__CAPTURE_SLOTS];
    uint32_t captures_pending; /* Slots to fill from this frame (bit per slot). */
    char capture_path[32];
    char sky_image[WPE_VFS_PATH_MAX]; /* Its path. */
    WPE_Session* session;             /* Multiplayer, when started (wpe_engine_net_start). */
    WPE_Replication* replication;     /* Networked entities (works with the session). */
    uint32_t session_generation;      /* Changes whenever the session is started or stopped. */
    WPE_EngineNetDesc
        net_defaults; /* What scripts' wpe.net_host/net_join use (wpe_engine_set_net_defaults). */
    WPE_DiscoveryBrowser* browser;   /* wpe.net_find_games, while looking. */
    uint8_t* net_buffer;             /* Outgoing script messages (WPE_NET_MAX_MESSAGE_SIZE bytes, lazily). */
    wpe__QueuedNetEvent* net_events; /* For the scripts, delivered after on_update. */
    uint32_t net_event_count;
    uint32_t net_event_capacity;
    wpe__NetRates net_rates;           /* Traffic per second, for the network overlay. */
    WPE_NetConditions net_conditions;  /* The running session's simulated network (the overlay shows it). */
    bool pending_from_host;            /* pending_scene is the host's scene (a player follows it). */
    char scene_path[WPE_VFS_PATH_MAX]; /* The scene file last loaded ("" after a text load): a host shares it.
                                        */
    uint64_t game_id;
    bool game_id_known;
    bool quit;
    bool embedded;        /* The host forwards events and shows the framebuffer. */
    bool simulating;      /* Scripts, physics, animation and audio run (false: edit mode). */
    bool draw_ui;         /* The UI is drawn over the scene (the editor's 3D view hides it). */
    bool draw_scene;      /* The 3D scene is rendered (the editor's 2D view of a scene without a camera). */
    bool mouse_captured;  /* As applied to the window. */
    double audio_backlog; /* Device-less mixing: frames owed to the mixer (fractional). */
    float* audio_mix;
    WPE_EngineStats stats;
};

/* ---- Script services ---------------------------------------------------------------------------- */

static void wpe__fill_net_services(WPE_ScriptNet* net); /* With the multiplayer code, below. */

/* How much of the screen the running transition covers (0..1). */
static float wpe__transition_coverage(const WPE_Engine* engine) {
    if (engine->transition.kind == WPE_TRANSITION_NONE) return 0.0f;
    const float t = wpe_ease(WPE_EASE_SMOOTH, engine->transition_time / engine->transition.duration);
    return engine->transition_opening ? 1.0f - t : t;
}

static void wpe__app_load_scene(void* user_data, const char* path, const WPE_Transition* transition) {
    WPE_Engine* engine = user_data;
    engine->pending_from_host = false; /* A script's own choice, even on a player. */
    if (!transition || transition->kind == WPE_TRANSITION_NONE || !(transition->duration > 0.0f)) {
        snprintf(engine->pending_scene, sizeof engine->pending_scene, "%s", path);
        return;
    }
    /* Closing toward this scene; already closing, only the scene changes; opening, it closes again from
     * where it is. */
    const float coverage = wpe__transition_coverage(engine);
    const bool closing = engine->transition.kind != WPE_TRANSITION_NONE && !engine->transition_opening;
    if (!closing) {
        engine->transition = *transition;
        engine->transition_opening = false;
        engine->transition_time = 0.0f;
        /* Pick up from the current coverage: the closing half's time where the curve reaches it. */
        for (int i = 0; i < 20 && coverage > 0.0f; ++i) {
            const float t = engine->transition_time / transition->duration;
            if (wpe_ease(WPE_EASE_SMOOTH, t) >= coverage) break;
            engine->transition_time += transition->duration / 20.0f;
        }
    }
    snprintf(engine->transition_scene, sizeof engine->transition_scene, "%s", path);
}

/* Advances a running transition by a frame's real time; at the closed point it asks for its scene. */
static void wpe__advance_transition(WPE_Engine* engine, double dt) {
    if (engine->transition.kind == WPE_TRANSITION_NONE) return;
    engine->transition_time += (float)dt;
    if (engine->transition_time < engine->transition.duration) return;
    if (engine->transition_opening) {
        engine->transition.kind = WPE_TRANSITION_NONE; /* Open again: done. */
        return;
    }
    /* Closed: this frame shows it covered, the scene loads after it, and the next ones open. */
    engine->transition_opening = true;
    engine->transition_time = 0.0f;
    memcpy(engine->pending_scene, engine->transition_scene, sizeof engine->pending_scene);
    engine->transition_loading = true;
}

static void wpe__app_quit(void* user_data) {
    ((WPE_Engine*)user_data)->quit = true;
}

static void wpe__app_set_camera(void* user_data, WPE_Entity camera) {
    wpe_engine_set_camera(user_data, camera);
}

static WPE_Entity wpe__app_get_camera(void* user_data) {
    return wpe_engine_camera(user_data);
}

static void wpe__app_screen_size(void* user_data, uint32_t* out_width, uint32_t* out_height) {
    const WPE_Engine* engine = user_data;
    *out_width = engine->project.width;
    *out_height = engine->project.height;
}

/* The save directory: the one the host gave, or the user's directory for this game, found and made on
 * first use. Only letters, digits and spaces of the project name make up the folder name. */
static const char* wpe__app_save_directory(void* user_data) {
    WPE_Engine* engine = user_data;
    if (engine->save_dir[0] && !engine->save_dir_made) {
        if (wpe_directory_create(engine->save_dir) != WPE_RESULT_OK) return NULL; /* Logged. */
        engine->save_dir_made = true;
    }
    if (engine->save_dir[0]) return engine->save_dir;
    char app[WPE_PROJECT_NAME_MAX];
    size_t length = 0;
    for (const char* c = engine->project.name; *c && length + 1 < sizeof app; ++c) {
        const bool kept =
            (*c >= 'a' && *c <= 'z') || (*c >= 'A' && *c <= 'Z') || (*c >= '0' && *c <= '9') || *c == ' ';
        if (kept) app[length++] = *c;
    }
    app[length] = '\0';
    if (length == 0) snprintf(app, sizeof app, "Game");
    if (wpe_platform_save_directory("WillPowerEngine", app, engine->save_dir, sizeof engine->save_dir) !=
        WPE_RESULT_OK) {
        engine->save_dir[0] = '\0';
        return NULL;
    }
    engine->save_dir_made = true; /* SDL makes it. */
    return engine->save_dir;
}

/* A capture slot's image asset, made the first time (showing the last finished frame until this frame's
 * scene pass fills it). */
static const char* wpe__app_capture_screen(void* user_data, uint32_t slot) {
    WPE_Engine* engine = user_data;
    if (slot < 1 || slot > WPE__CAPTURE_SLOTS) {
        wpe_log_error("capture slots are 1 to %u (not %u)", WPE__CAPTURE_SLOTS, slot);
        return NULL;
    }
    const uint32_t i = slot - 1;
    char name[32];
    snprintf(name, sizeof name, "capture/%u", slot);
    if (!engine->capture_textures[i]) {
        const WPE_Framebuffer* fb = wpe_renderer_framebuffer(engine->renderer);
        WPE_Texture* texture = NULL;
        WPE_Result result = wpe_texture_create(
            &(WPE_TextureDesc){.width = fb->width, .height = fb->height, .texels = fb->color, .srgb = true},
            &texture);
        if (result == WPE_RESULT_OK)
            result = wpe_asset_add(engine->assets, WPE_ASSET_IMAGE, name, texture, &engine->captures[i]);
        if (result != WPE_RESULT_OK) {
            wpe_log_error("cannot make the capture image '%s': %s", name, wpe_result_to_string(result));
            wpe_texture_destroy(texture);
            return NULL;
        }
        engine->capture_textures[i] = texture;
    }
    engine->captures_pending |= 1u << i;
    snprintf(engine->capture_path, sizeof engine->capture_path, "%s", name);
    return engine->capture_path;
}
static WPE_PostFxConfig* wpe__app_postfx(void* user_data, uint32_t** out_count) {
    WPE_Engine* engine = user_data;
    *out_count = &engine->environment.postfx_count;
    return engine->environment.postfx;
}
static WPE_SceneRenderSettings* wpe__app_render_settings(void* user_data) {
    WPE_Engine* engine = user_data;
    return &engine->environment.render;
}
static WPE_GameFeel* wpe__app_game_feel(void* user_data) {
    WPE_Engine* engine = user_data;
    return &engine->feel;
}

static WPE_SceneContext wpe__context(const WPE_Engine* engine) {
    return (WPE_SceneContext){
        .ecs = engine->ecs, .assets = engine->assets, .scripts = engine->scripts, .prefabs = engine->prefabs};
}

/* wpe.instantiate of a prefab file: by the project's prefab name, or by asset path. */
static WPE_Result wpe__app_instantiate(void* user_data, const char* name, WPE_Entity* out_root) {
    const WPE_Engine* engine = user_data;
    const char* path = name;
    for (uint32_t i = 0; i < engine->project.prefab_count; ++i) {
        if (strcmp(engine->project.prefabs[i].name, name) == 0) path = engine->project.prefabs[i].path;
    }
    if (!wpe_prefab_is_path(path)) return WPE_RESULT_ERROR_NOT_FOUND; /* A model: the host instantiates it. */
    const WPE_SceneContext context = wpe__context(engine);
    return wpe_prefab_instantiate(&context, path, out_root);
}

static void wpe__on_event(const WPE_PlatformEvent* event, void* user_data) {
    wpe_input_handle_event(((WPE_Engine*)user_data)->input, event);
}

/* ---- Lifecycle ---------------------------------------------------------------------------------- */

static WPE_Result wpe__mount(WPE_Engine* engine, const WPE_EngineDesc* desc) {
    WPE_Result result = wpe_vfs_create(&engine->vfs);
    if (result != WPE_RESULT_OK) return result;
    result = desc->project_dir ? wpe_vfs_mount_directory(engine->vfs, desc->project_dir)
                               : wpe_vfs_mount_pak(engine->vfs, desc->pak_path);
    if (result == WPE_RESULT_OK) result = wpe_project_load(engine->vfs, &engine->project);
    return result;
}

static WPE_Result wpe__create_graphics(WPE_Engine* engine, const WPE_EngineDesc* desc) {
    const WPE_ProjectSettings* p = &engine->project;
    WPE_Result result = wpe_jobs_create(desc->workers ? desc->workers : WPE_JOBS_AUTO_WORKERS, &engine->jobs);
    if (result == WPE_RESULT_OK) {
        result = wpe_renderer_create(
            &(WPE_RendererDesc){.width = p->width, .height = p->height, .jobs = engine->jobs},
            &engine->renderer);
    }
    if (result == WPE_RESULT_OK && !desc->headless && !desc->embedded) {
        result = wpe_window_create(&(WPE_WindowDesc){.title = p->name,
                                                     .width = p->width * p->scale,
                                                     .height = p->height * p->scale,
                                                     .resizable = p->resizable,
                                                     .vsync = p->vsync,
                                                     .fullscreen = p->fullscreen},
                                   &engine->window);
        if (result == WPE_RESULT_OK) wpe_window_set_event_handler(engine->window, wpe__on_event, engine);
    }
    if (result == WPE_RESULT_OK) result = wpe_scene_renderer_create(&engine->scene_renderer);
    if (result == WPE_RESULT_OK) result = wpe_postfx_create(&engine->postfx);
    if (result == WPE_RESULT_OK) result = wpe_draw2d_create(&engine->draw2d);
    if (result == WPE_RESULT_OK) result = wpe_debug_lines_create(&engine->debug_lines);
    return result;
}

/* The project's font, or the built-in one when the project names none or its file is missing (a sheet
 * that exists but is broken stays an error). */
static WPE_Result wpe__load_font(WPE_Engine* engine) {
    const char* path = engine->project.font;
    if (path[0]) {
        if (wpe_vfs_exists(engine->vfs, path))
            return wpe_asset_load(engine->assets, WPE_ASSET_FONT, path, &engine->font);
        wpe_log_warn("font '%s' not found; using the built-in font", path);
    }
    WPE_BitmapFont* font = NULL;
    WPE_Result result = wpe_font_create_builtin(&font);
    if (result == WPE_RESULT_OK) {
        result = wpe_asset_add(engine->assets, WPE_ASSET_FONT, "builtin/font", font, &engine->font);
        if (result != WPE_RESULT_OK) wpe_font_destroy(font);
    }
    return result;
}

static WPE_Result wpe__create_world(WPE_Engine* engine, const WPE_EngineDesc* desc) {
    const WPE_ProjectSettings* p = &engine->project;
    WPE_Result result = wpe_asset_manager_create(
        &(WPE_AssetManagerDesc){
            .vfs = engine->vfs, .jobs = engine->jobs, .max_texture_size = p->max_texture_size},
        &engine->assets);
    if (result == WPE_RESULT_OK) result = wpe__load_font(engine);
    if (result == WPE_RESULT_OK) result = wpe_ecs_create(&engine->ecs);
    if (result == WPE_RESULT_OK) result = wpe_scene_register_components(engine->ecs);
    if (result == WPE_RESULT_OK) result = wpe_graphics_register_components(engine->ecs, engine->assets);
    if (result == WPE_RESULT_OK) result = wpe_prefab_register(engine->ecs);
    if (result == WPE_RESULT_OK) result = wpe_replication_register_components(engine->ecs);
    if (result == WPE_RESULT_OK) result = wpe_prefab_cache_create(&engine->prefabs);
    if (result == WPE_RESULT_OK) {
        result = wpe_physics_create(&(WPE_PhysicsDesc){.ecs = engine->ecs,
                                                       .assets = engine->assets,
                                                       .gravity = engine->environment.gravity},
                                    &engine->physics);
    }
    if (result == WPE_RESULT_OK) {
        result =
            wpe_replication_create(&(WPE_ReplicationDesc){.ecs = engine->ecs,
                                                          .physics = engine->physics,
                                                          .tick_rate = p->network.tick_rate,
                                                          .interpolation_ms = p->network.interpolation_ms},
                                   &engine->replication);
    }
    if (result == WPE_RESULT_OK) {
        result = wpe_audio_create(&(WPE_AudioDesc){.ecs = engine->ecs,
                                                   .assets = engine->assets,
                                                   .no_device = desc->headless || desc->no_audio_device},
                                  &engine->audio);
    }
    for (uint32_t bus = 0; result == WPE_RESULT_OK && bus < WPE_AUDIO_BUS_COUNT; ++bus)
        wpe_audio_set_volume(engine->audio, (WPE_AudioBus)bus, p->volumes[bus]);
    if (result == WPE_RESULT_OK) {
        const WPE_AudioStats stats = wpe_audio_stats(engine->audio);
        engine->audio_mix =
            wpe_alloc((size_t)WPE__AUDIO_CHUNK_FRAMES * stats.channels * sizeof *engine->audio_mix);
        if (!engine->audio_mix) result = WPE_RESULT_ERROR_OUT_OF_MEMORY;
    }
    if (result == WPE_RESULT_OK)
        result = wpe_input_create(&(WPE_InputDesc){.settings = p->input}, &engine->input);
    if (result == WPE_RESULT_OK) result = wpe_input_set_actions(engine->input, p->actions, p->action_count);
    if (result == WPE_RESULT_OK) {
        result = wpe_ui_create(
            &(WPE_UiDesc){
                .ecs = engine->ecs, .assets = engine->assets, .input = engine->input, .font = engine->font},
            &engine->ui);
    }
    if (result == WPE_RESULT_OK) {
        const WPE_ScriptApp app = {.user_data = engine,
                                   .load_scene = wpe__app_load_scene,
                                   .quit = wpe__app_quit,
                                   .set_camera = wpe__app_set_camera,
                                   .get_camera = wpe__app_get_camera,
                                   .screen_size = wpe__app_screen_size,
                                   .instantiate = wpe__app_instantiate,
                                   .save_directory = wpe__app_save_directory,
                                   .postfx = wpe__app_postfx,
                                   .render_settings = wpe__app_render_settings,
                                   .game_feel = wpe__app_game_feel,
                                   .capture_screen = wpe__app_capture_screen};
        WPE_ScriptApp with_net = app;
        wpe__fill_net_services(&with_net.net);
        result =
            wpe_script_host_create(&(WPE_ScriptHostDesc){.ecs = engine->ecs,
                                                         .assets = engine->assets,
                                                         .physics = engine->physics,
                                                         .audio = engine->audio,
                                                         .input = engine->input,
                                                         .draw2d = engine->draw2d,
                                                         .font = engine->font,
                                                         .ui = engine->ui,
                                                         .app = &with_net,
                                                         .memory_limit = (size_t)p->script_memory_mb << 20,
                                                         .time_limit_ms = p->script_time_limit_ms},
                                   &engine->scripts);
    }
    /* Named prefabs for wpe.instantiate(). The host keeps the models loaded; prefab files are
     * instantiated through the app service (and cached once used). */
    for (uint32_t i = 0; result == WPE_RESULT_OK && i < p->prefab_count; ++i) {
        if (wpe_prefab_is_path(p->prefabs[i].path)) {
            if (wpe_vfs_exists(engine->vfs, p->prefabs[i].path)) continue;
            wpe_log_error("prefab '%s' ('%s') not found", p->prefabs[i].name, p->prefabs[i].path);
            result = WPE_RESULT_ERROR_NOT_FOUND;
            break;
        }
        WPE_AssetHandle model = WPE_ASSET_NONE;
        result = wpe_asset_load(engine->assets, WPE_ASSET_MODEL, p->prefabs[i].path, &model);
        if (result == WPE_RESULT_OK)
            result = wpe_script_host_register_prefab(engine->scripts, p->prefabs[i].name, model);
        wpe_asset_release(engine->assets, model);
        if (result != WPE_RESULT_OK)
            wpe_log_error("prefab '%s' ('%s') failed to load", p->prefabs[i].name, p->prefabs[i].path);
    }
    return result;
}

WPE_Result wpe_engine_create(const WPE_EngineDesc* desc, WPE_Engine** out_engine) {
    if (!desc || !out_engine) return WPE_RESULT_ERROR_NULL_ARG;
    *out_engine = NULL;
    if ((desc->project_dir != NULL) == (desc->pak_path != NULL)) {
        wpe_log_error("give the game either as a project directory or as an archive");
        return WPE_RESULT_ERROR_INVALID_ARG;
    }
    WPE_Engine* engine = wpe_alloc_zeroed(1, sizeof *engine);
    if (!engine) return WPE_RESULT_ERROR_OUT_OF_MEMORY;
    engine->environment = wpe_scene_environment_default();
    engine->feel = wpe_game_feel_default();
    engine->embedded = desc->embedded;
    engine->simulating = true;
    engine->draw_ui = true;
    engine->draw_scene = true;
    if (desc->save_dir && strlen(desc->save_dir) >= sizeof engine->save_dir) {
        wpe_log_error("the save directory path is too long");
        wpe_free(engine);
        return WPE_RESULT_ERROR_INVALID_ARG;
    }
    if (desc->save_dir) snprintf(engine->save_dir, sizeof engine->save_dir, "%s", desc->save_dir);

    WPE_Result result = wpe__mount(engine, desc);
    if (result == WPE_RESULT_OK) result = wpe__create_graphics(engine, desc);
    if (result == WPE_RESULT_OK) result = wpe__create_world(engine, desc);
    if (result == WPE_RESULT_OK) {
        wpe_fixed_step_init(&engine->clock, 1.0 / engine->project.fixed_rate, engine->project.max_steps);
        const char* scene = desc->scene ? desc->scene : engine->project.start_scene;
        if (scene[0]) result = wpe_engine_load_scene(engine, scene);
    }
    if (result != WPE_RESULT_OK) {
        wpe_log_error("the game failed to start: %s", wpe_result_to_string(result));
        wpe_engine_destroy(engine);
        return result;
    }
    *out_engine = engine;
    return WPE_RESULT_OK;
}

void wpe_engine_destroy(WPE_Engine* engine) {
    if (!engine) return;
    wpe_engine_net_stop(engine);
    wpe_discovery_browser_destroy(engine->browser);
    wpe_free(engine->net_buffer);
    for (uint32_t i = 0; i < engine->net_event_count; ++i)
        wpe_free(engine->net_events[i].value);
    wpe_free(engine->net_events);
    /* Users of the world and its assets first, then the world, then what they read from. */
    wpe_script_host_destroy(engine->scripts);
    wpe_replication_destroy(engine->replication);
    wpe_audio_destroy(engine->audio);
    wpe_physics_destroy(engine->physics);
    wpe_ecs_destroy(engine->ecs);
    wpe_ui_destroy(engine->ui);
    wpe_prefab_cache_destroy(engine->prefabs);
    wpe_asset_release(engine->assets, engine->font);
    wpe_asset_release(engine->assets, engine->sky);
    for (uint32_t i = 0; i < WPE__CAPTURE_SLOTS; ++i)
        wpe_asset_release(engine->assets, engine->captures[i]); /* The asset owns the texture. */
    wpe_asset_manager_destroy(engine->assets);
    wpe_vfs_destroy(engine->vfs);
    wpe_input_destroy(engine->input);
    wpe_draw2d_destroy(engine->draw2d);
    wpe_debug_lines_destroy(engine->debug_lines);
    wpe_postfx_destroy(engine->postfx);
    wpe_scene_renderer_destroy(engine->scene_renderer);
    wpe_window_destroy(engine->window);
    wpe_renderer_destroy(engine->renderer);
    wpe_jobs_destroy(engine->jobs);
    wpe_free(engine->audio_mix);
    wpe_free(engine);
}

/* ---- Scenes ------------------------------------------------------------------------------------- */

static void wpe__apply_music(WPE_Engine* engine) {
    const WPE_SceneEnvironment* env = &engine->environment;
    if (!env->music[0]) {
        if (engine->music[0]) wpe_audio_stop_music(engine->audio, env->music_fade);
        engine->music[0] = '\0';
        return;
    }
    /* The same track continues across scenes without restarting. */
    if (strcmp(env->music, engine->music) == 0 && wpe_audio_music_playing(engine->audio)) return;
    if (wpe_audio_play_music(engine->audio, env->music, env->music_fade, true) == WPE_RESULT_OK) {
        snprintf(engine->music, sizeof engine->music, "%s", env->music);
    } else {
        wpe_log_warn("scene music '%s' cannot be played", env->music);
        engine->music[0] = '\0';
    }
}

WPE_Result wpe_engine_load_scene_text(WPE_Engine* engine, const char* text, size_t size, const char* name) {
    if (!engine || !text) return WPE_RESULT_ERROR_NULL_ARG;
    if (!name) name = "scene";
    engine->scene_path[0] = '\0'; /* wpe_engine_load_scene() sets it once a file has loaded. */
    WPE_Result result = wpe_scene_clear(engine->ecs);
    engine->camera = WPE_ENTITY_NULL;
    engine->environment = wpe_scene_environment_default();
    wpe_game_feel_reset(&engine->feel, NULL); /* The world is gone: nothing to restore. */
    if (!engine->transition_loading) engine->transition.kind = WPE_TRANSITION_NONE; /* Other loads cut. */
    const WPE_SceneContext context = wpe__context(engine);
    WPE_SceneEnvironment environment = wpe_scene_environment_default();
    WPE_Entity camera = WPE_ENTITY_NULL;
    if (result == WPE_RESULT_OK) result = wpe_scene_load(&context, text, size, name, &environment, &camera);
    if (result != WPE_RESULT_OK) {
        wpe_log_error("cannot load scene '%s': %s", name, wpe_result_to_string(result));
        return result;
    }
    engine->environment = environment;
    engine->camera = camera;
    wpe_physics_set_gravity(engine->physics, environment.gravity);
    /* Bodies exist before the first fixed step, so scripts can use them from their first callback. */
    result = wpe_physics_create_bodies(engine->physics);
    if (result != WPE_RESULT_OK) return result;
    if (engine->simulating) wpe__apply_music(engine);
    /* Not shared: wpe__load_scene_file() marks the session's scene files as shared afterwards. */
    wpe_replication_scene_loaded(engine->replication, engine->session, false);
    wpe_log_info("loaded scene '%s' (%u entities)", name, wpe_ecs_entity_count(engine->ecs));
    return WPE_RESULT_OK;
}

/* Loads a scene file; `from_host` when a player follows the host there. */
static WPE_Result wpe__load_scene_file(WPE_Engine* engine, const char* path, bool from_host) {
    void* data = NULL;
    size_t size = 0;
    WPE_Result result = wpe_vfs_read_all(engine->vfs, path, &data, &size);
    if (result == WPE_RESULT_ERROR_NOT_FOUND) wpe_log_error("scene '%s' not found", path);
    if (result == WPE_RESULT_OK) {
        result = wpe_engine_load_scene_text(engine, data, size, path);
        if (result == WPE_RESULT_OK) {
            snprintf(engine->scene_path, sizeof engine->scene_path, "%s", path);
            /* A host takes its players along; replication runs on the session's scene only. */
            const bool host = wpe_session_is_host(engine->session);
            if (host) (void)wpe_session_set_scene(engine->session, path);
            wpe_replication_scene_loaded(engine->replication, engine->session, host || from_host);
        }
    } else {
        /* As with a failed load, the world is left empty. */
        const WPE_Result cleared = wpe_scene_clear(engine->ecs);
        engine->camera = WPE_ENTITY_NULL;
        if (cleared != WPE_RESULT_OK) result = cleared;
        wpe_log_error("cannot load scene '%s': %s", path, wpe_result_to_string(result));
    }
    wpe_free(data);
    return result;
}

WPE_Result wpe_engine_load_scene(WPE_Engine* engine, const char* path) {
    if (!engine || !path) return WPE_RESULT_ERROR_NULL_ARG;
    return wpe__load_scene_file(engine, path, false);
}

const char* wpe_engine_scene_path(const WPE_Engine* engine) {
    return engine ? engine->scene_path : "";
}

WPE_Result wpe_engine_save_scene(const WPE_Engine* engine, const char* os_path) {
    if (!engine || !os_path) return WPE_RESULT_ERROR_NULL_ARG;
    const WPE_SceneContext context = wpe__context(engine);
    return wpe_scene_save_file(&context, &engine->environment, wpe_engine_camera(engine), os_path);
}

void wpe_engine_set_camera(WPE_Engine* engine, WPE_Entity camera) {
    if (wpe_check(engine != NULL)) engine->camera = camera;
}

WPE_Entity wpe_engine_camera(const WPE_Engine* engine) {
    if (!engine) return WPE_ENTITY_NULL;
    if (wpe_ecs_has_component(engine->ecs, engine->camera, WPE_COMP_CAMERA)) return engine->camera;
    /* Fallback: the first camera in the world. */
    WPE_EcsIter it = wpe_ecs_query(
        engine->ecs, (WPE_EcsQuery){.all = WPE_ECS_MASK(WPE_COMP_CAMERA) | WPE_ECS_MASK(WPE_COMP_TRANSFORM)});
    return wpe_ecs_iter_next(&it) ? it.entity : WPE_ENTITY_NULL;
}

/* ---- Multiplayer --------------------------------------------------------------------------------- */

typedef struct wpe__GameHash {
    const WPE_Vfs* vfs;
    uint64_t hash;
    bool failed;
} wpe__GameHash;

static bool wpe__ends_with(const char* text, const char* suffix) {
    const size_t length = strlen(text);
    const size_t suffix_length = strlen(suffix);
    return length >= suffix_length && strcmp(text + length - suffix_length, suffix) == 0;
}

static void wpe__fnv(uint64_t* hash, const void* data, size_t size, bool skip_cr) {
    const uint8_t* bytes = data;
    for (size_t i = 0; i < size; ++i) {
        if (skip_cr && bytes[i] == '\r') continue; /* Checkouts may differ in line endings only. */
        *hash = (*hash ^ bytes[i]) * 0x100000001B3ull;
    }
}

/* Hashes the files that decide what the game does, in the VFS's (sorted) order. */
static bool wpe__hash_file(const char* path, void* user_data) {
    wpe__GameHash* h = user_data;
    if (!(strcmp(path, WPE_PROJECT_FILE) == 0 || wpe__ends_with(path, ".scene") ||
          wpe__ends_with(path, ".prefab") || wpe__ends_with(path, ".luau")))
        return true;
    void* data = NULL;
    size_t size = 0;
    if (wpe_vfs_read_all(h->vfs, path, &data, &size) != WPE_RESULT_OK) {
        h->failed = true;
        return true;
    }
    wpe__fnv(&h->hash, path, strlen(path) + 1, false);
    wpe__fnv(&h->hash, data, size, true);
    wpe_free(data);
    return true;
}

uint64_t wpe_engine_game_id(WPE_Engine* engine) {
    if (!engine) return 0;
    if (engine->game_id_known) return engine->game_id;
    wpe__GameHash h = {.vfs = engine->vfs, .hash = 0xCBF29CE484222325ull};
    if (wpe_vfs_list(engine->vfs, wpe__hash_file, &h) != WPE_RESULT_OK || h.failed)
        wpe_log_warn("some game files could not be read for the game id");
    engine->game_id = h.hash;
    engine->game_id_known = true;
    return engine->game_id;
}

void wpe_engine_net_stop(WPE_Engine* engine) {
    if (!engine || !engine->session) return;
    wpe_replication_stop(engine->replication);
    wpe_session_destroy(engine->session);
    engine->session = NULL;
    ++engine->session_generation; /* Whoever was reading the old session's events must stop. */
}

void wpe_engine_set_net_defaults(WPE_Engine* engine, const WPE_EngineNetDesc* defaults) {
    if (!engine) return;
    engine->net_defaults = defaults ? *defaults : (WPE_EngineNetDesc){0};
    engine->net_defaults.join = NULL;
    engine->net_defaults.player_name = NULL;
    engine->net_defaults.scene = NULL;
}

WPE_Result wpe_engine_net_set_owner(WPE_Engine* engine, WPE_Entity entity, WPE_PlayerId player) {
    if (!engine) return WPE_RESULT_ERROR_NULL_ARG;
    return wpe_replication_set_owner(engine->replication, engine->session, entity, player);
}

bool wpe_engine_net_is_local(const WPE_Engine* engine, WPE_Entity entity) {
    return !engine || wpe_replication_is_local(engine->replication, engine->session, entity);
}

WPE_Result wpe_engine_net_start(WPE_Engine* engine, const WPE_EngineNetDesc* desc) {
    if (!engine || !desc) return WPE_RESULT_ERROR_NULL_ARG;
    wpe_engine_net_stop(engine);
    ++engine->session_generation;
    engine->game_id_known = false; /* The editor may have changed files since the last game. */
    engine->net_conditions = desc->conditions;
    const char* scene = desc->scene ? desc->scene : engine->scene_path;
    const WPE_ProjectNetwork* settings = &engine->project.network;
    const WPE_SessionDesc session = {
        .transport = {.hub = desc->hub,
                      .port = desc->any_port ? 0
                              : desc->port   ? desc->port
                                             : settings->port,
                      .timeout_ms = settings->timeout_ms,
                      .local_only = desc->local_only,
                      .conditions = desc->conditions},
        .max_players = settings->max_players,
        .game_id = wpe_engine_game_id(engine),
        .player_name = desc->player_name,
        .game_name = engine->project.name,
        .discoverable = !desc->hidden,
        .discovery_port = settings->discovery_port,
    };
    WPE_Result result = WPE_RESULT_OK;
    if (desc->join) {
        WPE_NetAddress address = {0};
        result = wpe_network_address_parse(desc->join, settings->port, &address);
        if (result == WPE_RESULT_OK) result = wpe_session_join(&session, address, &engine->session);
        if (result == WPE_RESULT_OK) wpe_log_info("joining the game at %s", desc->join);
    } else {
        result = wpe_session_host(&session, &engine->session);
        if (result == WPE_RESULT_OK) {
            wpe_log_info("hosting on port %u",
                         (unsigned)wpe_network_local_port(wpe_session_transport(engine->session)));
            if (scene[0]) {
                (void)wpe_session_set_scene(engine->session, scene);
                wpe_replication_scene_loaded(engine->replication, engine->session, true);
            }
        }
    }
    /* A player waits in its own scene until it loads the host's. */
    if (result == WPE_RESULT_OK && desc->join) wpe_replication_stop(engine->replication);
    if (result != WPE_RESULT_OK)
        wpe_log_error("cannot start the multiplayer game: %s", wpe_result_to_string(result));
    return result;
}

WPE_Session* wpe_engine_session(const WPE_Engine* engine) {
    return engine ? engine->session : NULL;
}

/* ---- Script messages ----
 * Session kind 18. Layout (bits.h): target (4 bits: a player, or 15 for everyone else), sender (4
 * bits; the host fills it in when it passes a player's message on), name (varint length + bytes),
 * then, byte aligned, the value in the network value format. Players talk to the host; the host
 * passes on messages meant for others, so the sender field from the host can be trusted. */
#define WPE__KIND_SCRIPT 18u
#define WPE__TO_ALL 15u
#define WPE__SCRIPT_NAME_MAX 32u

/* The engine's buffer for outgoing script messages (made on first use). */
static uint8_t* wpe__net_buffer(WPE_Engine* engine) {
    if (!engine->net_buffer) engine->net_buffer = wpe_alloc(WPE_NET_MAX_MESSAGE_SIZE);
    return engine->net_buffer;
}

/* Builds a script message into `out` (WPE_NET_MAX_MESSAGE_SIZE bytes); 0 when it does not fit. */
static size_t wpe__script_message(uint8_t* out, uint32_t target, uint32_t from, const char* name,
                                  const uint8_t* value, size_t size) {
    WPE_BitWriter w = wpe_bits_writer(out, WPE_NET_MAX_MESSAGE_SIZE - 1);
    wpe_bits_write(&w, target, 4);
    wpe_bits_write(&w, from, 4);
    const size_t length = strlen(name);
    wpe_bits_write_varint(&w, (uint32_t)length);
    wpe_bits_write_bytes(&w, name, length);
    if (size) wpe_bits_write_bytes(&w, value, size);
    return w.error ? 0 : wpe_bits_writer_size(&w);
}

/* Queues an event for the scripts (copied): they hear it after this frame's on_update, when scripts of a
 * scene loaded at the end of the last frame have started too. Edit mode drops it (scripts don't run). */
static void wpe__queue_script_event(WPE_Engine* engine, WPE_ScriptNetEventType type, WPE_PlayerId player,
                                    const char* text, const char* reason, const uint8_t* value, size_t size) {
    if (!engine->simulating) return;
    if (engine->net_event_count == engine->net_event_capacity) {
        const uint32_t capacity = engine->net_event_capacity ? engine->net_event_capacity * 2 : 16;
        wpe__QueuedNetEvent* grown = wpe_realloc(engine->net_events, capacity * sizeof *grown);
        if (!grown) return;
        engine->net_events = grown;
        engine->net_event_capacity = capacity;
    }
    uint8_t* copy = NULL;
    if (size) {
        copy = wpe_alloc(size);
        if (!copy) return;
        memcpy(copy, value, size);
    }
    wpe__QueuedNetEvent* q = &engine->net_events[engine->net_event_count++];
    *q = (wpe__QueuedNetEvent){.type = type,
                               .player = player,
                               .reason = reason,
                               .value = copy,
                               .size = size,
                               .generation = engine->session_generation};
    snprintf(q->text, sizeof q->text, "%s", text ? text : "");
}

/* Hands the queued events to the scripts. A script may leave or start a game meanwhile: events of the
 * game it left are dropped. */
static void wpe__deliver_script_events(WPE_Engine* engine) {
    for (uint32_t i = 0; i < engine->net_event_count; ++i) {
        wpe__QueuedNetEvent* q = &engine->net_events[i];
        if (q->generation == engine->session_generation) {
            const WPE_ScriptNetEvent event = {.type = q->type,
                                              .player = q->player,
                                              .text = q->text,
                                              .reason = q->reason,
                                              .value = q->value,
                                              .value_size = q->size};
            (void)wpe_script_host_net_event(engine->scripts, &event);
        }
        q = &engine->net_events[i]; /* Callbacks may have queued more (and moved the array). */
        wpe_free(q->value);
        q->value = NULL;
    }
    engine->net_event_count = 0;
}

static void wpe__deliver_script_message(WPE_Engine* engine, uint32_t from, const char* name,
                                        const uint8_t* value, size_t size) {
    wpe__queue_script_event(engine, WPE_SCRIPT_NET_EVENT_MESSAGE, from, name, NULL, value, size);
}

/* A script message arrived from session player `sender`. */
static void wpe__receive_script_message(WPE_Engine* engine, WPE_Session* session, const WPE_SessionEvent* e) {
    WPE_BitReader r = wpe_bits_reader(e->data, e->size);
    const uint32_t target = wpe_bits_read(&r, 4);
    uint32_t from = wpe_bits_read(&r, 4);
    const uint32_t length = wpe_bits_read_varint(&r);
    if (r.error || length == 0 || length >= WPE__SCRIPT_NAME_MAX) return;
    const uint8_t* bytes = wpe_bits_read_bytes(&r, length);
    if (!bytes) return;
    char name[WPE__SCRIPT_NAME_MAX];
    memcpy(name, bytes, length);
    name[length] = '\0';
    if (memchr(name, '\0', length)) return;
    const size_t value_size = wpe_bits_reader_remaining(&r) / 8;
    const uint8_t* value = wpe_bits_read_bytes(&r, value_size);
    if (!wpe_session_is_host(session)) {
        if (from >= WPE_SESSION_MAX_PLAYERS) return;
        wpe__deliver_script_message(engine, from, name, value, value_size);
        return;
    }
    /* The host: the sender is who sent it, whatever the message says; pass it on where it goes. */
    from = e->player;
    if (target == WPE_SESSION_HOST || target == WPE__TO_ALL)
        wpe__deliver_script_message(engine, from, name, value, value_size);
    if (target == WPE_SESSION_HOST || target == from) return;
    uint8_t* message = wpe__net_buffer(engine);
    const size_t size = message ? wpe__script_message(message, target, from, name, value, value_size) : 0;
    if (size == 0) return;
    WPE_SessionPlayer info;
    for (WPE_PlayerId p = 1; p < wpe_session_max_players(session); ++p) {
        if (p == from || (target != WPE__TO_ALL && p != target) || !wpe_session_player(session, p, &info))
            continue;
        (void)wpe_session_send(session, p, e->channel, WPE__KIND_SCRIPT, message, size);
    }
}

/* Tells the scripts about a session event (queued, see wpe__queue_script_event). */
static void wpe__tell_scripts(WPE_Engine* engine, WPE_ScriptNetEventType type, WPE_PlayerId player,
                              const char* text, const char* reason) {
    wpe__queue_script_event(engine, type, player, text, reason, NULL, 0);
}

/* Takes the session's events: a player follows the host's scene, replication hears what concerns it,
 * scripts get theirs queued, joins and leaves are logged. */
static void wpe__pump_session(WPE_Engine* engine) {
    if (engine->browser) wpe_discovery_browser_update(engine->browser);
    WPE_Session* session = engine->session;
    if (!session) return;
    WPE_SessionEvent e;
    while (wpe_session_poll(session, &e)) {
        switch (e.type) {
        case WPE_SESSION_EVENT_JOINED:
            if (!wpe_session_is_host(session)) wpe_log_info("joined the game as player %u", e.player);
            wpe__tell_scripts(engine, WPE_SCRIPT_NET_EVENT_JOINED, e.player, e.text, NULL);
            break;
        case WPE_SESSION_EVENT_PLAYER_JOINED:
            wpe_log_info("%s joined (player %u)", e.text, e.player);
            wpe_replication_player_joined(engine->replication, session, e.player);
            wpe__tell_scripts(engine, WPE_SCRIPT_NET_EVENT_PLAYER_JOINED, e.player, e.text, NULL);
            break;
        case WPE_SESSION_EVENT_PLAYER_LEFT:
            wpe_log_info("%s left (%s)", e.text, wpe_session_reason_name(e.reason));
            wpe_replication_player_left(engine->replication, session, e.player);
            wpe__tell_scripts(engine, WPE_SCRIPT_NET_EVENT_PLAYER_LEFT, e.player, e.text,
                              wpe_session_reason_name(e.reason));
            break;
        case WPE_SESSION_EVENT_ENDED:
            wpe_log_warn("the multiplayer game ended: %s", wpe_session_reason_name(e.reason));
            wpe_replication_stop(engine->replication);
            wpe__tell_scripts(engine, WPE_SCRIPT_NET_EVENT_ENDED, WPE_SESSION_NO_PLAYER, NULL,
                              wpe_session_reason_name(e.reason));
            break;
        case WPE_SESSION_EVENT_SCENE:
            if (!wpe_vfs_path_is_valid(e.text) || !wpe_vfs_exists(engine->vfs, e.text)) {
                /* The game id should prevent this: the host plays a scene this game does not have. */
                wpe_log_error("the host plays scene '%s', which this game does not have: leaving", e.text);
                wpe_engine_net_stop(engine);
                return;
            }
            snprintf(engine->pending_scene, sizeof engine->pending_scene, "%s", e.text);
            engine->pending_from_host = true;
            wpe_replication_stop(engine->replication); /* Until the host's scene has loaded. */
            break;
        case WPE_SESSION_EVENT_MESSAGE:
            if (e.kind == WPE__KIND_SCRIPT)
                wpe__receive_script_message(engine, session, &e);
            else
                wpe_replication_receive(engine->replication, session, &e);
            break;
        case WPE_SESSION_EVENT_NONE:
            break;
        }
    }
}

/* ---- Multiplayer services for scripts (wpe.net_*) ---- */

static WPE_Result wpe__net_host(void* user_data, const char* player_name, bool hidden) {
    WPE_Engine* engine = user_data;
    WPE_EngineNetDesc desc = engine->net_defaults;
    desc.player_name = player_name;
    desc.hidden = desc.hidden || hidden;
    return wpe_engine_net_start(engine, &desc);
}

static WPE_Result wpe__net_join(void* user_data, const char* address, const char* player_name) {
    WPE_Engine* engine = user_data;
    WPE_EngineNetDesc desc = engine->net_defaults;
    desc.join = address;
    desc.player_name = player_name;
    return wpe_engine_net_start(engine, &desc);
}

static void wpe__net_leave(void* user_data) {
    wpe_engine_net_stop(user_data);
}

static WPE_ScriptNetStatus wpe__net_status(void* user_data, const char** out_reason) {
    const WPE_Engine* engine = user_data;
    const WPE_Session* session = engine->session;
    if (out_reason) *out_reason = NULL;
    if (!session) return WPE_SCRIPT_NET_OFFLINE;
    if (wpe_session_is_host(session)) return WPE_SCRIPT_NET_HOSTING;
    if (wpe_session_end_reason(session)) {
        if (out_reason) *out_reason = wpe_session_reason_name(wpe_session_end_reason(session));
        return WPE_SCRIPT_NET_ENDED;
    }
    return wpe_session_local_player(session) == WPE_SESSION_NO_PLAYER ? WPE_SCRIPT_NET_CONNECTING
                                                                      : WPE_SCRIPT_NET_JOINED;
}

static uint32_t wpe__net_local_player(void* user_data) {
    const WPE_Engine* engine = user_data;
    return engine->session ? wpe_session_local_player(engine->session) : UINT32_MAX;
}

static uint32_t wpe__net_players(void* user_data, WPE_ScriptNetPlayer* out, uint32_t capacity) {
    const WPE_Engine* engine = user_data;
    uint32_t count = 0;
    WPE_SessionPlayer info;
    for (WPE_PlayerId p = 0; p < wpe_session_max_players(engine->session) && count < capacity; ++p) {
        if (!wpe_session_player(engine->session, p, &info)) continue;
        out[count] = (WPE_ScriptNetPlayer){.id = p, .ping_ms = info.rtt_ms};
        snprintf(out[count].name, sizeof out[count].name, "%s", info.name);
        ++count;
    }
    return count;
}

static bool wpe__net_send(void* user_data, int64_t target, const char* name, const uint8_t* value,
                          size_t size, bool reliable) {
    WPE_Engine* engine = user_data;
    WPE_Session* session = engine->session;
    const WPE_PlayerId local = wpe_session_local_player(session);
    if (local == WPE_SESSION_NO_PLAYER) return false; /* Not in a game. */
    const uint32_t to = target == WPE_SCRIPT_NET_ALL ? WPE__TO_ALL : (uint32_t)target;
    if (to == local) return false;
    WPE_SessionPlayer info;
    if (to != WPE__TO_ALL && !wpe_session_player(session, to, &info)) return false;
    uint8_t* message = wpe__net_buffer(engine);
    const size_t length = message ? wpe__script_message(message, to, local, name, value, size) : 0;
    if (length == 0) return false;
    const WPE_NetChannel channel = reliable ? WPE_NET_CHANNEL_RELIABLE : WPE_NET_CHANNEL_UNRELIABLE;
    if (!wpe_session_is_host(session)) /* Everything goes through the host. */
        return wpe_session_send(session, WPE_SESSION_HOST, channel, WPE__KIND_SCRIPT, message, length) ==
               WPE_RESULT_OK;
    if (to == WPE__TO_ALL)
        return wpe_session_broadcast(session, channel, WPE__KIND_SCRIPT, message, length) == WPE_RESULT_OK;
    return wpe_session_send(session, to, channel, WPE__KIND_SCRIPT, message, length) == WPE_RESULT_OK;
}

static bool wpe__net_is_local(void* user_data, WPE_Entity entity) {
    return wpe_engine_net_is_local(user_data, entity);
}

static bool wpe__net_owner(void* user_data, WPE_Entity entity, uint32_t* out_player) {
    const WPE_Engine* engine = user_data;
    const WPE_NetIdentity* n = wpe_ecs_get_component(engine->ecs, entity, WPE_COMP_NET_IDENTITY);
    if (!n || n->net_id == 0) return false;
    *out_player = n->owner;
    return true;
}

static WPE_Result wpe__net_set_owner(void* user_data, WPE_Entity entity, uint32_t player) {
    return wpe_engine_net_set_owner(user_data, entity, player);
}

static WPE_Result wpe__net_kick(void* user_data, uint32_t player) {
    WPE_Engine* engine = user_data;
    return wpe_session_kick(engine->session, player, WPE_SESSION_REASON_GAME);
}

static uint32_t wpe__net_find_games(void* user_data, WPE_ScriptNetGame* out, uint32_t capacity) {
    WPE_Engine* engine = user_data;
    if (!engine->browser) {
        const WPE_DiscoveryDesc desc = {.hub = engine->net_defaults.hub,
                                        .port = engine->project.network.discovery_port,
                                        .local_only = engine->net_defaults.local_only,
                                        .game_id = wpe_engine_game_id(engine)};
        if (wpe_discovery_browser_create(&desc, &engine->browser) != WPE_RESULT_OK) return 0;
        wpe_discovery_browser_update(engine->browser); /* The first query goes out now. */
    }
    uint32_t count = 0;
    WPE_DiscoveredGame game;
    for (uint32_t i = 0; count < capacity && wpe_discovery_browser_game(engine->browser, i, &game); ++i) {
        WPE_ScriptNetGame* g = &out[count++];
        *g = (WPE_ScriptNetGame){.players = game.info.players,
                                 .max_players = game.info.max_players,
                                 .accepting = game.info.accepting};
        wpe_network_address_format(game.address, g->address, sizeof g->address);
        snprintf(g->game_name, sizeof g->game_name, "%s", game.info.game_name);
        snprintf(g->host_name, sizeof g->host_name, "%s", game.info.host_name);
    }
    return count;
}

static void wpe__net_stop_finding(void* user_data) {
    WPE_Engine* engine = user_data;
    wpe_discovery_browser_destroy(engine->browser);
    engine->browser = NULL;
}

static void wpe__fill_net_services(WPE_ScriptNet* net) {
    *net = (WPE_ScriptNet){.host = wpe__net_host,
                           .join = wpe__net_join,
                           .leave = wpe__net_leave,
                           .status = wpe__net_status,
                           .local_player = wpe__net_local_player,
                           .players = wpe__net_players,
                           .send = wpe__net_send,
                           .is_local = wpe__net_is_local,
                           .owner = wpe__net_owner,
                           .set_owner = wpe__net_set_owner,
                           .kick = wpe__net_kick,
                           .find_games = wpe__net_find_games,
                           .stop_finding = wpe__net_stop_finding};
}

/* ---- Frames -------------------------------------------------------------------------------------- */

static uint8_t wpe__channel(float value) {
    const float clamped = value < 0.0f ? 0.0f : value > 1.0f ? 1.0f : value;
    return (uint8_t)(clamped * 255.0f + 0.5f);
}

static WPE_Result wpe__simulate(WPE_Engine* engine, double real_dt) {
    /* Shakes come off before anything reads or moves transforms; they go back on before transforms update. */
    wpe_game_feel_restore_shakes(&engine->feel, engine->ecs);
    /* Game time (slow motion, hit-stops) drives the simulation; the UI keeps real time. */
    const double dt = wpe_game_feel_advance(&engine->feel, real_dt);
    /* Remote entities take their received poses before anything simulates (the network keeps real time). */
    wpe_replication_apply(engine->replication, engine->session, real_dt);
    /* The UI reacts to this frame's input first, so scripts see its clicks in this frame. */
    WPE_Result result =
        wpe_ui_update(engine->ui, engine->project.width, engine->project.height, (float)real_dt);
    /* Changes first (a toggle's click flips it), each with the new value. */
    uint32_t changes = 0;
    const WPE_Entity* changed = wpe_ui_changes(engine->ui, &changes);
    for (uint32_t i = 0; i < changes && result == WPE_RESULT_OK; ++i) {
        const WPE_Toggle* toggle = wpe_ecs_get_component(engine->ecs, changed[i], WPE_COMP_TOGGLE);
        const WPE_Slider* slider = wpe_ecs_get_component(engine->ecs, changed[i], WPE_COMP_SLIDER);
        WPE_ScriptValue value = {.type = WPE_SCRIPT_VALUE_NIL};
        if (slider) value = (WPE_ScriptValue){.type = WPE_SCRIPT_VALUE_NUMBER, .number = slider->value};
        if (toggle) value = (WPE_ScriptValue){.type = WPE_SCRIPT_VALUE_BOOL, .boolean = toggle->on};
        result = wpe_script_host_notify_value(engine->scripts, changed[i], "on_change", &value);
    }
    uint32_t clicks = 0;
    const WPE_Entity* clicked = wpe_ui_clicks(engine->ui, &clicks);
    for (uint32_t i = 0; i < clicks && result == WPE_RESULT_OK; ++i)
        result = wpe_script_host_notify(engine->scripts, clicked[i], "on_click");
    if (result != WPE_RESULT_OK) return result;
    const uint32_t steps = wpe_fixed_step_advance(&engine->clock, dt);
    engine->stats.physics_ms = 0.0;
    for (uint32_t s = 0; s < steps && result == WPE_RESULT_OK; ++s) {
        wpe_script_host_fixed_update(engine->scripts, (float)engine->clock.step);
        const double physics_start = wpe_platform_time_seconds();
        result = wpe_physics_step(engine->physics, (float)engine->clock.step);
        engine->stats.physics_ms += (wpe_platform_time_seconds() - physics_start) * 1000.0;
        wpe_script_host_dispatch_physics(engine->scripts);
    }
    engine->stats.fixed_steps = steps;
    if (result == WPE_RESULT_OK) result = wpe_script_host_update(engine->scripts, (float)dt);
    /* After on_update: scripts of a scene that just loaded have started and hear them too. */
    if (result == WPE_RESULT_OK) wpe__deliver_script_events(engine);
    /* State machines pick clips from what the scripts set, the animators play them, then the events the clips
     * passed reach the scripts. */
    if (result == WPE_RESULT_OK) wpe_anim_states_update(engine->ecs, (float)dt);
    if (result == WPE_RESULT_OK) result = wpe_animation_update(engine->ecs, (float)dt);
    if (result == WPE_RESULT_OK) {
        wpe_anim_states_scan(engine->ecs);
        result = wpe_script_host_dispatch_animation(engine->scripts);
    }
    if (result == WPE_RESULT_OK) wpe_billboard_update(engine->ecs, (float)dt);
    if (result == WPE_RESULT_OK) wpe_surface_update(engine->ecs, (float)dt);
    /* Before transforms: a slash's head entity moves, and emitters on it emit from there this frame. */
    if (result == WPE_RESULT_OK) wpe_slash_update(engine->ecs, (float)dt);
    /* Shakes run on real time, so they go on through a hit-stop. */
    if (result == WPE_RESULT_OK) wpe_game_feel_apply_shakes(&engine->feel, engine->ecs, (float)real_dt);
    if (result == WPE_RESULT_OK) result = wpe_scene_update_transforms(engine->ecs);
    /* After transforms, so emitters spawn where they are this frame. */
    if (result == WPE_RESULT_OK) result = wpe_particles_update(engine->ecs, engine->jobs, (float)dt);
    if (result == WPE_RESULT_OK) wpe_trail_update(engine->ecs, (float)dt);
    /* This side's entities as they are after the frame's simulation. */
    if (result == WPE_RESULT_OK) wpe_replication_send(engine->replication, engine->session);
    return result;
}

static WPE_Result wpe__mix_audio(WPE_Engine* engine, WPE_Entity camera, double dt) {
    wpe_audio_set_listener(engine->audio, camera);
    /* Effects slow down and drop in pitch with slow motion (not with hit-stops: the hit's sound plays on). */
    wpe_audio_set_time_scale(engine->audio, wpe_game_feel_audio_scale(&engine->feel));
    WPE_Result result = wpe_audio_update(engine->audio);
    const WPE_AudioStats stats = wpe_audio_stats(engine->audio);
    if (stats.device || result != WPE_RESULT_OK) return result;
    /* Without a device the mixer only advances when asked: mix this frame's share of samples. */
    engine->audio_backlog += dt * stats.sample_rate;
    while (engine->audio_backlog >= 1.0 && result == WPE_RESULT_OK) {
        const uint32_t frames = engine->audio_backlog < WPE__AUDIO_CHUNK_FRAMES
                                    ? (uint32_t)engine->audio_backlog
                                    : WPE__AUDIO_CHUNK_FRAMES;
        result = wpe_audio_render(engine->audio, engine->audio_mix, frames);
        engine->audio_backlog -= frames;
    }
    return result;
}

static void wpe__physics_line(WPE_Vec3 a, WPE_Vec3 b, uint32_t color, void* user_data) {
    wpe_debug_lines_add(user_data, a, b, 0xFF000000u | color);
}

/* The ground grid around `eye`: lines every `spacing`, split into pieces that fade with distance. */
static void wpe__draw_grid(WPE_DebugLines* lines, WPE_Vec3 eye, float spacing) {
    const float major = spacing * 10.0f;
    /* Centered on a major line under the camera, so lines stay put while the camera moves. */
    const float cx = floorf(eye.x / major + 0.5f) * major, cz = floorf(eye.z / major + 0.5f) * major;
    const float radius = (float)WPE__GRID_HALF_LINES * spacing;
    const float piece = spacing * WPE__GRID_PIECE;
    for (int i = -WPE__GRID_HALF_LINES; i <= WPE__GRID_HALF_LINES; ++i) {
        for (int axis = 0; axis < 2; ++axis) {
            /* axis 0: a line along X at z = c; axis 1: a line along Z at x = c. */
            const float c = (axis == 0 ? cz : cx) + (float)i * spacing;
            const long index = lroundf(c / spacing);
            uint32_t rgb = 0xA0A0A0u, alpha = 70;
            if (index == 0) {
                rgb =
                    axis == 0 ? 0xE64646u : 0x466EF0u; /* The X axis (z = 0) red, the Z axis (x = 0) blue. */
                alpha = 220;
            } else if (index % 10 == 0) {
                alpha = 140;
            }
            for (int p = 0; p < (int)(2.0f * WPE__GRID_HALF_LINES / WPE__GRID_PIECE); ++p) {
                const float s = -radius + (float)p * piece;
                const float mid = s + piece * 0.5f;
                const float dx = axis == 0 ? (cx + mid) - eye.x : c - eye.x;
                const float dz = axis == 0 ? c - eye.z : (cz + mid) - eye.z;
                const float fade = 1.0f - sqrtf(dx * dx + dz * dz) / radius;
                if (fade <= 0.0f) continue;
                const uint32_t a = (uint32_t)((float)alpha * fade * fade);
                const WPE_Vec3 p0 = axis == 0 ? wpe_vec3(cx + s, 0, c) : wpe_vec3(c, 0, cz + s);
                const WPE_Vec3 p1 =
                    axis == 0 ? wpe_vec3(cx + s + piece, 0, c) : wpe_vec3(c, 0, cz + s + piece);
                wpe_debug_lines_add(lines, p0, p1, (a << 24) | rgb);
            }
        }
    }
}

/* The debug overlays, into a recording frame (after post-processing, depth tested against the scene). */
static WPE_Result wpe__draw_debug(WPE_Engine* engine, WPE_Entity camera) {
    const WPE_EngineDebugDraw* debug = &engine->debug_draw;
    if (!debug->physics && !debug->grid) return WPE_RESULT_OK;
    const WPE_Framebuffer* fb = wpe_renderer_framebuffer(engine->renderer);
    WPE_Mat4 view, projection;
    if (!wpe_scene_camera_matrices(engine->ecs, camera, (float)fb->width / (float)fb->height, &view,
                                   &projection))
        return WPE_RESULT_OK;
    const WPE_Mat4 view_projection = wpe_mat4_mul(&projection, &view);
    wpe_debug_lines_begin(engine->debug_lines, &view_projection, fb->width, fb->height, 1.0f);
    if (debug->grid) {
        WPE_Mat4 world;
        if (wpe_scene_compute_world_matrix(engine->ecs, camera, &world))
            wpe__draw_grid(engine->debug_lines, wpe_mat4_transform_point(&world, wpe_vec3(0, 0, 0)),
                           debug->grid_spacing > 0.0f ? debug->grid_spacing : 1.0f);
    }
    if (debug->physics) wpe_physics_debug_draw(engine->physics, wpe__physics_line, engine->debug_lines);
    return wpe_debug_lines_submit(engine->debug_lines, engine->renderer, true);
}

/* Points the sky at its panorama image, loading it when the environment names a new one (a missing or
 * broken image is reported once and leaves the clear color showing). */
static void wpe__resolve_sky(WPE_Engine* engine) {
    WPE_SceneEnvironment* env = &engine->environment;
    if (strcmp(env->sky_image, engine->sky_image) != 0) {
        wpe_asset_release(engine->assets, engine->sky);
        engine->sky = WPE_ASSET_NONE;
        snprintf(engine->sky_image, sizeof engine->sky_image, "%s", env->sky_image);
        if (env->sky_image[0] &&
            wpe_asset_load(engine->assets, WPE_ASSET_IMAGE, env->sky_image, &engine->sky) != WPE_RESULT_OK) {
            wpe_log_warn("the sky image '%s' can't be loaded; the clear color shows instead", env->sky_image);
            engine->sky = WPE_ASSET_NONE;
        }
    }
    env->render.sky.panorama = wpe_asset_image(engine->assets, engine->sky);
}

/* Fills the capture slots scripts asked for with the frame as the scene rendered it, and gives shatter
 * effects the frames they name. */
static void wpe__fill_captures(WPE_Engine* engine) {
    const WPE_Framebuffer* fb = wpe_renderer_framebuffer(engine->renderer);
    for (uint32_t i = 0; i < WPE__CAPTURE_SLOTS; ++i) {
        WPE_Texture* texture = engine->capture_textures[i];
        if (!(engine->captures_pending & (1u << i)) || !texture) continue;
        if (texture->levels[0].width == fb->width && texture->levels[0].height == fb->height) {
            (void)wpe_texture_update(texture, fb->color); /* Same size and no mips: can't fail. */
        } else {
            wpe_log_warn("capture %u keeps its old frame: the screen changed size", i + 1);
        }
    }
    engine->captures_pending = 0;
    WPE_SceneEnvironment* env = &engine->environment;
    for (uint32_t i = 0; i < env->postfx_count; ++i) {
        WPE_ShatterParams* shatter = &env->postfx[i].shatter;
        const uint32_t slot = shatter->capture;
        shatter->image = slot >= 1 && slot <= WPE__CAPTURE_SLOTS ? engine->capture_textures[slot - 1] : NULL;
    }
}
/* The width of a line of text in the font, in texels. */
static float wpe__text_width(const WPE_BitmapFont* font, const char* text) {
    float width = 0.0f;
    for (const unsigned char* c = (const unsigned char*)text; *c; ++c)
        if (*c >= WPE_FONT_FIRST_CHAR && *c <= WPE_FONT_LAST_CHAR)
            width += (float)(font->glyphs[*c - WPE_FONT_FIRST_CHAR].width + font->glyph_spacing);
    return width;
}

/* Refreshes the traffic rates once a second (and starts over with a new session). */
static void wpe__measure_net_rates(WPE_Engine* engine) {
    wpe__NetRates* r = &engine->net_rates;
    const WPE_NetTransportStats totals = wpe_network_stats(wpe_session_transport(engine->session));
    const double now = wpe_platform_time_seconds();
    if (r->generation != engine->session_generation) {
        *r = (wpe__NetRates){.since = now,
                             .sent = totals.bytes_sent,
                             .received = totals.bytes_received,
                             .generation = engine->session_generation};
    } else if (now - r->since >= 1.0) {
        r->up = (double)(totals.bytes_sent - r->sent) / (now - r->since);
        r->down = (double)(totals.bytes_received - r->received) / (now - r->since);
        r->since = now;
        r->sent = totals.bytes_sent;
        r->received = totals.bytes_received;
    }
}

/* "340 B/s" or "12.5 KB/s". */
static void wpe__format_rate(char* out, size_t capacity, double bytes_per_second) {
    if (bytes_per_second < 1000.0)
        snprintf(out, capacity, "%.0f B/s", bytes_per_second);
    else
        snprintf(out, capacity, "%.1f KB/s", fmin(bytes_per_second, 1e9) / 1000.0);
}

/* The network overlay: who plays, their pings and the traffic, top left (over the scripts' 2D). */
static void wpe__draw_net_stats(WPE_Engine* engine) {
    const WPE_BitmapFont* font = wpe_asset_font(engine->assets, engine->font);
    if (!engine->debug_draw.network || !engine->session || !font) return;
    wpe__measure_net_rates(engine);
    const WPE_Session* s = engine->session;
    enum { LINES = WPE_SESSION_MAX_PLAYERS + 3, LINE = 128 };
    char lines[LINES][LINE];
    uint32_t count = 0;
    const WPE_PlayerId me = wpe_session_local_player(s);
    if (wpe_session_is_host(s)) {
        snprintf(lines[count++], LINE, "hosting on port %u, %u/%u players",
                 (unsigned)wpe_network_local_port(wpe_session_transport(s)),
                 (unsigned)wpe_session_player_count(s), (unsigned)wpe_session_max_players(s));
    } else if (me != WPE_SESSION_NO_PLAYER) {
        snprintf(lines[count++], LINE, "joined as player %u, %u/%u players", (unsigned)me,
                 (unsigned)wpe_session_player_count(s), (unsigned)wpe_session_max_players(s));
    } else if (wpe_session_end_reason(s) != 0) {
        snprintf(lines[count++], LINE, "game ended: %s", wpe_session_reason_name(wpe_session_end_reason(s)));
    } else {
        snprintf(lines[count++], LINE, "connecting...");
    }
    for (WPE_PlayerId p = 0; p < WPE_SESSION_MAX_PLAYERS; ++p) {
        WPE_SessionPlayer player;
        if (!wpe_session_player(s, p, &player)) continue;
        if (p == me)
            snprintf(lines[count++], LINE, " %u %s (you)", (unsigned)p, player.name);
        else if (wpe_session_is_host(s))
            snprintf(lines[count++], LINE, " %u %s  %u ms", (unsigned)p, player.name,
                     (unsigned)player.rtt_ms);
        else
            snprintf(lines[count++], LINE, " %u %s", (unsigned)p, player.name);
    }
    /* A player measures its round trip to the host only. */
    char up[24], down[24];
    wpe__format_rate(up, sizeof up, engine->net_rates.up);
    wpe__format_rate(down, sizeof down, engine->net_rates.down);
    WPE_NetPeerStats peer;
    if (!wpe_session_is_host(s) &&
        wpe_network_peer_stats(wpe_session_transport(s), 0, &peer) == WPE_RESULT_OK)
        snprintf(lines[count++], LINE, "ping %u ms, loss %.0f%%, up %s, down %s", (unsigned)peer.rtt_ms,
                 (double)peer.packet_loss * 100.0, up, down);
    else
        snprintf(lines[count++], LINE, "up %s, down %s", up, down);
    /* The simulated delay holds messages back after ENet has measured the ping, so it is not part of it. */
    const WPE_NetConditions* sim = &engine->net_conditions;
    if (sim->latency_ms > 0 || sim->jitter_ms > 0 || sim->loss > 0.0f)
        snprintf(lines[count++], LINE, "simulated: +%u..%u ms in, %.0f%% lost", (unsigned)sim->latency_ms,
                 (unsigned)(sim->latency_ms + sim->jitter_ms), (double)sim->loss * 100.0);

    const float pad = 3.0f, line_height = (float)font->line_height;
    float width = 0.0f;
    for (uint32_t i = 0; i < count; ++i)
        width = fmaxf(width, wpe__text_width(font, lines[i]));
    wpe_draw2d_rect(engine->draw2d,
                    (WPE_Rect){2.0f, 2.0f, width + 2.0f * pad, (float)count * line_height + 2.0f * pad},
                    wpe_color_rgba8(0, 0, 0, 160));
    for (uint32_t i = 0; i < count; ++i)
        wpe_draw2d_text(engine->draw2d, font, 2.0f + pad, 2.0f + pad + (float)i * line_height, lines[i],
                        i == 0 ? wpe_color_rgba8(255, 220, 120, 255) : wpe_color_rgba8(230, 230, 230, 255),
                        1.0f);
}

/* Milliseconds since *mark, which moves to now. */
static double wpe__ms_since(double* mark) {
    const double now = wpe_platform_time_seconds();
    const double ms = (now - *mark) * 1000.0;
    *mark = now;
    return ms;
}

static WPE_Result wpe__render(WPE_Engine* engine, WPE_Entity camera) {
    wpe__resolve_sky(engine);
    const WPE_SceneEnvironment* env = &engine->environment;
    const WPE_Color clear =
        wpe_color_rgba8(wpe__channel(env->clear_color.x), wpe__channel(env->clear_color.y),
                        wpe__channel(env->clear_color.z), 255);
    WPE_Result result = wpe_renderer_begin_frame(
        engine->renderer,
        &(WPE_ClearDesc){.clear_color = true, .color = clear, .clear_depth = true, .depth = 1.0f});
    if (result != WPE_RESULT_OK) return result;
    double mark = wpe_platform_time_seconds();
    if (camera != WPE_ENTITY_NULL)
        result = wpe_scene_render(engine->scene_renderer, engine->renderer, engine->ecs, camera, &env->render,
                                  NULL);
    const WPE_Result ended = wpe_renderer_end_frame(engine->renderer);
    if (result == WPE_RESULT_OK) result = ended;
    engine->stats.scene = wpe_renderer_stats(engine->renderer); /* Before the overlay frame resets them. */
    wpe__fill_captures(engine);
    engine->stats.render_ms = wpe__ms_since(&mark);

    const WPE_Camera* cam = wpe_ecs_get_component(engine->ecs, camera, WPE_COMP_CAMERA);
    if (result == WPE_RESULT_OK && cam) {
        /* Slashes' distortions warp the scene first; the environment's effects follow. */
        WPE_PostEffect effects[WPE__SLASH_DISTORTIONS + WPE_SCENE_MAX_POSTFX];
        WPE_CurveDistortParams distortions[WPE__SLASH_DISTORTIONS];
        float points[WPE__SLASH_DISTORTIONS * WPE_SLASH_DISTORTION_POINTS * 3];
        const WPE_Framebuffer* fb = wpe_renderer_framebuffer(engine->renderer);
        WPE_Mat4 view, projection;
        uint32_t count = 0;
        if (wpe_scene_camera_matrices(engine->ecs, camera, (float)fb->width / (float)fb->height, &view,
                                      &projection)) {
            const WPE_Mat4 view_projection = wpe_mat4_mul(&projection, &view);
            count = wpe_slash_distortions(engine->ecs, &view_projection, fb->width, fb->height, distortions,
                                          points, WPE__SLASH_DISTORTIONS);
            for (uint32_t i = 0; i < count; ++i)
                effects[i] = wpe_postfx_curve_distort(&distortions[i]);
        }
        count += wpe_scene_environment_effects(env, &effects[count]);
        const WPE_PostCamera post = {
            .near_z = cam->near_z, .far_z = cam->far_z, .orthographic = cam->orthographic};
        if (count > 0) result = wpe_postfx_apply(engine->postfx, engine->renderer, effects, count, &post);
    }
    engine->stats.postfx_ms = wpe__ms_since(&mark);
    /* Debug overlays, the UI and script 2D drawing go on top, untouched by post-processing. */
    if (result == WPE_RESULT_OK) result = wpe_renderer_begin_frame(engine->renderer, &(WPE_ClearDesc){0});
    if (result == WPE_RESULT_OK) {
        if (camera != WPE_ENTITY_NULL) result = wpe__draw_debug(engine, camera);
        if (result == WPE_RESULT_OK && engine->draw_ui) result = wpe_ui_draw(engine->ui, engine->renderer);
        wpe__draw_net_stats(engine);
        if (result == WPE_RESULT_OK) result = wpe_draw2d_submit(engine->draw2d, engine->renderer);
        const WPE_Result end = wpe_renderer_end_frame(engine->renderer);
        if (result == WPE_RESULT_OK) result = end;
    }
    /* A scene transition covers everything, the UI and the HUD included. */
    const float coverage = wpe__transition_coverage(engine);
    if (result == WPE_RESULT_OK && coverage > 0.0f)
        wpe_transition_cover(wpe_renderer_framebuffer_for_write(engine->renderer), &engine->transition,
                             coverage, engine->transition_opening);
    if (result == WPE_RESULT_OK && engine->window)
        result = wpe_window_present(engine->window, wpe_renderer_framebuffer(engine->renderer));
    engine->stats.overlay_ms = wpe__ms_since(&mark);
    return result;
}

WPE_Result wpe_engine_frame(WPE_Engine* engine, double dt) {
    if (!engine) return WPE_RESULT_ERROR_NULL_ARG;
    const double start = wpe_platform_time_seconds();
    if (!(dt >= 0.0)) dt = 0.0;
    if (dt > WPE__MAX_FRAME_SECONDS) dt = WPE__MAX_FRAME_SECONDS;

    /* Embedded engines get their events from the host (wpe_engine_handle_event) before the frame;
     * their input frame started at the end of the previous one. */
    if (!engine->embedded) {
        wpe_input_begin_frame(engine->input);
        const bool open = engine->window ? wpe_window_poll_events(engine->window)
                                         : wpe_platform_poll_events(wpe__on_event, engine);
        if (!open) engine->quit = true;
    }
    wpe_input_update(engine->input);
    wpe__pump_session(engine); /* Also in edit mode: connections stay alive. */
    const bool captured = wpe_input_mouse_captured(engine->input);
    if (engine->window && captured != engine->mouse_captured &&
        wpe_window_set_relative_mouse(engine->window, captured) == WPE_RESULT_OK)
        engine->mouse_captured = captured;

    wpe_draw2d_begin(engine->draw2d, engine->project.width, engine->project.height);
    double mark = wpe_platform_time_seconds();
    WPE_Result result =
        engine->simulating ? wpe__simulate(engine, dt) : wpe_scene_update_transforms(engine->ecs);
    engine->stats.simulate_ms = engine->simulating ? wpe__ms_since(&mark) : 0.0;
    if (!engine->simulating) engine->stats.physics_ms = 0.0;
    const WPE_Entity camera = wpe_engine_camera(engine);
    if (engine->simulating) wpe__advance_transition(engine, dt);
    if (result == WPE_RESULT_OK && engine->simulating) result = wpe__mix_audio(engine, camera, dt);
    if (result == WPE_RESULT_OK) result = wpe__render(engine, engine->draw_scene ? camera : WPE_ENTITY_NULL);
    if (engine->embedded) wpe_input_begin_frame(engine->input); /* Events for the next frame follow. */

    /* Scene switches requested by scripts happen between frames. */
    if (result == WPE_RESULT_OK && engine->pending_scene[0]) {
        char path[WPE_VFS_PATH_MAX];
        memcpy(path, engine->pending_scene, sizeof path);
        engine->pending_scene[0] = '\0';
        result = wpe__load_scene_file(engine, path, engine->pending_from_host);
        engine->pending_from_host = false;
        engine->transition_loading = false;
    }
    wpe_session_flush(engine->session);
    ++engine->stats.frames;
    engine->stats.entities = wpe_ecs_entity_count(engine->ecs);
    engine->stats.frame_ms = (wpe_platform_time_seconds() - start) * 1000.0;
    return result;
}

void wpe_engine_handle_event(WPE_Engine* engine, const WPE_PlatformEvent* event) {
    if (engine && event) wpe_input_handle_event(engine->input, event);
}

void wpe_engine_set_simulating(WPE_Engine* engine, bool simulating) {
    if (!wpe_check(engine != NULL) || engine->simulating == simulating) return;
    engine->simulating = simulating;
    if (simulating) {
        /* Start from a clean clock so the first frame does not catch up on the paused time. */
        wpe_fixed_step_init(&engine->clock, 1.0 / engine->project.fixed_rate, engine->project.max_steps);
        wpe__apply_music(engine);
    } else if (engine->music[0]) {
        wpe_audio_stop_music(engine->audio, 0.25f);
        engine->music[0] = '\0';
    }
}

bool wpe_engine_simulating(const WPE_Engine* engine) {
    return engine && engine->simulating;
}

bool wpe_engine_running(const WPE_Engine* engine) {
    return engine && !engine->quit;
}

void wpe_engine_request_quit(WPE_Engine* engine) {
    if (engine) engine->quit = true;
}

void wpe_engine_cancel_quit(WPE_Engine* engine) {
    if (engine) engine->quit = false;
}

WPE_Result wpe_engine_run(WPE_Engine* engine, const WPE_RunOptions* options) {
    if (!engine) return WPE_RESULT_ERROR_NULL_ARG;
    const WPE_RunOptions defaults = {0};
    if (!options) options = &defaults;
    double previous = wpe_platform_time_seconds(), last_watch = previous;
    WPE_Result result = WPE_RESULT_OK;
    for (uint32_t frame = 0; wpe_engine_running(engine) && (options->frames == 0 || frame < options->frames);
         ++frame) {
        const double now = wpe_platform_time_seconds();
        const double dt = options->fixed_dt > 0.0 ? options->fixed_dt : now - previous;
        previous = now;
        if (options->watch && now - last_watch > 0.5) {
            wpe_script_host_reload_changed(engine->scripts);
            last_watch = now;
        }
        result = wpe_engine_frame(engine, dt);
        if (result != WPE_RESULT_OK) {
            wpe_log_error("frame %u failed: %s", frame, wpe_result_to_string(result));
            return result;
        }
        if (options->max_fps > 0) {
            const double spare = 1.0 / (double)options->max_fps - (wpe_platform_time_seconds() - now);
            if (spare > 0.001) wpe_platform_sleep_ms((uint32_t)(spare * 1000.0));
        }
    }
    if (options->screenshot) result = wpe_engine_screenshot(engine, options->screenshot);
    return result;
}

WPE_Result wpe_engine_screenshot(const WPE_Engine* engine, const char* os_path) {
    if (!engine || !os_path) return WPE_RESULT_ERROR_NULL_ARG;
    const WPE_Framebuffer* fb = wpe_renderer_framebuffer(engine->renderer);
    return wpe_image_save_png(os_path, fb->width, fb->height, fb->color);
}

/* ---- Accessors ---------------------------------------------------------------------------------- */

WPE_Ecs* wpe_engine_ecs(const WPE_Engine* engine) {
    return engine ? engine->ecs : NULL;
}

const WPE_Vfs* wpe_engine_vfs(const WPE_Engine* engine) {
    return engine ? engine->vfs : NULL;
}

WPE_AssetManager* wpe_engine_assets(const WPE_Engine* engine) {
    return engine ? engine->assets : NULL;
}

WPE_Input* wpe_engine_input(const WPE_Engine* engine) {
    return engine ? engine->input : NULL;
}

WPE_Physics* wpe_engine_physics(const WPE_Engine* engine) {
    return engine ? engine->physics : NULL;
}

WPE_PrefabCache* wpe_engine_prefabs(const WPE_Engine* engine) {
    return engine ? engine->prefabs : NULL;
}

WPE_Audio* wpe_engine_audio(const WPE_Engine* engine) {
    return engine ? engine->audio : NULL;
}

WPE_Ui* wpe_engine_ui(const WPE_Engine* engine) {
    return engine ? engine->ui : NULL;
}

void wpe_engine_set_draw_ui(WPE_Engine* engine, bool draw_ui) {
    if (wpe_check(engine != NULL)) engine->draw_ui = draw_ui;
}

bool wpe_engine_draws_ui(const WPE_Engine* engine) {
    return engine && engine->draw_ui;
}

void wpe_engine_set_draw_scene(WPE_Engine* engine, bool draw_scene) {
    if (wpe_check(engine != NULL)) engine->draw_scene = draw_scene;
}

bool wpe_engine_draws_scene(const WPE_Engine* engine) {
    return engine && engine->draw_scene;
}

WPE_ScriptHost* wpe_engine_scripts(const WPE_Engine* engine) {
    return engine ? engine->scripts : NULL;
}

WPE_Renderer* wpe_engine_renderer(const WPE_Engine* engine) {
    return engine ? engine->renderer : NULL;
}

const WPE_ProjectSettings* wpe_engine_project(const WPE_Engine* engine) {
    return engine ? &engine->project : NULL;
}

WPE_SceneEnvironment* wpe_engine_environment(WPE_Engine* engine) {
    return engine ? &engine->environment : NULL;
}

WPE_GameFeel* wpe_engine_game_feel(WPE_Engine* engine) {
    return engine ? &engine->feel : NULL;
}

void wpe_engine_set_debug_draw(WPE_Engine* engine, const WPE_EngineDebugDraw* debug_draw) {
    if (wpe_check(engine != NULL && debug_draw != NULL)) engine->debug_draw = *debug_draw;
}

WPE_EngineDebugDraw wpe_engine_debug_draw(const WPE_Engine* engine) {
    return engine ? engine->debug_draw : (WPE_EngineDebugDraw){0};
}

WPE_EngineStats wpe_engine_stats(const WPE_Engine* engine) {
    return engine ? engine->stats : (WPE_EngineStats){0};
}
