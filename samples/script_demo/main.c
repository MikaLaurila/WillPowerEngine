/*
 * Script demo: scene behavior written in Luau (the .luau files in assets/scripts) driven by a fixed-timestep
 * game loop with rigid body physics. Edit the scripts while it runs with --watch to see hot reload.
 *
 * Usage: script_demo [--width N] [--height N] [--scale N] [--frames N] [--screenshot file.png]
 *                    [--watch] [--debug-physics] [--no-vsync]
 *   --watch          poll script files twice a second and hot-reload changes (state is preserved)
 *   --debug-physics  draw collider wireframes
 */
#define WPE_LOG_MODULE "scriptdemo"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "assets/font.h"
#include "assets/procedural.h"
#include "core/core.h"
#include "graphics/animation.h"
#include "graphics/debug_lines.h"
#include "graphics/draw2d.h"
#include "graphics/graphics.h"
#include "graphics/scene_render.h"
#include "physics/physics.h"
#include "platform/platform.h"
#include "script/script.h"

typedef struct Options {
    uint32_t width, height, scale, frames;
    const char* screenshot;
    bool watch, debug_physics, vsync;
} Options;

static bool parse_options(int argc, char** argv, Options* o) {
    *o = (Options){.width = 320, .height = 240, .scale = 3, .vsync = true};
    for (int i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "--watch") == 0) {
            o->watch = true;
        } else if (strcmp(argv[i], "--debug-physics") == 0) {
            o->debug_physics = true;
        } else if (strcmp(argv[i], "--no-vsync") == 0) {
            o->vsync = false;
        } else if (i + 1 < argc && strcmp(argv[i], "--screenshot") == 0) {
            o->screenshot = argv[++i];
        } else if (i + 1 < argc && (strcmp(argv[i], "--width") == 0 || strcmp(argv[i], "--height") == 0 ||
                                    strcmp(argv[i], "--scale") == 0 || strcmp(argv[i], "--frames") == 0)) {
            char* end = NULL;
            const unsigned long value = strtoul(argv[i + 1], &end, 10);
            if (*end != '\0' || value > 100000) {
                fprintf(stderr, "invalid value for %s\n", argv[i]);
                return false;
            }
            uint32_t* target = argv[i][2] == 'w' ? &o->width : argv[i][2] == 'h' ? &o->height : argv[i][2] == 's' ? &o->scale : &o->frames;
            *target = (uint32_t)value;
            ++i;
        } else {
            fprintf(stderr, "invalid argument: %s\n", argv[i]);
            return false;
        }
    }
    return o->width > 0 && o->height > 0 && o->scale > 0;
}

static WPE_Entity spawn_at(WPE_Ecs* ecs, WPE_Vec3 position, WPE_Result* result) {
    const WPE_Entity e = wpe_ecs_spawn(ecs);
    WPE_Transform t = wpe_transform_identity();
    t.position = position;
    if (*result == WPE_RESULT_OK) *result = e != WPE_ENTITY_NULL ? WPE_RESULT_OK : WPE_RESULT_ERROR_CAPACITY;
    if (*result == WPE_RESULT_OK) *result = wpe_ecs_add_component(ecs, e, WPE_COMP_TRANSFORM, &t, NULL);
    return e;
}

static void add_debug_line(WPE_Vec3 a, WPE_Vec3 b, uint32_t color, void* user_data) {
    wpe_debug_lines_add(user_data, a, b, 0xFF000000u | color);
}

