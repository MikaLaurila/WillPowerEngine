#include "wpe_test.h"

#include "assets/font.h"
#include "graphics/animation.h"
#include "graphics/debug_lines.h"
#include "graphics/draw2d.h"
#include "graphics/graphics.h"
#include "graphics/material_shader.h"
#include "graphics/scene_render.h"

#define ASSET(path) WPE_TEST_ASSETS_DIR "/" path

static WPE_Ecs* make_world(void) {
    WPE_Ecs* ecs = NULL;
    WPE_TEST_ASSERT_OK(wpe_ecs_create(&ecs));
    WPE_TEST_ASSERT_OK(wpe_scene_register_components(ecs));
    WPE_TEST_ASSERT_OK(wpe_graphics_register_components(ecs));
    return ecs;
}

/* ---- Animation sampling ---------------------------------------------------------------------------- */

static void test_graphics_sample_linear_step_and_clamp(void) {
    const float times[3] = {0.0f, 1.0f, 3.0f};
    const float values[9] = {0, 0, 0, 10, 20, 30, 10, 20, 50};
    WPE_AnimationChannel ch = {.path = WPE_ANIM_TRANSLATION,
                               .interpolation = WPE_INTERP_LINEAR,
                               .key_count = 3,
                               .times = times,
                               .values = values};
    float out[4];
    wpe_animation_sample_channel(&ch, 0.5f, out);
    WPE_TEST_ASSERT_NEAR(5.0, out[0], 1e-6);
    WPE_TEST_ASSERT_NEAR(15.0, out[2], 1e-6);
    wpe_animation_sample_channel(&ch, 2.0f, out);
    WPE_TEST_ASSERT_NEAR(40.0, out[2], 1e-5);
    wpe_animation_sample_channel(&ch, -1.0f, out); /* before first key */
    WPE_TEST_ASSERT_NEAR(0.0, out[0], 0);
    wpe_animation_sample_channel(&ch, 99.0f, out); /* after last key */
    WPE_TEST_ASSERT_NEAR(50.0, out[2], 0);
    wpe_animation_sample_channel(&ch, 1.0f, out); /* exactly on a key */
    WPE_TEST_ASSERT_NEAR(30.0, out[2], 0);

    ch.interpolation = WPE_INTERP_STEP;
    wpe_animation_sample_channel(&ch, 0.99f, out);
    WPE_TEST_ASSERT_NEAR(0.0, out[0], 0);
    wpe_animation_sample_channel(&ch, 2.5f, out);
    WPE_TEST_ASSERT_NEAR(30.0, out[2], 0);

    /* A single key is constant. */
    ch = (WPE_AnimationChannel){.path = WPE_ANIM_SCALE, .key_count = 1, .times = times, .values = values + 3};
    wpe_animation_sample_channel(&ch, 5.0f, out);
    WPE_TEST_ASSERT_NEAR(20.0, out[1], 0);
}

static void test_graphics_sample_rotation_slerps_shortest_path(void) {
    const WPE_Quat a = wpe_quat_identity();
    const WPE_Quat b = wpe_quat_from_axis_angle(wpe_vec3(0, 1, 0), WPE_PI / 2);
    /* Store b negated: same rotation, opposite hemisphere; interpolation must still take the short arc. */
    const float times[2] = {0, 1};
    const float values[8] = {a.x, a.y, a.z, a.w, -b.x, -b.y, -b.z, -b.w};
    const WPE_AnimationChannel ch = {
        .path = WPE_ANIM_ROTATION, .key_count = 2, .times = times, .values = values};
    float out[4];
    wpe_animation_sample_channel(&ch, 0.5f, out);
    const WPE_Quat half = wpe_quat_from_axis_angle(wpe_vec3(0, 1, 0), WPE_PI / 4);
    const float dot = out[0] * half.x + out[1] * half.y + out[2] * half.z + out[3] * half.w;
    WPE_TEST_ASSERT_NEAR(1.0, fabsf(dot), 1e-4);
    WPE_TEST_ASSERT_NEAR(1.0, sqrtf(out[0] * out[0] + out[1] * out[1] + out[2] * out[2] + out[3] * out[3]),
                         1e-5);
}

