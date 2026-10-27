#include "wpe_test.h"

#include "renderer/renderer.h"
#include "renderer/texture.h"

static WPE_Texture* make_texture(uint32_t w, uint32_t h, const uint32_t* texels, bool mips, bool srgb,
                                 uint32_t max_dim) {
    WPE_Texture* texture = NULL;
    WPE_TEST_ASSERT_OK(wpe_texture_create(&(WPE_TextureDesc){.width = w,
                                                             .height = h,
                                                             .texels = texels,
                                                             .generate_mips = mips,
                                                             .srgb = srgb,
                                                             .max_dimension = max_dim},
                                          &texture));
    return texture;
}

static const WPE_Sampler k_nearest = {.mag_filter = WPE_FILTER_NEAREST, .min_filter = WPE_FILTER_NEAREST};

static void test_texture_mip_chain_sizes(void) {
    static uint32_t texels[37 * 10];
    for (size_t i = 0; i < 37 * 10; ++i)
        texels[i] = 0xFF808080u;
    WPE_Texture* t = make_texture(37, 10, texels, true, false, 0);
    /* 37x10 -> 18x5 -> 9x2 -> 4x1 -> 2x1 -> 1x1 */
    const uint32_t expected[][2] = {{37, 10}, {18, 5}, {9, 2}, {4, 1}, {2, 1}, {1, 1}};
    WPE_TEST_ASSERT_EQ_UINT(6, t->level_count);
    for (uint32_t i = 0; i < 6; ++i) {
        WPE_TEST_ASSERT_EQ_UINT(expected[i][0], t->levels[i].width);
        WPE_TEST_ASSERT_EQ_UINT(expected[i][1], t->levels[i].height);
        WPE_TEST_ASSERT_EQ_UINT(0xFF808080u, t->levels[i].texels[0]); /* Uniform input stays uniform. */
    }
    wpe_texture_destroy(t);
}

static void test_texture_srgb_mips_average_in_linear_light(void) {
    const uint32_t checker[4] = {0xFFFFFFFFu, 0xFF000000u, 0xFF000000u, 0xFFFFFFFFu};
    WPE_Texture* srgb = make_texture(2, 2, checker, true, true, 0);
    WPE_Texture* linear = make_texture(2, 2, checker, true, false, 0);
    /* 50% linear light is sRGB 188; a naive average gives 128. Alpha is averaged linearly. */
    WPE_TEST_ASSERT_EQ_UINT(0xFFBCBCBCu, srgb->levels[1].texels[0]);
    WPE_TEST_ASSERT_EQ_UINT(0xFF808080u, linear->levels[1].texels[0]);
    wpe_texture_destroy(srgb);
    wpe_texture_destroy(linear);
}

static void test_texture_max_dimension_downscales(void) {
    static uint32_t texels[64 * 16];
    for (size_t i = 0; i < 64 * 16; ++i)
        texels[i] = 0xFF204060u;
    WPE_Texture* t = make_texture(64, 16, texels, true, true, 16);
    WPE_TEST_ASSERT_EQ_UINT(16, t->levels[0].width);
    WPE_TEST_ASSERT_EQ_UINT(4, t->levels[0].height);
    WPE_TEST_ASSERT_EQ_UINT(0xFF204060u, t->levels[0].texels[5]);
    wpe_texture_destroy(t);
}

static void test_texture_create_validation(void) {
    WPE_Texture* t = NULL;
    const uint32_t texel = 0;
    wpe_log_set_stderr_enabled(false);
    WPE_TEST_ASSERT_EQ_INT(
        WPE_RESULT_ERROR_INVALID_ARG,
        wpe_texture_create(&(WPE_TextureDesc){.width = 0, .height = 1, .texels = &texel}, &t));
    wpe_log_set_stderr_enabled(true);
    WPE_TEST_ASSERT_EQ_INT(WPE_RESULT_ERROR_NULL_ARG,
                           wpe_texture_create(&(WPE_TextureDesc){.width = 1, .height = 1}, &t));
    WPE_TEST_ASSERT(t == NULL);
    wpe_texture_destroy(NULL);
}

