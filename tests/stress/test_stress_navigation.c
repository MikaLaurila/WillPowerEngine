#include "wpe_test.h"

#include <math.h>
#include <stdio.h>
#include <time.h>

#include "navigation/nav_path.h"
#include "navigation/navigation.h"

/* Deterministic xorshift32 so failures are reproducible. */
static uint32_t g_rng_state = 0x9E3779B9u;

static uint32_t rng_next(void) {
    uint32_t x = g_rng_state;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    return g_rng_state = x;
}

static float rng_range(float lo, float hi) {
    return lo + (hi - lo) * (float)(rng_next() % 100000u) / 100000.0f;
}

static double now_ms(void) {
    struct timespec ts;
    timespec_get(&ts, TIME_UTC);
    return (double)ts.tv_sec * 1000.0 + (double)ts.tv_nsec / 1e6;
}

static void add_block(WPE_NavGeometry* g, WPE_Vec3 min, WPE_Vec3 max) {
    WPE_TEST_ASSERT_OK(wpe_nav_geometry_add_box(g, wpe_vec3_scale(wpe_vec3_add(min, max), 0.5f),
                                                wpe_vec3_scale(wpe_vec3_sub(max, min), 0.5f), NULL));
}

/* A 100 x 100 m floor with 250 random walls (some low enough to step over, some tall), a few raised
 * platforms and pillars: about 160 000 columns at the default 0.25 m. */
static WPE_NavGrid* build_level(void) {
    WPE_NavGeometry* g = NULL;
    WPE_TEST_ASSERT_OK(wpe_nav_geometry_create(&g));
    add_block(g, wpe_vec3(-50, -1, -50), wpe_vec3(50, 0, 50));
    for (int i = 0; i < 250; ++i) {
        const float x = rng_range(-48, 48), z = rng_range(-48, 48), length = rng_range(2, 14);
        const float height = (rng_next() % 8u) == 0 ? 0.2f : 2.5f;
        if (rng_next() % 2u) {
            add_block(g, wpe_vec3(x, 0, z), wpe_vec3(x + length, height, z + 0.3f));
        } else {
            add_block(g, wpe_vec3(x, 0, z), wpe_vec3(x + 0.3f, height, z + length));
        }
    }
    for (int i = 0; i < 20; ++i) {
        const float x = rng_range(-45, 40), z = rng_range(-45, 40);
        add_block(g, wpe_vec3(x, 0, z),
                  wpe_vec3(x + rng_range(2, 6), rng_range(0.5f, 3.0f), z + rng_range(2, 6)));
    }
    WPE_NavGrid* grid = NULL;
    WPE_TEST_ASSERT_OK(wpe_nav_grid_build(g, NULL, &grid));
    wpe_nav_geometry_destroy(g);
    return grid;
}

static uint32_t random_cell(const WPE_NavGrid* grid) {
    return rng_next() % wpe_nav_grid_info(grid).cell_count;
}

static WPE_Vec3 center_of(const WPE_NavGrid* grid, uint32_t cell) {
    WPE_NavCellInfo info;
    WPE_TEST_ASSERT(wpe_nav_grid_cell(grid, cell, &info));
    return info.position;
}

/* Model check: A*'s raw path between two cell centers is exactly as long as the Dijkstra distance the
 * flow field finds (both are optimal over the same moves); smoothed paths are walkable and no longer. */
static void test_stress_nav_paths_are_optimal_and_walkable(void) {
    WPE_NavGrid* grid = build_level();
    const WPE_NavGridInfo info = wpe_nav_grid_info(grid);
    fprintf(stderr, "    level: %u x %u columns, %u cells, %u islands, %zu KiB\n", info.width, info.depth,
            info.cell_count, info.island_count, info.memory / 1024u);
    WPE_NavQuery* q = NULL;
    WPE_TEST_ASSERT_OK(wpe_nav_query_create(grid, &q));
    WPE_NavFlowField* field = NULL;
    WPE_TEST_ASSERT_OK(wpe_nav_flow_create(grid, &field));
    WPE_NavPathOptions raw = wpe_nav_path_options_default();
    raw.raw = true;
    uint32_t checked = 0, unreachable = 0;
    for (int goal_round = 0; goal_round < 8; ++goal_round) {
        const uint32_t goal_cell = random_cell(grid);
        const WPE_Vec3 goal = center_of(grid, goal_cell);
        WPE_TEST_ASSERT_OK(wpe_nav_flow_build(field, &goal, 1, 0.0f, NULL));
        for (int i = 0; i < 40; ++i) {
            const uint32_t start_cell = random_cell(grid);
            const WPE_Vec3 start = center_of(grid, start_cell);
            WPE_NavPath path;
            const WPE_Result result = wpe_nav_find_path(q, start, goal, &raw, &path);
            const float flow = wpe_nav_flow_distance(field, start_cell);
            if (!wpe_nav_grid_reachable(grid, start_cell, goal_cell)) {
                WPE_TEST_ASSERT_EQ_INT(WPE_RESULT_ERROR_NOT_FOUND, result);
                WPE_TEST_ASSERT(isinf(flow));
                ++unreachable;
                continue;
            }
            WPE_TEST_ASSERT_OK(result);
            WPE_TEST_ASSERT_MSG(fabsf(path.length - flow) <= 1e-3f + 1e-4f * flow,
                                "A* found %.4f, Dijkstra %.4f (cells %u -> %u)", (double)path.length,
                                (double)flow, start_cell, goal_cell);
            for (uint32_t p = 1; p < path.count;
                 ++p) /* Every move, diagonals too, is walkable: no corner cut. */
                WPE_TEST_ASSERT_MSG(wpe_nav_grid_line_walkable(grid, path.points[p - 1], path.points[p]),
                                    "move %u of a raw path is blocked", p);
            const float raw_length = path.length;
            WPE_TEST_ASSERT_OK(wpe_nav_find_path(q, start, goal, NULL, &path));
            WPE_TEST_ASSERT(path.length <= raw_length + 1e-3f);
            for (uint32_t p = 1; p < path.count; ++p)
                WPE_TEST_ASSERT_MSG(wpe_nav_grid_line_walkable(grid, path.points[p - 1], path.points[p]),
                                    "segment %u of a smoothed path is blocked", p);
            ++checked;
        }
    }
    fprintf(stderr, "    %u paths matched Dijkstra, %u pairs on different islands\n", checked, unreachable);
    WPE_TEST_ASSERT(checked > 200);
    wpe_nav_flow_destroy(field);
    wpe_nav_query_destroy(q);
    wpe_nav_grid_destroy(grid);
}

