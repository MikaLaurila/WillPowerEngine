#define WPE_LOG_MODULE "engine"
#include "engine/engine.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

#include "core/alloc.h"
#include "core/debug.h"
#include "core/fixed_step.h"
#include "core/log.h"
#include "graphics/animation.h"
#include "graphics/debug_lines.h"
#include "graphics/draw2d.h"
#include "graphics/graphics.h"
#include "graphics/postfx.h"
#include "graphics/scene_render.h"
#include "jobs/jobs.h"
#include "platform/platform.h"
#include "scene/scene.h"

#define WPE__MAX_FRAME_SECONDS 0.25
#define WPE__GRID_HALF_LINES 50 /* Grid lines on each side of the center. */
#define WPE__GRID_PIECE 5.0f    /* Grid line pieces, in spacings (each fades on its own). */
#define WPE__AUDIO_CHUNK_FRAMES 1024u

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
    char music[WPE_VFS_PATH_MAX]; /* Track started by the current scene ("" = none). */
    char pending_scene[WPE_VFS_PATH_MAX];
    bool quit;
    bool embedded;        /* The host forwards events and shows the framebuffer. */
    bool simulating;      /* Scripts, physics, animation and audio run (false: edit mode). */
    bool draw_ui;         /* The UI is drawn over the scene (the editor's 3D view hides it). */
    bool mouse_captured;  /* As applied to the window. */
    double audio_backlog; /* Device-less mixing: frames owed to the mixer (fractional). */
    float* audio_mix;
    WPE_EngineStats stats;
};

/* ---- Script services ---------------------------------------------------------------------------- */

