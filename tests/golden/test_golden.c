/*
 * Golden image tests: render fixed scenes and compare them with reviewed reference PNGs in
 * tests/golden/references/.
 *
 * Small differences are tolerated because transcendental functions (sinf/cosf) differ by an ulp
 * between C libraries, which can move a few edge pixels. On mismatch the actual image is written
 * next to the test binary as <name>_actual.png for inspection.
 *
 * To (re)generate references after a reviewed, intentional change:
 *   WPE_UPDATE_GOLDEN=1 ctest --test-dir build -R golden
 * then inspect the new PNGs before committing them.
 */
#include "wpe_test.h"
#include "wpe_test_assets.h"

#include <stdio.h>
#include <stdlib.h>

#include "assets/font.h"
#include "assets/procedural.h"
#include "cube_mesh.h"
#include "graphics/postfx.h"
#include "graphics/animation.h"
#include "graphics/draw2d.h"
#include "graphics/graphics.h"
#include "graphics/scene_render.h"
#include "platform/platform.h"
#include "renderer/renderer.h"
#include "renderer/shaders.h"

enum { GOLDEN_W = 160, GOLDEN_H = 120 };

/* Pixels whose channels all differ by at most this much count as equal. */
#define GOLDEN_CHANNEL_TOLERANCE 3
/* Fraction of pixels allowed to exceed the channel tolerance. */
#define GOLDEN_MAX_BAD_FRACTION 0.004

static uint32_t channel_diff(WPE_Color a, WPE_Color b, int shift) {
    const int ca = (int)((a >> shift) & 0xFF), cb = (int)((b >> shift) & 0xFF);
    return (uint32_t)(ca > cb ? ca - cb : cb - ca);
}

static void compare_with_reference(const char* name, const WPE_Framebuffer* fb) {
    char reference_path[512], actual_path[512];
    snprintf(reference_path, sizeof reference_path, "%s/%s.png", WPE_GOLDEN_REFERENCE_DIR, name);
    snprintf(actual_path, sizeof actual_path, "%s_actual.png", name);

    const char* update = getenv("WPE_UPDATE_GOLDEN");
    if (update && update[0] == '1') {
        WPE_TEST_ASSERT_OK(wpe_image_save_png(reference_path, fb->width, fb->height, fb->color));
        fprintf(stderr, "    updated reference %s\n", reference_path);
        return;
    }

    WPE_Image reference;
    const WPE_Result loaded = wpe_image_load_png(reference_path, &reference);
    if (loaded != WPE_RESULT_OK) {
        WPE_TEST_ASSERT_OK(wpe_image_save_png(actual_path, fb->width, fb->height, fb->color));
        WPE_TEST_ASSERT_MSG(false, "missing reference %s (actual output written to %s)", reference_path,
                            actual_path);
    }
    const bool same_size = reference.width == fb->width && reference.height == fb->height;

    uint32_t bad = 0;
    uint32_t worst = 0;
    if (same_size) {
        for (uint32_t i = 0; i < fb->width * fb->height; ++i) {
            uint32_t diff = 0;
            for (int shift = 0; shift <= 16; shift += 8) {
                const uint32_t d = channel_diff(reference.pixels[i], fb->color[i], shift);
                diff = d > diff ? d : diff;
            }
            worst = diff > worst ? diff : worst;
            if (diff > GOLDEN_CHANNEL_TOLERANCE) ++bad;
        }
    }
    wpe_image_free(&reference);

    const uint32_t allowed = (uint32_t)(GOLDEN_MAX_BAD_FRACTION * fb->width * fb->height);
    if (!same_size || bad > allowed) {
        WPE_TEST_ASSERT_OK(wpe_image_save_png(actual_path, fb->width, fb->height, fb->color));
        WPE_TEST_ASSERT_MSG(
            false, "%s: %u pixels differ (allowed %u, worst channel diff %u%s); actual written to %s", name,
            bad, allowed, worst, same_size ? "" : ", size mismatch", actual_path);
    }
    fprintf(stderr, "    %s: %u differing pixels (allowed %u), worst channel diff %u\n", name, bad, allowed,
            worst);
}

