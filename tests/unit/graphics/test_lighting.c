#include "wpe_test.h"
#include "wpe_test_assets.h"

#include <stdio.h>

#include "assets/procedural.h"
#include "graphics/graphics.h"
#include "graphics/material_shader.h"
#include "graphics/postfx.h"
#include "graphics/scene_render.h"

#define ASSET(path) WPE_TEST_ASSETS_DIR "/" path

enum { S = 64 };

/* ---- Helpers --------------------------------------------------------------------------------------- */

typedef struct TestScene {
    WPE_Ecs* ecs;
    WPE_Renderer* renderer;
    WPE_SceneRenderer* scene_renderer;
    WPE_TestAssets assets;
    uint32_t model_count;
    WPE_Entity camera;
} TestScene;

static void scene_init(TestScene* s) {
    *s = (TestScene){0};
    s->assets = wpe_test_assets_create(NULL, 0);
    WPE_TEST_ASSERT_OK(wpe_ecs_create(&s->ecs));
    WPE_TEST_ASSERT_OK(wpe_scene_register_components(s->ecs));
    WPE_TEST_ASSERT_OK(wpe_graphics_register_components(s->ecs, s->assets.assets));
    WPE_TEST_ASSERT_OK(wpe_renderer_create(&(WPE_RendererDesc){.width = S, .height = S}, &s->renderer));
    WPE_TEST_ASSERT_OK(wpe_scene_renderer_create(&s->scene_renderer));
}

static void scene_free(TestScene* s) {
    wpe_ecs_destroy(s->ecs); /* Releases the models (the entities hold the only references). */
    WPE_TEST_ASSERT_EQ_UINT(0, wpe_asset_manager_stats(s->assets.assets).assets);
    wpe_test_assets_destroy(&s->assets);
    wpe_scene_renderer_destroy(s->scene_renderer);
    wpe_renderer_destroy(s->renderer);
}

static WPE_Entity spawn(TestScene* s, WPE_Vec3 position, WPE_Quat rotation) {
    const WPE_Entity e = wpe_ecs_spawn(s->ecs);
    WPE_Transform t = wpe_transform_identity();
    t.position = position;
    t.rotation = rotation;
    WPE_TEST_ASSERT_OK(wpe_ecs_add_component(s->ecs, e, WPE_COMP_TRANSFORM, &t, NULL));
    return e;
}

/* Hands a procedural model to the asset manager and instantiates it; the entities own it. */
static void add_model(TestScene* s, WPE_Model* model, WPE_Vec3 position) {
    char name[32];
    snprintf(name, sizeof name, "test/model%u", s->model_count++);
    const WPE_AssetHandle handle = wpe_test_add_model(s->assets.assets, name, model);
    WPE_Entity root;
    WPE_TEST_ASSERT_OK(wpe_model_instantiate(s->ecs, s->assets.assets, handle, NULL, &root));
    wpe_asset_release(s->assets.assets, handle);
    ((WPE_Transform*)wpe_ecs_get_component(s->ecs, root, WPE_COMP_TRANSFORM))->position = position;
}

/* Camera at `height` above the origin looking straight down (-Y), image top = -Z. */
static void add_top_camera(TestScene* s, float height) {
    s->camera = spawn(s, wpe_vec3(0, height, 0), wpe_quat_from_axis_angle(wpe_vec3(1, 0, 0), -WPE_PI / 2));
    WPE_Camera c = wpe_camera_default();
    c.fov_y = WPE_PI / 2;
    WPE_TEST_ASSERT_OK(wpe_ecs_add_component(s->ecs, s->camera, WPE_COMP_CAMERA, &c, NULL));
}

static void add_light(TestScene* s, WPE_Light light, WPE_Vec3 position, WPE_Quat rotation) {
    const WPE_Entity e = spawn(s, position, rotation);
    WPE_TEST_ASSERT_OK(wpe_ecs_add_component(s->ecs, e, WPE_COMP_LIGHT, &light, NULL));
}

static const WPE_Framebuffer* render_scene(TestScene* s, const WPE_SceneRenderSettings* settings,
                                           WPE_SceneRenderStats* stats) {
    WPE_TEST_ASSERT_OK(wpe_scene_update_transforms(s->ecs));
    WPE_TEST_ASSERT_OK(wpe_renderer_begin_frame(
        s->renderer,
        &(WPE_ClearDesc){.clear_color = true, .color = 0xFF000000u, .clear_depth = true, .depth = 1.0f}));
    WPE_TEST_ASSERT_OK(wpe_scene_render(s->scene_renderer, s->renderer, s->ecs, s->camera, settings, stats));
    WPE_TEST_ASSERT_OK(wpe_renderer_end_frame(s->renderer));
    return wpe_renderer_framebuffer(s->renderer);
}

static float luminance(WPE_Color c) {
    const WPE_Vec4 v = wpe_color_to_vec4(c);
    return 0.299f * v.x + 0.587f * v.y + 0.114f * v.z;
}

/* Settings with no default sun, no ambient, no shadows: only explicit lights contribute. */
static WPE_SceneRenderSettings dark_settings(void) {
    WPE_SceneRenderSettings settings = wpe_scene_render_settings_default();
    settings.ambient = wpe_vec3(0, 0, 0);
    settings.default_sun = false;
    settings.subdivision_levels = 0;
    return settings;
}

static WPE_Model* make_ground(float size, uint32_t segments) {
    const WPE_Material white = wpe_material_solid(wpe_vec4(1, 1, 1, 1), 0.0f, 1.0f);
    WPE_Model* plane = NULL;
    WPE_TEST_ASSERT_OK(wpe_model_create_plane(size, segments, &white, &plane));
    return plane;
}

/* ---- Light math ------------------------------------------------------------------------------------ */

static void test_lighting_attenuation_shape(void) {
    WPE_TEST_ASSERT_NEAR(1.0, wpe_light_attenuation(0.0f, 10.0f), 1e-6);
    WPE_TEST_ASSERT_NEAR(0.0, wpe_light_attenuation(10.0f, 10.0f), 0);
    WPE_TEST_ASSERT_NEAR(0.0, wpe_light_attenuation(50.0f, 10.0f), 0);
    WPE_TEST_ASSERT_NEAR(0.0, wpe_light_attenuation(1.0f, 0.0f), 0);
    float previous = 2.0f;
    for (float d = 0.0f; d < 10.0f; d += 0.25f) {
        const float a = wpe_light_attenuation(d, 10.0f);
        WPE_TEST_ASSERT(a < previous && a >= 0.0f);
        previous = a;
    }
    /* Scale invariance: the falloff depends on distance / range only. */
    WPE_TEST_ASSERT_NEAR(wpe_light_attenuation(2.0f, 10.0f), wpe_light_attenuation(20.0f, 100.0f), 1e-6);
}