static void test_texture_nearest_and_wrap_modes(void) {
    /* 4x1 gradient: texel i has red = i * 64. */
    const uint32_t texels[4] = {0xFF000000u, 0xFF400000u, 0xFF800000u, 0xFFC00000u};
    WPE_Texture* t = make_texture(4, 1, texels, false, false, 0);
    WPE_Sampler s = k_nearest;
    const float r = 1.0f / 255.0f;

    WPE_TEST_ASSERT_NEAR(64 * r, wpe_texture_sample_lod(t, &s, (WPE_Vec2){{0.30f, 0.5f}}, 0).x, 1e-6);
    /* Repeat: 1.30 wraps to 0.30. */
    WPE_TEST_ASSERT_NEAR(64 * r, wpe_texture_sample_lod(t, &s, (WPE_Vec2){{1.30f, 0.5f}}, 0).x, 1e-6);
    WPE_TEST_ASSERT_NEAR(192 * r, wpe_texture_sample_lod(t, &s, (WPE_Vec2){{-0.10f, 0.5f}}, 0).x, 1e-6);
    /* Huge coordinates stay well-defined. */
    WPE_TEST_ASSERT_NEAR(0.0, wpe_texture_sample_lod(t, &s, (WPE_Vec2){{4096.05f, 0.5f}}, 0).x, 1e-6);

    s.wrap_u = WPE_WRAP_CLAMP;
    WPE_TEST_ASSERT_NEAR(192 * r, wpe_texture_sample_lod(t, &s, (WPE_Vec2){{1.30f, 0.5f}}, 0).x, 1e-6);
    WPE_TEST_ASSERT_NEAR(0.0, wpe_texture_sample_lod(t, &s, (WPE_Vec2){{-5.0f, 0.5f}}, 0).x, 1e-6);
    WPE_TEST_ASSERT_NEAR(192 * r, wpe_texture_sample_lod(t, &s, (WPE_Vec2){{1e30f, 0.5f}}, 0).x, 1e-6);

    s.wrap_u = WPE_WRAP_MIRROR;
    /* 1.10 mirrors to 0.90 -> texel 3; 1.80 mirrors to 0.20 -> texel 0. */
    WPE_TEST_ASSERT_NEAR(192 * r, wpe_texture_sample_lod(t, &s, (WPE_Vec2){{1.10f, 0.5f}}, 0).x, 1e-6);
    WPE_TEST_ASSERT_NEAR(0.0, wpe_texture_sample_lod(t, &s, (WPE_Vec2){{1.80f, 0.5f}}, 0).x, 1e-6);

    /* Non-finite coordinates sample texel (0,0) rather than invoking undefined conversions. */
    WPE_TEST_ASSERT_NEAR(0.0, wpe_texture_sample_lod(t, &s, (WPE_Vec2){{NAN, INFINITY}}, 0).x, 1e-6);
    wpe_texture_destroy(t);
}

static void test_texture_bilinear_interpolates_between_texel_centers(void) {
    const uint32_t texels[2] = {0xFF000000u, 0xFFFF0000u};
    WPE_Texture* t = make_texture(2, 1, texels, false, false, 0);
    const WPE_Sampler s = {.mag_filter = WPE_FILTER_BILINEAR,
                           .min_filter = WPE_FILTER_BILINEAR,
                           .wrap_u = WPE_WRAP_CLAMP,
                           .wrap_v = WPE_WRAP_CLAMP};
    WPE_TEST_ASSERT_NEAR(0.0, wpe_texture_sample_lod(t, &s, (WPE_Vec2){{0.25f, 0.5f}}, 0).x,
                         1e-6); /* texel 0 center */
    WPE_TEST_ASSERT_NEAR(1.0, wpe_texture_sample_lod(t, &s, (WPE_Vec2){{0.75f, 0.5f}}, 0).x,
                         1e-6); /* texel 1 center */
    WPE_TEST_ASSERT_NEAR(0.5, wpe_texture_sample_lod(t, &s, (WPE_Vec2){{0.50f, 0.5f}}, 0).x, 1e-6);
    wpe_texture_destroy(t);
}