static WPE_DrawDesc cube_draw(const WPE_VertexColored* vertices, const uint32_t* indices,
                              const WPE_VertexColorUniforms* uniforms, WPE_RenderState state) {
    return (WPE_DrawDesc){
        .program = wpe_shader_vertex_color(),
        .uniforms = uniforms,
        .uniforms_size = sizeof *uniforms,
        .vertices = vertices,
        .vertex_stride = sizeof(WPE_VertexColored),
        .vertex_count = WPE_CUBE_VERTEX_COUNT,
        .indices = indices,
        .index_count = WPE_CUBE_INDEX_COUNT,
        .state = state,
    };
}

static void test_golden_cubes_scene(void) {
    WPE_VertexColored cube_vertices[WPE_CUBE_VERTEX_COUNT];
    uint32_t cube_indices[WPE_CUBE_INDEX_COUNT];
    wpe_cube_mesh_build(cube_vertices, cube_indices);

    /* Ground quad with a color gradient, extending behind the camera to exercise near clipping. */
    const WPE_VertexColored ground[4] = {
        {.position = {{-20, 0, 6}}, .color = {0.20f, 0.22f, 0.30f, 1}},
        {.position = {{20, 0, 6}}, .color = {0.20f, 0.30f, 0.22f, 1}},
        {.position = {{20, 0, -30}}, .color = {0.05f, 0.05f, 0.08f, 1}},
        {.position = {{-20, 0, -30}}, .color = {0.05f, 0.05f, 0.08f, 1}},
    };
    const uint32_t ground_indices[6] = {0, 1, 2, 0, 2, 3};

    const WPE_Mat4 projection = wpe_mat4_perspective(WPE_PI / 3.0f, (float)GOLDEN_W / GOLDEN_H, 0.1f, 50.0f);
    const WPE_Mat4 view = wpe_mat4_look_at(wpe_vec3(0, 2.2f, 4.5f), wpe_vec3(0, 0.6f, 0), wpe_vec3(0, 1, 0));
    const WPE_Mat4 view_projection = wpe_mat4_mul(&projection, &view);

    typedef struct {
        WPE_Vec3 position;
        WPE_Vec3 euler;
        float scale;
        WPE_Vec4 tint;
        WPE_BlendMode blend;
    } Placement;
    const Placement placements[] = {
        {{{-1.6f, 0.6f, -0.5f}}, {{0.3f, 0.8f, 0.0f}}, 1.1f, {{1, 1, 1, 1}}, WPE_BLEND_OPAQUE},
        {{{0.2f, 0.7f, -1.8f}}, {{0.9f, -0.4f, 0.5f}}, 1.3f, {{1, 1, 1, 1}}, WPE_BLEND_OPAQUE},
        {{{1.7f, 0.5f, 0.2f}}, {{-0.2f, 2.3f, 0.7f}}, 0.9f, {{1, 1, 1, 1}}, WPE_BLEND_OPAQUE},
        {{{0.0f, 0.9f, 0.9f}}, {{0.6f, 0.6f, 0.0f}}, 0.8f, {{1, 1, 1, 0.5f}}, WPE_BLEND_ALPHA_DITHER},
        {{{1.0f, 1.6f, -0.4f}}, {{0.0f, 0.4f, 0.9f}}, 0.6f, {{1, 1, 1, 0.45f}}, WPE_BLEND_ALPHA},
    };
    enum { PLACEMENTS = sizeof placements / sizeof placements[0] };

    WPE_JobSystem* jobs = NULL;
    WPE_TEST_ASSERT_OK(wpe_jobs_create(3, &jobs));
    WPE_Renderer* renderer = NULL;
    WPE_TEST_ASSERT_OK(wpe_renderer_create(
        &(WPE_RendererDesc){.width = GOLDEN_W, .height = GOLDEN_H, .jobs = jobs}, &renderer));
    WPE_TEST_ASSERT_OK(
        wpe_renderer_begin_frame(renderer, &(WPE_ClearDesc){.clear_color = true,
                                                            .color = wpe_color_rgba8(40, 44, 60, 255),
                                                            .clear_depth = true,
                                                            .depth = 1}));

    const WPE_VertexColorUniforms ground_uniforms = {.mvp = view_projection, .tint = wpe_vec4(1, 1, 1, 1)};
    WPE_RenderState ground_state = wpe_render_state_default();
    WPE_TEST_ASSERT_OK(wpe_renderer_submit(renderer, &(WPE_DrawDesc){.program = wpe_shader_vertex_color(),
                                                                     .uniforms = &ground_uniforms,
                                                                     .uniforms_size = sizeof ground_uniforms,
                                                                     .vertices = ground,
                                                                     .vertex_stride = sizeof ground[0],
                                                                     .vertex_count = 4,
                                                                     .indices = ground_indices,
                                                                     .index_count = 6,
                                                                     .state = ground_state}));

    WPE_VertexColorUniforms uniforms[PLACEMENTS];
    for (int i = 0; i < PLACEMENTS; ++i) {
        const Placement* p = &placements[i];
        const WPE_Mat4 model = wpe_mat4_from_trs(p->position, wpe_quat_from_euler_xyz(p->euler),
                                                 wpe_vec3(p->scale, p->scale, p->scale));
        uniforms[i] =
            (WPE_VertexColorUniforms){.mvp = wpe_mat4_mul(&view_projection, &model), .tint = p->tint};
        WPE_RenderState state = wpe_render_state_default();
        state.blend = p->blend;
        if (p->blend == WPE_BLEND_ALPHA) state.depth_write = false;
        WPE_DrawDesc draw = cube_draw(cube_vertices, cube_indices, &uniforms[i], state);
        draw.sort_key = p->blend == WPE_BLEND_ALPHA ? 1 : 0; /* Blended geometry after opaque. */
        WPE_TEST_ASSERT_OK(wpe_renderer_submit(renderer, &draw));
    }
    WPE_TEST_ASSERT_OK(wpe_renderer_end_frame(renderer));

    compare_with_reference("cubes", wpe_renderer_framebuffer(renderer));
    wpe_renderer_destroy(renderer);
    wpe_jobs_destroy(jobs);
}