static void test_lighting_shadow_visibility_pcf(void) {
    /* 4x4 map: left half occluder at depth 0.2, right half empty (1.0). */
    float depth[16];
    for (int y = 0; y < 4; ++y) {
        for (int x = 0; x < 4; ++x)
            depth[y * 4 + x] = x < 2 ? 0.2f : 1.0f;
    }
    const WPE_ShadowSampler shadow = {.depth = depth, .size = 4, .bias = 0.01f};
    WPE_TEST_ASSERT_NEAR(0.0, wpe_shadow_visibility(&shadow, 0.1f, 0.5f, 0.5f), 1e-6);  /* deep in shadow */
    WPE_TEST_ASSERT_NEAR(1.0, wpe_shadow_visibility(&shadow, 0.95f, 0.5f, 0.5f), 1e-6); /* fully lit */
    const float edge = wpe_shadow_visibility(&shadow, 0.6f, 0.5f, 0.5f); /* PCF across the edge */
    WPE_TEST_ASSERT(edge > 0.0f && edge < 1.0f);
    WPE_TEST_ASSERT_NEAR(1.0, wpe_shadow_visibility(&shadow, 0.1f, 0.5f, 0.15f),
                         1e-6); /* receiver in front of occluder */
    WPE_TEST_ASSERT_NEAR(1.0, wpe_shadow_visibility(&shadow, -0.5f, 0.5f, 0.5f), 0); /* outside the map */
    WPE_TEST_ASSERT_NEAR(1.0, wpe_shadow_visibility(NULL, 0.5f, 0.5f, 0.5f), 0);
}

/* ---- Scene lighting -------------------------------------------------------------------------------- */

static void test_lighting_point_light_pool(void) {
    TestScene s;
    scene_init(&s);
    add_model(&s, make_ground(20.0f, 32), wpe_vec3(0, 0, 0));
    add_top_camera(&s, 6.0f);
    WPE_Light point = wpe_light_default(WPE_LIGHT_POINT);
    point.range = 4.0f;
    add_light(&s, point, wpe_vec3(0, 1, 0), wpe_quat_identity());

    WPE_SceneRenderSettings settings = dark_settings();
    WPE_SceneRenderStats stats;
    const WPE_Framebuffer* fb = render_scene(&s, &settings, &stats);
    WPE_TEST_ASSERT_EQ_UINT(1, stats.lights);
    const float center = luminance(fb->color[(S / 2) * S + S / 2]);
    const float mid = luminance(fb->color[(S / 2) * S + S / 2 + 10]);
    const float corner = luminance(fb->color[2 * S + 2]); /* beyond the light's range */
    /* Falloff is 1 / (1 + (4d/range)^2), i.e. 0.5 at a quarter of the range (here 1 unit below the light). */
    WPE_TEST_ASSERT_MSG(center > 0.4f && mid < center && corner == 0.0f, "center %.3f mid %.3f corner %.3f",
                        (double)center, (double)mid, (double)corner);
    scene_free(&s);
}

static void test_lighting_spot_cone(void) {
    TestScene s;
    scene_init(&s);
    add_model(&s, make_ground(20.0f, 48), wpe_vec3(0, 0, 0));
    add_top_camera(&s, 6.0f);
    /* Spot pointing straight down (-Z rotated onto -Y) from 3 units up with a narrow cone. */
    WPE_Light spot = wpe_light_default(WPE_LIGHT_SPOT);
    spot.range = 10.0f;
    spot.inner_cone = 0.15f;
    spot.outer_cone = 0.25f;
    add_light(&s, spot, wpe_vec3(0, 3, 0), wpe_quat_from_axis_angle(wpe_vec3(1, 0, 0), -WPE_PI / 2));

    WPE_SceneRenderSettings settings = dark_settings();
    const WPE_Framebuffer* fb = render_scene(&s, &settings, NULL);
    /* Cone radius on the ground ~ 3 * tan(0.25) = 0.77 units; at 6 units with 90 degree FOV a unit is ~5.3
     * px. */
    WPE_TEST_ASSERT(luminance(fb->color[(S / 2) * S + S / 2]) > 0.3f);
    WPE_TEST_ASSERT_NEAR(0.0, luminance(fb->color[(S / 2) * S + S / 2 + 12]), 0);
    scene_free(&s);
}

static void test_lighting_more_lights_than_object_limit(void) {
    /* Six lights on one object: only WPE_MAX_OBJECT_LIGHTS are used and the strongest win. */
    TestScene s;
    scene_init(&s);
    add_model(&s, make_ground(4.0f, 8), wpe_vec3(0, 0, 0));
    add_top_camera(&s, 3.0f);
    for (int i = 0; i < 6; ++i) {
        WPE_Light point = wpe_light_default(WPE_LIGHT_POINT);
        point.range = 6.0f;
        point.intensity = i == 5 ? 4.0f : 0.05f; /* The last one dominates. */
        add_light(&s, point, wpe_vec3((float)i * 0.1f, 1.0f, 0), wpe_quat_identity());
    }
    WPE_SceneRenderSettings settings = dark_settings();
    WPE_SceneRenderStats stats;
    const WPE_Framebuffer* fb = render_scene(&s, &settings, &stats);
    WPE_TEST_ASSERT_EQ_UINT(6, stats.lights);
    WPE_TEST_ASSERT(luminance(fb->color[(S / 2) * S + S / 2]) > 0.9f);
    scene_free(&s);
}

