/*
 * Lighting demo: a shadow-casting sun, orbiting colored point lights and a sweeping spot light over
 * a scene with an imported model, a skinned animated figure and procedural boxes; optional fog and
 * post-processing.
 *
 * Usage: lighting_demo [--width N] [--height N] [--scale N] [--workers N] [--frames N]
 *                      [--screenshot file.png] [--time SECONDS] [--no-shadows] [--fog] [--ssao]
 *                      [--ps1] [--chroma] [--vignette] [--dither] [--subdiv LEVELS] [--no-hud]
 *                      [--no-vsync]
 *   --time T          freeze the scene clock at T seconds (reproducible screenshots)
 *   --shadow-map F    also save the sun's shadow map (depth as grayscale) to F on exit
 */
#define WPE_LOG_MODULE "lighting"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "assets/font.h"
#include "assets/model.h"
#include "assets/procedural.h"
#include "core/core.h"
#include "graphics/animation.h"
#include "graphics/draw2d.h"
#include "graphics/graphics.h"
#include "graphics/postfx.h"
#include "graphics/scene_render.h"
#include "platform/platform.h"

typedef struct Options {
    uint32_t width, height, scale, workers, frames, subdiv;
    const char* screenshot;
    const char* shadow_map;
    float time;
    bool fixed_time, shadows, fog, ssao, ps1, chroma, vignette, dither, hud, vsync;
} Options;

static bool parse_u32(const char* text, uint32_t* out) {
    char* end = NULL;
    const unsigned long value = strtoul(text, &end, 10);
    if (!text[0] || *end != '\0' || value > UINT32_MAX) return false;
    *out = (uint32_t)value;
    return true;
}

static bool parse_options(int argc, char** argv, Options* o) {
    *o = (Options){.width = 320, .height = 240, .scale = 3, .workers = WPE_JOBS_AUTO_WORKERS, .subdiv = 3,
                   .shadows = true, .hud = true, .vsync = true};
    const struct {
        const char* name;
        bool* flag;
        bool value;
    } flags[] = {{"--no-shadows", &o->shadows, false}, {"--fog", &o->fog, true},          {"--ssao", &o->ssao, true},
                 {"--ps1", &o->ps1, true},             {"--chroma", &o->chroma, true},    {"--vignette", &o->vignette, true},
                 {"--dither", &o->dither, true},       {"--no-hud", &o->hud, false},      {"--no-vsync", &o->vsync, false}};
    for (int i = 1; i < argc; ++i) {
        bool matched = false;
        for (size_t f = 0; f < sizeof flags / sizeof flags[0]; ++f) {
            if (strcmp(argv[i], flags[f].name) == 0) {
                *flags[f].flag = flags[f].value;
                matched = true;
            }
        }
        if (matched) continue;
        const char* value = i + 1 < argc ? argv[i + 1] : NULL;
        bool ok = value != NULL;
        if (ok && strcmp(argv[i], "--width") == 0) ok = parse_u32(value, &o->width);
        else if (ok && strcmp(argv[i], "--height") == 0) ok = parse_u32(value, &o->height);
        else if (ok && strcmp(argv[i], "--scale") == 0) ok = parse_u32(value, &o->scale) && o->scale > 0;
        else if (ok && strcmp(argv[i], "--workers") == 0) ok = parse_u32(value, &o->workers);
        else if (ok && strcmp(argv[i], "--frames") == 0) ok = parse_u32(value, &o->frames);
        else if (ok && strcmp(argv[i], "--subdiv") == 0) ok = parse_u32(value, &o->subdiv) && o->subdiv <= WPE_MAX_SUBDIVISION_LEVELS;
        else if (ok && strcmp(argv[i], "--screenshot") == 0) o->screenshot = value;
        else if (ok && strcmp(argv[i], "--shadow-map") == 0) o->shadow_map = value;
        else if (ok && strcmp(argv[i], "--time") == 0) {
            char* end = NULL;
            o->time = strtof(value, &end);
            ok = *end == '\0';
            o->fixed_time = true;
        } else {
            ok = false;
        }
        if (!ok) {
            fprintf(stderr, "invalid argument: %s\n", argv[i]);
            return false;
        }
        ++i;
    }
    return true;
}

static WPE_Result spawn_with(WPE_Ecs* ecs, WPE_Vec3 position, WPE_Quat rotation, WPE_Vec3 scale, WPE_Entity* out) {
    const WPE_Entity e = wpe_ecs_spawn(ecs);
    if (e == WPE_ENTITY_NULL) return WPE_RESULT_ERROR_CAPACITY;
    WPE_Transform t = wpe_transform_identity();
    t.position = position;
    t.rotation = rotation;
    t.scale = scale;
    *out = e;
    return wpe_ecs_add_component(ecs, e, WPE_COMP_TRANSFORM, &t, NULL);
}