static void test_graphics_sample_cubic_spline(void) {
    /* Hermite from 0 to 1 over [0, 2] with zero tangents: value at the midpoint is exactly 0.5. */
    const float times[2] = {0, 2};
    /* Per key: in-tangent(3), value(3), out-tangent(3). */
    const float values[18] = {0, 0, 0, 0, 0, 0, 0, 0, 0, /**/ 0, 0, 0, 1, 1, 1, 0, 0, 0};
    WPE_AnimationChannel ch = {.path = WPE_ANIM_TRANSLATION,
                               .interpolation = WPE_INTERP_CUBIC_SPLINE,
                               .key_count = 2,
                               .times = times,
                               .values = values};
    float out[4];
    wpe_animation_sample_channel(&ch, 1.0f, out);
    WPE_TEST_ASSERT_NEAR(0.5, out[0], 1e-6);
    wpe_animation_sample_channel(&ch, 0.5f, out); /* s = 0.25: 3s^2 - 2s^3 = 0.15625 */
    WPE_TEST_ASSERT_NEAR(0.15625, out[1], 1e-6);
    wpe_animation_sample_channel(&ch, 2.0f, out);
    WPE_TEST_ASSERT_NEAR(1.0, out[2], 0);

    /* Tangents are scaled by the key interval: out-tangent 1 on key 0 adds h10 * span. */
    const float tangent_values[18] = {0, 0, 0, 0, 0, 0, 1, 0, 0, /**/ 0, 0, 0, 0, 0, 0, 0, 0, 0};
    ch.values = tangent_values;
    wpe_animation_sample_channel(&ch, 1.0f, out); /* s = 0.5: h10 = 0.125, span = 2 */
    WPE_TEST_ASSERT_NEAR(0.25, out[0], 1e-6);
}

/* ---- Instantiation and animation playback --------------------------------------------------------- */

static void test_graphics_instantiate_rigged_figure(void) {
    WPE_Model* model = NULL;
    WPE_TEST_ASSERT_OK(wpe_model_load(ASSET("meshes/RiggedFigure.glb"), NULL, &model));
    WPE_Ecs* ecs = make_world();

    WPE_Entity root = WPE_ENTITY_NULL;
    WPE_TEST_ASSERT_OK(
        wpe_model_instantiate(ecs, model, &(WPE_InstantiateOptions){.animation = 0, .play = true}, &root));
    WPE_TEST_ASSERT_EQ_UINT(model->node_count + 1, wpe_ecs_entity_count(ecs));
    WPE_TEST_ASSERT_EQ_STR("model", wpe_scene_get_name(ecs, root));

    WPE_EcsPoolView meshes;
    WPE_TEST_ASSERT_OK(wpe_ecs_pool_view(ecs, WPE_COMP_MESH, &meshes));
    WPE_TEST_ASSERT_EQ_UINT(1, meshes.count);
    const WPE_MeshRenderer* mesh = meshes.data;
    WPE_TEST_ASSERT_EQ_UINT(0, mesh->skin);
    WPE_TEST_ASSERT_EQ_UINT(19, mesh->joint_count);
    for (uint32_t j = 0; j < mesh->joint_count; ++j)
        WPE_TEST_ASSERT(wpe_ecs_is_alive(ecs, mesh->joints[j]));

    /* Every node entity sits under the root. */
    WPE_EcsIter it = wpe_ecs_query(ecs, (WPE_EcsQuery){.all = WPE_ECS_MASK(WPE_COMP_TRANSFORM)});
    while (wpe_ecs_iter_next(&it)) {
        if (it.entity != root) WPE_TEST_ASSERT(wpe_scene_is_ancestor(ecs, root, it.entity));
    }

    /* Playback moves the joints; looping wraps the time. */
    const WPE_Entity joint = mesh->joints[5];
    WPE_TEST_ASSERT_OK(wpe_animation_update(ecs, 0.0f));
    const WPE_Quat before = ((WPE_Transform*)wpe_ecs_get_component(ecs, joint, WPE_COMP_TRANSFORM))->rotation;
    WPE_TEST_ASSERT_OK(wpe_animation_update(ecs, 0.4f));
    const WPE_Quat after = ((WPE_Transform*)wpe_ecs_get_component(ecs, joint, WPE_COMP_TRANSFORM))->rotation;
    WPE_TEST_ASSERT(fabsf(before.x - after.x) + fabsf(before.y - after.y) + fabsf(before.z - after.z) >
                    1e-4f);
    WPE_Animator* animator = wpe_ecs_get_component(ecs, root, WPE_COMP_ANIMATOR);
    const float duration = model->animations[0].duration;
    WPE_TEST_ASSERT_OK(wpe_animation_update(ecs, duration * 2.5f));
    WPE_TEST_ASSERT(animator->time >= 0.0f && animator->time < duration);
    WPE_TEST_ASSERT(animator->playing);

    /* Non-looping playback clamps at the end and stops; backwards stops at zero. */
    animator->loop = false;
    animator->time = 0.0f;
    WPE_TEST_ASSERT_OK(wpe_animation_update(ecs, duration + 1.0f));
    WPE_TEST_ASSERT_NEAR(duration, animator->time, 0);
    WPE_TEST_ASSERT(!animator->playing);
    animator->playing = true;
    animator->speed = -1.0f;
    WPE_TEST_ASSERT_OK(wpe_animation_update(ecs, duration * 3.0f));
    WPE_TEST_ASSERT_NEAR(0.0, animator->time, 0);
    WPE_TEST_ASSERT(!animator->playing);

    /* Destroying the instance releases joint/node arrays through the component hooks (leak-checked). */
    WPE_TEST_ASSERT_OK(wpe_scene_destroy_recursive(ecs, root));
    WPE_TEST_ASSERT_EQ_UINT(0, wpe_ecs_entity_count(ecs));
    wpe_ecs_destroy(ecs);
    wpe_model_destroy(model);
}

