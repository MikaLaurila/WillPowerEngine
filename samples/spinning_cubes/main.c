/*
 * Spinning cubes: renders a field of rotating cubes through the ECS, the scene transform system and
 * the software rasterizer, and presents it in a window.
 *
 * Usage: spinning_cubes [--width N] [--height N] [--scale N] [--workers N] [--frames N]
 *                       [--screenshot file.png] [--no-vsync]
 *   --frames N        exit after N frames (0 = run until the window is closed)
 *   --screenshot F    save the last rendered frame as PNG (useful for automated checks)
 */
#define WPE_LOG_MODULE "sample"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../common/cube_mesh.h"
#include "core/core.h"
#include "jobs/jobs.h"
#include "platform/platform.h"
#include "renderer/renderer.h"
#include "renderer/shaders.h"
#include "scene/scene.h"

#define GRID 7
#define SPACING 1.6f

typedef struct Options {
    uint32_t width, height, scale, workers, frames;
    const char* screenshot;
    bool vsync;
} Options;

typedef struct Spin {
    WPE_Vec3 axis;
    float speed;
} Spin;

enum { COMP_SPIN = WPE_COMP_USER_FIRST };

static bool parse_u32(const char* text, uint32_t* out) {
    char* end = NULL;
    const unsigned long value = strtoul(text, &end, 10);
    if (!text[0] || *end != '\0' || value > UINT32_MAX) return false;
    *out = (uint32_t)value;
    return true;
}

static bool parse_options(int argc, char** argv, Options* options) {
    *options = (Options){.width = 320, .height = 240, .scale = 3, .workers = WPE_JOBS_AUTO_WORKERS, .vsync = true};
    for (int i = 1; i < argc; ++i) {
        const char* arg = argv[i];
        const char* value = i + 1 < argc ? argv[i + 1] : NULL;
        bool ok = true;
        if (strcmp(arg, "--no-vsync") == 0) {
            options->vsync = false;
            continue;
        }
        if (!value) {
            ok = false;
        } else if (strcmp(arg, "--width") == 0) {
            ok = parse_u32(value, &options->width);
        } else if (strcmp(arg, "--height") == 0) {
            ok = parse_u32(value, &options->height);
        } else if (strcmp(arg, "--scale") == 0) {
            ok = parse_u32(value, &options->scale) && options->scale > 0;
        } else if (strcmp(arg, "--workers") == 0) {
            ok = parse_u32(value, &options->workers);
        } else if (strcmp(arg, "--frames") == 0) {
            ok = parse_u32(value, &options->frames);
        } else if (strcmp(arg, "--screenshot") == 0) {
            options->screenshot = value;
        } else {
            ok = false;
        }
        if (!ok) {
            fprintf(stderr, "invalid argument: %s\n", arg);
            return false;
        }
        ++i;
    }
    return true;
}

static WPE_Result build_scene(WPE_Ecs* ecs) {
    WPE_Result result = wpe_scene_register_components(ecs);
    if (result != WPE_RESULT_OK) return result;
    result = wpe_ecs_register_component(
        ecs, COMP_SPIN, &(WPE_ComponentDesc){.name = "Spin", .size = sizeof(Spin), .alignment = alignof(Spin)});
    if (result != WPE_RESULT_OK) return result;

    for (int gz = 0; gz < GRID; ++gz) {
        for (int gx = 0; gx < GRID; ++gx) {
            const WPE_Entity e = wpe_ecs_spawn(ecs);
            if (e == WPE_ENTITY_NULL) return WPE_RESULT_ERROR_CAPACITY;
            WPE_Transform t = wpe_transform_identity();
            t.position = wpe_vec3(((float)gx - (GRID - 1) * 0.5f) * SPACING, 0.0f, -(float)gz * SPACING);
            const float s = 0.6f + 0.05f * (float)((gx + gz) % 5);
            t.scale = wpe_vec3(s, s, s);
            const Spin spin = {
                .axis = wpe_vec3_normalize(wpe_vec3((float)(gx % 3) - 1.0f, 1.0f, (float)(gz % 2) * 0.5f)),
                .speed = 0.6f + 0.15f * (float)((gx * 7 + gz * 3) % 6),
            };
            result = wpe_ecs_add_component(ecs, e, WPE_COMP_TRANSFORM, &t, NULL);
            if (result == WPE_RESULT_OK) result = wpe_ecs_add_component(ecs, e, COMP_SPIN, &spin, NULL);
            if (result != WPE_RESULT_OK) return result;
        }
    }
    return WPE_RESULT_OK;
}

static void update_spin(WPE_Ecs* ecs, float dt) {
    WPE_EcsIter it = wpe_ecs_query(ecs, (WPE_EcsQuery){.all = WPE_ECS_MASK(WPE_COMP_TRANSFORM) | WPE_ECS_MASK(COMP_SPIN)});
    while (wpe_ecs_iter_next(&it)) {
        WPE_Transform* t = wpe_ecs_iter_get(&it, WPE_COMP_TRANSFORM);
        const Spin* spin = wpe_ecs_iter_get(&it, COMP_SPIN);
        const WPE_Quat delta = wpe_quat_from_axis_angle(spin->axis, spin->speed * dt);
        t->rotation = wpe_quat_normalize(wpe_quat_mul(delta, t->rotation));
    }
}