static WPE_Result place_model(WPE_Ecs* ecs, const WPE_Model* model, WPE_Vec3 position, float yaw, float scale,
                              const WPE_InstantiateOptions* options, WPE_Entity* out_root) {
    WPE_Result result = wpe_model_instantiate(ecs, model, options, out_root);
    if (result != WPE_RESULT_OK) return result;
    WPE_Transform* t = wpe_ecs_get_component(ecs, *out_root, WPE_COMP_TRANSFORM);
    t->position = position;
    t->rotation = wpe_quat_from_axis_angle(wpe_vec3(0, 1, 0), yaw);
    t->scale = wpe_vec3(scale, scale, scale);
    return WPE_RESULT_OK;
}

typedef struct Scene {
    WPE_Model* models[8];
    uint32_t model_count;
    WPE_Entity point_lights[3];
    WPE_Entity spot;
    WPE_Entity sun;
    WPE_Entity camera;
} Scene;

static WPE_Result track(Scene* scene, WPE_Result result, WPE_Model* model) {
    if (result == WPE_RESULT_OK) scene->models[scene->model_count++] = model;
    return result;
}

static WPE_Result build_scene(WPE_Ecs* ecs, Scene* scene, WPE_JobSystem* jobs) {
    WPE_Model *ground = NULL, *box = NULL, *helmet = NULL, *figure = NULL, *pillar = NULL;
    const WPE_Material ground_material = wpe_material_solid(wpe_vec4(0.55f, 0.55f, 0.58f, 1), 0.0f, 0.8f);
    const WPE_Material box_material = wpe_material_solid(wpe_vec4(0.8f, 0.35f, 0.2f, 1), 0.0f, 0.4f);
    const WPE_Material pillar_material = wpe_material_solid(wpe_vec4(0.75f, 0.78f, 0.82f, 1), 1.0f, 0.35f);
    WPE_Result r = track(scene, wpe_model_create_plane(40.0f, 24, &ground_material, &ground), ground);
    if (r == WPE_RESULT_OK) r = track(scene, wpe_model_create_box(wpe_vec3(1, 1, 1), &box_material, &box), box);
    if (r == WPE_RESULT_OK) r = track(scene, wpe_model_create_box(wpe_vec3(0.6f, 3.0f, 0.6f), &pillar_material, &pillar), pillar);
    const WPE_ModelLoadOptions load = {.jobs = jobs, .max_texture_size = 256};
    if (r == WPE_RESULT_OK) r = track(scene, wpe_model_load(WPE_SAMPLE_ASSETS_DIR "/meshes/damaged_helmet.glb", &load, &helmet), helmet);
    if (r == WPE_RESULT_OK) r = track(scene, wpe_model_load(WPE_SAMPLE_ASSETS_DIR "/meshes/RiggedFigure.glb", &load, &figure), figure);
    if (r != WPE_RESULT_OK) return r;

    WPE_Entity e;
    r = place_model(ecs, ground, wpe_vec3(0, 0, 0), 0, 1, NULL, &e);
    if (r == WPE_RESULT_OK) r = place_model(ecs, helmet, wpe_vec3(0, 1.2f, 0), 0.4f, 1.0f, NULL, &e);
    if (r == WPE_RESULT_OK) {
        r = place_model(ecs, figure, wpe_vec3(2.6f, 0, 0.8f), -0.6f, 1.0f, &(WPE_InstantiateOptions){.animation = 0, .play = true}, &e);
    }
    const float box_spots[5][3] = {{-2.5f, 0.5f, 1.5f}, {-3.2f, 0.5f, -1.2f}, {-2.2f, 1.5f, -1.0f}, {1.8f, 0.5f, -2.4f}, {3.5f, 0.5f, -1.0f}};
    for (int i = 0; i < 5 && r == WPE_RESULT_OK; ++i) {
        r = place_model(ecs, box, wpe_vec3(box_spots[i][0], box_spots[i][1], box_spots[i][2]), (float)i * 0.7f, 1.0f, NULL, &e);
    }
    for (int i = 0; i < 4 && r == WPE_RESULT_OK; ++i) {
        const float a = (float)i * WPE_PI * 0.5f + WPE_PI * 0.25f;
        r = place_model(ecs, pillar, wpe_vec3(cosf(a) * 6.0f, 1.5f, sinf(a) * 6.0f), 0, 1.0f, NULL, &e);
    }
    if (r != WPE_RESULT_OK) return r;

    /* Lights. */
    WPE_Light sun = wpe_light_default(WPE_LIGHT_DIRECTIONAL);
    sun.color = wpe_vec3(1.0f, 0.95f, 0.85f);
    sun.intensity = 0.9f;
    sun.cast_shadows = true;
    /* Tilted down 45 degrees and turned so shadows fall toward the camera's side of the scene. */
    const WPE_Quat sun_rotation = wpe_quat_mul(wpe_quat_from_axis_angle(wpe_vec3(0, 1, 0), 2.6f),
                                               wpe_quat_from_axis_angle(wpe_vec3(1, 0, 0), -0.8f));
    r = spawn_with(ecs, wpe_vec3(0, 10, 0), sun_rotation, wpe_vec3(1, 1, 1), &scene->sun);
    if (r == WPE_RESULT_OK) r = wpe_ecs_add_component(ecs, scene->sun, WPE_COMP_LIGHT, &sun, NULL);

    const WPE_Vec3 colors[3] = {wpe_vec3(1.0f, 0.3f, 0.2f), wpe_vec3(0.2f, 1.0f, 0.4f), wpe_vec3(0.3f, 0.4f, 1.0f)};
    for (int i = 0; i < 3 && r == WPE_RESULT_OK; ++i) {
        WPE_Light point = wpe_light_default(WPE_LIGHT_POINT);
        point.color = colors[i];
        point.intensity = 2.0f;
        point.range = 6.0f;
        r = spawn_with(ecs, wpe_vec3(0, 1, 0), wpe_quat_identity(), wpe_vec3(1, 1, 1), &scene->point_lights[i]);
        if (r == WPE_RESULT_OK) r = wpe_ecs_add_component(ecs, scene->point_lights[i], WPE_COMP_LIGHT, &point, NULL);
    }
    WPE_Light spot = wpe_light_default(WPE_LIGHT_SPOT);
    spot.color = wpe_vec3(1.0f, 0.9f, 0.6f);
    spot.intensity = 3.0f;
    spot.range = 14.0f;
    if (r == WPE_RESULT_OK) r = spawn_with(ecs, wpe_vec3(0, 7, 0), wpe_quat_identity(), wpe_vec3(1, 1, 1), &scene->spot);
    if (r == WPE_RESULT_OK) r = wpe_ecs_add_component(ecs, scene->spot, WPE_COMP_LIGHT, &spot, NULL);

    if (r == WPE_RESULT_OK) r = spawn_with(ecs, wpe_vec3(0, 0, 0), wpe_quat_identity(), wpe_vec3(1, 1, 1), &scene->camera);
    if (r == WPE_RESULT_OK) {
        const WPE_Camera camera = wpe_camera_default();
        r = wpe_ecs_add_component(ecs, scene->camera, WPE_COMP_CAMERA, &camera, NULL);
    }
    return r;
}