static void test_stress_nav_performance(void) {
    double start = now_ms();
    WPE_NavGrid* grid = build_level();
    const double build = now_ms() - start;
    WPE_NavQuery* q = NULL;
    WPE_TEST_ASSERT_OK(wpe_nav_query_create(grid, &q));
    /* Long paths: pairs of reachable cells at least 40 m apart. */
    enum { PATHS = 200 };
    WPE_Vec3 from[PATHS], to[PATHS];
    for (int i = 0; i < PATHS;) {
        const uint32_t a = random_cell(grid), b = random_cell(grid);
        from[i] = center_of(grid, a);
        to[i] = center_of(grid, b);
        if (wpe_nav_grid_reachable(grid, a, b) && wpe_vec3_length(wpe_vec3_sub(from[i], to[i])) > 40.0f) ++i;
    }
    WPE_NavPath path;
    uint64_t visited = 0;
    start = now_ms();
    for (int i = 0; i < PATHS; ++i) {
        WPE_TEST_ASSERT_OK(wpe_nav_find_path(q, from[i], to[i], NULL, &path));
        visited += path.visited;
    }
    const double per_path = (now_ms() - start) / PATHS;
    WPE_NavPathOptions raw = wpe_nav_path_options_default();
    raw.raw = true;
    start = now_ms();
    for (int i = 0; i < PATHS; ++i)
        WPE_TEST_ASSERT_OK(wpe_nav_find_path(q, from[i], to[i], &raw, &path));
    const double per_raw = (now_ms() - start) / PATHS;
    WPE_NavFlowField* field = NULL;
    WPE_TEST_ASSERT_OK(wpe_nav_flow_create(grid, &field));
    start = now_ms();
    WPE_TEST_ASSERT_OK(wpe_nav_flow_build(field, &to[0], 1, 0.0f, NULL));
    const double flow = now_ms() - start;
    start = now_ms();
    WPE_Vec3 dir;
    for (int i = 0; i < 10000; ++i)
        (void)wpe_nav_flow_direction(field, from[i % PATHS], &dir, NULL);
    const double directions = (now_ms() - start) / 10.0; /* Per 1000. */
    fprintf(stderr,
            "    build %.1f ms, path (>40 m) %.3f ms avg (%.3f ms unsmoothed, %.0f cells expanded), flow "
            "field %.1f "
            "ms, 1000 directions %.3f ms\n",
            build, per_path, per_raw, (double)visited / PATHS, flow, directions);
#if !defined(WPE_DEBUG)
    /* Budgets (Release): a level loads in well under a second, an agent's long repath fits in a frame
     * alongside everything else, and a whole-level flow field rebuilds a few times a second. */
    WPE_TEST_ASSERT_MSG(build < 1000.0, "building took %.1f ms (budget 1000 ms)", build);
    /* Grid A* expands many cells on long paths through a maze (tens of thousands here); what the code
     * controls is the cost of each. */
    const double per_cell_us = per_raw * 1000.0 / ((double)visited / PATHS);
    WPE_TEST_ASSERT_MSG(per_cell_us < 0.5, "A* took %.3f us per expanded cell (budget 0.5 us)", per_cell_us);
    WPE_TEST_ASSERT_MSG(per_path < 10.0, "a long path took %.3f ms (budget 10 ms)", per_path);
    WPE_TEST_ASSERT_MSG(flow < 50.0, "a whole-level flow field took %.1f ms (budget 50 ms)", flow);
    WPE_TEST_ASSERT_MSG(directions < 2.0, "1000 flow directions took %.3f ms (budget 2 ms)", directions);
#endif
    wpe_nav_flow_destroy(field);
    wpe_nav_query_destroy(q);
    wpe_nav_grid_destroy(grid);
}

WPE_TEST_MAIN(WPE_TEST_CASE(test_stress_nav_paths_are_optimal_and_walkable),
              WPE_TEST_CASE(test_stress_nav_performance))