static WPE_Result render_scene(WPE_Renderer* renderer, WPE_Ecs* ecs, const WPE_VertexColored* vertices,
                               const uint32_t* indices, float aspect) {
    const WPE_Mat4 projection = wpe_mat4_perspective(WPE_PI / 3.0f, aspect, 0.1f, 100.0f);
    const WPE_Mat4 view = wpe_mat4_look_at(wpe_vec3(0.0f, 4.0f, 5.5f), wpe_vec3(0.0f, -0.5f, -4.0f), wpe_vec3(0, 1, 0));
    const WPE_Mat4 view_projection = wpe_mat4_mul(&projection, &view);

    WPE_Result result = wpe_renderer_begin_frame(
        renderer, &(WPE_ClearDesc){.clear_color = true, .color = wpe_color_rgba8(24, 26, 36, 255), .clear_depth = true, .depth = 1.0f});
    if (result != WPE_RESULT_OK) return result;

    WPE_EcsIter it = wpe_ecs_query(ecs, (WPE_EcsQuery){.all = WPE_ECS_MASK(WPE_COMP_TRANSFORM)});
    while (wpe_ecs_iter_next(&it)) {
        const WPE_Transform* t = wpe_ecs_iter_get(&it, WPE_COMP_TRANSFORM);
        const WPE_VertexColorUniforms uniforms = {.mvp = wpe_mat4_mul(&view_projection, &t->world), .tint = wpe_vec4(1, 1, 1, 1)};
        result = wpe_renderer_submit(renderer, &(WPE_DrawDesc){
                                                   .program = wpe_shader_vertex_color(),
                                                   .uniforms = &uniforms,
                                                   .uniforms_size = sizeof uniforms,
                                                   .vertices = vertices,
                                                   .vertex_stride = sizeof(WPE_VertexColored),
                                                   .vertex_count = WPE_CUBE_VERTEX_COUNT,
                                                   .indices = indices,
                                                   .index_count = WPE_CUBE_INDEX_COUNT,
                                                   .state = wpe_render_state_default(),
                                               });
        if (result != WPE_RESULT_OK) break;
    }
    const WPE_Result end_result = wpe_renderer_end_frame(renderer);
    return result != WPE_RESULT_OK ? result : end_result;
}

int main(int argc, char** argv) {
    Options options;
    if (!parse_options(argc, argv, &options)) return EXIT_FAILURE;

    WPE_JobSystem* jobs = NULL;
    WPE_Renderer* renderer = NULL;
    WPE_Window* window = NULL;
    WPE_Ecs* ecs = NULL;
    int exit_code = EXIT_FAILURE;

    WPE_VertexColored vertices[WPE_CUBE_VERTEX_COUNT];
    uint32_t indices[WPE_CUBE_INDEX_COUNT];
    wpe_cube_mesh_build(vertices, indices);

    WPE_Result result = wpe_jobs_create(options.workers, &jobs);
    if (result == WPE_RESULT_OK) {
        result = wpe_renderer_create(&(WPE_RendererDesc){.width = options.width, .height = options.height, .jobs = jobs},
                                     &renderer);
    }
    if (result == WPE_RESULT_OK) {
        result = wpe_window_create(&(WPE_WindowDesc){.title = "WillPowerEngine - Spinning Cubes",
                                                     .width = options.width * options.scale,
                                                     .height = options.height * options.scale,
                                                     .resizable = true,
                                                     .vsync = options.vsync},
                                   &window);
    }
    if (result == WPE_RESULT_OK) result = wpe_ecs_create(&ecs);
    if (result == WPE_RESULT_OK) result = build_scene(ecs);
    if (result != WPE_RESULT_OK) {
        wpe_log_error("startup failed: %s", wpe_result_to_string(result));
        goto cleanup;
    }
    wpe_log_info("rendering %ux%u with %u thread(s)", options.width, options.height, wpe_jobs_thread_count(jobs));

    const float aspect = (float)options.width / (float)options.height;
    double previous = wpe_platform_time_seconds();
    double stats_start = previous, render_time = 0.0;
    uint32_t stats_frames = 0;

    for (uint32_t frame = 0; options.frames == 0 || frame < options.frames; ++frame) {
        if (!wpe_window_poll_events(window)) break;
        const double now = wpe_platform_time_seconds();
        /* Fixed step when capturing so screenshots are reproducible. */
        const float dt = options.screenshot ? 1.0f / 60.0f : (float)(now - previous);
        previous = now;

        update_spin(ecs, dt);
        result = wpe_scene_update_transforms(ecs);
        const double render_start = wpe_platform_time_seconds();
        if (result == WPE_RESULT_OK) result = render_scene(renderer, ecs, vertices, indices, aspect);
        render_time += wpe_platform_time_seconds() - render_start;
        if (result == WPE_RESULT_OK) result = wpe_window_present(window, wpe_renderer_framebuffer(renderer));
        if (result != WPE_RESULT_OK) {
            wpe_log_error("frame %u failed: %s", frame, wpe_result_to_string(result));
            goto cleanup;
        }

        ++stats_frames;
        if (now - stats_start >= 1.0) {
            const WPE_RenderStats stats = wpe_renderer_stats(renderer);
            wpe_log_info("%.1f fps, render %.2f ms/frame, %u tris rasterized, %llu fragments", stats_frames / (now - stats_start),
                         render_time * 1000.0 / stats_frames, stats.triangles_rasterized,
                         (unsigned long long)stats.fragments_shaded);
            stats_start = now;
            render_time = 0.0;
            stats_frames = 0;
        }
    }

    if (options.screenshot) {
        const WPE_Framebuffer* fb = wpe_renderer_framebuffer(renderer);
        if (wpe_image_save_png(options.screenshot, fb->width, fb->height, fb->color) != WPE_RESULT_OK) goto cleanup;
        wpe_log_info("saved screenshot to %s", options.screenshot);
    }
    exit_code = EXIT_SUCCESS;

cleanup:
    wpe_ecs_destroy(ecs);
    wpe_window_destroy(window);
    wpe_renderer_destroy(renderer);
    wpe_jobs_destroy(jobs);
    return exit_code;
}