static void test_lighting_directional_shadow(void) {
    TestScene s;
    scene_init(&s);
    add_model(&s, make_ground(20.0f, 16), wpe_vec3(0, 0, 0));
    const WPE_Material gray = wpe_material_solid(wpe_vec4(0.5f, 0.5f, 0.5f, 1), 0.0f, 1.0f);
    WPE_Model* box = NULL;
    WPE_TEST_ASSERT_OK(wpe_model_create_box(wpe_vec3(1, 1, 1), &gray, &box));
    add_model(&s, box, wpe_vec3(0, 2, 0));
    add_top_camera(&s, 6.0f);
    /* Sun travelling down and toward +X at 45 degrees: the shadow lands 2 units to the +X side. */
    WPE_Light sun = wpe_light_default(WPE_LIGHT_DIRECTIONAL);
    sun.cast_shadows = true;
    const WPE_Quat down_x = wpe_quat_mul(wpe_quat_from_axis_angle(wpe_vec3(0, 1, 0), -WPE_PI / 2),
                                         wpe_quat_from_axis_angle(wpe_vec3(1, 0, 0), -WPE_PI / 4));
    add_light(&s, sun, wpe_vec3(0, 5, 0), down_x);

    WPE_SceneRenderSettings settings = dark_settings();
    settings.shadows.map_size = 256;
    settings.shadows.distance = 12.0f;
    WPE_SceneRenderStats stats;
    const WPE_Framebuffer* fb = render_scene(&s, &settings, &stats);
    WPE_TEST_ASSERT(stats.shadow_casters >= 1);
    WPE_TEST_ASSERT(wpe_scene_renderer_shadow_map(s.scene_renderer) != NULL);
    /* Ground units -> pixels: camera at 6, fov 90: half-width 6 units over 32 px. */
    const int shadow_x = S / 2 + (int)(2.0f * 32.0f / 6.0f);
    const int lit_x = S / 2 - (int)(4.0f * 32.0f / 6.0f);
    const float in_shadow = luminance(fb->color[(S / 2) * S + shadow_x]);
    const float lit = luminance(fb->color[(S / 2) * S + lit_x]);
    WPE_TEST_ASSERT_MSG(in_shadow < 0.05f && lit > 0.4f, "shadow %.3f lit %.3f", (double)in_shadow,
                        (double)lit);

    settings.shadows.enabled = false;
    fb = render_scene(&s, &settings, &stats);
    WPE_TEST_ASSERT(luminance(fb->color[(S / 2) * S + shadow_x]) > 0.4f);
    WPE_TEST_ASSERT_EQ_UINT(0, stats.shadow_casters);
    WPE_TEST_ASSERT(wpe_scene_renderer_shadow_map(s.scene_renderer) == NULL);
    scene_free(&s);
}

static void test_lighting_fog_and_default_sun(void) {
    TestScene s;
    scene_init(&s);
    add_model(&s, make_ground(400.0f, 4), wpe_vec3(0, 0, 0));
    add_top_camera(&s, 6.0f);
    WPE_SceneRenderSettings settings =
        wpe_scene_render_settings_default(); /* default sun, no lights in world */
    settings.subdivision_levels = 0;
    const WPE_Framebuffer* fb = render_scene(&s, &settings, NULL);
    WPE_TEST_ASSERT(luminance(fb->color[(S / 2) * S + S / 2]) > 0.4f);

    /* Fog fully covering everything beyond 1 unit turns the ground into the fog color. */
    settings.fog =
        (WPE_FogParams){.mode = WPE_FOG_LINEAR, .color = wpe_vec3(1, 0, 0), .start = 0.0f, .end = 1.0f};
    fb = render_scene(&s, &settings, NULL);
    WPE_TEST_ASSERT_EQ_UINT(0xFFFF0000u, fb->color[(S / 2) * S + S / 2]);
    scene_free(&s);
}

static void test_lighting_subdivision_near_camera(void) {
    TestScene s;
    scene_init(&s);
    add_model(&s, make_ground(20.0f, 1), wpe_vec3(0, 0, 0)); /* two huge triangles */
    add_top_camera(&s, 2.0f);
    WPE_SceneRenderSettings settings = dark_settings();
    settings.subdivision_levels = 3;
    settings.subdivision_edge_pixels = 16.0f;
    render_scene(&s, &settings, NULL);
    const WPE_RenderStats refined = wpe_renderer_stats(s.renderer);
    settings.subdivision_levels = 0;
    render_scene(&s, &settings, NULL);
    const WPE_RenderStats plain = wpe_renderer_stats(s.renderer);
    WPE_TEST_ASSERT_EQ_UINT(2, refined.triangles_subdivided);
    WPE_TEST_ASSERT_EQ_UINT(0, plain.triangles_subdivided);
    /* Many sub-triangles fall outside the view, but those on screen still outnumber the originals. */
    WPE_TEST_ASSERT_MSG(refined.triangles_rasterized > plain.triangles_rasterized * 2, "refined %u plain %u",
                        refined.triangles_rasterized, plain.triangles_rasterized);
    scene_free(&s);
}

/* ---- Procedural models and tangents ----------------------------------------------------------------- */

static void test_lighting_procedural_models(void) {
    WPE_Model* plane = NULL;
    WPE_TEST_ASSERT_OK(wpe_model_create_plane(2.0f, 3, NULL, &plane));
    WPE_TEST_ASSERT_EQ_UINT(16, plane->vertex_count);
    WPE_TEST_ASSERT_EQ_UINT(54, plane->index_count);
    WPE_TEST_ASSERT_NEAR(-1.0, plane->primitives[0].bounds.min.x, 1e-6);
    WPE_TEST_ASSERT_NEAR(1.0, plane->primitives[0].bounds.max.z, 1e-6);
    /* Every triangle faces +Y (counter-clockwise from above). */
    for (uint32_t t = 0; t < plane->index_count; t += 3) {
        const WPE_Vec3 a = plane->vertices[plane->indices[t]].position,
                       b = plane->vertices[plane->indices[t + 1]].position,
                       c = plane->vertices[plane->indices[t + 2]].position;
        WPE_TEST_ASSERT(wpe_vec3_cross(wpe_vec3_sub(b, a), wpe_vec3_sub(c, a)).y > 0.0f);
    }
    wpe_model_destroy(plane);

    WPE_Model* box = NULL;
    WPE_TEST_ASSERT_OK(wpe_model_create_box(wpe_vec3(2, 4, 6), NULL, &box));
    WPE_TEST_ASSERT_NEAR(-2.0, box->meshes[0].bounds.min.y, 1e-6);
    WPE_TEST_ASSERT_NEAR(3.0, box->meshes[0].bounds.max.z, 1e-6);
    /* Outward winding: each face normal agrees with its geometric normal. */
    for (uint32_t t = 0; t < box->index_count; t += 3) {
        const WPE_MeshVertex* a = &box->vertices[box->indices[t]];
        const WPE_Vec3 n =
            wpe_vec3_cross(wpe_vec3_sub(box->vertices[box->indices[t + 1]].position, a->position),
                           wpe_vec3_sub(box->vertices[box->indices[t + 2]].position, a->position));
        WPE_TEST_ASSERT(wpe_vec3_dot(n, a->normal) > 0.0f);
    }
    wpe_model_destroy(box);

    const WPE_MeshVertex v[3] = {
        {.position = {{0, 0, 0}}}, {.position = {{1, 0, 0}}}, {.position = {{0, 1, 0}}}};
    const uint32_t bad[3] = {0, 1, 3};
    WPE_Model* m = NULL;
    wpe_log_set_stderr_enabled(false);
    WPE_TEST_ASSERT_EQ_INT(WPE_RESULT_ERROR_INVALID_ARG, wpe_model_create_mesh(v, 3, bad, 3, NULL, NULL, &m));
    WPE_Material textured = wpe_material_solid(wpe_vec4(1, 1, 1, 1), 0, 1);
    textured.diffuse_texture.texture = 0;
    const uint32_t good[3] = {0, 1, 2};
    WPE_TEST_ASSERT_EQ_INT(WPE_RESULT_ERROR_INVALID_ARG,
                           wpe_model_create_mesh(v, 3, good, 3, &textured, NULL, &m));
    wpe_log_set_stderr_enabled(true);
    WPE_TEST_ASSERT_EQ_INT(WPE_RESULT_ERROR_INVALID_ARG, wpe_model_create_plane(0.0f, 4, NULL, &m));
    WPE_TEST_ASSERT(m == NULL);
}

