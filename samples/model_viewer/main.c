/*
 * Model viewer: loads a glTF/GLB model, frames it with an orbiting camera, plays its first
 * animation and shows a text overlay.
 *
 * Usage: model_viewer [model.glb] [--width N] [--height N] [--scale N] [--workers N] [--frames N]
 *                     [--screenshot file.png] [--max-texture N] [--yaw DEGREES] [--pitch DEGREES]
 *                     [--distance FACTOR] [--time SECONDS] [--dither] [--filter NAME] [--no-hud]
 *                     [--no-vsync] [--attach NODE=model.glb]... [--animation NAME]
 *   --animation NAME  play this clip instead of the first: its name or number (1 = first). Without a match,
 *                     the error lists the model's clips
 *   --attach NODE=F   show model F held by the node (a bone or socket) named NODE, the way a game parents
 *                     it there: its origin on the node, no offset. Give it once per model, up to 8 times.
 *                     Paths are like the main model's; the framing includes what is attached
 *   --filter NAME     filter every texture one way: material (as authored), nearest, bilinear or
 *                     three_point (Nintendo 64 style)
 *   --frames N       exit after N frames (0 = run until the window is closed)
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
#include "scene/scene.h"

#define MAX_ATTACHMENTS 8

typedef struct Attachment {
    char node[WPE_NAME_MAX];
    const char* path;
} Attachment;

typedef struct Options {
    const char* model_path;
    Attachment attachments[MAX_ATTACHMENTS];
    uint32_t attachment_count;
    uint32_t width, height, scale, workers, frames, max_texture;
    const char* screenshot;
    const char* animation;
    float yaw, pitch, distance, time;
    WPE_TextureFiltering filter;
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

/* "NODE=path/to/model.glb": the first '=' ends the node's name, so paths may contain one. */
static bool parse_attachment(const char* text, Options* o) {
    const char* equals = strchr(text, '=');
    if (!equals || equals == text || !equals[1] || (size_t)(equals - text) >= WPE_NAME_MAX) return false;
    if (o->attachment_count == MAX_ATTACHMENTS) return false;
    Attachment* attachment = &o->attachments[o->attachment_count++];
    memcpy(attachment->node, text, (size_t)(equals - text));
    attachment->node[equals - text] = '\0';
    attachment->path = equals + 1;
    return true;
}

static bool parse_options(int argc, char** argv, Options* o) {
    *o = (Options){.model_path = WPE_SAMPLE_ASSETS_DIR "/meshes/damaged_helmet.glb",
                   .width = 320,
                   .height = 240,
                   .scale = 3,
                   .workers = WPE_JOBS_AUTO_WORKERS,
                   .max_texture = 512,
                   .pitch = 15.0f,
                   .distance = 1.0f,
                   .hud = true,
                   .vsync = true};
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
        } else if (strcmp(arg, "--filter") == 0) {
            ok = wpe_texture_filtering_from_name(value, &o->filter);
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
        } else if (strcmp(arg, "--attach") == 0) {
            ok = parse_attachment(value, o);
        } else if (strcmp(arg, "--animation") == 0) {
            o->animation = value;
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
    WPE_Aabb total = {.min = wpe_vec3(INFINITY, INFINITY, INFINITY),
                      .max = wpe_vec3(-INFINITY, -INFINITY, -INFINITY)};
    WPE_EcsIter it = wpe_ecs_query(
        ecs, (WPE_EcsQuery){.all = WPE_ECS_MASK(WPE_COMP_MESH) | WPE_ECS_MASK(WPE_COMP_TRANSFORM)});
    while (wpe_ecs_iter_next(&it)) {
        const WPE_MeshRenderer* mesh = wpe_ecs_iter_get(&it, WPE_COMP_MESH);
        const WPE_Transform* t = wpe_ecs_iter_get(&it, WPE_COMP_TRANSFORM);
        if (mesh->skin != WPE_MODEL_NONE && mesh->joints) {
            for (uint32_t j = 0; j < mesh->joint_count; ++j) {
                const WPE_Transform* joint = wpe_ecs_get_component(ecs, mesh->joints[j], WPE_COMP_TRANSFORM);
                if (joint)
                    grow_bounds(&total,
                                wpe_vec3(joint->world.m[3][0], joint->world.m[3][1], joint->world.m[3][2]));
            }
            continue;
        }
        const WPE_Aabb b = mesh->model->meshes[mesh->mesh].bounds;
        const WPE_Mat4 world = t->world;
        for (int c = 0; c < 8; ++c) {
            const WPE_Vec3 corner =
                wpe_vec3(c & 1 ? b.max.x : b.min.x, c & 2 ? b.max.y : b.min.y, c & 4 ? b.max.z : b.min.z);
            grow_bounds(&total, wpe_mat4_transform_point(&world, corner));
        }
    }
    if (!(total.min.x <= total.max.x))
        return (WPE_Aabb){.min = wpe_vec3(-1, -1, -1), .max = wpe_vec3(1, 1, 1)};
    /* Joints sit inside the skin, so pad by a fraction of the extent to include the surface. */
    const WPE_Vec3 pad = wpe_vec3_scale(wpe_vec3_sub(total.max, total.min), 0.15f);
    total.min = wpe_vec3_sub(total.min, pad);
    total.max = wpe_vec3_add(total.max, pad);
    return total;
}

