#include "wpe_test.h"

#include "jobs/jobs.h"
#include "renderer/renderer.h"
#include "renderer/shaders.h"

/* ---- Test shaders ----------------------------------------------------------------------------------
 * Vertices carry a clip-space position and four attributes that become varyings unchanged.
 * Fragment shaders either output the varyings, a uniform color, or record per-pixel data into
 * test-owned buffers (each pixel is touched by exactly one tile thread, so this is race-free).
 */
typedef struct ClipVertex {
    float position[4];
    float attr[4];
} ClipVertex;

typedef struct TestUniforms {
    WPE_Vec4 color;
    uint32_t* counts; /* Optional: per-pixel invocation counter. */
    float* captured;  /* Optional: per-pixel copy of varying 0. */
    uint32_t width;
    int32_t discard_left_of; /* Discard fragments with x < this value. */
} TestUniforms;

static void clip_vs(const void* uniforms, const void* vertex, WPE_Vec4* out_position, float* out_varyings) {
    (void)uniforms;
    const ClipVertex* v = vertex;
    *out_position = wpe_vec4(v->position[0], v->position[1], v->position[2], v->position[3]);
    for (int i = 0; i < 4; ++i)
        out_varyings[i] = v->attr[i];
}

static bool uniform_color_fs(const void* uniforms, const WPE_FragmentInput* in, WPE_Vec4* out_color) {
    const TestUniforms* u = uniforms;
    if (in->x < u->discard_left_of) return false;
    if (u->counts) ++u->counts[(size_t)in->y * u->width + (size_t)in->x];
    if (u->captured) u->captured[(size_t)in->y * u->width + (size_t)in->x] = in->varyings[0];
    *out_color = u->color;
    return true;
}

static bool varying_color_fs(const void* uniforms, const WPE_FragmentInput* in, WPE_Vec4* out_color) {
    (void)uniforms;
    *out_color = wpe_vec4(in->varyings[0], in->varyings[1], in->varyings[2], in->varyings[3]);
    return true;
}

static const WPE_ShaderProgram g_uniform_program = {
    .vertex = clip_vs, .fragment = uniform_color_fs, .varying_count = 4};
static const WPE_ShaderProgram g_varying_program = {
    .vertex = clip_vs, .fragment = varying_color_fs, .varying_count = 4};

#define W 96
#define H 64

static WPE_Renderer* make_renderer(uint32_t width, uint32_t height, WPE_JobSystem* jobs) {
    WPE_Renderer* renderer = NULL;
    WPE_TEST_ASSERT_OK(
        wpe_renderer_create(&(WPE_RendererDesc){.width = width, .height = height, .jobs = jobs}, &renderer));
    return renderer;
}

static const WPE_ClearDesc k_clear_black = {
    .clear_color = true, .color = 0xFF000000u, .clear_depth = true, .depth = 1.0f};

static ClipVertex cv(float x, float y, float z, float w) {
    return (ClipVertex){.position = {x, y, z, w}, .attr = {0, 0, 0, 1}};
}

static WPE_DrawDesc draw_of(const ClipVertex* vertices, uint32_t count, const TestUniforms* uniforms,
                            WPE_RenderState state) {
    return (WPE_DrawDesc){
        .program = &g_uniform_program,
        .uniforms = uniforms,
        .uniforms_size = sizeof *uniforms,
        .vertices = vertices,
        .vertex_stride = sizeof(ClipVertex),
        .vertex_count = count,
        .state = state,
    };
}

static WPE_RenderState state_no_cull(void) {
    WPE_RenderState s = wpe_render_state_default();
    s.cull = WPE_CULL_NONE;
    return s;
}

/* Renders the given draws into a fresh frame and returns the renderer's framebuffer. */
static const WPE_Framebuffer* render(WPE_Renderer* renderer, const WPE_DrawDesc* draws, uint32_t count) {
    WPE_TEST_ASSERT_OK(wpe_renderer_begin_frame(renderer, &k_clear_black));
    for (uint32_t i = 0; i < count; ++i)
        WPE_TEST_ASSERT_OK(wpe_renderer_submit(renderer, &draws[i]));
    WPE_TEST_ASSERT_OK(wpe_renderer_end_frame(renderer));
    return wpe_renderer_framebuffer(renderer);
}

/* ---- Color ----------------------------------------------------------------------------------------- */

static void test_renderer_color_conversion(void) {
    WPE_TEST_ASSERT_EQ_UINT(0xFFFF8000u, wpe_color_from_vec4(wpe_vec4(1, 0.5f, 0, 1)));
    WPE_TEST_ASSERT_EQ_UINT(0x00000000u, wpe_color_from_vec4(wpe_vec4(-1, NAN, 0, -5)));
    WPE_TEST_ASSERT_EQ_UINT(0xFFFFFFFFu, wpe_color_from_vec4(wpe_vec4(9, 9, 9, 9)));
    WPE_TEST_ASSERT_EQ_UINT(wpe_color_rgba8(10, 20, 30, 40),
                            wpe_color_from_vec4(wpe_color_to_vec4(wpe_color_rgba8(10, 20, 30, 40))));
}

/* ---- Lifecycle and validation ------------------------------------------------------------------------- */