static void test_lighting_generated_tangents_are_orthonormal(void) {
    /* The helmet has a normal map but no TANGENT attribute, so tangents are generated at load. */
    WPE_Model* helmet = NULL;
    WPE_TEST_ASSERT_OK(wpe_model_load(ASSET("meshes/damaged_helmet.glb"),
                                      &(WPE_ModelLoadOptions){.max_texture_size = 16}, &helmet));
    for (uint32_t v = 0; v < helmet->vertex_count; v += 13) {
        const WPE_MeshVertex* vert = &helmet->vertices[v];
        const WPE_Vec3 t = wpe_vec3(vert->tangent[0], vert->tangent[1], vert->tangent[2]);
        WPE_TEST_ASSERT_NEAR(1.0, wpe_vec3_length(t), 1e-3);
        WPE_TEST_ASSERT_NEAR(0.0, wpe_vec3_dot(t, wpe_vec3_normalize(vert->normal)), 1e-3);
        WPE_TEST_ASSERT(vert->tangent[3] == 1.0f || vert->tangent[3] == -1.0f);
    }
    wpe_model_destroy(helmet);
}

/* ---- Post-processing -------------------------------------------------------------------------------- */

static WPE_Renderer* filled_renderer(WPE_Color color) {
    WPE_Renderer* r = NULL;
    WPE_TEST_ASSERT_OK(wpe_renderer_create(&(WPE_RendererDesc){.width = S, .height = S}, &r));
    WPE_TEST_ASSERT_OK(wpe_renderer_begin_frame(
        r, &(WPE_ClearDesc){.clear_color = true, .color = color, .clear_depth = true, .depth = 1}));
    WPE_TEST_ASSERT_OK(wpe_renderer_end_frame(r));
    return r;
}

static const WPE_PostCamera k_post_camera = {.near_z = 0.1f, .far_z = 100.0f};

static void test_lighting_postfx_color_effects(void) {
    WPE_PostChain* chain = NULL;
    WPE_TEST_ASSERT_OK(wpe_postfx_create(&chain));

    WPE_Renderer* r = filled_renderer(wpe_color_rgba8(200, 100, 50, 255));
    const WPE_ColorAdjustParams gray = {.contrast = 1.0f, .saturation = 0.0f};
    WPE_PostEffect effect = wpe_postfx_color_adjust(&gray);
    WPE_TEST_ASSERT_OK(wpe_postfx_apply(chain, r, &effect, 1, &k_post_camera));
    const WPE_Color px = wpe_renderer_framebuffer(r)->color[0];
    WPE_TEST_ASSERT(((px >> 16) & 0xFF) == ((px >> 8) & 0xFF) && ((px >> 8) & 0xFF) == (px & 0xFF));

    /* PS1 quantization to 2 levels without dither: every channel becomes 0 or 255. */
    const WPE_Ps1Params two = {.levels = 2, .dither_strength = 0.0f};
    effect = wpe_postfx_ps1(&two);
    WPE_TEST_ASSERT_OK(wpe_postfx_apply(chain, r, &effect, 1, &k_post_camera));
    for (int c = 0; c < 3; ++c) {
        const uint32_t channel = (wpe_renderer_framebuffer(r)->color[S * 7 + 9] >> (c * 8)) & 0xFF;
        WPE_TEST_ASSERT(channel == 0 || channel == 255);
    }
    wpe_renderer_destroy(r);

    /* Vignette leaves the center and darkens corners; disabled effects are skipped. */
    r = filled_renderer(0xFFFFFFFFu);
    WPE_PostEffect effects[2] = {wpe_postfx_vignette(NULL), wpe_postfx_ps1(&two)};
    effects[1].enabled = false;
    WPE_TEST_ASSERT_OK(wpe_postfx_apply(chain, r, effects, 2, &k_post_camera));
    const WPE_Framebuffer* fb = wpe_renderer_framebuffer(r);
    WPE_TEST_ASSERT_EQ_UINT(0xFFFFFFFFu, fb->color[(S / 2) * S + S / 2]);
    WPE_TEST_ASSERT(luminance(fb->color[0]) < 0.7f);

    /* Chromatic aberration: a white column on black splits into red and blue fringes off-center. */
    WPE_Framebuffer* writable = wpe_renderer_framebuffer_for_write(r);
    for (uint32_t i = 0; i < S * S; ++i)
        writable->color[i] = (i % S == S - 8) ? 0xFFFFFFFFu : 0xFF000000u;
    const WPE_ChromaticAberrationParams ca = {.strength_pixels = 6.0f};
    effect = wpe_postfx_chromatic_aberration(&ca);
    WPE_TEST_ASSERT_OK(wpe_postfx_apply(chain, r, &effect, 1, &k_post_camera));
    bool red_only = false, blue_only = false;
    for (uint32_t x = 0; x < S; ++x) {
        const WPE_Color c = fb->color[(S / 2) * S + x];
        red_only = red_only || c == 0xFFFF0000u;
        blue_only = blue_only || c == 0xFF0000FFu;
    }
    WPE_TEST_ASSERT(red_only && blue_only);

    /* Applying while a frame records is rejected; a broken effect is rejected. */
    WPE_TEST_ASSERT_OK(wpe_renderer_begin_frame(r, NULL));
    wpe_log_set_stderr_enabled(false);
    WPE_TEST_ASSERT_EQ_INT(WPE_RESULT_ERROR_INVALID_STATE,
                           wpe_postfx_apply(chain, r, &effect, 1, &k_post_camera));
    WPE_TEST_ASSERT_OK(wpe_renderer_end_frame(r));
    const WPE_PostEffect broken = {.name = "broken", .enabled = true};
    WPE_TEST_ASSERT_EQ_INT(WPE_RESULT_ERROR_INVALID_ARG,
                           wpe_postfx_apply(chain, r, &broken, 1, &k_post_camera));
    wpe_log_set_stderr_enabled(true);
    wpe_renderer_destroy(r);
    wpe_postfx_destroy(chain);
}