static void test_graphics_instantiate_validates_and_cleans_up(void) {
    WPE_Model* model = NULL;
    WPE_TEST_ASSERT_OK(wpe_model_load(ASSET("meshes/RiggedFigure.glb"), NULL, &model));
    WPE_Ecs* ecs = make_world();
    WPE_Entity root = WPE_ENTITY_NULL;
    wpe_log_set_stderr_enabled(false);
    WPE_TEST_ASSERT_EQ_INT(
        WPE_RESULT_ERROR_INVALID_ARG,
        wpe_model_instantiate(ecs, model, &(WPE_InstantiateOptions){.animation = 7}, &root));
    wpe_log_set_stderr_enabled(true);
    WPE_TEST_ASSERT(root == WPE_ENTITY_NULL);
    WPE_TEST_ASSERT_EQ_UINT(0, wpe_ecs_entity_count(ecs));
    WPE_TEST_ASSERT_EQ_INT(WPE_RESULT_ERROR_NULL_ARG, wpe_model_instantiate(ecs, NULL, NULL, &root));

    /* Instances are independent; despawning the world with live instances frees everything. */
    WPE_TEST_ASSERT_OK(wpe_model_instantiate(ecs, model, NULL, &root));
    WPE_TEST_ASSERT_OK(wpe_model_instantiate(ecs, model, &(WPE_InstantiateOptions){.animation = 0}, &root));
    WPE_TEST_ASSERT_EQ_UINT(2 * (model->node_count + 1), wpe_ecs_entity_count(ecs));
    wpe_ecs_destroy(ecs);
    wpe_model_destroy(model);
}

/* ---- Skinning (vertex shader) --------------------------------------------------------------------- */