static void test_renderer_create_validates_size(void) {
    WPE_Renderer* renderer = NULL;
    wpe_log_set_stderr_enabled(false);
    WPE_TEST_ASSERT_EQ_INT(WPE_RESULT_ERROR_INVALID_ARG,
                           wpe_renderer_create(&(WPE_RendererDesc){.width = 0, .height = 10}, &renderer));
    WPE_TEST_ASSERT_EQ_INT(
        WPE_RESULT_ERROR_INVALID_ARG,
        wpe_renderer_create(&(WPE_RendererDesc){.width = WPE_RENDERER_MAX_DIMENSION + 1, .height = 10},
                            &renderer));
    wpe_log_set_stderr_enabled(true);
    WPE_TEST_ASSERT_EQ_INT(WPE_RESULT_ERROR_NULL_ARG, wpe_renderer_create(NULL, &renderer));
    WPE_TEST_ASSERT(renderer == NULL);
    wpe_renderer_destroy(NULL);
}

static void test_renderer_frame_state_machine(void) {
    WPE_Renderer* renderer = make_renderer(W, H, NULL);
    wpe_log_set_stderr_enabled(false);
    WPE_TEST_ASSERT_EQ_INT(WPE_RESULT_ERROR_INVALID_STATE, wpe_renderer_end_frame(renderer));
    const TestUniforms u = {0};
    const ClipVertex tri[3] = {cv(-1, -1, 0, 1), cv(1, -1, 0, 1), cv(0, 1, 0, 1)};
    const WPE_DrawDesc d = draw_of(tri, 3, &u, state_no_cull());
    WPE_TEST_ASSERT_EQ_INT(WPE_RESULT_ERROR_INVALID_STATE, wpe_renderer_submit(renderer, &d));

    WPE_TEST_ASSERT_OK(wpe_renderer_begin_frame(renderer, NULL));
    WPE_TEST_ASSERT_EQ_INT(WPE_RESULT_ERROR_INVALID_STATE, wpe_renderer_begin_frame(renderer, NULL));
    WPE_TEST_ASSERT_EQ_INT(WPE_RESULT_ERROR_INVALID_STATE, wpe_renderer_resize(renderer, 10, 10));
    WPE_TEST_ASSERT_OK(wpe_renderer_end_frame(renderer));
    wpe_log_set_stderr_enabled(true);

    WPE_TEST_ASSERT_OK(wpe_renderer_resize(renderer, 33, 17));
    WPE_TEST_ASSERT_EQ_UINT(33, wpe_renderer_framebuffer(renderer)->width);
    WPE_TEST_ASSERT_EQ_UINT(17, wpe_renderer_framebuffer(renderer)->height);
    wpe_renderer_destroy(renderer);
}

static void test_renderer_submit_validates_draws(void) {
    WPE_Renderer* renderer = make_renderer(W, H, NULL);
    const TestUniforms u = {0};
    const ClipVertex tri[3] = {cv(-1, -1, 0, 1), cv(1, -1, 0, 1), cv(0, 1, 0, 1)};
    const uint32_t bad_indices[3] = {0, 1, 3};
    const WPE_ShaderProgram too_many = {
        .vertex = clip_vs, .fragment = uniform_color_fs, .varying_count = WPE_MAX_VARYINGS + 1};

    WPE_TEST_ASSERT_OK(wpe_renderer_begin_frame(renderer, NULL));
    wpe_log_set_stderr_enabled(false);
    WPE_DrawDesc d = draw_of(tri, 3, &u, state_no_cull());
    d.indices = bad_indices;
    d.index_count = 3;
    WPE_TEST_ASSERT_EQ_INT(WPE_RESULT_ERROR_INVALID_ARG, wpe_renderer_submit(renderer, &d));
    d.index_count = 2;
    WPE_TEST_ASSERT_EQ_INT(WPE_RESULT_ERROR_INVALID_ARG, wpe_renderer_submit(renderer, &d));

    d = draw_of(tri, 3, &u, state_no_cull());
    d.program = &too_many;
    WPE_TEST_ASSERT_EQ_INT(WPE_RESULT_ERROR_INVALID_ARG, wpe_renderer_submit(renderer, &d));
    d.program = NULL;
    WPE_TEST_ASSERT_EQ_INT(WPE_RESULT_ERROR_INVALID_ARG, wpe_renderer_submit(renderer, &d));

    d = draw_of(tri, 3, &u, state_no_cull());
    d.vertices = NULL;
    WPE_TEST_ASSERT_EQ_INT(WPE_RESULT_ERROR_INVALID_ARG, wpe_renderer_submit(renderer, &d));
    d = draw_of(tri, 3, &u, state_no_cull());
    d.state.blend = (WPE_BlendMode)99;
    WPE_TEST_ASSERT_EQ_INT(WPE_RESULT_ERROR_INVALID_ARG, wpe_renderer_submit(renderer, &d));
    d = draw_of(tri, 3, &u, state_no_cull());
    d.uniforms = NULL;
    WPE_TEST_ASSERT_EQ_INT(WPE_RESULT_ERROR_INVALID_ARG, wpe_renderer_submit(renderer, &d));
    wpe_log_set_stderr_enabled(true);

    WPE_TEST_ASSERT_EQ_INT(WPE_RESULT_ERROR_NULL_ARG, wpe_renderer_submit(renderer, NULL));
    WPE_TEST_ASSERT_OK(wpe_renderer_end_frame(renderer));
    WPE_TEST_ASSERT_EQ_UINT(0, wpe_renderer_stats(renderer).draws);
    wpe_renderer_destroy(renderer);
}

/* ---- Coverage ---------------------------------------------------------------------------------------- */

