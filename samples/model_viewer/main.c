/*
 * Model viewer: loads a glTF/GLB model, frames it with an orbiting camera, plays its first
 * animation and shows a text overlay.
 *
 * Usage: model_viewer [model.glb] [--width N] [--height N] [--scale N] [--workers N] [--frames N]
 *                     [--screenshot file.png] [--max-texture N] [--yaw DEGREES] [--pitch DEGREES]
 *                     [--distance FACTOR] [--time SECONDS] [--dither] [--no-hud] [--no-vsync]
 *   --frames N        exit after N frames (0 = run until the window is closed)
 *   --screenshot F    save the last frame as PNG; also freezes the orbit/animation clock so
 *                     captures are reproducible (use --yaw/--time to choose the view and pose)
 */
#define WPE_LOG_MODULE "viewer"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "assets/font.h"
#include "assets/model.h"
#include "core/core.h"
#include "graphics/animation.h"
#include "graphics/draw2d.h"
#include "graphics/graphics.h"
#include "graphics/scene_render.h"
#include "jobs/jobs.h"
#include "platform/platform.h"

typedef struct Options {
    const char* model_path;
    uint32_t width, height, scale, workers, frames, max_texture;
    const char* screenshot;
    float yaw, pitch, distance, time;
    bool dither, hud, vsync;
} Options;

static bool parse_float(const char* text, float* out) {
    char* end = NULL;
    const float value = strtof(text, &end);
    if (!text[0] || *end != '\0' || !isfinite(value)) return false;
    *out = value;
    return true;
}

static bool parse_u32(const char* text, uint32_t* out) {
    char* end = NULL;
    const unsigned long value = strtoul(text, &end, 10);
    if (!text[0] || *end != '\0' || value > UINT32_MAX) return false;
    *out = (uint32_t)value;
    return true;
}

static bool parse_options(int argc, char** argv, Options* o) {
    *o = (Options){.model_path = WPE_SAMPLE_ASSETS_DIR "/meshes/damaged_helmet.glb", .width = 320, .height = 240,
                   .scale = 3, .workers = WPE_JOBS_AUTO_WORKERS, .max_texture = 512, .pitch = 15.0f,
                   .distance = 1.0f, .hud = true, .vsync = true};
    for (int i = 1; i < argc; ++i) {
        const char* arg = argv[i];
        if (arg[0] != '-') {
            o->model_path = arg;
            continue;
        }
        if (strcmp(arg, "--dither") == 0) {
            o->dither = true;
            continue;
        }
        if (strcmp(arg, "--no-hud") == 0) {
            o->hud = false;
            continue;
        }
        if (strcmp(arg, "--no-vsync") == 0) {
            o->vsync = false;
            continue;
        }
        const char* value = i + 1 < argc ? argv[++i] : NULL;
        bool ok = value != NULL;
        if (!ok) {
        } else if (strcmp(arg, "--width") == 0) {
            ok = parse_u32(value, &o->width);
        } else if (strcmp(arg, "--height") == 0) {
            ok = parse_u32(value, &o->height);
        } else if (strcmp(arg, "--scale") == 0) {
            ok = parse_u32(value, &o->scale) && o->scale > 0;
        } else if (strcmp(arg, "--workers") == 0) {
            ok = parse_u32(value, &o->workers);
        } else if (strcmp(arg, "--frames") == 0) {
            ok = parse_u32(value, &o->frames);
        } else if (strcmp(arg, "--max-texture") == 0) {
            ok = parse_u32(value, &o->max_texture);
        } else if (strcmp(arg, "--screenshot") == 0) {
            o->screenshot = value;
        } else if (strcmp(arg, "--yaw") == 0) {
            ok = parse_float(value, &o->yaw);
        } else if (strcmp(arg, "--pitch") == 0) {
            ok = parse_float(value, &o->pitch);
        } else if (strcmp(arg, "--distance") == 0) {
            ok = parse_float(value, &o->distance) && o->distance > 0.0f;
        } else if (strcmp(arg, "--time") == 0) {
            ok = parse_float(value, &o->time);
        } else {
            ok = false;
        }
        if (!ok) {
            fprintf(stderr, "invalid argument: %s\n", arg);
            return false;
        }
    }
    return true;
}