/* Renders an instantiated model from a fixed orbit position, optionally posed and with a text overlay. */
static void render_model_golden(const char* name, const char* model_path, float yaw_deg, float pitch_deg,
                                float distance, float target_y, float anim_time, const char* overlay) {
    WPE_TestAssets assets = wpe_test_assets_create(WPE_GOLDEN_ASSETS_DIR, 128);
    const WPE_AssetHandle model = wpe_test_load_model(assets.assets, model_path);
    WPE_Ecs* ecs = NULL;
    WPE_TEST_ASSERT_OK(wpe_ecs_create(&ecs));
    WPE_TEST_ASSERT_OK(wpe_scene_register_components(ecs));
    WPE_TEST_ASSERT_OK(wpe_graphics_register_components(ecs, assets.assets));
    WPE_Entity root;
    const bool animated = wpe_asset_model(assets.assets, model)->animation_count > 0;
    WPE_TEST_ASSERT_OK(
        wpe_model_instantiate(ecs, assets.assets, model,
                              &(WPE_InstantiateOptions){.animation = animated ? 0 : WPE_MODEL_NONE}, &root));
    WPE_Animator* animator = wpe_ecs_get_component(ecs, root, WPE_COMP_ANIMATOR);
    if (animator) {
        animator->time = anim_time;
        wpe_animation_apply(ecs, animator);
    }

    /* Orbit camera aimed at (0, target_y, 0). */
    const float yaw = yaw_deg * WPE_PI / 180.0f, pitch = pitch_deg * WPE_PI / 180.0f;
    const WPE_Vec3 target = wpe_vec3(0, target_y, 0);
    const WPE_Vec3 eye =
        wpe_vec3_add(target, wpe_vec3(sinf(yaw) * cosf(pitch) * distance, sinf(pitch) * distance,
                                      cosf(yaw) * cosf(pitch) * distance));
    const WPE_Mat4 view = wpe_mat4_look_at(eye, target, wpe_vec3(0, 1, 0));
    WPE_Mat4 camera_world;
    WPE_TEST_ASSERT(wpe_mat4_inverse(&view, &camera_world));
    WPE_Transform camera_transform = wpe_transform_identity();
    WPE_TEST_ASSERT(wpe_mat4_decompose(&camera_world, &camera_transform.position, &camera_transform.rotation,
                                       &camera_transform.scale));
    const WPE_Entity camera = wpe_ecs_spawn(ecs);
    const WPE_Camera camera_params = wpe_camera_default();
    WPE_TEST_ASSERT_OK(wpe_ecs_add_component(ecs, camera, WPE_COMP_TRANSFORM, &camera_transform, NULL));
    WPE_TEST_ASSERT_OK(wpe_ecs_add_component(ecs, camera, WPE_COMP_CAMERA, &camera_params, NULL));
    WPE_TEST_ASSERT_OK(wpe_scene_update_transforms(ecs));

    WPE_JobSystem* jobs = NULL;
    WPE_TEST_ASSERT_OK(wpe_jobs_create(3, &jobs));
    WPE_Renderer* renderer = NULL;
    WPE_TEST_ASSERT_OK(wpe_renderer_create(
        &(WPE_RendererDesc){.width = GOLDEN_W, .height = GOLDEN_H, .jobs = jobs}, &renderer));
    WPE_SceneRenderer* sr = NULL;
    WPE_TEST_ASSERT_OK(wpe_scene_renderer_create(&sr));
    WPE_Draw2D* d2d = NULL;
    WPE_TEST_ASSERT_OK(wpe_draw2d_create(&d2d));
    WPE_AssetHandle font_asset = WPE_ASSET_NONE;
    WPE_TEST_ASSERT_OK(wpe_asset_load(assets.assets, WPE_ASSET_FONT, "fonts/small.png", &font_asset));
    const WPE_BitmapFont* font = wpe_asset_font(assets.assets, font_asset);

    WPE_TEST_ASSERT_OK(
        wpe_renderer_begin_frame(renderer, &(WPE_ClearDesc){.clear_color = true,
                                                            .color = wpe_color_rgba8(36, 40, 54, 255),
                                                            .clear_depth = true,
                                                            .depth = 1}));
    WPE_TEST_ASSERT_OK(wpe_scene_render(sr, renderer, ecs, camera, NULL, NULL));
    if (overlay) {
        wpe_draw2d_begin(d2d, GOLDEN_W, GOLDEN_H);
        wpe_draw2d_rect(d2d, (WPE_Rect){0, 0, GOLDEN_W, 9}, wpe_color_rgba8(0, 0, 0, 140));
        wpe_draw2d_text(d2d, font, 2, 2, overlay, 0xFFFFFFFFu, 1.0f);
        WPE_TEST_ASSERT_OK(wpe_draw2d_submit(d2d, renderer));
    }
    WPE_TEST_ASSERT_OK(wpe_renderer_end_frame(renderer));
    compare_with_reference(name, wpe_renderer_framebuffer(renderer));

    wpe_draw2d_destroy(d2d);
    wpe_scene_renderer_destroy(sr);
    wpe_renderer_destroy(renderer);
    wpe_jobs_destroy(jobs);
    wpe_ecs_destroy(ecs);
    wpe_asset_release(assets.assets, font_asset);
    wpe_asset_release(assets.assets, model);
    wpe_test_assets_destroy(&assets);
}