/* Moves lights and camera for scene time t. */
static void animate_scene(WPE_Ecs* ecs, const Scene* scene, float t) {
    for (int i = 0; i < 3; ++i) {
        WPE_Transform* lt = wpe_ecs_get_component(ecs, scene->point_lights[i], WPE_COMP_TRANSFORM);
        const float a = t * 0.8f + (float)i * 2.0944f;
        lt->position = wpe_vec3(cosf(a) * 3.5f, 0.8f + 0.4f * sinf(t * 1.3f + (float)i), sinf(a) * 3.5f);
    }
    WPE_Transform* st = wpe_ecs_get_component(ecs, scene->spot, WPE_COMP_TRANSFORM);
    /* Point the spot's -Z axis down and sweep it across the scene. */
    st->rotation = wpe_quat_mul(wpe_quat_from_axis_angle(wpe_vec3(0, 1, 0), t * 0.5f),
                                wpe_quat_from_axis_angle(wpe_vec3(1, 0, 0), -WPE_PI * 0.5f + 0.45f));

    WPE_Transform* ct = wpe_ecs_get_component(ecs, scene->camera, WPE_COMP_TRANSFORM);
    const float orbit = 0.6f + t * 0.1f;
    ct->position = wpe_vec3(sinf(orbit) * 8.5f, 4.0f, cosf(orbit) * 8.5f);
    const WPE_Mat4 view = wpe_mat4_look_at(ct->position, wpe_vec3(0, 1.0f, 0), wpe_vec3(0, 1, 0));
    WPE_Mat4 world;
    WPE_Vec3 p, s;
    if (wpe_mat4_inverse(&view, &world)) wpe_mat4_decompose(&world, &p, &ct->rotation, &s);
}