static void test_renderer_clear_only_frame(void) {
    WPE_Renderer* renderer = make_renderer(W, H, NULL);
    WPE_TEST_ASSERT_OK(wpe_renderer_begin_frame(
        renderer,
        &(WPE_ClearDesc){.clear_color = true, .color = 0xFF112233u, .clear_depth = true, .depth = 0.25f}));
    WPE_TEST_ASSERT_OK(wpe_renderer_end_frame(renderer));
    const WPE_Framebuffer* fb = wpe_renderer_framebuffer(renderer);
    for (uint32_t i = 0; i < W * H; ++i) {
        WPE_TEST_ASSERT_EQ_UINT(0xFF112233u, fb->color[i]);
        WPE_TEST_ASSERT_NEAR(0.25, fb->depth[i], 0);
    }
    wpe_renderer_destroy(renderer);
}

static void assert_counts_all(const uint32_t* counts, uint32_t width, uint32_t height, uint32_t expected) {
    for (uint32_t y = 0; y < height; ++y) {
        for (uint32_t x = 0; x < width; ++x) {
            WPE_TEST_ASSERT_MSG(counts[y * width + x] == expected,
                                "pixel (%u,%u) covered %u times, expected %u", x, y, counts[y * width + x],
                                expected);
        }
    }
}

static void test_renderer_fullscreen_quad_covers_each_pixel_once(void) {
    WPE_Renderer* renderer = make_renderer(W, H, NULL);
    static uint32_t counts[W * H];
    memset(counts, 0, sizeof counts);
    const TestUniforms u = {.color = wpe_vec4(1, 1, 1, 1), .counts = counts, .width = W};
    const ClipVertex quad[6] = {cv(-1, -1, 0, 1), cv(1, -1, 0, 1), cv(1, 1, 0, 1),
                                cv(-1, -1, 0, 1), cv(1, 1, 0, 1),  cv(-1, 1, 0, 1)};
    const WPE_DrawDesc d = draw_of(quad, 6, &u, wpe_render_state_default());
    render(renderer, &d, 1);
    assert_counts_all(counts, W, H, 1);
    WPE_TEST_ASSERT_EQ_UINT((uint64_t)W * H, wpe_renderer_stats(renderer).fragments_shaded);
    wpe_renderer_destroy(renderer);
}

static void test_renderer_jittered_mesh_is_watertight(void) {
    /* A grid of irregular triangles with shared edges and vertices must cover every pixel exactly once. */
    enum { CELLS_X = 13, CELLS_Y = 9 };
    static ClipVertex grid[(CELLS_X + 1) * (CELLS_Y + 1)];
    static uint32_t indices[CELLS_X * CELLS_Y * 6];
    uint32_t rng = 0xC0FFEEu;
    for (int gy = 0; gy <= CELLS_Y; ++gy) {
        for (int gx = 0; gx <= CELLS_X; ++gx) {
            float x = -1.0f + 2.0f * (float)gx / CELLS_X;
            float y = -1.0f + 2.0f * (float)gy / CELLS_Y;
            if (gx > 0 && gx < CELLS_X && gy > 0 && gy < CELLS_Y) {
                rng = rng * 1664525u + 1013904223u;
                x += ((float)(rng >> 8) / 16777216.0f - 0.5f) * 0.08f;
                rng = rng * 1664525u + 1013904223u;
                y += ((float)(rng >> 8) / 16777216.0f - 0.5f) * 0.12f;
            }
            grid[gy * (CELLS_X + 1) + gx] = cv(x, y, 0, 1);
        }
    }
    uint32_t n = 0;
    for (uint32_t gy = 0; gy < CELLS_Y; ++gy) {
        for (uint32_t gx = 0; gx < CELLS_X; ++gx) {
            const uint32_t a = gy * (CELLS_X + 1) + gx, b = a + 1, c = a + CELLS_X + 1, d = c + 1;
            /* Alternate the diagonal and the winding to exercise every edge orientation. */
            if ((gx + gy) % 2) {
                indices[n++] = a, indices[n++] = b, indices[n++] = d;
                indices[n++] = a, indices[n++] = c, indices[n++] = d; /* clockwise */
            } else {
                indices[n++] = a, indices[n++] = b, indices[n++] = c;
                indices[n++] = b, indices[n++] = d, indices[n++] = c;
            }
        }
    }

    static uint32_t counts[W * H];
    memset(counts, 0, sizeof counts);
    const TestUniforms u = {.color = wpe_vec4(1, 1, 1, 1), .counts = counts, .width = W};
    WPE_DrawDesc d = draw_of(grid, (CELLS_X + 1) * (CELLS_Y + 1), &u, state_no_cull());
    d.indices = indices;
    d.index_count = n;
    WPE_JobSystem* jobs = NULL;
    WPE_TEST_ASSERT_OK(wpe_jobs_create(3, &jobs));
    WPE_Renderer* renderer = make_renderer(W, H, jobs);
    render(renderer, &d, 1);
    assert_counts_all(counts, W, H, 1);
    wpe_renderer_destroy(renderer);
    wpe_jobs_destroy(jobs);
}