static void test_lighting_postfx_ssao_darkens_creases_only(void) {
    /* Depth step: left half near (z ~ 2), right half farther (z ~ 2.5). Occlusion appears on the far
     * side next to the step and nowhere on flat areas or the near side. */
    WPE_Renderer* r = filled_renderer(0xFFFFFFFFu);
    WPE_Framebuffer* fb = wpe_renderer_framebuffer_for_write(r);
    const float n = k_post_camera.near_z, f = k_post_camera.far_z;
    for (uint32_t y = 0; y < S; ++y) {
        for (uint32_t x = 0; x < S; ++x) {
            const float z = x < S / 2 ? 2.0f : 2.5f;
            const float ndc = (f + n - 2.0f * n * f / z) / (f - n); /* inverse of wpe_postfx_linear_depth */
            fb->depth[y * S + x] = ndc * 0.5f + 0.5f;
        }
    }
    WPE_TEST_ASSERT_NEAR(2.5, wpe_postfx_linear_depth(&k_post_camera, fb->depth[S - 1]), 1e-3);

    WPE_PostChain* chain = NULL;
    WPE_TEST_ASSERT_OK(wpe_postfx_create(&chain));
    const WPE_SsaoParams params = {
        .radius_pixels = 4.0f, .strength = 1.0f, .depth_range = 1.0f, .bias = 0.05f};
    const WPE_PostEffect effect = wpe_postfx_ssao(&params);
    WPE_TEST_ASSERT_OK(wpe_postfx_apply(chain, r, &effect, 1, &k_post_camera));

    const uint32_t row = (S / 2) * S;
    WPE_TEST_ASSERT(luminance(fb->color[row + S / 2 + 1]) < 0.95f);   /* far side of the step */
    WPE_TEST_ASSERT_EQ_UINT(0xFFFFFFFFu, fb->color[row + S / 2 - 6]); /* near side */
    WPE_TEST_ASSERT_EQ_UINT(0xFFFFFFFFu, fb->color[row + S - 4]);     /* flat far area */
    wpe_postfx_destroy(chain);
    wpe_renderer_destroy(r);
}

/* The window depth of a view distance (inverse of wpe_postfx_linear_depth). */
static float window_depth(float z) {
    const float n = k_post_camera.near_z, f = k_post_camera.far_z;
    return (f + n - 2.0f * n * f / z) / (f - n) * 0.5f + 0.5f;
}

/* Left half: solid red at `left_z`. Right half: one-pixel black and white columns at `right_z`. */
static WPE_Renderer* two_halves(float left_z, float right_z) {
    WPE_Renderer* r = filled_renderer(0xFF000000u);
    WPE_Framebuffer* fb = wpe_renderer_framebuffer_for_write(r);
    for (uint32_t y = 0; y < S; ++y) {
        for (uint32_t x = 0; x < S; ++x) {
            const bool left = x < S / 2;
            fb->color[y * S + x] = left ? 0xFFFF0000u : ((x & 1) ? 0xFFFFFFFFu : 0xFF000000u);
            fb->depth[y * S + x] = window_depth(left ? left_z : right_z);
        }
    }
    return r;
}

static void test_lighting_postfx_depth_of_field(void) {
    WPE_PostChain* chain = NULL;
    WPE_TEST_ASSERT_OK(wpe_postfx_create(&chain));
    const WPE_DepthOfFieldParams params = {
        .focus_distance = 5.0f, .focus_range = 1.0f, .falloff = 5.0f, .radius_pixels = 3.0f};
    const WPE_PostEffect effect = wpe_postfx_depth_of_field(&params);
    const uint32_t row = (S / 2) * S;

    /* Red in focus in front, stripes far behind: the stripes blur to gray; the red stays sharp, and none of
     * it bleeds onto the blurred background beside it. */
    WPE_Renderer* r = two_halves(5.0f, 50.0f);
    WPE_TEST_ASSERT_OK(wpe_postfx_apply(chain, r, &effect, 1, &k_post_camera));
    const WPE_Framebuffer* fb = wpe_renderer_framebuffer(r);
    for (uint32_t x = 0; x < S / 2; ++x)
        WPE_TEST_ASSERT_EQ_UINT(0xFFFF0000u, fb->color[row + x]);
    for (uint32_t x = S / 2; x < S - 4; ++x) {
        const WPE_Color c = fb->color[row + x];
        const uint32_t red = (c >> 16) & 0xFFu, green = (c >> 8) & 0xFFu;
        WPE_TEST_ASSERT_MSG(red == green, "x %u: 0x%08x has red from the sharp side", x, c);
        WPE_TEST_ASSERT_MSG(green > 0x40u && green < 0xC0u, "x %u: 0x%08x not blurred", x, c);
    }
    wpe_renderer_destroy(r);

    /* A blurred red foreground in front of sharp stripes softens over them. */
    r = two_halves(1.0f, 5.0f);
    WPE_TEST_ASSERT_OK(wpe_postfx_apply(chain, r, &effect, 1, &k_post_camera));
    fb = wpe_renderer_framebuffer(r);
    /* The red is 3 past the sharp range: blurred 0.6 of the way, 1.8 pixels, reaching the first stripe. */
    const WPE_Color edge = fb->color[row + S / 2];
    WPE_TEST_ASSERT_MSG(((edge >> 16) & 0xFFu) > ((edge >> 8) & 0xFFu), "0x%08x has no red", edge);
    WPE_TEST_ASSERT_EQ_UINT(0xFF000000u, fb->color[row + S - 8]); /* Sharp, far from the edge. */
    WPE_TEST_ASSERT_EQ_UINT(0xFFFFFFFFu, fb->color[row + S - 7]);
    wpe_renderer_destroy(r);

    /* No radius, no change. */
    const WPE_DepthOfFieldParams none = {.focus_distance = 5.0f, .radius_pixels = 0.0f};
    const WPE_PostEffect off = wpe_postfx_depth_of_field(&none);
    r = two_halves(5.0f, 50.0f);
    WPE_TEST_ASSERT_OK(wpe_postfx_apply(chain, r, &off, 1, &k_post_camera));
    WPE_TEST_ASSERT_EQ_UINT(0xFF000000u, wpe_renderer_framebuffer(r)->color[row + S - 8]);
    wpe_renderer_destroy(r);
    wpe_postfx_destroy(chain);
}