static void grow_bounds(WPE_Aabb* box, WPE_Vec3 p) {
    box->min = wpe_vec3(fminf(box->min.x, p.x), fminf(box->min.y, p.y), fminf(box->min.z, p.z));
    box->max = wpe_vec3(fmaxf(box->max.x, p.x), fmaxf(box->max.y, p.y), fmaxf(box->max.z, p.z));
}

/*
 * World-space bounds of all meshes. Skinned meshes use their joints' world positions: their
 * vertices only reach world space through the skin, so bind-pose bounds can be in any frame.
 */
static WPE_Aabb scene_bounds(WPE_Ecs* ecs) {
    WPE_Aabb total = {.min = wpe_vec3(INFINITY, INFINITY, INFINITY), .max = wpe_vec3(-INFINITY, -INFINITY, -INFINITY)};
    WPE_EcsIter it = wpe_ecs_query(ecs, (WPE_EcsQuery){.all = WPE_ECS_MASK(WPE_COMP_MESH) | WPE_ECS_MASK(WPE_COMP_TRANSFORM)});
    while (wpe_ecs_iter_next(&it)) {
        const WPE_MeshRenderer* mesh = wpe_ecs_iter_get(&it, WPE_COMP_MESH);
        const WPE_Transform* t = wpe_ecs_iter_get(&it, WPE_COMP_TRANSFORM);
        if (mesh->skin != WPE_MODEL_NONE && mesh->joints) {
            for (uint32_t j = 0; j < mesh->joint_count; ++j) {
                const WPE_Transform* joint = wpe_ecs_get_component(ecs, mesh->joints[j], WPE_COMP_TRANSFORM);
                if (joint) grow_bounds(&total, wpe_vec3(joint->world.m[3][0], joint->world.m[3][1], joint->world.m[3][2]));
            }
            continue;
        }
        const WPE_Aabb b = mesh->model->meshes[mesh->mesh].bounds;
        const WPE_Mat4 world = t->world;
        for (int c = 0; c < 8; ++c) {
            const WPE_Vec3 corner = wpe_vec3(c & 1 ? b.max.x : b.min.x, c & 2 ? b.max.y : b.min.y, c & 4 ? b.max.z : b.min.z);
            grow_bounds(&total, wpe_mat4_transform_point(&world, corner));
        }
    }
    if (!(total.min.x <= total.max.x)) return (WPE_Aabb){.min = wpe_vec3(-1, -1, -1), .max = wpe_vec3(1, 1, 1)};
    /* Joints sit inside the skin, so pad by a fraction of the extent to include the surface. */
    const WPE_Vec3 pad = wpe_vec3_scale(wpe_vec3_sub(total.max, total.min), 0.15f);
    total.min = wpe_vec3_sub(total.min, pad);
    total.max = wpe_vec3_add(total.max, pad);
    return total;
}

static void place_camera(WPE_Ecs* ecs, WPE_Entity camera, WPE_Vec3 target, float radius, float yaw_deg, float pitch_deg) {
    const float yaw = yaw_deg * WPE_PI / 180.0f, pitch = pitch_deg * WPE_PI / 180.0f;
    const WPE_Vec3 offset = wpe_vec3(sinf(yaw) * cosf(pitch) * radius, sinf(pitch) * radius, cosf(yaw) * cosf(pitch) * radius);
    WPE_Transform* t = wpe_ecs_get_component(ecs, camera, WPE_COMP_TRANSFORM);
    t->position = wpe_vec3_add(target, offset);
    /* Camera looks down -Z: build the rotation from the inverse of a look-at view matrix. */
    const WPE_Mat4 view = wpe_mat4_look_at(t->position, target, wpe_vec3(0, 1, 0));
    WPE_Mat4 world;
    WPE_Vec3 position, scale;
    if (wpe_mat4_inverse(&view, &world) && wpe_mat4_decompose(&world, &position, &t->rotation, &scale)) return;
    t->rotation = wpe_quat_identity();
}