static void test_renderer_fill_rule_on_pixel_center_edges(void) {
    /*
     * All vertices lie exactly on pixel centers, so horizontal, vertical and diagonal edges pass
     * through pixel centers and coverage depends entirely on the top-left fill rule: shared edges
     * must be counted once, the grid's outer top/left edges included and bottom/right excluded.
     */
    enum { S = 64, N = 9, FIRST = 4, STEP = 7 };
    static ClipVertex verts[N * N];
    static uint32_t indices[(N - 1) * (N - 1) * 6];
    for (int r = 0; r < N; ++r) {
        for (int c = 0; c < N; ++c) {
            const float sx = (float)(FIRST + c * STEP) + 0.5f;
            const float sy = (float)(FIRST + r * STEP) + 0.5f;
            verts[r * N + c] = cv(sx / S * 2.0f - 1.0f, 1.0f - sy / S * 2.0f, 0, 1);
        }
    }
    uint32_t n = 0;
    for (uint32_t r = 0; r + 1 < N; ++r) {
        for (uint32_t c = 0; c + 1 < N; ++c) {
            const uint32_t a = r * N + c, b = a + 1, d = a + N, e = d + 1;
            if ((r + c) % 2) {
                indices[n++] = a, indices[n++] = b, indices[n++] = e;
                indices[n++] = a, indices[n++] = e, indices[n++] = d;
            } else {
                indices[n++] = a, indices[n++] = d, indices[n++] = b; /* opposite winding */
                indices[n++] = b, indices[n++] = d, indices[n++] = e;
            }
        }
    }

    static uint32_t counts[S * S];
    memset(counts, 0, sizeof counts);
    const TestUniforms u = {.color = wpe_vec4(1, 1, 1, 1), .counts = counts, .width = S};
    WPE_DrawDesc d = draw_of(verts, N * N, &u, state_no_cull());
    d.indices = indices;
    d.index_count = n;
    WPE_Renderer* renderer = make_renderer(S, S, NULL);
    render(renderer, &d, 1);

    const uint32_t last = FIRST + (N - 1) * STEP;
    for (uint32_t y = 0; y < S; ++y) {
        for (uint32_t x = 0; x < S; ++x) {
            const uint32_t expected = (x >= FIRST && x < last && y >= FIRST && y < last) ? 1 : 0;
            WPE_TEST_ASSERT_MSG(counts[y * S + x] == expected, "pixel (%u,%u) covered %u times, expected %u",
                                x, y, counts[y * S + x], expected);
        }
    }
    wpe_renderer_destroy(renderer);
}

static void test_renderer_guard_band_triangle_fills_screen(void) {
    /* One huge triangle far beyond the viewport (and beyond the guard band) still covers each pixel once. */
    WPE_Renderer* renderer = make_renderer(W, H, NULL);
    static uint32_t counts[W * H];
    memset(counts, 0, sizeof counts);
    const TestUniforms u = {.color = wpe_vec4(1, 1, 1, 1), .counts = counts, .width = W};
    const ClipVertex tri[3] = {cv(-1000, -1000, 0, 1), cv(3000, -1000, 0, 1), cv(-1000, 3000, 0, 1)};
    const WPE_DrawDesc d = draw_of(tri, 3, &u, state_no_cull());
    render(renderer, &d, 1);
    assert_counts_all(counts, W, H, 1);
    WPE_TEST_ASSERT_EQ_UINT(1, wpe_renderer_stats(renderer).triangles_clipped);
    wpe_renderer_destroy(renderer);
}

/* ---- Culling --------------------------------------------------------------------------------------- */

static void test_renderer_backface_culling(void) {
    WPE_Renderer* renderer = make_renderer(W, H, NULL);
    const TestUniforms u = {.color = wpe_vec4(1, 0, 0, 1)};
    const ClipVertex ccw[3] = {cv(-0.5f, -0.5f, 0, 1), cv(0.5f, -0.5f, 0, 1), cv(0, 0.5f, 0, 1)};
    const ClipVertex cw[3] = {cv(-0.5f, -0.5f, 0, 1), cv(0, 0.5f, 0, 1), cv(0.5f, -0.5f, 0, 1)};
    const size_t center = (size_t)(H / 2) * W + W / 2;

    WPE_DrawDesc d = draw_of(ccw, 3, &u, wpe_render_state_default());
    WPE_TEST_ASSERT_EQ_UINT(0xFFFF0000u, render(renderer, &d, 1)->color[center]);
    d = draw_of(cw, 3, &u, wpe_render_state_default());
    WPE_TEST_ASSERT_EQ_UINT(0xFF000000u, render(renderer, &d, 1)->color[center]);
    WPE_TEST_ASSERT_EQ_UINT(1, wpe_renderer_stats(renderer).triangles_culled);

    WPE_RenderState front = wpe_render_state_default();
    front.cull = WPE_CULL_FRONT;
    d = draw_of(cw, 3, &u, front);
    WPE_TEST_ASSERT_EQ_UINT(0xFFFF0000u, render(renderer, &d, 1)->color[center]);
    d = draw_of(ccw, 3, &u, front);
    WPE_TEST_ASSERT_EQ_UINT(0xFF000000u, render(renderer, &d, 1)->color[center]);
    wpe_renderer_destroy(renderer);
}

static void test_renderer_degenerate_and_nonfinite_triangles_are_dropped(void) {
    WPE_Renderer* renderer = make_renderer(W, H, NULL);
    const TestUniforms u = {.color = wpe_vec4(1, 1, 1, 1)};
    const ClipVertex tris[9] = {
        cv(-0.5f, -0.5f, 0, 1), cv(0.5f, 0.5f, 0, 1),  cv(0, 0, 0, 1),       /* collinear */
        cv(NAN, 0, 0, 1),       cv(0.5f, -0.5f, 0, 1), cv(0, 0.5f, 0, 1),    /* NaN */
        cv(-0.5f, -0.5f, 0, 1), cv(0.5f, -0.5f, 0, 1), cv(0, INFINITY, 0, 1) /* infinite */
    };
    const WPE_DrawDesc d = draw_of(tris, 9, &u, state_no_cull());
    const WPE_Framebuffer* fb = render(renderer, &d, 1);
    for (uint32_t i = 0; i < W * H; ++i)
        WPE_TEST_ASSERT_EQ_UINT(0xFF000000u, fb->color[i]);
    WPE_TEST_ASSERT_EQ_UINT(0, wpe_renderer_stats(renderer).triangles_rasterized);
    wpe_renderer_destroy(renderer);
}