/* One-pixel black and white columns over the whole frame. */
static WPE_Renderer* striped(void) {
    WPE_Renderer* r = filled_renderer(0xFF000000u);
    WPE_Framebuffer* fb = wpe_renderer_framebuffer_for_write(r);
    for (uint32_t i = 0; i < S * S; ++i)
        fb->color[i] = (i % S) & 1 ? 0xFFFFFFFFu : 0xFF000000u;
    return r;
}

static uint32_t changed_pixels(const WPE_Framebuffer* fb, uint32_t y0, uint32_t y1, uint32_t x0,
                               uint32_t x1) {
    uint32_t changed = 0;
    for (uint32_t y = y0; y < y1; ++y)
        for (uint32_t x = x0; x < x1; ++x)
            changed += fb->color[y * S + x] != ((x & 1) ? 0xFFFFFFFFu : 0xFF000000u);
    return changed;
}

static void test_lighting_postfx_distortions(void) {
    WPE_PostChain* chain = NULL;
    WPE_TEST_ASSERT_OK(wpe_postfx_create(&chain));

    /* A shockwave ring around the center, 0.25 of the height out (16 pixels) and 0.05 (3 pixels) thick:
     * the image moves on the ring and nowhere else. */
    const WPE_ShockwaveParams ring = {
        .center_x = 0.5f, .center_y = 0.5f, .radius = 0.25f, .width = 0.05f, .strength_pixels = 3.0f};
    WPE_PostEffect effect = wpe_postfx_shockwave(&ring);
    WPE_Renderer* r = striped();
    WPE_TEST_ASSERT_OK(wpe_postfx_apply(chain, r, &effect, 1, &k_post_camera));
    const WPE_Framebuffer* fb = wpe_renderer_framebuffer(r);
    WPE_TEST_ASSERT_EQ_UINT(0, changed_pixels(fb, 26, 38, 26, 38));          /* Inside the ring. */
    WPE_TEST_ASSERT_EQ_UINT(0, changed_pixels(fb, 0, 8, 0, S));              /* Outside it. */
    WPE_TEST_ASSERT(changed_pixels(fb, 28, 36, S / 2 + 12, S / 2 + 20) > 0); /* On it, beside the center. */
    wpe_renderer_destroy(r);

    /* A radial blur leaves the center and smears toward it more at the edges. */
    const WPE_RadialBlurParams blur = {.center_x = 0.5f, .center_y = 0.5f, .strength = 0.3f};
    effect = wpe_postfx_radial_blur(&blur);
    r = striped();
    WPE_TEST_ASSERT_OK(wpe_postfx_apply(chain, r, &effect, 1, &k_post_camera));
    fb = wpe_renderer_framebuffer(r);
    WPE_TEST_ASSERT_EQ_UINT(0, changed_pixels(fb, 31, 33, 31, 33));
    const WPE_Color edge = fb->color[(S / 2) * S + 3];
    WPE_TEST_ASSERT_MSG(edge != 0xFFFFFFFFu && edge != 0xFF000000u, "0x%08x is not blurred", edge);
    wpe_renderer_destroy(r);
    wpe_postfx_destroy(chain);
}

/* The red, green and blue of a pixel, 0..255. */
static void channels(WPE_Color c, uint32_t out[3]) {
    out[0] = (c >> 16) & 0xFFu;
    out[1] = (c >> 8) & 0xFFu;
    out[2] = c & 0xFFu;
}

/* Grayscale keeps brightness (luma), mixes in by strength, and tints the gray. */
static void test_lighting_postfx_grayscale(void) {
    WPE_PostChain* chain = NULL;
    WPE_TEST_ASSERT_OK(wpe_postfx_create(&chain));
    uint32_t rgb[3];

    /* Pure red: gray at its luma, 0.299 of full. */
    WPE_GrayscaleParams params = {.strength = 1.0f, .tint_r = 1.0f, .tint_g = 1.0f, .tint_b = 1.0f};
    WPE_PostEffect effect = wpe_postfx_grayscale(&params);
    WPE_Renderer* r = filled_renderer(0xFFFF0000u);
    WPE_TEST_ASSERT_OK(wpe_postfx_apply(chain, r, &effect, 1, &k_post_camera));
    channels(wpe_renderer_framebuffer(r)->color[100], rgb);
    WPE_TEST_ASSERT(rgb[0] == rgb[1] && rgb[1] == rgb[2]);
    WPE_TEST_ASSERT_NEAR(76.0, (double)rgb[0], 1.0);
    wpe_renderer_destroy(r);

    /* Half strength: halfway from red to that gray. */
    params.strength = 0.5f;
    r = filled_renderer(0xFFFF0000u);
    WPE_TEST_ASSERT_OK(wpe_postfx_apply(chain, r, &effect, 1, &k_post_camera));
    channels(wpe_renderer_framebuffer(r)->color[100], rgb);
    WPE_TEST_ASSERT_NEAR(165.0, (double)rgb[0], 1.0);
    WPE_TEST_ASSERT_NEAR(38.0, (double)rgb[1], 1.0);
    wpe_renderer_destroy(r);

    /* A tint colors the gray (sepia-like: no blue). */
    params = (WPE_GrayscaleParams){.strength = 1.0f, .tint_r = 1.0f, .tint_g = 0.5f, .tint_b = 0.0f};
    r = filled_renderer(0xFFFF0000u);
    WPE_TEST_ASSERT_OK(wpe_postfx_apply(chain, r, &effect, 1, &k_post_camera));
    channels(wpe_renderer_framebuffer(r)->color[100], rgb);
    WPE_TEST_ASSERT_NEAR(76.0, (double)rgb[0], 1.0);
    WPE_TEST_ASSERT_NEAR(38.0, (double)rgb[1], 1.0);
    WPE_TEST_ASSERT_EQ_UINT(0, rgb[2]);
    wpe_renderer_destroy(r);
    wpe_postfx_destroy(chain);
}