static void test_texture_lod_selection_from_derivatives(void) {
    /* 8x8 texture whose levels have distinct colors: 8x8 red, 4x4 green, 2x2 blue, 1x1 white. */
    static uint32_t texels[64];
    for (int i = 0; i < 64; ++i)
        texels[i] = 0xFFFF0000u;
    WPE_Texture* t = make_texture(8, 8, texels, true, false, 0);
    uint32_t* level_colors[3] = {(uint32_t*)t->levels[1].texels, (uint32_t*)t->levels[2].texels,
                                 (uint32_t*)t->levels[3].texels};
    const uint32_t colors[3] = {0xFF00FF00u, 0xFF0000FFu, 0xFFFFFFFFu};
    for (int l = 0; l < 3; ++l) {
        for (uint32_t i = 0; i < t->levels[l + 1].width * t->levels[l + 1].height; ++i)
            level_colors[l][i] = colors[l];
    }
    WPE_Sampler s = {
        .min_filter = WPE_FILTER_NEAREST, .mag_filter = WPE_FILTER_NEAREST, .mip_filter = WPE_MIP_NEAREST};
    const WPE_Vec2 uv = {{0.5f, 0.5f}}, zero = {{0, 0}};

    /* One texel per pixel -> level 0; four texels per pixel -> level 2; tiny footprint -> level 0. */
    WPE_TEST_ASSERT_NEAR(1.0, wpe_texture_sample(t, &s, uv, (WPE_Vec2){{1.0f / 8, 0}}, zero).x, 1e-6);
    const WPE_Vec4 lod2 = wpe_texture_sample(t, &s, uv, (WPE_Vec2){{4.0f / 8, 0}}, (WPE_Vec2){{0, 4.0f / 8}});
    WPE_TEST_ASSERT(lod2.z > 0.99f && lod2.x < 0.01f);
    WPE_TEST_ASSERT_NEAR(1.0, wpe_texture_sample(t, &s, uv, (WPE_Vec2){{0.001f, 0}}, zero).x, 1e-6);
    /* Huge footprint clamps to the last level; NaN derivatives fall back to level 0. */
    const WPE_Vec4 last = wpe_texture_sample(t, &s, uv, (WPE_Vec2){{1000, 0}}, zero);
    WPE_TEST_ASSERT(last.x > 0.99f && last.y > 0.99f && last.z > 0.99f);
    WPE_TEST_ASSERT_NEAR(1.0, wpe_texture_sample(t, &s, uv, (WPE_Vec2){{NAN, 0}}, zero).x, 1e-6);

    /* Trilinear halfway between level 1 (green) and level 2 (blue). */
    s.mip_filter = WPE_MIP_LINEAR;
    const WPE_Vec4 mixed = wpe_texture_sample_lod(t, &s, uv, 1.5f);
    WPE_TEST_ASSERT_NEAR(0.5, mixed.y, 1e-3);
    WPE_TEST_ASSERT_NEAR(0.5, mixed.z, 1e-3);
    /* lod_bias shifts the selection. */
    s.mip_filter = WPE_MIP_NEAREST;
    s.lod_bias = 1.0f;
    WPE_TEST_ASSERT_NEAR(1.0, wpe_texture_sample_lod(t, &s, uv, 0.2f).y, 1e-6);
    wpe_texture_destroy(t);
}

/* ---- Rasterizer-provided UV derivatives ------------------------------------------------------------ */

typedef struct DerivVertex {
    float position[4];
    float uv[2];
} DerivVertex;

typedef struct DerivUniforms {
    WPE_Vec2* captured_dx;
    WPE_Vec2* captured_dy;
    uint32_t width;
} DerivUniforms;

static void deriv_vs(const void* uniforms, const void* vertex, WPE_Vec4* out_position, float* out_varyings) {
    (void)uniforms;
    const DerivVertex* v = vertex;
    *out_position = wpe_vec4(v->position[0], v->position[1], v->position[2], v->position[3]);
    out_varyings[0] = v->uv[0];
    out_varyings[1] = v->uv[1];
}

static bool deriv_fs(const void* uniforms, const WPE_FragmentInput* in, WPE_Vec4* out_color) {
    const DerivUniforms* u = uniforms;
    u->captured_dx[(size_t)in->y * u->width + (size_t)in->x] = in->uv_dx;
    u->captured_dy[(size_t)in->y * u->width + (size_t)in->x] = in->uv_dy;
    *out_color = wpe_vec4(1, 1, 1, 1);
    return true;
}