static void test_graphics_material_vertex_shader_skinning(void) {
    const WPE_Material material = {.workflow = WPE_MATERIAL_UNLIT, .diffuse_factor = {{1, 1, 1, 1}}};
    WPE_Mat4 palette[2] = {wpe_mat4_identity(),
                           wpe_mat4_from_trs(wpe_vec3(10, 0, 0), wpe_quat_identity(), wpe_vec3(1, 1, 1))};
    WPE_MaterialUniforms u = {.mvp = wpe_mat4_identity(),
                              .world = wpe_mat4_identity(),
                              .normal_matrix = wpe_mat4_identity(),
                              .material = &material,
                              .joint_palette = palette,
                              .joint_count = 2};
    WPE_MeshVertex v = {.position = {{1, 2, 3}},
                        .normal = {{0, 1, 0}},
                        .joints = {0, 1, 0, 0},
                        .weights = {0.25f, 0.75f, 0, 0}};
    WPE_Vec4 clip;
    float varyings[WPE_MAX_VARYINGS];
    wpe_shader_material()->vertex(&u, &v, &clip, varyings);
    WPE_TEST_ASSERT_NEAR(1 + 7.5, clip.x, 1e-5); /* 0.25 * 1 + 0.75 * 11 */
    WPE_TEST_ASSERT_NEAR(2.0, clip.y, 1e-6);

    /* Out-of-range joint indices are ignored rather than read out of bounds. */
    v.joints[1] = 9;
    wpe_shader_material()->vertex(&u, &v, &clip, varyings);
    WPE_TEST_ASSERT_NEAR(1.0, clip.x, 1e-6);

    /* Without a palette the vertex is untouched. */
    u.joint_palette = NULL;
    wpe_shader_material()->vertex(&u, &v, &clip, varyings);
    WPE_TEST_ASSERT_NEAR(1.0, clip.x, 1e-6);
    WPE_TEST_ASSERT_NEAR(3.0, clip.z, 1e-6);
}

static void test_graphics_shininess_range(void) {
    WPE_TEST_ASSERT_NEAR(2.0, wpe_material_shininess(0.0f), 1e-4);
    WPE_TEST_ASSERT_NEAR(2048.0, wpe_material_shininess(1.0f), 1e-1);
    WPE_TEST_ASSERT_NEAR(2.0, wpe_material_shininess(-5.0f), 1e-4);
}

/* ---- Scene rendering ------------------------------------------------------------------------------ */

static WPE_Entity spawn_camera(WPE_Ecs* ecs, WPE_Vec3 position, WPE_Quat rotation) {
    const WPE_Entity camera = wpe_ecs_spawn(ecs);
    WPE_Transform t = wpe_transform_identity();
    t.position = position;
    t.rotation = rotation;
    const WPE_Camera c = wpe_camera_default();
    WPE_TEST_ASSERT_OK(wpe_ecs_add_component(ecs, camera, WPE_COMP_TRANSFORM, &t, NULL));
    WPE_TEST_ASSERT_OK(wpe_ecs_add_component(ecs, camera, WPE_COMP_CAMERA, &c, NULL));
    return camera;
}

static void test_graphics_scene_render_frustum_culling(void) {
    WPE_Model* model = NULL;
    WPE_TEST_ASSERT_OK(wpe_model_load(ASSET("meshes/damaged_helmet.glb"),
                                      &(WPE_ModelLoadOptions){.max_texture_size = 32}, &model));
    WPE_Ecs* ecs = make_world();
    WPE_Entity root;
    WPE_TEST_ASSERT_OK(wpe_model_instantiate(ecs, model, NULL, &root));
    const WPE_Entity camera = spawn_camera(ecs, wpe_vec3(0, 0, 5), wpe_quat_identity());
    WPE_TEST_ASSERT_OK(wpe_scene_update_transforms(ecs));

    WPE_Renderer* renderer = NULL;
    WPE_TEST_ASSERT_OK(wpe_renderer_create(&(WPE_RendererDesc){.width = 64, .height = 48}, &renderer));
    WPE_SceneRenderer* sr = NULL;
    WPE_TEST_ASSERT_OK(wpe_scene_renderer_create(&sr));

    /* Looking at the helmet: one primitive drawn. */
    WPE_SceneRenderStats stats;
    WPE_TEST_ASSERT_OK(wpe_renderer_begin_frame(renderer, NULL));
    WPE_TEST_ASSERT_OK(wpe_scene_render(sr, renderer, ecs, camera, NULL, &stats));
    WPE_TEST_ASSERT_OK(wpe_renderer_end_frame(renderer));
    WPE_TEST_ASSERT_EQ_UINT(1, stats.primitives_submitted);
    WPE_TEST_ASSERT_EQ_UINT(0, stats.primitives_culled);
    WPE_TEST_ASSERT(wpe_renderer_stats(renderer).fragments_shaded > 100);

    /* Turned around: culled before reaching the rasterizer. */
    ((WPE_Transform*)wpe_ecs_get_component(ecs, camera, WPE_COMP_TRANSFORM))->rotation =
        wpe_quat_from_axis_angle(wpe_vec3(0, 1, 0), WPE_PI);
    WPE_TEST_ASSERT_OK(wpe_scene_update_transforms(ecs));
    WPE_TEST_ASSERT_OK(wpe_renderer_begin_frame(renderer, NULL));
    WPE_TEST_ASSERT_OK(wpe_scene_render(sr, renderer, ecs, camera, NULL, &stats));
    WPE_TEST_ASSERT_OK(wpe_renderer_end_frame(renderer));
    WPE_TEST_ASSERT_EQ_UINT(0, stats.primitives_submitted);
    WPE_TEST_ASSERT_EQ_UINT(1, stats.primitives_culled);

    /* Invisible mesh renderers are skipped; invalid cameras are rejected. */
    WPE_EcsPoolView meshes;
    WPE_TEST_ASSERT_OK(wpe_ecs_pool_view(ecs, WPE_COMP_MESH, &meshes));
    ((WPE_MeshRenderer*)meshes.data)->visible = false;
    WPE_TEST_ASSERT_OK(wpe_renderer_begin_frame(renderer, NULL));
    WPE_TEST_ASSERT_OK(wpe_scene_render(sr, renderer, ecs, camera, NULL, &stats));
    wpe_log_set_stderr_enabled(false);
    WPE_TEST_ASSERT_EQ_INT(WPE_RESULT_ERROR_INVALID_ARG,
                           wpe_scene_render(sr, renderer, ecs, root, NULL, &stats));
    wpe_log_set_stderr_enabled(true);
    WPE_TEST_ASSERT_OK(wpe_renderer_end_frame(renderer));
    WPE_TEST_ASSERT_EQ_UINT(0, wpe_renderer_stats(renderer).draws);

    wpe_scene_renderer_destroy(sr);
    wpe_renderer_destroy(renderer);
    wpe_ecs_destroy(ecs);
    wpe_model_destroy(model);
}