int main(int argc, char** argv) {
    Options o;
    if (!parse_options(argc, argv, &o)) return EXIT_FAILURE;

    WPE_JobSystem* jobs = NULL;
    WPE_Renderer* renderer = NULL;
    WPE_Window* window = NULL;
    WPE_Ecs* ecs = NULL;
    WPE_Model* model = NULL;
    WPE_BitmapFont* font = NULL;
    WPE_SceneRenderer* scene_renderer = NULL;
    WPE_Draw2D* draw2d = NULL;
    int exit_code = EXIT_FAILURE;

    WPE_Result result = wpe_jobs_create(o.workers, &jobs);
    if (result == WPE_RESULT_OK) {
        const double start = wpe_platform_time_seconds();
        result = wpe_model_load(o.model_path, &(WPE_ModelLoadOptions){.jobs = jobs, .max_texture_size = o.max_texture}, &model);
        if (result == WPE_RESULT_OK) wpe_log_info("loaded %s in %.0f ms", o.model_path, (wpe_platform_time_seconds() - start) * 1000.0);
    }
    if (result == WPE_RESULT_OK) result = wpe_font_load(WPE_SAMPLE_ASSETS_DIR "/fonts/small.png", &font);
    if (result == WPE_RESULT_OK) result = wpe_renderer_create(&(WPE_RendererDesc){.width = o.width, .height = o.height, .jobs = jobs}, &renderer);
    if (result == WPE_RESULT_OK) {
        result = wpe_window_create(&(WPE_WindowDesc){.title = "WillPowerEngine - Model Viewer", .width = o.width * o.scale,
                                                     .height = o.height * o.scale, .resizable = true, .vsync = o.vsync},
                                   &window);
    }
    if (result == WPE_RESULT_OK) result = wpe_scene_renderer_create(&scene_renderer);
    if (result == WPE_RESULT_OK) result = wpe_draw2d_create(&draw2d);
    if (result == WPE_RESULT_OK) result = wpe_ecs_create(&ecs);
    if (result == WPE_RESULT_OK) result = wpe_scene_register_components(ecs);
    if (result == WPE_RESULT_OK) result = wpe_graphics_register_components(ecs);

    WPE_Entity root = WPE_ENTITY_NULL, camera = WPE_ENTITY_NULL;
    if (result == WPE_RESULT_OK) {
        const WPE_InstantiateOptions instantiate = {.animation = model->animation_count ? 0 : WPE_MODEL_NONE,
                                                    .play = !o.screenshot};
        result = wpe_model_instantiate(ecs, model, &instantiate, &root);
    }
    if (result == WPE_RESULT_OK) {
        camera = wpe_ecs_spawn(ecs);
        const WPE_Transform t = wpe_transform_identity();
        const WPE_Camera c = wpe_camera_default();
        result = wpe_ecs_add_component(ecs, camera, WPE_COMP_TRANSFORM, &t, NULL);
        if (result == WPE_RESULT_OK) result = wpe_ecs_add_component(ecs, camera, WPE_COMP_CAMERA, &c, NULL);
    }
    if (result != WPE_RESULT_OK) {
        wpe_log_error("startup failed: %s", wpe_result_to_string(result));
        goto cleanup;
    }

    /* Pose the animation at --time, then frame the model's bounds. */
    WPE_Animator* animator = wpe_ecs_get_component(ecs, root, WPE_COMP_ANIMATOR);
    if (animator) {
        animator->time = o.time;
        wpe_animation_apply(ecs, animator);
    }
    if (wpe_scene_update_transforms(ecs) != WPE_RESULT_OK) goto cleanup;
    const WPE_Aabb bounds = scene_bounds(ecs);
    const WPE_Vec3 center = wpe_vec3_scale(wpe_vec3_add(bounds.min, bounds.max), 0.5f);
    const float radius = wpe_vec3_length(wpe_vec3_sub(bounds.max, bounds.min)) * 1.1f * o.distance;
    WPE_Camera* cam = wpe_ecs_get_component(ecs, camera, WPE_COMP_CAMERA);
    cam->near_z = radius * 0.01f;
    cam->far_z = radius * 10.0f;

    WPE_SceneRenderSettings settings = wpe_scene_render_settings_default();
    settings.dither_transparency = o.dither;
    char hud[256] = "";
    double previous = wpe_platform_time_seconds(), stats_start = previous, render_ms = 0.0;
    uint32_t stats_frames = 0;
    float yaw = o.yaw;

    for (uint32_t frame = 0; o.frames == 0 || frame < o.frames; ++frame) {
        if (!wpe_window_poll_events(window)) break;
        const double now = wpe_platform_time_seconds();
        const float dt = o.screenshot ? 0.0f : (float)(now - previous);
        previous = now;
        yaw += dt * 20.0f;

        result = wpe_animation_update(ecs, dt);
        if (result == WPE_RESULT_OK) result = wpe_scene_update_transforms(ecs);
        place_camera(ecs, camera, center, radius, yaw, o.pitch);
        if (result == WPE_RESULT_OK) result = wpe_scene_update_transforms(ecs);

        const double render_start = wpe_platform_time_seconds();
        WPE_SceneRenderStats scene_stats = {0};
        if (result == WPE_RESULT_OK) {
            result = wpe_renderer_begin_frame(renderer, &(WPE_ClearDesc){.clear_color = true, .color = wpe_color_rgba8(36, 40, 54, 255),
                                                                         .clear_depth = true, .depth = 1.0f});
        }
        if (result == WPE_RESULT_OK) result = wpe_scene_render(scene_renderer, renderer, ecs, camera, &settings, &scene_stats);
        if (result == WPE_RESULT_OK && o.hud) {
            wpe_draw2d_begin(draw2d, o.width, o.height);
            wpe_draw2d_rect(draw2d, (WPE_Rect){0, 0, (float)o.width, 9}, wpe_color_rgba8(0, 0, 0, 140));
            wpe_draw2d_text(draw2d, font, 2, 2, hud, wpe_color_rgba8(255, 255, 255, 255), 1.0f);
            result = wpe_draw2d_submit(draw2d, renderer);
        }
        const WPE_Result end_result = wpe_renderer_end_frame(renderer);
        if (result == WPE_RESULT_OK) result = end_result;
        render_ms += (wpe_platform_time_seconds() - render_start) * 1000.0;
        if (result == WPE_RESULT_OK) result = wpe_window_present(window, wpe_renderer_framebuffer(renderer));
        if (result != WPE_RESULT_OK) {
            wpe_log_error("frame %u failed: %s", frame, wpe_result_to_string(result));
            goto cleanup;
        }

        ++stats_frames;
        if (frame == 0 || now - stats_start >= 0.5) {
            const WPE_RenderStats rs = wpe_renderer_stats(renderer);
            snprintf(hud, sizeof hud, "%.1f ms  %u tris  %u draws (%u culled)", render_ms / stats_frames,
                     rs.triangles_rasterized, scene_stats.primitives_submitted, scene_stats.primitives_culled);
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
    exit_code = EXIT_SUCCESS;

cleanup:
    wpe_ecs_destroy(ecs);
    wpe_draw2d_destroy(draw2d);
    wpe_scene_renderer_destroy(scene_renderer);
    wpe_window_destroy(window);
    wpe_renderer_destroy(renderer);
    wpe_font_destroy(font);
    wpe_model_destroy(model);
    wpe_jobs_destroy(jobs);
    return exit_code;
}