/* Cracks: the same seed gives the same lines, each crossing near the center; a fractional count grows the
 * newest; lines glow where they are and leave the rest alone; the image slides along them. */
static void test_lighting_postfx_cracks(void) {
    WPE_CracksParams params = wpe_postfx_config_default(WPE_POSTFX_CRACKS).cracks;
    params.count = 5.5f;
    WPE_CrackLine a[WPE_POSTFX_MAX_CRACKS], b[WPE_POSTFX_MAX_CRACKS];
    WPE_TEST_ASSERT_EQ_UINT(6, wpe_postfx_crack_lines(&params, S, S, a));
    WPE_TEST_ASSERT_EQ_UINT(6, wpe_postfx_crack_lines(&params, S, S, b));
    WPE_TEST_ASSERT(memcmp(a, b, sizeof a[0] * 6) == 0);
    const float diagonal = sqrtf(2.0f) * (float)S;
    for (int i = 0; i < 6; ++i) {
        /* Within spread (in heights) of the center, measured across the line. */
        const float across = fabsf((S * 0.5f - a[i].x) * -a[i].dy + (S * 0.5f - a[i].y) * a[i].dx);
        WPE_TEST_ASSERT(across <= params.spread * S + 1e-3f);
        WPE_TEST_ASSERT_NEAR(1.0, sqrt(a[i].dx * a[i].dx + a[i].dy * a[i].dy), 1e-4);
        WPE_TEST_ASSERT_NEAR(i < 5 ? diagonal : diagonal * 0.5f, a[i].reach, 1e-3);
    }
    params.seed = 2;
    wpe_postfx_crack_lines(&params, S, S, b);
    WPE_TEST_ASSERT(memcmp(a, b, sizeof a[0] * 6) != 0);
    params.count = 0.0f;
    WPE_TEST_ASSERT_EQ_UINT(0, wpe_postfx_crack_lines(&params, S, S, b));

    /* One line with a white core and nothing else: it lights the pixels on it; past its reach (a few
     * pixels) nothing changes. */
    WPE_PostChain* chain = NULL;
    WPE_TEST_ASSERT_OK(wpe_postfx_create(&chain));
    params = wpe_postfx_config_default(WPE_POSTFX_CRACKS).cracks;
    params.count = 1.0f;
    params.offset_pixels = 0.0f;
    params.bevel_pixels = 0.0f;
    params.glow_pixels = 0.0f;
    params.width_pixels = 1.0f;
    params.core_r = params.core_g = params.core_b = 1.0f;
    WPE_PostEffect effect = wpe_postfx_cracks(&params);
    WPE_Renderer* r = filled_renderer(0xFF404040u);
    WPE_TEST_ASSERT_OK(wpe_postfx_apply(chain, r, &effect, 1, &k_post_camera));
    const WPE_Framebuffer* fb = wpe_renderer_framebuffer(r);
    WPE_CrackLine line;
    wpe_postfx_crack_lines(&params, S, S, &line);
    uint32_t lit = 0, kept = 0, changed = 0;
    for (uint32_t y = 0; y < S; ++y) {
        for (uint32_t x = 0; x < S; ++x) {
            const float d =
                fabsf(((float)x + 0.5f - line.x) * -line.dy + ((float)y + 0.5f - line.y) * line.dx);
            const WPE_Color c = fb->color[y * S + x];
            if (d < 0.3f) lit += ((c >> 8) & 0xFFu) > 0x80u;
            if (d > 3.0f) {
                ++kept;
                changed += c != 0xFF404040u;
            }
        }
    }
    WPE_TEST_ASSERT(lit > S / 2);
    WPE_TEST_ASSERT(kept > S * S / 2);
    WPE_TEST_ASSERT_EQ_UINT(0, changed);
    wpe_renderer_destroy(r);

    /* The rim is on one side only: over a dark gray, pixels just beside the cut take the tint (bluer) on
     * that side and keep their color on the other. */
    params = wpe_postfx_config_default(WPE_POSTFX_CRACKS).cracks;
    params.count = 1.0f;
    r = filled_renderer(0xFF404040u);
    WPE_TEST_ASSERT_OK(wpe_postfx_apply(chain, r, &effect, 1, &k_post_camera));
    fb = wpe_renderer_framebuffer(r);
    uint32_t tinted[2] = {0, 0}, beside[2] = {0, 0};
    for (uint32_t y = 0; y < S; ++y) {
        for (uint32_t x = 0; x < S; ++x) {
            const float side = ((float)x + 0.5f - line.x) * -line.dy + ((float)y + 0.5f - line.y) * line.dx;
            if (fabsf(side) < 1.0f || fabsf(side) > 2.5f) continue;
            const int k = side > 0.0f;
            ++beside[k];
            tinted[k] += (fb->color[y * S + x] & 0xFFu) > 0x40u;
        }
    }
    WPE_TEST_ASSERT(beside[0] > 20 && beside[1] > 20);
    WPE_TEST_ASSERT_MSG((tinted[0] == 0) != (tinted[1] == 0), "tinted beside the cut: %u and %u", tinted[0],
                        tinted[1]);
    wpe_renderer_destroy(r);

    /* The seam and the streaks move the image near the cut, and only there. */
    params.glow_pixels = 0.0f;
    r = striped();
    WPE_TEST_ASSERT_OK(wpe_postfx_apply(chain, r, &effect, 1, &k_post_camera));
    fb = wpe_renderer_framebuffer(r);
    uint32_t near_moved = 0, far_moved = 0;
    for (uint32_t y = 0; y < S; ++y) {
        for (uint32_t x = 0; x < S; ++x) {
            const float d =
                fabsf(((float)x + 0.5f - line.x) * -line.dy + ((float)y + 0.5f - line.y) * line.dx);
            const bool moved = fb->color[y * S + x] != ((x & 1) ? 0xFFFFFFFFu : 0xFF000000u);
            if (d < 2.0f) near_moved += moved;
            if (d > params.bevel_pixels + 3.0f) far_moved += moved;
        }
    }
    WPE_TEST_ASSERT(near_moved > S / 4);
    WPE_TEST_ASSERT_EQ_UINT(0, far_moved);
    wpe_renderer_destroy(r);
    /* Strength 0: untouched. */
    params.strength = 0.0f;
    r = striped();
    WPE_TEST_ASSERT_OK(wpe_postfx_apply(chain, r, &effect, 1, &k_post_camera));
    WPE_TEST_ASSERT_EQ_UINT(0, changed_pixels(wpe_renderer_framebuffer(r), 0, S, 0, S));
    wpe_renderer_destroy(r);
    wpe_postfx_destroy(chain);
}