static void test_graphics_camera_matrices_validation(void) {
    WPE_Ecs* ecs = make_world();
    const WPE_Entity camera = spawn_camera(ecs, wpe_vec3(0, 0, 0), wpe_quat_identity());
    WPE_TEST_ASSERT_OK(wpe_scene_update_transforms(ecs));
    WPE_Mat4 view, proj;
    WPE_TEST_ASSERT(wpe_scene_camera_matrices(ecs, camera, 1.5f, &view, &proj));
    WPE_TEST_ASSERT(!wpe_scene_camera_matrices(ecs, camera, 0.0f, &view, &proj));

    WPE_Camera* c = wpe_ecs_get_component(ecs, camera, WPE_COMP_CAMERA);
    c->near_z = 0.0f;
    WPE_TEST_ASSERT(!wpe_scene_camera_matrices(ecs, camera, 1.0f, &view, &proj));
    *c = wpe_camera_default();
    c->orthographic = true;
    c->ortho_height = 4.0f;
    WPE_TEST_ASSERT(wpe_scene_camera_matrices(ecs, camera, 2.0f, &view, &proj));
    /* Orthographic: the top edge of a 4-unit-tall view maps to NDC y = 1. */
    const WPE_Vec4 top = wpe_mat4_mul_vec4(&proj, wpe_vec4(0, 2, -1, 1));
    WPE_TEST_ASSERT_NEAR(1.0, top.y, 1e-5);

    /* A camera with zero scale has no valid view. */
    ((WPE_Transform*)wpe_ecs_get_component(ecs, camera, WPE_COMP_TRANSFORM))->scale = wpe_vec3(0, 0, 0);
    WPE_TEST_ASSERT_OK(wpe_scene_update_transforms(ecs));
    WPE_TEST_ASSERT(!wpe_scene_camera_matrices(ecs, camera, 1.0f, &view, &proj));
    wpe_ecs_destroy(ecs);
}

/* ---- 2D ------------------------------------------------------------------------------------------- */