static void wpe__app_load_scene(void* user_data, const char* path) {
    WPE_Engine* engine = user_data;
    snprintf(engine->pending_scene, sizeof engine->pending_scene, "%s", path);
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

static WPE_Result wpe__create_world(WPE_Engine* engine, const WPE_EngineDesc* desc) {
    const WPE_ProjectSettings* p = &engine->project;
    WPE_Result result = wpe_asset_manager_create(
        &(WPE_AssetManagerDesc){
            .vfs = engine->vfs, .jobs = engine->jobs, .max_texture_size = p->max_texture_size},
        &engine->assets);
    if (result == WPE_RESULT_OK && p->font[0])
        result = wpe_asset_load(engine->assets, WPE_ASSET_FONT, p->font, &engine->font);
    if (result == WPE_RESULT_OK) result = wpe_ecs_create(&engine->ecs);
    if (result == WPE_RESULT_OK) result = wpe_scene_register_components(engine->ecs);
    if (result == WPE_RESULT_OK) result = wpe_graphics_register_components(engine->ecs, engine->assets);
    if (result == WPE_RESULT_OK) result = wpe_prefab_register(engine->ecs);
    if (result == WPE_RESULT_OK) result = wpe_prefab_cache_create(&engine->prefabs);
    if (result == WPE_RESULT_OK) {
        result = wpe_physics_create(&(WPE_PhysicsDesc){.ecs = engine->ecs,
                                                       .assets = engine->assets,
                                                       .gravity = engine->environment.gravity},
                                    &engine->physics);
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
                                   .instantiate = wpe__app_instantiate};
        result =
            wpe_script_host_create(&(WPE_ScriptHostDesc){.ecs = engine->ecs,
                                                         .assets = engine->assets,
                                                         .physics = engine->physics,
                                                         .audio = engine->audio,
                                                         .input = engine->input,
                                                         .draw2d = engine->draw2d,
                                                         .font = engine->font,
                                                         .ui = engine->ui,
                                                         .app = &app,
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
    engine->embedded = desc->embedded;
    engine->simulating = true;
    engine->draw_ui = true;

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
    /* Users of the world and its assets first, then the world, then what they read from. */
    wpe_script_host_destroy(engine->scripts);
    wpe_audio_destroy(engine->audio);
    wpe_physics_destroy(engine->physics);
    wpe_ecs_destroy(engine->ecs);
    wpe_ui_destroy(engine->ui);
    wpe_prefab_cache_destroy(engine->prefabs);
    wpe_asset_release(engine->assets, engine->font);
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
    WPE_Result result = wpe_scene_clear(engine->ecs);
    engine->camera = WPE_ENTITY_NULL;
    engine->environment = wpe_scene_environment_default();
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
    wpe_log_info("loaded scene '%s' (%u entities)", name, wpe_ecs_entity_count(engine->ecs));
    return WPE_RESULT_OK;
}

WPE_Result wpe_engine_load_scene(WPE_Engine* engine, const char* path) {
    if (!engine || !path) return WPE_RESULT_ERROR_NULL_ARG;
    void* data = NULL;
    size_t size = 0;
    WPE_Result result = wpe_vfs_read_all(engine->vfs, path, &data, &size);
    if (result == WPE_RESULT_ERROR_NOT_FOUND) wpe_log_error("scene '%s' not found", path);
    if (result == WPE_RESULT_OK) {
        result = wpe_engine_load_scene_text(engine, data, size, path);
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

/* ---- Frames -------------------------------------------------------------------------------------- */

static uint8_t wpe__channel(float value) {
    const float clamped = value < 0.0f ? 0.0f : value > 1.0f ? 1.0f : value;
    return (uint8_t)(clamped * 255.0f + 0.5f);
}

static WPE_Result wpe__simulate(WPE_Engine* engine, double dt) {
    /* The UI reacts to this frame's input first, so scripts see its clicks in this frame. */
    WPE_Result result = wpe_ui_update(engine->ui, engine->project.width, engine->project.height, (float)dt);
    uint32_t clicks = 0;
    const WPE_Entity* clicked = wpe_ui_clicks(engine->ui, &clicks);
    for (uint32_t i = 0; i < clicks && result == WPE_RESULT_OK; ++i)
        result = wpe_script_host_notify(engine->scripts, clicked[i], "on_click");
    if (result != WPE_RESULT_OK) return result;
    const uint32_t steps = wpe_fixed_step_advance(&engine->clock, dt);
    for (uint32_t s = 0; s < steps && result == WPE_RESULT_OK; ++s) {
        wpe_script_host_fixed_update(engine->scripts, (float)engine->clock.step);
        result = wpe_physics_step(engine->physics, (float)engine->clock.step);
        wpe_script_host_dispatch_physics(engine->scripts);
    }
    engine->stats.fixed_steps = steps;
    if (result == WPE_RESULT_OK) result = wpe_script_host_update(engine->scripts, (float)dt);
    if (result == WPE_RESULT_OK) result = wpe_animation_update(engine->ecs, (float)dt);
    if (result == WPE_RESULT_OK) result = wpe_scene_update_transforms(engine->ecs);
    return result;
}

static WPE_Result wpe__mix_audio(WPE_Engine* engine, WPE_Entity camera, double dt) {
    wpe_audio_set_listener(engine->audio, camera);
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

static WPE_Result wpe__render(WPE_Engine* engine, WPE_Entity camera) {
    const WPE_SceneEnvironment* env = &engine->environment;
    const WPE_Color clear =
        wpe_color_rgba8(wpe__channel(env->clear_color.x), wpe__channel(env->clear_color.y),
                        wpe__channel(env->clear_color.z), 255);
    WPE_Result result = wpe_renderer_begin_frame(
        engine->renderer,
        &(WPE_ClearDesc){.clear_color = true, .color = clear, .clear_depth = true, .depth = 1.0f});
    if (result != WPE_RESULT_OK) return result;
    if (camera != WPE_ENTITY_NULL)
        result = wpe_scene_render(engine->scene_renderer, engine->renderer, engine->ecs, camera, &env->render,
                                  NULL);
    const WPE_Result ended = wpe_renderer_end_frame(engine->renderer);
    if (result == WPE_RESULT_OK) result = ended;

    const WPE_Camera* cam = wpe_ecs_get_component(engine->ecs, camera, WPE_COMP_CAMERA);
    if (result == WPE_RESULT_OK && cam && env->postfx_count > 0) {
        WPE_PostEffect effects[WPE_SCENE_MAX_POSTFX];
        const uint32_t count = wpe_scene_environment_effects(env, effects);
        const WPE_PostCamera post = {
            .near_z = cam->near_z, .far_z = cam->far_z, .orthographic = cam->orthographic};
        result = wpe_postfx_apply(engine->postfx, engine->renderer, effects, count, &post);
    }
    /* Debug overlays, the UI and script 2D drawing go on top, untouched by post-processing. */
    if (result == WPE_RESULT_OK) result = wpe_renderer_begin_frame(engine->renderer, &(WPE_ClearDesc){0});
    if (result == WPE_RESULT_OK) {
        if (camera != WPE_ENTITY_NULL) result = wpe__draw_debug(engine, camera);
        if (result == WPE_RESULT_OK && engine->draw_ui) result = wpe_ui_draw(engine->ui, engine->renderer);
        if (result == WPE_RESULT_OK) result = wpe_draw2d_submit(engine->draw2d, engine->renderer);
        const WPE_Result end = wpe_renderer_end_frame(engine->renderer);
        if (result == WPE_RESULT_OK) result = end;
    }
    if (result == WPE_RESULT_OK && engine->window)
        result = wpe_window_present(engine->window, wpe_renderer_framebuffer(engine->renderer));
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
    const bool captured = wpe_input_mouse_captured(engine->input);
    if (engine->window && captured != engine->mouse_captured &&
        wpe_window_set_relative_mouse(engine->window, captured) == WPE_RESULT_OK)
        engine->mouse_captured = captured;

    wpe_draw2d_begin(engine->draw2d, engine->project.width, engine->project.height);
    WPE_Result result =
        engine->simulating ? wpe__simulate(engine, dt) : wpe_scene_update_transforms(engine->ecs);
    const WPE_Entity camera = wpe_engine_camera(engine);
    if (result == WPE_RESULT_OK && engine->simulating) result = wpe__mix_audio(engine, camera, dt);
    if (result == WPE_RESULT_OK) result = wpe__render(engine, camera);
    if (engine->embedded) wpe_input_begin_frame(engine->input); /* Events for the next frame follow. */

    /* Scene switches requested by scripts happen between frames. */
    if (result == WPE_RESULT_OK && engine->pending_scene[0]) {
        char path[WPE_VFS_PATH_MAX];
        memcpy(path, engine->pending_scene, sizeof path);
        engine->pending_scene[0] = '\0';
        result = wpe_engine_load_scene(engine, path);
    }
    ++engine->stats.frames;
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

void wpe_engine_set_debug_draw(WPE_Engine* engine, const WPE_EngineDebugDraw* debug_draw) {
    if (wpe_check(engine != NULL && debug_draw != NULL)) engine->debug_draw = *debug_draw;
}

WPE_EngineDebugDraw wpe_engine_debug_draw(const WPE_Engine* engine) {
    return engine ? engine->debug_draw : (WPE_EngineDebugDraw){0};
}

WPE_EngineStats wpe_engine_stats(const WPE_Engine* engine) {
    return engine ? engine->stats : (WPE_EngineStats){0};
}