/* ---- Depth ----------------------------------------------------------------------------------------- */

static void test_renderer_depth_test_orders_by_depth(void) {
    WPE_Renderer* renderer = make_renderer(W, H, NULL);
    const TestUniforms near_u = {.color = wpe_vec4(0, 1, 0, 1)};
    const TestUniforms far_u = {.color = wpe_vec4(1, 0, 0, 1)};
    const ClipVertex near_q[6] = {cv(-1, -1, -0.5f, 1), cv(1, -1, -0.5f, 1), cv(1, 1, -0.5f, 1),
                                  cv(-1, -1, -0.5f, 1), cv(1, 1, -0.5f, 1),  cv(-1, 1, -0.5f, 1)};
    const ClipVertex far_q[6] = {cv(-1, -1, 0.5f, 1), cv(1, -1, 0.5f, 1), cv(1, 1, 0.5f, 1),
                                 cv(-1, -1, 0.5f, 1), cv(1, 1, 0.5f, 1),  cv(-1, 1, 0.5f, 1)};
    const size_t center = (size_t)(H / 2) * W + W / 2;

    WPE_DrawDesc draws[2] = {draw_of(near_q, 6, &near_u, wpe_render_state_default()),
                             draw_of(far_q, 6, &far_u, wpe_render_state_default())};
    WPE_TEST_ASSERT_EQ_UINT(0xFF00FF00u, render(renderer, draws, 2)->color[center]);
    WPE_TEST_ASSERT_NEAR(0.25, wpe_renderer_framebuffer(renderer)->depth[center], 1e-6);

    const WPE_DrawDesc reversed[2] = {draws[1], draws[0]};
    WPE_TEST_ASSERT_EQ_UINT(0xFF00FF00u, render(renderer, reversed, 2)->color[center]);

    /* ALWAYS: last draw wins regardless of depth. It does not write depth, so the first draw's depth remains.
     */
    draws[0].state.depth_func = WPE_DEPTH_ALWAYS;
    draws[1].state.depth_func = WPE_DEPTH_ALWAYS;
    draws[1].state.depth_write = false;
    WPE_TEST_ASSERT_EQ_UINT(0xFFFF0000u, render(renderer, draws, 2)->color[center]);
    WPE_TEST_ASSERT_NEAR(0.25, wpe_renderer_framebuffer(renderer)->depth[center], 1e-6);

    /* LEQUAL accepts equal depth; LESS rejects it. */
    WPE_DrawDesc same[2] = {draw_of(near_q, 6, &far_u, wpe_render_state_default()),
                            draw_of(near_q, 6, &near_u, wpe_render_state_default())};
    WPE_TEST_ASSERT_EQ_UINT(0xFFFF0000u, render(renderer, same, 2)->color[center]);
    same[1].state.depth_func = WPE_DEPTH_LEQUAL;
    WPE_TEST_ASSERT_EQ_UINT(0xFF00FF00u, render(renderer, same, 2)->color[center]);
    wpe_renderer_destroy(renderer);
}

/* ---- Clipping and interpolation --------------------------------------------------------------------- */

static void test_renderer_near_plane_clipping(void) {
    /* A floor quad that passes behind the camera: must render the visible part only, without artifacts. */
    WPE_Renderer* renderer = make_renderer(W, H, NULL);
    const WPE_Mat4 proj = wpe_mat4_perspective(WPE_PI / 2, (float)W / H, 0.1f, 100.0f);
    /* Wide enough to span the whole view width even at the far end. */
    const WPE_Vec3 corners[4] = {wpe_vec3(-1000, -1, 10), wpe_vec3(1000, -1, 10), wpe_vec3(1000, -1, -50),
                                 wpe_vec3(-1000, -1, -50)};
    ClipVertex verts[4];
    for (int i = 0; i < 4; ++i) {
        const WPE_Vec4 c = wpe_mat4_mul_vec4(&proj, wpe_vec4(corners[i].x, corners[i].y, corners[i].z, 1));
        verts[i] = cv(c.x, c.y, c.z, c.w);
    }
    const ClipVertex quad[6] = {verts[0], verts[1], verts[2], verts[0], verts[2], verts[3]};
    static uint32_t counts[W * H];
    memset(counts, 0, sizeof counts);
    const TestUniforms u = {.color = wpe_vec4(1, 1, 1, 1), .counts = counts, .width = W};
    const WPE_DrawDesc d = draw_of(quad, 6, &u, wpe_render_state_default());
    render(renderer, &d, 1);

    WPE_TEST_ASSERT(wpe_renderer_stats(renderer).triangles_clipped >= 1);
    /* The floor fills the lower half (below the horizon) and nothing above it, each pixel once. */
    for (uint32_t y = 0; y < H; ++y) {
        for (uint32_t x = 0; x < W; ++x) {
            const uint32_t expected = y >= H / 2 + 1 ? 1 : (y < H / 2 - 1 ? 0 : counts[y * W + x]);
            WPE_TEST_ASSERT_MSG(counts[y * W + x] == expected && counts[y * W + x] <= 1,
                                "pixel (%u,%u) count %u", x, y, counts[y * W + x]);
        }
    }

    /* Entirely behind the camera: nothing is drawn. */
    const ClipVertex behind[3] = {cv(-1, -1, -2, -1), cv(1, -1, -2, -1), cv(0, 1, -2, -1)};
    memset(counts, 0, sizeof counts);
    const WPE_DrawDesc db = draw_of(behind, 3, &u, state_no_cull());
    render(renderer, &db, 1);
    assert_counts_all(counts, W, H, 0);
    wpe_renderer_destroy(renderer);
}