static void place_camera(WPE_Ecs* ecs, WPE_Entity camera, WPE_Vec3 target, float radius, float yaw_deg,
                         float pitch_deg) {
    const float yaw = yaw_deg * WPE_PI / 180.0f, pitch = pitch_deg * WPE_PI / 180.0f;
    const WPE_Vec3 offset =
        wpe_vec3(sinf(yaw) * cosf(pitch) * radius, sinf(pitch) * radius, cosf(yaw) * cosf(pitch) * radius);
    WPE_Transform* t = wpe_ecs_get_component(ecs, camera, WPE_COMP_TRANSFORM);
    t->position = wpe_vec3_add(target, offset);
    /* Camera looks down -Z: build the rotation from the inverse of a look-at view matrix. */
    const WPE_Mat4 view = wpe_mat4_look_at(t->position, target, wpe_vec3(0, 1, 0));
    WPE_Mat4 world;
    WPE_Vec3 position, scale;
    if (wpe_mat4_inverse(&view, &world) && wpe_mat4_decompose(&world, &position, &t->rotation, &scale))
        return;
    t->rotation = wpe_quat_identity();
}

/* Splits a path into its directory (written to `dir`, "." if it has none) and returns the file name. */
static const char* split_path(const char* path, char* dir, size_t dir_size) {
    snprintf(dir, dir_size, "%s", path);
    char* separator = strrchr(dir, '/');
    char* backslash = strrchr(dir, '\\');
    if (backslash && (!separator || backslash > separator)) separator = backslash;
    const char* file = separator ? path + (separator - dir) + 1 : path;
    if (separator) {
        *separator = '\0';
    } else {
        snprintf(dir, dir_size, ".");
    }
    return file;
}

/* The first entity called `name` at or under `entity`. */
static WPE_Entity find_named(const WPE_Ecs* ecs, WPE_Entity entity, const char* name) {
    const WPE_Name* own = wpe_ecs_get_component(ecs, entity, WPE_COMP_NAME);
    if (own && strcmp(own->value, name) == 0) return entity;
    const WPE_Hierarchy* hierarchy = wpe_ecs_get_component(ecs, entity, WPE_COMP_HIERARCHY);
    if (!hierarchy) return WPE_ENTITY_NULL;
    for (WPE_Entity child = hierarchy->first_child; child != WPE_ENTITY_NULL;) {
        const WPE_Entity found = find_named(ecs, child, name);
        if (found != WPE_ENTITY_NULL) return found;
        const WPE_Hierarchy* link = wpe_ecs_get_component(ecs, child, WPE_COMP_HIERARCHY);
        child = link ? link->next_sibling : WPE_ENTITY_NULL;
    }
    return WPE_ENTITY_NULL;
}

/* Lists the names at and under `entity`, so a mistyped node can be corrected from the error. */
static void print_names(const WPE_Ecs* ecs, WPE_Entity entity) {
    const WPE_Name* own = wpe_ecs_get_component(ecs, entity, WPE_COMP_NAME);
    if (own) fprintf(stderr, "  %s\n", own->value);
    const WPE_Hierarchy* hierarchy = wpe_ecs_get_component(ecs, entity, WPE_COMP_HIERARCHY);
    if (!hierarchy) return;
    for (WPE_Entity child = hierarchy->first_child; child != WPE_ENTITY_NULL;) {
        print_names(ecs, child);
        const WPE_Hierarchy* link = wpe_ecs_get_component(ecs, child, WPE_COMP_HIERARCHY);
        child = link ? link->next_sibling : WPE_ENTITY_NULL;
    }
}