static void test_graphics_draw2d_text_is_pixel_exact(void) {
    WPE_BitmapFont* font = NULL;
    WPE_TEST_ASSERT_OK(wpe_font_load(ASSET("fonts/small.png"), &font));
    WPE_Renderer* renderer = NULL;
    WPE_TEST_ASSERT_OK(wpe_renderer_create(&(WPE_RendererDesc){.width = 40, .height = 20}, &renderer));
    WPE_Draw2D* d = NULL;
    WPE_TEST_ASSERT_OK(wpe_draw2d_create(&d));

    WPE_TEST_ASSERT_OK(
        wpe_renderer_begin_frame(renderer, &(WPE_ClearDesc){.clear_color = true, .color = 0xFF000000u}));
    wpe_draw2d_begin(d, 40, 20);
    wpe_draw2d_text(d, font, 3, 4, "AB", 0xFFFFFFFFu, 1.0f);
    WPE_TEST_ASSERT_OK(wpe_draw2d_submit(d, renderer));
    WPE_TEST_ASSERT_OK(wpe_renderer_end_frame(renderer));

    /* Every framebuffer pixel equals the atlas glyph texel it should show (ink white, rest black). */
    const WPE_Framebuffer* fb = wpe_renderer_framebuffer(renderer);
    const WPE_TextureLevel* atlas = &font->atlas->levels[0];
    const WPE_FontGlyph* glyphs[2] = {wpe_font_glyph(font, 'A'), wpe_font_glyph(font, 'B')};
    uint32_t pen = 3, ink = 0;
    for (int g = 0; g < 2; ++g) {
        for (uint32_t y = 0; y < glyphs[g]->height; ++y) {
            for (uint32_t x = 0; x < glyphs[g]->width; ++x) {
                const uint32_t texel = atlas->texels[(glyphs[g]->y + y) * atlas->width + glyphs[g]->x + x];
                const WPE_Color expected = (texel >> 24) ? 0xFFFFFFFFu : 0xFF000000u;
                WPE_TEST_ASSERT_EQ_UINT(expected, fb->color[(4 + y) * 40 + pen + x]);
                ink += expected == 0xFFFFFFFFu;
            }
        }
        pen += glyphs[g]->width + font->glyph_spacing;
    }
    WPE_TEST_ASSERT(ink > 10);
    /* Nothing outside the text area was touched. */
    for (uint32_t x = 0; x < 40; ++x)
        WPE_TEST_ASSERT_EQ_UINT(0xFF000000u, fb->color[x]);

    /* Rectangles blend over 3D content and respect alpha. */
    WPE_TEST_ASSERT_OK(
        wpe_renderer_begin_frame(renderer, &(WPE_ClearDesc){.clear_color = true, .color = 0xFF0000FFu}));
    wpe_draw2d_begin(d, 40, 20);
    wpe_draw2d_rect(d, (WPE_Rect){0, 0, 10, 10}, wpe_color_rgba8(255, 0, 0, 128));
    wpe_draw2d_rect(d, (WPE_Rect){20, 0, 0, 10}, 0xFFFFFFFFu); /* zero-size: ignored */
    WPE_TEST_ASSERT_OK(wpe_draw2d_submit(d, renderer));
    WPE_TEST_ASSERT_OK(wpe_renderer_end_frame(renderer));
    WPE_TEST_ASSERT_EQ_UINT(wpe_color_rgba8(128, 0, 127, 255), fb->color[5 * 40 + 5]);
    WPE_TEST_ASSERT_EQ_UINT(0xFF0000FFu, fb->color[5 * 40 + 15]);
    WPE_TEST_ASSERT_EQ_UINT(1, wpe_renderer_stats(renderer).draws);

    wpe_draw2d_destroy(d);
    wpe_renderer_destroy(renderer);
    wpe_font_destroy(font);
}

/* ---- Debug lines ------------------------------------------------------------------------------------ */

static uint32_t count_color(const WPE_Framebuffer* fb, WPE_Color color) {
    uint32_t count = 0;
    for (uint32_t i = 0; i < fb->width * fb->height; ++i)
        count += fb->color[i] == color;
    return count;
}