static void test_renderer_perspective_correct_varyings(void) {
    /*
     * Floor plane y = -1 viewed by a camera at the origin looking down -Z with a 90 degree FOV.
     * Varying 0 is the world-space z. For a pixel row with NDC y < 0 the view ray hits the floor at
     * z = 1 / y_ndc; affine (non-perspective) interpolation would be far off.
     */
    enum { S = 64 };
    WPE_Renderer* renderer = make_renderer(S, S, NULL);
    const WPE_Mat4 proj = wpe_mat4_perspective(WPE_PI / 2, 1.0f, 0.1f, 100.0f);
    const WPE_Vec3 corners[4] = {wpe_vec3(-1000, -1, -0.5f), wpe_vec3(1000, -1, -0.5f),
                                 wpe_vec3(1000, -1, -60), wpe_vec3(-1000, -1, -60)};
    ClipVertex verts[4];
    for (int i = 0; i < 4; ++i) {
        const WPE_Vec4 c = wpe_mat4_mul_vec4(&proj, wpe_vec4(corners[i].x, corners[i].y, corners[i].z, 1));
        verts[i] = (ClipVertex){.position = {c.x, c.y, c.z, c.w}, .attr = {corners[i].z, 0, 0, 1}};
    }
    const ClipVertex quad[6] = {verts[0], verts[1], verts[2], verts[0], verts[2], verts[3]};
    static float captured[S * S];
    for (int i = 0; i < S * S; ++i)
        captured[i] = NAN;
    const TestUniforms u = {.color = wpe_vec4(1, 1, 1, 1), .captured = captured, .width = S};
    const WPE_DrawDesc d = draw_of(quad, 6, &u, wpe_render_state_default());
    render(renderer, &d, 1);

    int checked = 0;
    for (uint32_t y = S / 2 + 2; y < S; ++y) {
        const float y_ndc = 1.0f - 2.0f * ((float)y + 0.5f) / S;
        const float expected = 1.0f / y_ndc;
        if (expected < -59.0f || expected > -0.6f) continue; /* Outside the quad's z range. */
        const float actual = captured[y * S + S / 2];
        WPE_TEST_ASSERT_MSG(fabsf(actual - expected) <= fabsf(expected) * 2e-3f,
                            "row %u: expected z %.4f, got %.4f", y, (double)expected, (double)actual);
        ++checked;
    }
    WPE_TEST_ASSERT(checked > 10);
    wpe_renderer_destroy(renderer);
}

/* ---- Blending, ordering, discard ------------------------------------------------------------------- */

static void test_renderer_blend_modes(void) {
    WPE_Renderer* renderer = make_renderer(W, H, NULL);
    const ClipVertex quad[6] = {cv(-1, -1, 0, 1), cv(1, -1, 0, 1), cv(1, 1, 0, 1),
                                cv(-1, -1, 0, 1), cv(1, 1, 0, 1),  cv(-1, 1, 0, 1)};
    const TestUniforms blue = {.color = wpe_vec4(0, 0, 1, 1)};
    const TestUniforms half_red = {.color = wpe_vec4(1, 0, 0, 0.5f)};
    WPE_RenderState over = state_no_cull();
    over.depth_func = WPE_DEPTH_ALWAYS;

    WPE_DrawDesc draws[2] = {draw_of(quad, 6, &blue, over), draw_of(quad, 6, &half_red, over)};
    draws[1].state.blend = WPE_BLEND_ALPHA;
    WPE_TEST_ASSERT_EQ_UINT(wpe_color_rgba8(128, 0, 128, 255), render(renderer, draws, 2)->color[0]);

    draws[1].state.blend = WPE_BLEND_ADDITIVE;
    WPE_TEST_ASSERT_EQ_UINT(wpe_color_rgba8(128, 0, 255, 255), render(renderer, draws, 2)->color[0]);
    wpe_renderer_destroy(renderer);
}

static void test_renderer_dithered_alpha(void) {
    enum { S = 64 };
    WPE_Renderer* renderer = make_renderer(S, S, NULL);
    const ClipVertex quad[6] = {cv(-1, -1, 0, 1), cv(1, -1, 0, 1), cv(1, 1, 0, 1),
                                cv(-1, -1, 0, 1), cv(1, 1, 0, 1),  cv(-1, 1, 0, 1)};
    const float alphas[] = {0.0f, 0.25f, 0.5f, 0.75f, 1.0f};
    for (size_t a = 0; a < sizeof alphas / sizeof alphas[0]; ++a) {
        const TestUniforms u = {.color = wpe_vec4(1, 1, 1, alphas[a])};
        WPE_DrawDesc d = draw_of(quad, 6, &u, wpe_render_state_default());
        d.state.blend = WPE_BLEND_ALPHA_DITHER;
        const WPE_Framebuffer* fb = render(renderer, &d, 1);
        uint32_t written = 0;
        for (uint32_t i = 0; i < S * S; ++i) {
            /* Written pixels are fully opaque white with depth; others keep the clear values. */
            if (fb->color[i] == 0xFFFFFFFFu) {
                ++written;
                WPE_TEST_ASSERT_NEAR(0.5, fb->depth[i], 1e-6);
            } else {
                WPE_TEST_ASSERT_EQ_UINT(0xFF000000u, fb->color[i]);
                WPE_TEST_ASSERT_NEAR(1.0, fb->depth[i], 0);
            }
        }
        /* The 4x4 Bayer pattern keeps exactly alpha * 16 of every 16 pixels at these alphas. */
        WPE_TEST_ASSERT_EQ_UINT((uint32_t)(alphas[a] * S * S), written);
    }
    wpe_renderer_destroy(renderer);
}