int main(int argc, char** argv) {
    Options o;
    if (!parse_options(argc, argv, &o)) return EXIT_FAILURE;

    WPE_JobSystem* jobs = NULL;
    WPE_Renderer* renderer = NULL;
    WPE_Window* window = NULL;
    WPE_Ecs* ecs = NULL;
    WPE_Physics* physics = NULL;
    WPE_ScriptHost* host = NULL;
    WPE_SceneRenderer* scene_renderer = NULL;
    WPE_Draw2D* draw2d = NULL;
    WPE_DebugLines* debug_lines = NULL;
    WPE_BitmapFont* font = NULL;
    WPE_Model *ground = NULL, *box = NULL, *helmet = NULL, *figure = NULL;
    int exit_code = EXIT_FAILURE;

    WPE_Result r = wpe_jobs_create(WPE_JOBS_AUTO_WORKERS, &jobs);
    if (r == WPE_RESULT_OK) r = wpe_renderer_create(&(WPE_RendererDesc){.width = o.width, .height = o.height, .jobs = jobs}, &renderer);
    if (r == WPE_RESULT_OK) {
        r = wpe_window_create(&(WPE_WindowDesc){.title = "WillPowerEngine - Scripting", .width = o.width * o.scale,
                                                .height = o.height * o.scale, .resizable = true, .vsync = o.vsync},
                              &window);
    }
    if (r == WPE_RESULT_OK) r = wpe_scene_renderer_create(&scene_renderer);
    if (r == WPE_RESULT_OK) r = wpe_draw2d_create(&draw2d);
    if (r == WPE_RESULT_OK) r = wpe_debug_lines_create(&debug_lines);
    if (r == WPE_RESULT_OK) r = wpe_font_load(WPE_SAMPLE_ASSETS_DIR "/fonts/small.png", &font);
    if (r == WPE_RESULT_OK) r = wpe_ecs_create(&ecs);
    if (r == WPE_RESULT_OK) r = wpe_scene_register_components(ecs);
    if (r == WPE_RESULT_OK) r = wpe_graphics_register_components(ecs);
    if (r == WPE_RESULT_OK) r = wpe_physics_create(&(WPE_PhysicsDesc){.ecs = ecs, .gravity = wpe_vec3(0, -9.81f, 0)}, &physics);
    if (r == WPE_RESULT_OK) {
        r = wpe_script_host_create(&(WPE_ScriptHostDesc){.ecs = ecs, .physics = physics, .asset_root = WPE_SAMPLE_ASSETS_DIR,
                                                         .jobs = jobs, .max_texture_size = 256},
                                   &host);
    }

    /* Assets: procedural ground and box, imported helmet and figure (registered as script prefabs). */
    const WPE_Material ground_material = wpe_material_solid(wpe_vec4(0.5f, 0.52f, 0.55f, 1), 0.0f, 0.8f);
    const WPE_Material box_material = wpe_material_solid(wpe_vec4(0.9f, 0.55f, 0.15f, 1), 0.0f, 0.5f);
    if (r == WPE_RESULT_OK) r = wpe_model_create_plane(30.0f, 20, &ground_material, &ground);
    if (r == WPE_RESULT_OK) r = wpe_model_create_box(wpe_vec3(1, 1, 1), &box_material, &box);
    const WPE_ModelLoadOptions load = {.jobs = jobs, .max_texture_size = 256};
    if (r == WPE_RESULT_OK) r = wpe_model_load(WPE_SAMPLE_ASSETS_DIR "/meshes/damaged_helmet.glb", &load, &helmet);
    if (r == WPE_RESULT_OK) r = wpe_model_load(WPE_SAMPLE_ASSETS_DIR "/meshes/RiggedFigure.glb", &load, &figure);
    if (r == WPE_RESULT_OK) r = wpe_script_host_register_prefab(host, "box", box);
    if (r == WPE_RESULT_OK) r = wpe_script_host_register_prefab(host, "figure", figure);

    /* Scene: everything that moves is driven by scripts and physics. */
    WPE_Entity e = WPE_ENTITY_NULL;
    if (r == WPE_RESULT_OK) r = wpe_model_instantiate(ecs, ground, NULL, &e);
    if (r == WPE_RESULT_OK) {
        WPE_Collider floor = wpe_collider_default(WPE_COLLIDER_BOX); /* Static: no rigid body. */
        floor.size = wpe_vec3(30, 1, 30);
        floor.offset = wpe_vec3(0, -0.5f, 0);
        r = wpe_ecs_add_component(ecs, e, WPE_COMP_PHYSICS_COLLIDER, &floor, NULL);
    }
    for (int i = 0; i < 2 && r == WPE_RESULT_OK; ++i) {
        r = wpe_model_instantiate(ecs, helmet, NULL, &e);
        if (r == WPE_RESULT_OK) {
            ((WPE_Transform*)wpe_ecs_get_component(ecs, e, WPE_COMP_TRANSFORM))->position = wpe_vec3(i ? 1.4f : -1.4f, 1.2f, 0);
            r = wpe_script_attach(host, e, "scripts/spinner.luau");
        }
        /* Kinematic: the spinner script turns the helmets and they bat falling boxes away. */
        const WPE_RigidBody body = wpe_rigidbody_default(WPE_BODY_KINEMATIC);
        WPE_Collider sphere = wpe_collider_default(WPE_COLLIDER_SPHERE);
        sphere.size.x = 0.85f;
        if (r == WPE_RESULT_OK) r = wpe_ecs_add_component(ecs, e, WPE_COMP_PHYSICS_RIGIDBODY, &body, NULL);
        if (r == WPE_RESULT_OK) r = wpe_ecs_add_component(ecs, e, WPE_COMP_PHYSICS_COLLIDER, &sphere, NULL);
    }
    for (int i = 0; i < 3 && r == WPE_RESULT_OK; ++i) {
        const WPE_Entity light = spawn_at(ecs, wpe_vec3(0, 1, 0), &r);
        if (r == WPE_RESULT_OK) r = wpe_script_attach(host, light, "scripts/orbit_light.luau");
    }
    const WPE_Entity spawner = spawn_at(ecs, wpe_vec3(0, 0, 0), &r);
    if (r == WPE_RESULT_OK) r = wpe_script_attach(host, spawner, "scripts/spawner.luau");

    const WPE_Entity sun = spawn_at(ecs, wpe_vec3(0, 8, 0), &r);
    if (r == WPE_RESULT_OK) {
        WPE_Light light = wpe_light_default(WPE_LIGHT_DIRECTIONAL);
        light.intensity = 0.8f;
        light.cast_shadows = true;
        ((WPE_Transform*)wpe_ecs_get_component(ecs, sun, WPE_COMP_TRANSFORM))->rotation =
            wpe_quat_mul(wpe_quat_from_axis_angle(wpe_vec3(0, 1, 0), 2.4f), wpe_quat_from_axis_angle(wpe_vec3(1, 0, 0), -0.9f));
        r = wpe_ecs_add_component(ecs, sun, WPE_COMP_LIGHT, &light, NULL);
    }
    const WPE_Entity camera = spawn_at(ecs, wpe_vec3(0, 0, 0), &r);
    if (r == WPE_RESULT_OK) {
        WPE_Transform* t = wpe_ecs_get_component(ecs, camera, WPE_COMP_TRANSFORM);
        const WPE_Mat4 view = wpe_mat4_look_at(wpe_vec3(5.5f, 4.5f, 7.0f), wpe_vec3(0, 1.0f, 0), wpe_vec3(0, 1, 0));
        WPE_Mat4 world;
        WPE_Vec3 s;
        if (wpe_mat4_inverse(&view, &world)) wpe_mat4_decompose(&world, &t->position, &t->rotation, &s);
        const WPE_Camera cam = wpe_camera_default();
        r = wpe_ecs_add_component(ecs, camera, WPE_COMP_CAMERA, &cam, NULL);
    }
    if (r != WPE_RESULT_OK) {
        wpe_log_error("startup failed: %s", wpe_result_to_string(r));
        goto cleanup;
    }

    WPE_SceneRenderSettings settings = wpe_scene_render_settings_default();
    settings.ambient = wpe_vec3(0.16f, 0.17f, 0.21f);
    settings.shadows.distance = 18.0f;
    WPE_FixedStep clock;
    wpe_fixed_step_init(&clock, 1.0 / 60.0, 8);

    char hud[160] = "measuring...";
    double previous = wpe_platform_time_seconds(), last_watch = previous, stats_start = previous;
    uint32_t stats_frames = 0;
    for (uint32_t frame = 0; o.frames == 0 || frame < o.frames; ++frame) {
        if (!wpe_window_poll_events(window)) break;
        const double now = wpe_platform_time_seconds();
        /* Screenshots use a fixed frame time so captures are reproducible. */
        const double dt = o.screenshot ? 1.0 / 30.0 : now - previous;
        previous = now;
        if (o.watch && now - last_watch > 0.5) {
            wpe_script_host_reload_changed(host);
            last_watch = now;
        }

        /* Game loop: fixed-step simulation (scripts, physics, collision callbacks), then per-frame
         * updates, then rendering. */
        const uint32_t steps = wpe_fixed_step_advance(&clock, dt);
        for (uint32_t s = 0; s < steps && r == WPE_RESULT_OK; ++s) {
            wpe_script_host_fixed_update(host, (float)clock.step);
            r = wpe_physics_step(physics, (float)clock.step);
            wpe_script_host_dispatch_physics(host);
        }
        wpe_script_host_update(host, (float)dt);
        if (r == WPE_RESULT_OK) r = wpe_animation_update(ecs, (float)dt);
        if (r == WPE_RESULT_OK) r = wpe_scene_update_transforms(ecs);
        if (r == WPE_RESULT_OK) {
            r = wpe_renderer_begin_frame(renderer, &(WPE_ClearDesc){.clear_color = true, .color = wpe_color_rgba8(28, 32, 44, 255),
                                                                    .clear_depth = true, .depth = 1.0f});
        }
        if (r == WPE_RESULT_OK) r = wpe_scene_render(scene_renderer, renderer, ecs, camera, &settings, NULL);
        WPE_Mat4 view, projection;
        if (r == WPE_RESULT_OK && o.debug_physics &&
            wpe_scene_camera_matrices(ecs, camera, (float)o.width / (float)o.height, &view, &projection)) {
            const WPE_Mat4 view_projection = wpe_mat4_mul(&projection, &view);
            wpe_debug_lines_begin(debug_lines, &view_projection, o.width, o.height, 1.0f);
            wpe_physics_debug_draw(physics, add_debug_line, debug_lines);
            r = wpe_debug_lines_submit(debug_lines, renderer, true);
        }
        if (r == WPE_RESULT_OK) {
            wpe_draw2d_begin(draw2d, o.width, o.height);
            wpe_draw2d_rect(draw2d, (WPE_Rect){0, 0, (float)o.width, 9}, wpe_color_rgba8(0, 0, 0, 140));
            wpe_draw2d_text(draw2d, font, 2, 2, hud, 0xFFFFFFFFu, 1.0f);
            r = wpe_draw2d_submit(draw2d, renderer);
        }
        const WPE_Result end = wpe_renderer_end_frame(renderer);
        if (r == WPE_RESULT_OK) r = end;
        if (r == WPE_RESULT_OK) r = wpe_window_present(window, wpe_renderer_framebuffer(renderer));
        if (r != WPE_RESULT_OK) {
            wpe_log_error("frame %u failed: %s", frame, wpe_result_to_string(r));
            goto cleanup;
        }

        ++stats_frames;
        if (now - stats_start >= 0.5) { /* Full windows only: the first frame includes startup stalls. */
            const WPE_ScriptStats stats = wpe_script_host_stats(host);
            const WPE_PhysicsStats physics_stats = wpe_physics_stats(physics);
            snprintf(hud, sizeof hud, "%.0f fps  %u scripts  %u instances  %llu errors  %u bodies (%u awake)%s",
                     stats_frames / fmax(now - stats_start, 1e-6), stats.scripts, stats.instances,
                     (unsigned long long)stats.errors, physics_stats.bodies, physics_stats.awake_bodies,
                     o.watch ? "  [watching]" : "");
            stats_start = now;
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
    wpe_script_host_destroy(host); /* Before physics and the world: runs on_destroy for every script. */
    wpe_physics_destroy(physics);
    wpe_ecs_destroy(ecs);
    wpe_model_destroy(figure);
    wpe_model_destroy(helmet);
    wpe_model_destroy(box);
    wpe_model_destroy(ground);
    wpe_font_destroy(font);
    wpe_draw2d_destroy(draw2d);
    wpe_debug_lines_destroy(debug_lines);
    wpe_scene_renderer_destroy(scene_renderer);
    wpe_window_destroy(window);
    wpe_renderer_destroy(renderer);
    wpe_jobs_destroy(jobs);
    return exit_code;
}