/* Shatter: at time 0 every piece is still in place (the image is unchanged); later they have moved, and
 * where none is left the background (black at 1) shows. The same seed gives the same pieces. */
static void test_lighting_postfx_shatter(void) {
    WPE_PostChain* chain = NULL;
    WPE_TEST_ASSERT_OK(wpe_postfx_create(&chain));
    WPE_ShatterParams params = wpe_postfx_config_default(WPE_POSTFX_SHATTER).shatter;
    params.background = 1.0f;
    const WPE_PostEffect effect = wpe_postfx_shatter(&params);
    WPE_TEST_ASSERT(effect.prepare != NULL && effect.scratch_size > 0);

    WPE_Renderer* r = striped();
    WPE_TEST_ASSERT_OK(wpe_postfx_apply(chain, r, &effect, 1, &k_post_camera));
    /* Pieces too thin to be sampled may show the background: allow a few pixels. */
    WPE_TEST_ASSERT(changed_pixels(wpe_renderer_framebuffer(r), 0, S, 0, S) < S * S / 100);
    wpe_renderer_destroy(r);

    params.time = 0.8f;
    r = filled_renderer(0xFFFFFFFFu);
    WPE_TEST_ASSERT_OK(wpe_postfx_apply(chain, r, &effect, 1, &k_post_camera));
    const WPE_Framebuffer* fb = wpe_renderer_framebuffer(r);
    uint32_t black = 0;
    for (uint32_t i = 0; i < S * S; ++i)
        black += (fb->color[i] & 0x00FFFFFFu) == 0;
    WPE_TEST_ASSERT_MSG(black > S * S / 10, "only %u pixels uncovered", black);
    /* Again: the same picture. */
    WPE_Color* first = wpe_alloc((size_t)S * S * sizeof *first);
    WPE_TEST_ASSERT(first != NULL);
    memcpy(first, fb->color, (size_t)S * S * sizeof *first);
    wpe_renderer_destroy(r);
    r = filled_renderer(0xFFFFFFFFu);
    WPE_TEST_ASSERT_OK(wpe_postfx_apply(chain, r, &effect, 1, &k_post_camera));
    WPE_TEST_ASSERT(memcmp(first, wpe_renderer_framebuffer(r)->color, (size_t)S * S * sizeof *first) == 0);
    wpe_free(first);
    wpe_renderer_destroy(r);
    wpe_postfx_destroy(chain);
}

/* Effect configs as data: names, parameters, clamping, and the effect they make. */
static void test_lighting_postfx_configs(void) {
    WPE_PostFxKind kind;
    WPE_TEST_ASSERT(wpe_postfx_kind_from_name("shockwave", &kind) && kind == WPE_POSTFX_SHOCKWAVE);
    WPE_TEST_ASSERT(!wpe_postfx_kind_from_name("bloom", &kind) && !wpe_postfx_kind_from_name(NULL, &kind));
    for (uint32_t k = 0; k < WPE_POSTFX_KIND_COUNT; ++k) {
        WPE_TEST_ASSERT(wpe_postfx_kind_name((WPE_PostFxKind)k) != NULL);
        uint32_t count = 0;
        WPE_TEST_ASSERT(wpe_postfx_params((WPE_PostFxKind)k, &count) != NULL && count > 0);
        const WPE_PostFxConfig config = wpe_postfx_config_default((WPE_PostFxKind)k);
        const WPE_PostEffect effect = wpe_postfx_from_config(&config);
        WPE_TEST_ASSERT_EQ_STR(wpe_postfx_kind_name((WPE_PostFxKind)k), effect.name);
        WPE_TEST_ASSERT(effect.enabled && effect.pass && effect.pass_count > 0);
    }
    WPE_TEST_ASSERT(wpe_postfx_kind_name((WPE_PostFxKind)WPE_POSTFX_KIND_COUNT) == NULL);

    WPE_PostFxConfig ps1 = wpe_postfx_config_default(WPE_POSTFX_PS1);
    uint32_t count = 0;
    const WPE_PostFxParam* params = wpe_postfx_params(WPE_POSTFX_PS1, &count);
    WPE_TEST_ASSERT(count == 2 && strcmp(params[0].name, "levels") == 0 && params[0].integer);
    wpe_postfx_param_set(&ps1, &params[0], 9.6);
    WPE_TEST_ASSERT_EQ_UINT(10, ps1.ps1.levels);
    wpe_postfx_param_set(&ps1, &params[0], 1e9); /* Clamped. */
    WPE_TEST_ASSERT_EQ_UINT(256, ps1.ps1.levels);
    wpe_postfx_param_set(&ps1, &params[1], -5.0);
    WPE_TEST_ASSERT_NEAR(0.0, wpe_postfx_param_get(&ps1, &params[1]), 0);
    ps1.enabled = false;
    WPE_TEST_ASSERT(!wpe_postfx_from_config(&ps1).enabled);
}

WPE_TEST_MAIN(
    WPE_TEST_CASE(test_lighting_attenuation_shape), WPE_TEST_CASE(test_lighting_shadow_visibility_pcf),
    WPE_TEST_CASE(test_lighting_point_light_pool), WPE_TEST_CASE(test_lighting_spot_cone),
    WPE_TEST_CASE(test_lighting_more_lights_than_object_limit),
    WPE_TEST_CASE(test_lighting_directional_shadow), WPE_TEST_CASE(test_lighting_fog_and_default_sun),
    WPE_TEST_CASE(test_lighting_subdivision_near_camera), WPE_TEST_CASE(test_lighting_procedural_models),
    WPE_TEST_CASE(test_lighting_generated_tangents_are_orthonormal),
    WPE_TEST_CASE(test_lighting_postfx_color_effects),
    WPE_TEST_CASE(test_lighting_postfx_ssao_darkens_creases_only),
    WPE_TEST_CASE(test_lighting_postfx_depth_of_field), WPE_TEST_CASE(test_lighting_postfx_distortions),
    WPE_TEST_CASE(test_lighting_postfx_grayscale), WPE_TEST_CASE(test_lighting_postfx_cracks),
    WPE_TEST_CASE(test_lighting_postfx_shatter), WPE_TEST_CASE(test_lighting_postfx_configs))