static void test_graphics_debug_lines(void) {
    WPE_Renderer* renderer = NULL;
    WPE_TEST_ASSERT_OK(wpe_renderer_create(&(WPE_RendererDesc){.width = 64, .height = 48}, &renderer));
    WPE_DebugLines* lines = NULL;
    WPE_TEST_ASSERT_OK(wpe_debug_lines_create(&lines));
    const WPE_Mat4 identity = wpe_mat4_identity(); /* World coordinates are clip coordinates. */
    const WPE_Framebuffer* fb = wpe_renderer_framebuffer(renderer);
    const WPE_ClearDesc clear = {
        .clear_color = true, .color = 0xFF000000u, .clear_depth = true, .depth = 1.0f};

    /* A horizontal line across half the screen covers one pixel row of 32 pixels. */
    WPE_TEST_ASSERT_OK(wpe_renderer_begin_frame(renderer, &clear));
    wpe_debug_lines_begin(lines, &identity, 64, 48, 1.0f);
    wpe_debug_lines_add(lines, wpe_vec3(-0.5f, 0.01f, 0.0f), wpe_vec3(0.5f, 0.01f, 0.0f), 0xFFFF0000u);
    /* Entirely behind the near plane: rejected. */
    wpe_debug_lines_add(lines, wpe_vec3(0.0f, 0.0f, -2.0f), wpe_vec3(0.5f, 0.5f, -3.0f), 0xFF00FF00u);
    WPE_TEST_ASSERT_EQ_UINT(1, wpe_debug_lines_count(lines));
    WPE_TEST_ASSERT_OK(wpe_debug_lines_submit(lines, renderer, true));
    WPE_TEST_ASSERT_OK(wpe_renderer_end_frame(renderer));
    WPE_TEST_ASSERT_EQ_UINT(32, count_color(fb, 0xFFFF0000u));
    WPE_TEST_ASSERT_EQ_UINT(0xFFFF0000u, fb->color[23 * 64 + 32]);
    WPE_TEST_ASSERT_NEAR(1.0, fb->depth[23 * 64 + 32], 0); /* No depth writes. */

    /* A wider vertical line: 3 pixel columns over half the height. */
    WPE_TEST_ASSERT_OK(wpe_renderer_begin_frame(renderer, &clear));
    wpe_debug_lines_begin(lines, &identity, 64, 48, 3.0f);
    wpe_debug_lines_add(lines, wpe_vec3(0.01f, -0.5f, 0.0f), wpe_vec3(0.01f, 0.5f, 0.0f), 0xFF00FF00u);
    WPE_TEST_ASSERT_OK(wpe_debug_lines_submit(lines, renderer, true));
    WPE_TEST_ASSERT_OK(wpe_renderer_end_frame(renderer));
    WPE_TEST_ASSERT_EQ_UINT(3 * 24, count_color(fb, 0xFF00FF00u));

    /* Depth testing hides lines behind the stored depth unless disabled. */
    const WPE_ClearDesc near_depth = {
        .clear_color = true, .color = 0xFF000000u, .clear_depth = true, .depth = 0.25f};
    for (int test = 0; test < 2; ++test) {
        WPE_TEST_ASSERT_OK(wpe_renderer_begin_frame(renderer, &near_depth));
        wpe_debug_lines_begin(lines, &identity, 64, 48, 1.0f);
        wpe_debug_lines_add(lines, wpe_vec3(-0.5f, 0.01f, 0.0f), wpe_vec3(0.5f, 0.01f, 0.0f), 0xFFFF0000u);
        WPE_TEST_ASSERT_OK(wpe_debug_lines_submit(lines, renderer, test == 0));
        WPE_TEST_ASSERT_OK(wpe_renderer_end_frame(renderer));
        WPE_TEST_ASSERT_EQ_UINT(test == 0 ? 0u : 32u, count_color(fb, 0xFFFF0000u));
    }
    wpe_debug_lines_destroy(lines);
    wpe_renderer_destroy(renderer);
}

WPE_TEST_MAIN(WPE_TEST_CASE(test_graphics_sample_linear_step_and_clamp),
              WPE_TEST_CASE(test_graphics_sample_rotation_slerps_shortest_path),
              WPE_TEST_CASE(test_graphics_sample_cubic_spline),
              WPE_TEST_CASE(test_graphics_instantiate_rigged_figure),
              WPE_TEST_CASE(test_graphics_instantiate_validates_and_cleans_up),
              WPE_TEST_CASE(test_graphics_material_vertex_shader_skinning),
              WPE_TEST_CASE(test_graphics_shininess_range),
              WPE_TEST_CASE(test_graphics_scene_render_frustum_culling),
              WPE_TEST_CASE(test_graphics_camera_matrices_validation),
              WPE_TEST_CASE(test_graphics_draw2d_text_is_pixel_exact),
              WPE_TEST_CASE(test_graphics_debug_lines))