/*
 * Loads an attachment and parents it to its node with no offset. Assets are cached by file name, so two
 * different files of one name can't both be shown: main() refuses that before getting here.
 */
static WPE_Result attach_model(WPE_Ecs* ecs, WPE_AssetManager* assets, WPE_Vfs* vfs, WPE_Entity root,
                               const Attachment* attachment) {
    const WPE_Entity node = find_named(ecs, root, attachment->node);
    if (node == WPE_ENTITY_NULL) {
        wpe_log_error("--attach: no node named \"%s\". The model has:", attachment->node);
        print_names(ecs, root);
        return WPE_RESULT_ERROR_NOT_FOUND;
    }
    char dir[1024];
    const char* file = split_path(attachment->path, dir, sizeof dir);
    WPE_Result result = wpe_vfs_mount_directory(vfs, dir);
    if (result != WPE_RESULT_OK) return result;
    WPE_AssetHandle handle = WPE_ASSET_NONE;
    result = wpe_asset_load(assets, WPE_ASSET_MODEL, file, &handle);
    if (result != WPE_RESULT_OK) return result;
    WPE_Entity instance = WPE_ENTITY_NULL;
    result = wpe_model_instantiate(ecs, assets, handle, NULL, &instance);
    /* The mesh renderers hold their own references. */
    wpe_asset_release(assets, handle);
    if (result == WPE_RESULT_OK) result = wpe_scene_set_parent(ecs, instance, node, false);
    return result;
}

/*
 * The clip chosen by --animation (a name first, so a clip may be called "2"; then a 1-based number), or
 * the first one without the option. WPE_MODEL_NONE: the model has no clips, or `wanted` matches none.
 */
static uint32_t find_animation(const WPE_Model* model, const char* wanted) {
    if (!wanted) return model->animation_count ? 0 : WPE_MODEL_NONE;
    for (uint32_t i = 0; i < model->animation_count; ++i) {
        if (strcmp(model->animations[i].name, wanted) == 0) return i;
    }
    uint32_t number = 0;
    if (parse_u32(wanted, &number) && number >= 1 && number <= model->animation_count) return number - 1;
    return WPE_MODEL_NONE;
}

/* Whether two of the given paths are different files with the same name (see attach_model). */
static bool attachment_names_clash(const Options* o) {
    char dir[1024], other_dir[1024];
    for (uint32_t i = 0; i < o->attachment_count; ++i) {
        const char* file = split_path(o->attachments[i].path, dir, sizeof dir);
        const char* other = split_path(o->model_path, other_dir, sizeof other_dir);
        if (strcmp(file, other) == 0 && strcmp(dir, other_dir) != 0) return true;
        for (uint32_t j = 0; j < i; ++j) {
            other = split_path(o->attachments[j].path, other_dir, sizeof other_dir);
            if (strcmp(file, other) == 0 && strcmp(dir, other_dir) != 0) return true;
        }
    }
    return false;
}