int main(int argc, char** argv) {
    Options o;
    if (!parse_options(argc, argv, &o)) return EXIT_FAILURE;

    WPE_JobSystem* jobs = NULL;
    WPE_Renderer* renderer = NULL;
    WPE_Window* window = NULL;
    WPE_Ecs* ecs = NULL;
    WPE_SceneRenderer* scene_renderer = NULL;
    WPE_PostChain* post = NULL;
    WPE_Draw2D* draw2d = NULL;
    WPE_BitmapFont* font = NULL;
    Scene scene = {0};
    int exit_code = EXIT_FAILURE;

    WPE_Result result = wpe_jobs_create(o.workers, &jobs);
    if (result == WPE_RESULT_OK) result = wpe_renderer_create(&(WPE_RendererDesc){.width = o.width, .height = o.height, .jobs = jobs}, &renderer);
    if (result == WPE_RESULT_OK) {
        result = wpe_window_create(&(WPE_WindowDesc){.title = "WillPowerEngine - Lighting", .width = o.width * o.scale,
                                                     .height = o.height * o.scale, .resizable = true, .vsync = o.vsync},
                                   &window);
    }
    if (result == WPE_RESULT_OK) result = wpe_scene_renderer_create(&scene_renderer);
    if (result == WPE_RESULT_OK) result = wpe_postfx_create(&post);
    if (result == WPE_RESULT_OK) result = wpe_draw2d_create(&draw2d);
    if (result == WPE_RESULT_OK) result = wpe_font_load(WPE_SAMPLE_ASSETS_DIR "/fonts/small.png", &font);
    if (result == WPE_RESULT_OK) result = wpe_ecs_create(&ecs);
    if (result == WPE_RESULT_OK) result = wpe_scene_register_components(ecs);
    if (result == WPE_RESULT_OK) result = wpe_graphics_register_components(ecs);
    if (result == WPE_RESULT_OK) result = build_scene(ecs, &scene, jobs);
    if (result != WPE_RESULT_OK) {
        wpe_log_error("startup failed: %s", wpe_result_to_string(result));
        goto cleanup;
    }

    WPE_SceneRenderSettings settings = wpe_scene_render_settings_default();
    settings.ambient = wpe_vec3(0.16f, 0.17f, 0.21f);
    settings.shadows.enabled = o.shadows;
    settings.shadows.distance = 22.0f;
    settings.dither_transparency = o.dither;
    settings.subdivision_levels = o.subdiv;
    if (o.fog) settings.fog = (WPE_FogParams){.mode = WPE_FOG_LINEAR, .color = wpe_vec3(0.18f, 0.2f, 0.26f), .start = 6.0f, .end = 22.0f};

    WPE_PostEffect effects[5] = {wpe_postfx_ssao(NULL), wpe_postfx_chromatic_aberration(NULL), wpe_postfx_vignette(NULL),
                                 wpe_postfx_color_adjust(NULL), wpe_postfx_ps1(NULL)};
    effects[0].enabled = o.ssao;
    effects[1].enabled = o.chroma;
    effects[2].enabled = o.vignette;
    effects[3].enabled = false;
    effects[4].enabled = o.ps1;
    const WPE_Camera* camera_params = wpe_ecs_get_component(ecs, scene.camera, WPE_COMP_CAMERA);
    const WPE_PostCamera post_camera = {.near_z = camera_params->near_z, .far_z = camera_params->far_z};
    const WPE_Color background = o.fog ? wpe_color_from_vec4(wpe_vec4(0.18f, 0.2f, 0.26f, 1)) : wpe_color_rgba8(30, 34, 46, 255);

    char hud[160] = "";
    double start = wpe_platform_time_seconds(), previous = start, stats_start = start, render_ms = 0.0;
    uint32_t stats_frames = 0;
    for (uint32_t frame = 0; o.frames == 0 || frame < o.frames; ++frame) {
        if (!wpe_window_poll_events(window)) break;
        const double now = wpe_platform_time_seconds();
        const float t = o.fixed_time ? o.time : (float)(now - start);
        const float dt = o.fixed_time ? 0.0f : (float)(now - previous);
        previous = now;

        if (o.fixed_time && frame == 0) {
            /* Pose animated models at the fixed time. */
            WPE_EcsIter it = wpe_ecs_query(ecs, (WPE_EcsQuery){.all = WPE_ECS_MASK(WPE_COMP_ANIMATOR)});
            while (wpe_ecs_iter_next(&it)) {
                WPE_Animator* animator = wpe_ecs_iter_get(&it, WPE_COMP_ANIMATOR);
                animator->time = fmodf(t, animator->model->animations[animator->animation].duration);
                wpe_animation_apply(ecs, animator);
            }
        }
        animate_scene(ecs, &scene, t);
        result = wpe_animation_update(ecs, dt);
        if (result == WPE_RESULT_OK) result = wpe_scene_update_transforms(ecs);

        const double render_start = wpe_platform_time_seconds();
        WPE_SceneRenderStats scene_stats = {0};
        if (result == WPE_RESULT_OK) {
            result = wpe_renderer_begin_frame(renderer, &(WPE_ClearDesc){.clear_color = true, .color = background,
                                                                         .clear_depth = true, .depth = 1.0f});
        }
        if (result == WPE_RESULT_OK) result = wpe_scene_render(scene_renderer, renderer, ecs, scene.camera, &settings, &scene_stats);
        const WPE_Result end_result = wpe_renderer_end_frame(renderer);
        if (result == WPE_RESULT_OK) result = end_result;
        const WPE_RenderStats scene_frame_stats = wpe_renderer_stats(renderer); /* Before the HUD frame replaces them. */
        if (result == WPE_RESULT_OK) result = wpe_postfx_apply(post, renderer, effects, 5, &post_camera);

        /* HUD in a second, non-clearing frame so post effects do not touch the text. */
        if (result == WPE_RESULT_OK && o.hud) {
            result = wpe_renderer_begin_frame(renderer, NULL);
            wpe_draw2d_begin(draw2d, o.width, o.height);
            wpe_draw2d_rect(draw2d, (WPE_Rect){0, 0, (float)o.width, 9}, wpe_color_rgba8(0, 0, 0, 140));
            wpe_draw2d_text(draw2d, font, 2, 2, hud, 0xFFFFFFFFu, 1.0f);
            if (result == WPE_RESULT_OK) result = wpe_draw2d_submit(draw2d, renderer);
            const WPE_Result hud_end = wpe_renderer_end_frame(renderer);
            if (result == WPE_RESULT_OK) result = hud_end;
        }
        render_ms += (wpe_platform_time_seconds() - render_start) * 1000.0;
        if (result == WPE_RESULT_OK) result = wpe_window_present(window, wpe_renderer_framebuffer(renderer));
        if (result != WPE_RESULT_OK) {
            wpe_log_error("frame %u failed: %s", frame, wpe_result_to_string(result));
            goto cleanup;
        }

        ++stats_frames;
        if (frame == 0 || now - stats_start >= 0.5) {
            snprintf(hud, sizeof hud, "%.1f ms  %u tris  %u subdiv  %u casters  %u lights", render_ms / stats_frames,
                     scene_frame_stats.triangles_rasterized, scene_frame_stats.triangles_subdivided,
                     scene_stats.shadow_casters, scene_stats.lights);
            stats_start = now;
            render_ms = 0.0;
            stats_frames = 0;
        }
    }

    if (o.screenshot) {
        const WPE_Framebuffer* fb = wpe_renderer_framebuffer(renderer);
        if (wpe_image_save_png(o.screenshot, fb->width, fb->height, fb->color) != WPE_RESULT_OK) goto cleanup;
        wpe_log_info("saved screenshot to %s", o.screenshot);
    }
    const WPE_ShadowSampler* shadow = wpe_scene_renderer_shadow_map(scene_renderer);
    if (o.shadow_map && shadow) {
        /* Grayscale depth visualization: near = dark, far/empty = white. */
        WPE_Color* pixels = wpe_alloc((size_t)shadow->size * shadow->size * sizeof *pixels);
        if (!pixels) goto cleanup;
        for (size_t i = 0; i < (size_t)shadow->size * shadow->size; ++i) {
            const uint8_t v = (uint8_t)(shadow->depth[i] * 255.0f);
            pixels[i] = wpe_color_rgba8(v, v, v, 255);
        }
        const WPE_Result saved = wpe_image_save_png(o.shadow_map, shadow->size, shadow->size, pixels);
        wpe_free(pixels);
        if (saved != WPE_RESULT_OK) goto cleanup;
        wpe_log_info("saved shadow map to %s", o.shadow_map);
    }
    exit_code = EXIT_SUCCESS;

cleanup:
    wpe_ecs_destroy(ecs);
    for (uint32_t i = 0; i < scene.model_count; ++i) wpe_model_destroy(scene.models[i]);
    wpe_font_destroy(font);
    wpe_draw2d_destroy(draw2d);
    wpe_postfx_destroy(post);
    wpe_scene_renderer_destroy(scene_renderer);
    wpe_window_destroy(window);
    wpe_renderer_destroy(renderer);
    wpe_jobs_destroy(jobs);
    return exit_code;
}