static void test_texture_rasterizer_provides_exact_uv_derivatives(void) {
    enum { S = 32 };
    const WPE_ShaderProgram program = {.vertex = deriv_vs,
                                       .fragment = deriv_fs,
                                       .varying_count = 2,
                                       .uv_derivatives = true,
                                       .uv_varying = 0};
    /* Screen-aligned quad, uv (0,0) top-left .. (2,1) bottom-right: du/dx = 2/S, dv/dy = 1/S. */
    const DerivVertex quad[6] = {
        {{-1, 1, 0, 1}, {0, 0}}, {{-1, -1, 0, 1}, {0, 1}}, {{1, -1, 0, 1}, {2, 1}},
        {{-1, 1, 0, 1}, {0, 0}}, {{1, -1, 0, 1}, {2, 1}},  {{1, 1, 0, 1}, {2, 0}},
    };
    static WPE_Vec2 dx[S * S], dy[S * S];
    const DerivUniforms u = {.captured_dx = dx, .captured_dy = dy, .width = S};
    WPE_RenderState state = wpe_render_state_default();
    state.cull = WPE_CULL_NONE;

    WPE_Renderer* renderer = NULL;
    WPE_TEST_ASSERT_OK(wpe_renderer_create(&(WPE_RendererDesc){.width = S, .height = S}, &renderer));
    WPE_TEST_ASSERT_OK(wpe_renderer_begin_frame(renderer, NULL));
    WPE_TEST_ASSERT_OK(wpe_renderer_submit(renderer, &(WPE_DrawDesc){.program = &program,
                                                                     .uniforms = &u,
                                                                     .uniforms_size = sizeof u,
                                                                     .vertices = quad,
                                                                     .vertex_stride = sizeof quad[0],
                                                                     .vertex_count = 6,
                                                                     .state = state}));
    WPE_TEST_ASSERT_OK(wpe_renderer_end_frame(renderer));
    for (int i = 0; i < S * S; ++i) {
        WPE_TEST_ASSERT_NEAR(2.0 / S, dx[i].x, 1e-5);
        WPE_TEST_ASSERT_NEAR(0.0, dx[i].y, 1e-5);
        WPE_TEST_ASSERT_NEAR(0.0, dy[i].x, 1e-5);
        WPE_TEST_ASSERT_NEAR(1.0 / S, dy[i].y, 1e-5);
    }

    /* A program asking for derivatives of non-existent varyings is rejected. */
    const WPE_ShaderProgram bad = {.vertex = deriv_vs,
                                   .fragment = deriv_fs,
                                   .varying_count = 2,
                                   .uv_derivatives = true,
                                   .uv_varying = 1};
    WPE_TEST_ASSERT_OK(wpe_renderer_begin_frame(renderer, NULL));
    wpe_log_set_stderr_enabled(false);
    WPE_TEST_ASSERT_EQ_INT(WPE_RESULT_ERROR_INVALID_ARG,
                           wpe_renderer_submit(renderer, &(WPE_DrawDesc){.program = &bad,
                                                                         .vertices = quad,
                                                                         .vertex_stride = sizeof quad[0],
                                                                         .vertex_count = 6}));
    wpe_log_set_stderr_enabled(true);
    WPE_TEST_ASSERT_OK(wpe_renderer_end_frame(renderer));
    wpe_renderer_destroy(renderer);
}