int main(int argc, char** argv) {
    Options o;
    if (!parse_options(argc, argv, &o)) return EXIT_FAILURE;
    if (attachment_names_clash(&o)) {
        fprintf(stderr, "--attach: different files can't share a file name\n");
        return EXIT_FAILURE;
    }

    WPE_JobSystem* jobs = NULL;
    WPE_Renderer* renderer = NULL;
    WPE_Window* window = NULL;
    WPE_Ecs* ecs = NULL;
    WPE_Vfs* vfs = NULL;
    WPE_AssetManager* assets = NULL;
    WPE_AssetHandle model = WPE_ASSET_NONE, font_asset = WPE_ASSET_NONE;
    WPE_SceneRenderer* scene_renderer = NULL;
    WPE_Draw2D* draw2d = NULL;
    int exit_code = EXIT_FAILURE;

    /* The model may live anywhere: mount its directory (over the sample assets, for the font) and
     * load it by file name. */
    char model_dir[1024];
    const char* model_file = split_path(o.model_path, model_dir, sizeof model_dir);

    WPE_Result result = wpe_jobs_create(o.workers, &jobs);
    if (result == WPE_RESULT_OK) result = wpe_vfs_create(&vfs);
    if (result == WPE_RESULT_OK) result = wpe_vfs_mount_directory(vfs, WPE_SAMPLE_ASSETS_DIR);
    if (result == WPE_RESULT_OK) result = wpe_vfs_mount_directory(vfs, model_dir);
    if (result == WPE_RESULT_OK) {
        result = wpe_asset_manager_create(
            &(WPE_AssetManagerDesc){.vfs = vfs, .jobs = jobs, .max_texture_size = o.max_texture}, &assets);
    }
    if (result == WPE_RESULT_OK) {
        const double start = wpe_platform_time_seconds();
        result = wpe_asset_load(assets, WPE_ASSET_MODEL, model_file, &model);
        if (result == WPE_RESULT_OK)
            wpe_log_info("loaded %s in %.0f ms", o.model_path,
                         (wpe_platform_time_seconds() - start) * 1000.0);
    }
    if (result == WPE_RESULT_OK)
        result = wpe_asset_load(assets, WPE_ASSET_FONT, "fonts/small.png", &font_asset);
    const WPE_BitmapFont* font = wpe_asset_font(assets, font_asset);
    if (result == WPE_RESULT_OK)
        result = wpe_renderer_create(&(WPE_RendererDesc){.width = o.width, .height = o.height, .jobs = jobs},
                                     &renderer);
    if (result == WPE_RESULT_OK) {
        result = wpe_window_create(&(WPE_WindowDesc){.title = "WillPowerEngine - Model Viewer",
                                                     .width = o.width * o.scale,
                                                     .height = o.height * o.scale,
                                                     .resizable = true,
                                                     .vsync = o.vsync},
                                   &window);
    }
    if (result == WPE_RESULT_OK) result = wpe_scene_renderer_create(&scene_renderer);
    if (result == WPE_RESULT_OK) result = wpe_draw2d_create(&draw2d);
    if (result == WPE_RESULT_OK) result = wpe_ecs_create(&ecs);
    if (result == WPE_RESULT_OK) result = wpe_scene_register_components(ecs);
    if (result == WPE_RESULT_OK) result = wpe_graphics_register_components(ecs, assets);

    WPE_Entity root = WPE_ENTITY_NULL, camera = WPE_ENTITY_NULL;
    if (result == WPE_RESULT_OK) {
        const WPE_Model* loaded = wpe_asset_model(assets, model);
        const uint32_t animation = find_animation(loaded, o.animation);
        if (o.animation && animation == WPE_MODEL_NONE) {
            wpe_log_error("--animation: no clip \"%s\". The model has %u:", o.animation, loaded->animation_count);
            for (uint32_t i = 0; i < loaded->animation_count; ++i) {
                const WPE_Animation* clip = &loaded->animations[i];
                fprintf(stderr, "  %u: %s (%.2f s)\n", i + 1, clip->name[0] ? clip->name : "(no name)", (double)clip->duration);
            }
            result = WPE_RESULT_ERROR_NOT_FOUND;
        } else {
            const WPE_InstantiateOptions instantiate = {.animation = animation, .play = !o.screenshot};
            result = wpe_model_instantiate(ecs, assets, model, &instantiate, &root);
        }
    }
    for (uint32_t i = 0; result == WPE_RESULT_OK && i < o.attachment_count; ++i) {
        result = attach_model(ecs, assets, vfs, root, &o.attachments[i]);
        if (result != WPE_RESULT_OK) wpe_log_error("--attach: couldn't attach %s", o.attachments[i].path);
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
    settings.texture_filtering = o.filter;
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
            result =
                wpe_renderer_begin_frame(renderer, &(WPE_ClearDesc){.clear_color = true,
                                                                    .color = wpe_color_rgba8(36, 40, 54, 255),
                                                                    .clear_depth = true,
                                                                    .depth = 1.0f});
        }
        if (result == WPE_RESULT_OK)
            result = wpe_scene_render(scene_renderer, renderer, ecs, camera, &settings, &scene_stats);
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
                     rs.triangles_rasterized, scene_stats.primitives_submitted,
                     scene_stats.primitives_culled);
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
    wpe_asset_release(assets, font_asset);
    wpe_asset_release(assets, model);
    wpe_asset_manager_destroy(assets);
    wpe_vfs_destroy(vfs);
    wpe_draw2d_destroy(draw2d);
    wpe_scene_renderer_destroy(scene_renderer);
    wpe_window_destroy(window);
    wpe_renderer_destroy(renderer);
    wpe_jobs_destroy(jobs);
    return exit_code;
}