static void test_renderer_sort_key_orders_draws(void) {
    WPE_Renderer* renderer = make_renderer(W, H, NULL);
    const ClipVertex quad[6] = {cv(-1, -1, 0, 1), cv(1, -1, 0, 1), cv(1, 1, 0, 1),
                                cv(-1, -1, 0, 1), cv(1, 1, 0, 1),  cv(-1, 1, 0, 1)};
    const TestUniforms red = {.color = wpe_vec4(1, 0, 0, 1)};
    const TestUniforms green = {.color = wpe_vec4(0, 1, 0, 1)};
    WPE_RenderState over = state_no_cull();
    over.depth_func = WPE_DEPTH_ALWAYS;

    WPE_DrawDesc draws[2] = {draw_of(quad, 6, &red, over), draw_of(quad, 6, &green, over)};
    WPE_TEST_ASSERT_EQ_UINT(0xFF00FF00u,
                            render(renderer, draws, 2)->color[0]); /* equal keys: submission order */
    draws[0].sort_key = 2;
    draws[1].sort_key = 1;
    WPE_TEST_ASSERT_EQ_UINT(0xFFFF0000u, render(renderer, draws, 2)->color[0]); /* red sorts last */
    wpe_renderer_destroy(renderer);
}

static void test_renderer_fragment_discard(void) {
    WPE_Renderer* renderer = make_renderer(W, H, NULL);
    const ClipVertex quad[6] = {cv(-1, -1, 0, 1), cv(1, -1, 0, 1), cv(1, 1, 0, 1),
                                cv(-1, -1, 0, 1), cv(1, 1, 0, 1),  cv(-1, 1, 0, 1)};
    const TestUniforms u = {.color = wpe_vec4(1, 1, 1, 1), .discard_left_of = W / 2};
    const WPE_DrawDesc d = draw_of(quad, 6, &u, wpe_render_state_default());
    const WPE_Framebuffer* fb = render(renderer, &d, 1);
    for (uint32_t y = 0; y < H; ++y) {
        for (uint32_t x = 0; x < W; ++x) {
            const bool kept = x >= W / 2;
            WPE_TEST_ASSERT_EQ_UINT(kept ? 0xFFFFFFFFu : 0xFF000000u, fb->color[y * W + x]);
            WPE_TEST_ASSERT_NEAR(kept ? 0.5 : 1.0, fb->depth[y * W + x], 1e-6);
        }
    }
    wpe_renderer_destroy(renderer);
}

static void test_renderer_varyings_interpolate_vertex_colors(void) {
    WPE_Renderer* renderer = make_renderer(W, H, NULL);
    const ClipVertex quad[6] = {
        {{-1, -1, 0, 1}, {1, 0, 0, 1}}, {{1, -1, 0, 1}, {1, 0, 0, 1}}, {{1, 1, 0, 1}, {0, 0, 1, 1}},
        {{-1, -1, 0, 1}, {1, 0, 0, 1}}, {{1, 1, 0, 1}, {0, 0, 1, 1}},  {{-1, 1, 0, 1}, {0, 0, 1, 1}},
    };
    const WPE_DrawDesc d = {.program = &g_varying_program,
                            .vertices = quad,
                            .vertex_stride = sizeof(ClipVertex),
                            .vertex_count = 6,
                            .state = wpe_render_state_default()};
    const WPE_Framebuffer* fb = render(renderer, &d, 1);
    /* Top row is nearly blue, bottom row nearly red, middle is a mix. */
    const WPE_Vec4 top = wpe_color_to_vec4(fb->color[W / 2]);
    const WPE_Vec4 bottom = wpe_color_to_vec4(fb->color[(H - 1) * W + W / 2]);
    const WPE_Vec4 middle = wpe_color_to_vec4(fb->color[(H / 2) * W + W / 2]);
    WPE_TEST_ASSERT(top.z > 0.97f && top.x < 0.03f);
    WPE_TEST_ASSERT(bottom.x > 0.97f && bottom.z < 0.03f);
    WPE_TEST_ASSERT_NEAR(0.5, middle.x, 0.03);
    WPE_TEST_ASSERT_NEAR(0.5, middle.z, 0.03);
    wpe_renderer_destroy(renderer);
}

/* ---- Determinism ----------------------------------------------------------------------------------- */

static void build_random_scene(ClipVertex* verts, uint32_t count) {
    uint32_t rng = 0x5EEDu;
    for (uint32_t i = 0; i < count; ++i) {
        float r[7];
        for (int k = 0; k < 7; ++k) {
            rng = rng * 1664525u + 1013904223u;
            r[k] = (float)(rng >> 8) / 16777216.0f;
        }
        verts[i] = (ClipVertex){.position = {r[0] * 3 - 1.5f, r[1] * 3 - 1.5f, r[2] * 1.8f - 0.9f, 1},
                                .attr = {r[3], r[4], r[5], 0.3f + 0.7f * r[6]}};
    }
}