static void test_texture_perspective_derivatives_grow_with_distance(void) {
    /* On a receding floor, texture footprint per pixel must grow toward the horizon. */
    enum { S = 64 };
    const WPE_ShaderProgram program = {.vertex = deriv_vs,
                                       .fragment = deriv_fs,
                                       .varying_count = 2,
                                       .uv_derivatives = true,
                                       .uv_varying = 0};
    const WPE_Mat4 proj = wpe_mat4_perspective(WPE_PI / 2, 1.0f, 0.1f, 100.0f);
    const float corners[4][5] = {
        {-20, -1, -1, 0, 0}, {20, -1, -1, 40, 0}, {20, -1, -60, 40, 59}, {-20, -1, -60, 0, 59}};
    DerivVertex v[4];
    for (int i = 0; i < 4; ++i) {
        const WPE_Vec4 c = wpe_mat4_mul_vec4(&proj, wpe_vec4(corners[i][0], corners[i][1], corners[i][2], 1));
        v[i] = (DerivVertex){{c.x, c.y, c.z, c.w}, {corners[i][3], corners[i][4]}};
    }
    const DerivVertex quad[6] = {v[0], v[1], v[2], v[0], v[2], v[3]};
    static WPE_Vec2 dx[S * S], dy[S * S];
    memset(dy, 0, sizeof dy);
    const DerivUniforms u = {.captured_dx = dx, .captured_dy = dy, .width = S};
    WPE_RenderState state = wpe_render_state_default();
    state.cull = WPE_CULL_NONE;

    WPE_Renderer* renderer = NULL;
    WPE_TEST_ASSERT_OK(wpe_renderer_create(&(WPE_RendererDesc){.width = S, .height = S}, &renderer));
    WPE_TEST_ASSERT_OK(wpe_renderer_begin_frame(renderer, NULL));
    WPE_TEST_ASSERT_OK(wpe_renderer_submit(renderer, &(WPE_DrawDesc){.program = &program,
                                                                     .uniforms = &u,
                                                                     .uniforms_size = sizeof u,
                                                                     .vertices = quad,
                                                                     .vertex_stride = sizeof quad[0],
                                                                     .vertex_count = 6,
                                                                     .state = state}));
    WPE_TEST_ASSERT_OK(wpe_renderer_end_frame(renderer));

    /* |dv/dy| at a row near the horizon is much larger than near the bottom of the screen. */
    const float near_row = fabsf(dy[(S - 2) * S + S / 2].y);
    const float far_row = fabsf(dy[(S / 2 + 3) * S + S / 2].y);
    WPE_TEST_ASSERT_MSG(near_row > 0.0f && far_row > near_row * 10.0f, "near %.4f far %.4f", (double)near_row,
                        (double)far_row);
    wpe_renderer_destroy(renderer);
}

static void test_texture_update_replaces_texels(void) {
    const uint32_t a[4] = {0xFF000001u, 0xFF000002u, 0xFF000003u, 0xFF000004u};
    const uint32_t b[4] = {0xFF0000AAu, 0xFF0000BBu, 0xFF0000CCu, 0xFF0000DDu};
    WPE_Texture* texture = NULL;
    WPE_TEST_ASSERT_OK(
        wpe_texture_create(&(WPE_TextureDesc){.width = 2, .height = 2, .texels = a}, &texture));
    WPE_TEST_ASSERT_OK(wpe_texture_update(texture, b));
    WPE_TEST_ASSERT(memcmp(texture->levels[0].texels, b, sizeof b) == 0);
    WPE_Texture* mipped = NULL;
    WPE_TEST_ASSERT_OK(wpe_texture_create(
        &(WPE_TextureDesc){.width = 2, .height = 2, .texels = a, .generate_mips = true}, &mipped));
    WPE_TEST_ASSERT_EQ_INT(WPE_RESULT_ERROR_INVALID_STATE, wpe_texture_update(mipped, b));
    WPE_TEST_ASSERT_EQ_INT(WPE_RESULT_ERROR_NULL_ARG, wpe_texture_update(NULL, b));
    wpe_texture_destroy(mipped);
    wpe_texture_destroy(texture);
}

WPE_TEST_MAIN(WPE_TEST_CASE(test_texture_update_replaces_texels), WPE_TEST_CASE(test_texture_mip_chain_sizes),
              WPE_TEST_CASE(test_texture_srgb_mips_average_in_linear_light),
              WPE_TEST_CASE(test_texture_max_dimension_downscales),
              WPE_TEST_CASE(test_texture_create_validation),
              WPE_TEST_CASE(test_texture_nearest_and_wrap_modes),
              WPE_TEST_CASE(test_texture_bilinear_interpolates_between_texel_centers),
              WPE_TEST_CASE(test_texture_lod_selection_from_derivatives),
              WPE_TEST_CASE(test_texture_rasterizer_provides_exact_uv_derivatives),
              WPE_TEST_CASE(test_texture_perspective_derivatives_grow_with_distance))