static void test_golden_textured_helmet_with_text(void) {
    render_model_golden("helmet", "meshes/damaged_helmet.glb", 25.0f, 10.0f, 3.2f, 0.0f, 0.0f,
                        "Helmet 0123456789 !?#");
}

static void test_golden_skinned_figure_posed(void) {
    render_model_golden("rigged_figure", "meshes/RiggedFigure.glb", 30.0f, 5.0f, 1.6f, 0.75f, 0.6f, NULL);
}

static WPE_Entity golden_spawn(WPE_Ecs* ecs, WPE_Vec3 position, WPE_Quat rotation) {
    const WPE_Entity e = wpe_ecs_spawn(ecs);
    WPE_Transform t = wpe_transform_identity();
    t.position = position;
    t.rotation = rotation;
    WPE_TEST_ASSERT_OK(wpe_ecs_add_component(ecs, e, WPE_COMP_TRANSFORM, &t, NULL));
    return e;
}

/* Ground, boxes, a posed skinned figure, a shadow-casting sun, point and spot lights. */
static void render_lighting_golden(const char* name, bool post) {
    WPE_TestAssets assets = wpe_test_assets_create(WPE_GOLDEN_ASSETS_DIR, 0);
    WPE_Ecs* ecs = NULL;
    WPE_TEST_ASSERT_OK(wpe_ecs_create(&ecs));
    WPE_TEST_ASSERT_OK(wpe_scene_register_components(ecs));
    WPE_TEST_ASSERT_OK(wpe_graphics_register_components(ecs, assets.assets));

    const WPE_Material ground_mat = wpe_material_solid(wpe_vec4(0.6f, 0.6f, 0.62f, 1), 0.0f, 0.8f);
    const WPE_Material box_mat = wpe_material_solid(wpe_vec4(0.8f, 0.35f, 0.2f, 1), 0.0f, 0.4f);
    WPE_Model *ground_model = NULL, *box_model = NULL;
    WPE_TEST_ASSERT_OK(wpe_model_create_plane(20.0f, 12, &ground_mat, &ground_model));
    WPE_TEST_ASSERT_OK(wpe_model_create_box(wpe_vec3(1, 1, 1), &box_mat, &box_model));
    const WPE_AssetHandle ground = wpe_test_add_model(assets.assets, "golden/ground", ground_model);
    const WPE_AssetHandle box = wpe_test_add_model(assets.assets, "golden/box", box_model);
    const WPE_AssetHandle figure = wpe_test_load_model(assets.assets, "meshes/RiggedFigure.glb");

    WPE_Entity root;
    WPE_TEST_ASSERT_OK(wpe_model_instantiate(ecs, assets.assets, ground, NULL, &root));
    const float boxes[3][3] = {{-1.8f, 0.5f, 0.6f}, {1.6f, 0.5f, -1.2f}, {-0.6f, 0.5f, -2.0f}};
    for (int i = 0; i < 3; ++i) {
        WPE_TEST_ASSERT_OK(wpe_model_instantiate(ecs, assets.assets, box, NULL, &root));
        WPE_Transform* t = wpe_ecs_get_component(ecs, root, WPE_COMP_TRANSFORM);
        t->position = wpe_vec3(boxes[i][0], boxes[i][1], boxes[i][2]);
        t->rotation = wpe_quat_from_axis_angle(wpe_vec3(0, 1, 0), (float)i * 0.6f);
    }
    WPE_TEST_ASSERT_OK(
        wpe_model_instantiate(ecs, assets.assets, figure, &(WPE_InstantiateOptions){.animation = 0}, &root));
    WPE_Animator* animator = wpe_ecs_get_component(ecs, root, WPE_COMP_ANIMATOR);
    animator->time = 0.6f;
    wpe_animation_apply(ecs, animator);

    WPE_Light sun = wpe_light_default(WPE_LIGHT_DIRECTIONAL);
    sun.intensity = 0.8f;
    sun.cast_shadows = true;
    const WPE_Entity sun_e = golden_spawn(ecs, wpe_vec3(0, 5, 0),
                                          wpe_quat_mul(wpe_quat_from_axis_angle(wpe_vec3(0, 1, 0), 2.6f),
                                                       wpe_quat_from_axis_angle(wpe_vec3(1, 0, 0), -0.8f)));
    WPE_TEST_ASSERT_OK(wpe_ecs_add_component(ecs, sun_e, WPE_COMP_LIGHT, &sun, NULL));
    WPE_Light point = wpe_light_default(WPE_LIGHT_POINT);
    point.color = wpe_vec3(0.3f, 0.5f, 1.0f);
    point.intensity = 2.5f;
    point.range = 5.0f;
    const WPE_Entity point_e = golden_spawn(ecs, wpe_vec3(1.2f, 0.7f, 1.2f), wpe_quat_identity());
    WPE_TEST_ASSERT_OK(wpe_ecs_add_component(ecs, point_e, WPE_COMP_LIGHT, &point, NULL));
    WPE_Light spot = wpe_light_default(WPE_LIGHT_SPOT);
    spot.color = wpe_vec3(1.0f, 0.8f, 0.4f);
    spot.intensity = 3.0f;
    spot.range = 9.0f;
    const WPE_Entity spot_e =
        golden_spawn(ecs, wpe_vec3(-2.0f, 4.0f, -1.0f), wpe_quat_from_axis_angle(wpe_vec3(1, 0, 0), -1.3f));
    WPE_TEST_ASSERT_OK(wpe_ecs_add_component(ecs, spot_e, WPE_COMP_LIGHT, &spot, NULL));

    const WPE_Mat4 view =
        wpe_mat4_look_at(wpe_vec3(4.0f, 3.5f, 5.0f), wpe_vec3(0, 0.6f, -0.3f), wpe_vec3(0, 1, 0));
    WPE_Mat4 cam_world;
    WPE_TEST_ASSERT(wpe_mat4_inverse(&view, &cam_world));
    WPE_Transform cam_t = wpe_transform_identity();
    WPE_TEST_ASSERT(wpe_mat4_decompose(&cam_world, &cam_t.position, &cam_t.rotation, &cam_t.scale));
    const WPE_Entity camera = wpe_ecs_spawn(ecs);
    const WPE_Camera cam = wpe_camera_default();
    WPE_TEST_ASSERT_OK(wpe_ecs_add_component(ecs, camera, WPE_COMP_TRANSFORM, &cam_t, NULL));
    WPE_TEST_ASSERT_OK(wpe_ecs_add_component(ecs, camera, WPE_COMP_CAMERA, &cam, NULL));
    WPE_TEST_ASSERT_OK(wpe_scene_update_transforms(ecs));

    WPE_JobSystem* jobs = NULL;
    WPE_TEST_ASSERT_OK(wpe_jobs_create(3, &jobs));
    WPE_Renderer* renderer = NULL;
    WPE_TEST_ASSERT_OK(wpe_renderer_create(
        &(WPE_RendererDesc){.width = GOLDEN_W, .height = GOLDEN_H, .jobs = jobs}, &renderer));
    WPE_SceneRenderer* sr = NULL;
    WPE_TEST_ASSERT_OK(wpe_scene_renderer_create(&sr));
    WPE_SceneRenderSettings settings = wpe_scene_render_settings_default();
    settings.ambient = wpe_vec3(0.15f, 0.16f, 0.2f);
    settings.shadows.map_size = 512;
    settings.shadows.distance = 15.0f;

    WPE_TEST_ASSERT_OK(
        wpe_renderer_begin_frame(renderer, &(WPE_ClearDesc){.clear_color = true,
                                                            .color = wpe_color_rgba8(30, 34, 46, 255),
                                                            .clear_depth = true,
                                                            .depth = 1}));
    WPE_TEST_ASSERT_OK(wpe_scene_render(sr, renderer, ecs, camera, &settings, NULL));
    WPE_TEST_ASSERT_OK(wpe_renderer_end_frame(renderer));
    if (post) {
        WPE_PostChain* chain = NULL;
        WPE_TEST_ASSERT_OK(wpe_postfx_create(&chain));
        const WPE_PostEffect effects[3] = {wpe_postfx_ssao(NULL), wpe_postfx_vignette(NULL),
                                           wpe_postfx_ps1(NULL)};
        WPE_TEST_ASSERT_OK(wpe_postfx_apply(chain, renderer, effects, 3,
                                            &(WPE_PostCamera){.near_z = cam.near_z, .far_z = cam.far_z}));
        wpe_postfx_destroy(chain);
    }
    compare_with_reference(name, wpe_renderer_framebuffer(renderer));

    wpe_scene_renderer_destroy(sr);
    wpe_renderer_destroy(renderer);
    wpe_jobs_destroy(jobs);
    wpe_ecs_destroy(ecs);
    wpe_asset_release(assets.assets, figure);
    wpe_asset_release(assets.assets, box);
    wpe_asset_release(assets.assets, ground);
    wpe_test_assets_destroy(&assets);
}

static void test_golden_lights_and_shadows(void) {
    render_lighting_golden("lighting", false);
}

static void test_golden_post_processing(void) {
    render_lighting_golden("postfx", true);
}

WPE_TEST_MAIN(WPE_TEST_CASE(test_golden_cubes_scene), WPE_TEST_CASE(test_golden_textured_helmet_with_text),
              WPE_TEST_CASE(test_golden_skinned_figure_posed), WPE_TEST_CASE(test_golden_lights_and_shadows),
              WPE_TEST_CASE(test_golden_post_processing))