static void test_renderer_output_is_independent_of_thread_count(void) {
    enum { TRIS = 3000, S = 200 };
    static ClipVertex verts[TRIS * 3];
    build_random_scene(verts, TRIS * 3);
    WPE_RenderState state = state_no_cull();
    state.blend = WPE_BLEND_ALPHA; /* Order-dependent blending makes any ordering bug visible. */
    WPE_DrawDesc draws[3];
    for (int i = 0; i < 3; ++i) {
        draws[i] = (WPE_DrawDesc){.program = &g_varying_program,
                                  .vertices = verts + i * TRIS,
                                  .vertex_stride = sizeof(ClipVertex),
                                  .vertex_count = TRIS,
                                  .state = state,
                                  .sort_key = (uint64_t)(2 - i)};
    }

    static WPE_Color reference[S * S];
    WPE_RenderStats reference_stats = {0};
    const uint32_t worker_counts[] = {0, 1, 3, 7};
    for (size_t w = 0; w < sizeof worker_counts / sizeof worker_counts[0]; ++w) {
        WPE_JobSystem* jobs = NULL;
        WPE_TEST_ASSERT_OK(wpe_jobs_create(worker_counts[w], &jobs));
        WPE_Renderer* renderer = make_renderer(S, S, jobs);
        const WPE_Framebuffer* fb = render(renderer, draws, 3);
        const WPE_RenderStats stats = wpe_renderer_stats(renderer);
        if (w == 0) {
            memcpy(reference, fb->color, sizeof reference);
            reference_stats = stats;
            WPE_TEST_ASSERT(stats.fragments_shaded > 0);
        } else {
            WPE_TEST_ASSERT_MSG(memcmp(reference, fb->color, sizeof reference) == 0,
                                "framebuffer differs with %u workers", worker_counts[w]);
            WPE_TEST_ASSERT_EQ_UINT(reference_stats.fragments_shaded, stats.fragments_shaded);
            WPE_TEST_ASSERT_EQ_UINT(reference_stats.triangles_rasterized, stats.triangles_rasterized);
        }
        wpe_renderer_destroy(renderer);
        wpe_jobs_destroy(jobs);
    }
}

static void test_renderer_builtin_vertex_color_shader(void) {
    WPE_Renderer* renderer = make_renderer(W, H, NULL);
    const WPE_VertexColored tri[3] = {
        {.position = {{-1, -1, 0}}, .color = {1, 1, 1, 1}},
        {.position = {{3, -1, 0}}, .color = {1, 1, 1, 1}},
        {.position = {{-1, 3, 0}}, .color = {1, 1, 1, 1}},
    };
    const WPE_VertexColorUniforms u = {.mvp = wpe_mat4_identity(), .tint = wpe_vec4(0, 1, 0, 1)};
    const WPE_DrawDesc d = {.program = wpe_shader_vertex_color(),
                            .uniforms = &u,
                            .uniforms_size = sizeof u,
                            .vertices = tri,
                            .vertex_stride = sizeof tri[0],
                            .vertex_count = 3,
                            .state = wpe_render_state_default()};
    const WPE_Framebuffer* fb = render(renderer, &d, 1);
    for (uint32_t i = 0; i < W * H; ++i)
        WPE_TEST_ASSERT_EQ_UINT(0xFF00FF00u, fb->color[i]);
    wpe_renderer_destroy(renderer);
}

static void test_renderer_steady_state_frames_do_not_allocate(void) {
    enum { TRIS = 2000, S = 128 };
    static ClipVertex verts[TRIS * 3];
    build_random_scene(verts, TRIS * 3);
    const WPE_DrawDesc d = {.program = &g_varying_program,
                            .vertices = verts,
                            .vertex_stride = sizeof(ClipVertex),
                            .vertex_count = TRIS * 3,
                            .state = state_no_cull()};
    WPE_JobSystem* jobs = NULL;
    WPE_TEST_ASSERT_OK(wpe_jobs_create(3, &jobs));
    WPE_Renderer* renderer = make_renderer(S, S, jobs);
    for (int warmup = 0; warmup < 3; ++warmup)
        render(renderer, &d, 1);

    WPE_AllocStats before, after;
    wpe_alloc_get_stats(&before);
    for (int frame = 0; frame < 5; ++frame)
        render(renderer, &d, 1);
    wpe_alloc_get_stats(&after);
    WPE_TEST_ASSERT_EQ_UINT(before.total_allocations, after.total_allocations);
    wpe_renderer_destroy(renderer);
    wpe_jobs_destroy(jobs);
}

WPE_TEST_MAIN(
    WPE_TEST_CASE(test_renderer_color_conversion), WPE_TEST_CASE(test_renderer_create_validates_size),
    WPE_TEST_CASE(test_renderer_frame_state_machine), WPE_TEST_CASE(test_renderer_submit_validates_draws),
    WPE_TEST_CASE(test_renderer_clear_only_frame),
    WPE_TEST_CASE(test_renderer_fullscreen_quad_covers_each_pixel_once),
    WPE_TEST_CASE(test_renderer_jittered_mesh_is_watertight),
    WPE_TEST_CASE(test_renderer_fill_rule_on_pixel_center_edges),
    WPE_TEST_CASE(test_renderer_guard_band_triangle_fills_screen),
    WPE_TEST_CASE(test_renderer_backface_culling),
    WPE_TEST_CASE(test_renderer_degenerate_and_nonfinite_triangles_are_dropped),
    WPE_TEST_CASE(test_renderer_depth_test_orders_by_depth), WPE_TEST_CASE(test_renderer_near_plane_clipping),
    WPE_TEST_CASE(test_renderer_perspective_correct_varyings), WPE_TEST_CASE(test_renderer_blend_modes),
    WPE_TEST_CASE(test_renderer_dithered_alpha), WPE_TEST_CASE(test_renderer_sort_key_orders_draws),
    WPE_TEST_CASE(test_renderer_fragment_discard),
    WPE_TEST_CASE(test_renderer_varyings_interpolate_vertex_colors),
    WPE_TEST_CASE(test_renderer_output_is_independent_of_thread_count),
    WPE_TEST_CASE(test_renderer_builtin_vertex_color_shader),
    WPE_TEST_CASE(test_renderer_steady_state_frames_do_not_allocate))
