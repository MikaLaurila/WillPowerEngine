#include "wpe_test.h"
#include "wpe_test_assets.h"

#include <math.h>
#include <string.h>

#include "assets/procedural.h"
#include "navigation/navigation.h"
#include "physics/physics.h"
#include "scene/scene.h"

#define DEG (3.14159265f / 180.0f)

/* ---- Helpers ----------------------------------------------------------------------------------------- */

static WPE_NavGeometry* new_geometry(void) {
    WPE_NavGeometry* g = NULL;
    WPE_TEST_ASSERT_OK(wpe_nav_geometry_create(&g));
    return g;
}

/* A solid box from `min` to `max`. */
static void add_block(WPE_NavGeometry* g, WPE_Vec3 min, WPE_Vec3 max) {
    const WPE_Vec3 center = wpe_vec3_scale(wpe_vec3_add(min, max), 0.5f);
    const WPE_Vec3 half = wpe_vec3_scale(wpe_vec3_sub(max, min), 0.5f);
    WPE_TEST_ASSERT_OK(wpe_nav_geometry_add_box(g, center, half, NULL));
}

/* A 10 x 10 floor, 1 thick, whose top is at y = 0, centered on the origin. */
static void add_floor(WPE_NavGeometry* g) {
    add_block(g, wpe_vec3(-5, -1, -5), wpe_vec3(5, 0, 5));
}

static WPE_NavGrid* build(const WPE_NavGeometry* g, const WPE_NavSettings* settings) {
    WPE_NavGrid* grid = NULL;
    WPE_TEST_ASSERT_OK(wpe_nav_grid_build(g, settings, &grid));
    WPE_TEST_ASSERT(grid != NULL);
    return grid;
}

/* The cell standing at a point (within a few centimeters of its floor), or WPE_NAV_CELL_NONE. */
static uint32_t cell_at(const WPE_NavGrid* grid, float x, float y, float z) {
    return wpe_nav_grid_find_cell(grid, wpe_vec3(x, y, z), 0.15f);
}

static WPE_NavCellInfo info_of(const WPE_NavGrid* grid, uint32_t cell) {
    WPE_NavCellInfo info;
    WPE_TEST_ASSERT(wpe_nav_grid_cell(grid, cell, &info));
    return info;
}

static uint32_t island_at(const WPE_NavGrid* grid, float x, float y, float z) {
    const uint32_t cell = cell_at(grid, x, y, z);
    WPE_TEST_ASSERT(cell != WPE_NAV_CELL_NONE);
    return info_of(grid, cell).island;
}

/* ---- Settings and geometry ------------------------------------------------------------------------------
 */

static void test_nav_settings_are_validated(void) {
    WPE_NavSettings s = wpe_nav_settings_default();
    WPE_TEST_ASSERT_OK(wpe_nav_settings_validate(&s));
    /* The defaults fit the default character controller. */
    const WPE_CharacterController character = wpe_character_default();
    WPE_TEST_ASSERT_NEAR(character.radius, s.agent_radius, 1e-6);
    WPE_TEST_ASSERT_NEAR(character.height, s.agent_height, 1e-6);
    WPE_TEST_ASSERT_NEAR(character.max_slope, s.max_slope, 1e-6);
    WPE_TEST_ASSERT_NEAR(character.step_height, s.step_height, 1e-6);
    WPE_TEST_ASSERT_EQ_INT(WPE_RESULT_ERROR_NULL_ARG, wpe_nav_settings_validate(NULL));

    const float bad_values[] = {0.0f, -1.0f, NAN, INFINITY};
    for (size_t i = 0; i < sizeof bad_values / sizeof bad_values[0]; ++i) {
        s = wpe_nav_settings_default();
        s.cell_size = bad_values[i];
        WPE_TEST_ASSERT_EQ_INT(WPE_RESULT_ERROR_INVALID_ARG, wpe_nav_settings_validate(&s));
        s = wpe_nav_settings_default();
        s.agent_height = bad_values[i];
        WPE_TEST_ASSERT_EQ_INT(WPE_RESULT_ERROR_INVALID_ARG, wpe_nav_settings_validate(&s));
    }
    s = wpe_nav_settings_default();
    s.agent_radius = 0.0f; /* A point agent, and no steps, are fine. */
    s.step_height = 0.0f;
    WPE_TEST_ASSERT_OK(wpe_nav_settings_validate(&s));
    s = wpe_nav_settings_default();
    s.max_slope = 90.0f;
    WPE_TEST_ASSERT_EQ_INT(WPE_RESULT_ERROR_INVALID_ARG, wpe_nav_settings_validate(&s));
    s = wpe_nav_settings_default();
    s.step_height = s.agent_height;
    WPE_TEST_ASSERT_EQ_INT(WPE_RESULT_ERROR_INVALID_ARG, wpe_nav_settings_validate(&s));
    s = wpe_nav_settings_default();
    s.bounds_max.y = NAN;
    WPE_TEST_ASSERT_EQ_INT(WPE_RESULT_ERROR_INVALID_ARG, wpe_nav_settings_validate(&s));

    WPE_NavGeometry* g = new_geometry();
    add_floor(g);
    WPE_NavGrid* grid = NULL;
    s = wpe_nav_settings_default();
    s.max_slope = -5.0f;
    WPE_TEST_ASSERT_EQ_INT(WPE_RESULT_ERROR_INVALID_ARG, wpe_nav_grid_build(g, &s, &grid));
    WPE_TEST_ASSERT(grid == NULL);
    wpe_nav_geometry_destroy(g);
}

static void test_nav_geometry_validates_triangles(void) {
    WPE_NavGeometry* g = new_geometry();
    WPE_Vec3 min, max;
    WPE_TEST_ASSERT(!wpe_nav_geometry_bounds(g, &min, &max));
    const WPE_Vec3 vertices[4] = {wpe_vec3(0, 0, 0), wpe_vec3(1, 0, 0), wpe_vec3(0, 0, -1),
                                  wpe_vec3(2, 0, 0)};
    const uint32_t out_of_range[3] = {0, 1, 4};
    WPE_TEST_ASSERT_EQ_INT(WPE_RESULT_ERROR_INVALID_ARG,
                           wpe_nav_geometry_add_triangles(g, vertices, 4, out_of_range, 3, NULL));
    const WPE_Vec3 not_finite[3] = {wpe_vec3(0, 0, 0), wpe_vec3(NAN, 0, 0), wpe_vec3(0, 0, 1)};
    const uint32_t triangle[3] = {0, 1, 2};
    WPE_TEST_ASSERT_EQ_INT(WPE_RESULT_ERROR_INVALID_ARG,
                           wpe_nav_geometry_add_triangles(g, not_finite, 3, triangle, 3, NULL));
    WPE_TEST_ASSERT_EQ_INT(WPE_RESULT_ERROR_NULL_ARG,
                           wpe_nav_geometry_add_triangles(g, NULL, 3, triangle, 3, NULL));
    WPE_TEST_ASSERT_EQ_UINT(0, wpe_nav_geometry_triangle_count(g)); /* Failures add nothing. */

    /* A degenerate triangle (three points on a line) is skipped; extra indices are ignored. */
    const uint32_t two[7] = {0, 1, 3, 0, 1, 2, 0};
    WPE_TEST_ASSERT_OK(wpe_nav_geometry_add_triangles(g, vertices, 4, two, 7, NULL));
    WPE_TEST_ASSERT_EQ_UINT(1, wpe_nav_geometry_triangle_count(g));
    WPE_TEST_ASSERT(wpe_nav_geometry_bounds(g, &min, &max));
    WPE_TEST_ASSERT_NEAR(0.0f, min.x, 1e-6);
    WPE_TEST_ASSERT_NEAR(1.0f, max.x, 1e-6); /* Vertex 3 belongs to no triangle. */
    WPE_TEST_ASSERT_NEAR(-1.0f, min.z, 1e-6);

    const WPE_Mat4 moved = wpe_mat4_from_trs(wpe_vec3(10, 2, 0), wpe_quat_identity(), wpe_vec3(1, 1, 1));
    WPE_TEST_ASSERT_OK(wpe_nav_geometry_add_box(g, wpe_vec3(0, 0, 0), wpe_vec3(1, 1, 1), &moved));
    WPE_TEST_ASSERT_EQ_UINT(13, wpe_nav_geometry_triangle_count(g));
    WPE_TEST_ASSERT(wpe_nav_geometry_bounds(g, &min, &max));
    WPE_TEST_ASSERT_NEAR(11.0f, max.x, 1e-5);
    WPE_TEST_ASSERT_NEAR(3.0f, max.y, 1e-5);
    WPE_TEST_ASSERT_EQ_INT(WPE_RESULT_ERROR_INVALID_ARG,
                           wpe_nav_geometry_add_box(g, wpe_vec3(0, INFINITY, 0), wpe_vec3(1, 1, 1), NULL));
    wpe_nav_geometry_destroy(g);
    wpe_nav_geometry_destroy(NULL);
}

static void test_nav_build_needs_geometry(void) {
    WPE_NavGeometry* g = new_geometry();
    WPE_NavGrid* grid = NULL;
    WPE_TEST_ASSERT_EQ_INT(WPE_RESULT_ERROR_NOT_FOUND, wpe_nav_grid_build(g, NULL, &grid));
    WPE_TEST_ASSERT_EQ_INT(WPE_RESULT_ERROR_NULL_ARG, wpe_nav_grid_build(NULL, NULL, &grid));
    WPE_TEST_ASSERT_EQ_INT(WPE_RESULT_ERROR_NULL_ARG, wpe_nav_grid_build(g, NULL, NULL));
    add_floor(g);
    /* Too many columns for the cell size. */
    WPE_NavSettings s = wpe_nav_settings_default();
    s.cell_size = 0.001f;
    WPE_TEST_ASSERT_EQ_INT(WPE_RESULT_ERROR_CAPACITY, wpe_nav_grid_build(g, &s, &grid));
    s = wpe_nav_settings_default();
    s.cell_height = 0.00001f;
    WPE_TEST_ASSERT_EQ_INT(WPE_RESULT_ERROR_CAPACITY, wpe_nav_grid_build(g, &s, &grid));
    WPE_TEST_ASSERT(grid == NULL);
    /* Queries on nothing. */
    WPE_TEST_ASSERT_EQ_UINT(0, wpe_nav_grid_info(NULL).cell_count);
    WPE_TEST_ASSERT_EQ_UINT(WPE_NAV_CELL_NONE, wpe_nav_grid_find_cell(NULL, wpe_vec3(0, 0, 0), 1.0f));
    WPE_NavCellInfo info;
    WPE_TEST_ASSERT(!wpe_nav_grid_cell(NULL, 0, &info));
    wpe_nav_grid_destroy(NULL);
    wpe_nav_geometry_destroy(g);
}

/* ---- Walkable area --------------------------------------------------------------------------------------
 */

static void test_nav_flat_floor_is_one_island_kept_off_its_edges(void) {
    WPE_NavGeometry* g = new_geometry();
    add_floor(g);
    WPE_NavGrid* grid = build(g, NULL);
    const WPE_NavGridInfo info = wpe_nav_grid_info(grid);
    WPE_TEST_ASSERT_EQ_UINT(40, info.width); /* 10 m in 0.25 m columns. */
    WPE_TEST_ASSERT_EQ_UINT(40, info.depth);
    WPE_TEST_ASSERT_EQ_UINT(1, info.island_count);
    WPE_TEST_ASSERT_EQ_UINT(1, info.max_layers);
    /* The radius (0.4) keeps the two outer columns of each side free: the second one's center is only
     * 0.375 from the edge. */
    WPE_TEST_ASSERT_EQ_UINT(36u * 36u, info.cell_count);
    WPE_TEST_ASSERT(info.memory > info.cell_count * 8u);
    WPE_TEST_ASSERT_NEAR(-5.0f, info.settings.bounds_min.x, 1e-6); /* The geometry's bounds were used. */

    const uint32_t center = cell_at(grid, 0.1f, 0.0f, 0.1f);
    WPE_TEST_ASSERT(center != WPE_NAV_CELL_NONE);
    const WPE_NavCellInfo c = info_of(grid, center);
    WPE_TEST_ASSERT_NEAR(0.0f, c.position.y, 1e-4); /* The floor's top, exactly. */
    WPE_TEST_ASSERT_NEAR(0.125f, c.position.x, 1e-5);
    WPE_TEST_ASSERT_EQ_UINT(20, c.x);
    WPE_TEST_ASSERT_EQ_UINT(0, c.layer);
    WPE_TEST_ASSERT(isinf(c.ceiling));
    for (int dir = 0; dir < 4; ++dir) {
        WPE_TEST_ASSERT(c.neighbors[dir] != WPE_NAV_CELL_NONE);
        const WPE_NavCellInfo n = info_of(grid, c.neighbors[dir]);
        const int dx = (int)n.x - (int)c.x, dz = (int)n.z - (int)c.z;
        const int want_dx[4] = {1, 0, -1, 0}, want_dz[4] = {0, 1, 0, -1};
        WPE_TEST_ASSERT_EQ_INT(want_dx[dir], dx);
        WPE_TEST_ASSERT_EQ_INT(want_dz[dir], dz);
    }
    /* Edges: the third column in is the first one kept. */
    WPE_TEST_ASSERT_EQ_UINT(WPE_NAV_CELL_NONE, cell_at(grid, -4.6f, 0.0f, 0.0f));
    const uint32_t edge = cell_at(grid, -4.4f, 0.0f, 0.0f);
    WPE_TEST_ASSERT(edge != WPE_NAV_CELL_NONE);
    WPE_TEST_ASSERT_EQ_UINT(WPE_NAV_CELL_NONE, info_of(grid, edge).neighbors[WPE_NAV_NEG_X]);
    /* Off the grid, and too far above or below the floor. */
    WPE_TEST_ASSERT_EQ_UINT(WPE_NAV_CELL_NONE, cell_at(grid, 20.0f, 0.0f, 0.0f));
    WPE_TEST_ASSERT_EQ_UINT(WPE_NAV_CELL_NONE, cell_at(grid, 0.0f, 1.0f, 0.0f));
    WPE_TEST_ASSERT_EQ_UINT(center, wpe_nav_grid_find_cell(grid, wpe_vec3(0.1f, 1.0f, 0.1f), 1.0f));
    WPE_TEST_ASSERT_EQ_UINT(WPE_NAV_CELL_NONE, wpe_nav_grid_find_cell(grid, wpe_vec3(0.0f, NAN, 0.0f), 1.0f));
    WPE_TEST_ASSERT(!wpe_nav_grid_cell(grid, info.cell_count, &(WPE_NavCellInfo){0}));
    wpe_nav_grid_destroy(grid);

    /* A point agent keeps every column. */
    WPE_NavSettings point = wpe_nav_settings_default();
    point.agent_radius = 0.0f;
    grid = build(g, &point);
    WPE_TEST_ASSERT_EQ_UINT(40u * 40u, wpe_nav_grid_info(grid).cell_count);
    wpe_nav_grid_destroy(grid);
    wpe_nav_geometry_destroy(g);
}

static void test_nav_walls_split_and_doorways_join(void) {
    /* A wall across the floor at x = 0, 2 m tall: two rooms. */
    WPE_NavGeometry* g = new_geometry();
    add_floor(g);
    add_block(g, wpe_vec3(-0.1f, 0, -5), wpe_vec3(0.1f, 2, 5));
    WPE_NavGrid* grid = build(g, NULL);
    WPE_TEST_ASSERT_EQ_UINT(2, wpe_nav_grid_info(grid).island_count);
    WPE_TEST_ASSERT(island_at(grid, -2, 0, 0) != island_at(grid, 2, 0, 0));
    /* Keeps the agent's radius from the wall too. */
    WPE_TEST_ASSERT_EQ_UINT(WPE_NAV_CELL_NONE, cell_at(grid, -0.3f, 0, 0));
    wpe_nav_grid_destroy(grid);
    wpe_nav_geometry_destroy(g);

    /* A 2 m doorway in the middle joins them. */
    g = new_geometry();
    add_floor(g);
    add_block(g, wpe_vec3(-0.1f, 0, -5), wpe_vec3(0.1f, 2, -1));
    add_block(g, wpe_vec3(-0.1f, 0, 1), wpe_vec3(0.1f, 2, 5));
    grid = build(g, NULL);
    WPE_TEST_ASSERT_EQ_UINT(1, wpe_nav_grid_info(grid).island_count);
    WPE_TEST_ASSERT(cell_at(grid, 0.0f, 0, 0.1f) != WPE_NAV_CELL_NONE); /* Through the doorway. */
    wpe_nav_grid_destroy(grid);
    wpe_nav_geometry_destroy(g);

    /* A 0.5 m gap is narrower than the agent (0.8 m wide): still two rooms. */
    g = new_geometry();
    add_floor(g);
    add_block(g, wpe_vec3(-0.1f, 0, -5), wpe_vec3(0.1f, 2, -0.25f));
    add_block(g, wpe_vec3(-0.1f, 0, 0.25f), wpe_vec3(0.1f, 2, 5));
    grid = build(g, NULL);
    WPE_TEST_ASSERT_EQ_UINT(2, wpe_nav_grid_info(grid).island_count);
    wpe_nav_grid_destroy(grid);
    /* ...but a thin agent fits through (both rows' centers are at least 0.125 from the walls). */
    WPE_NavSettings thin = wpe_nav_settings_default();
    thin.agent_radius = 0.1f;
    grid = build(g, &thin);
    WPE_TEST_ASSERT_EQ_UINT(island_at(grid, -2, 0, 0), island_at(grid, 2, 0, 0));
    /* It can stand on the wall's top too (thin as it is), which nothing walks up to. */
    WPE_TEST_ASSERT(island_at(grid, 0, 2, 3) != island_at(grid, 2, 0, 0));
    wpe_nav_grid_destroy(grid);
    wpe_nav_geometry_destroy(g);

    /* A wall exactly on column lines (x from 0 to 0.5: columns 20 and 21) fills those columns only, not
     * the ones beside its faces. */
    g = new_geometry();
    add_floor(g);
    add_block(g, wpe_vec3(0, 0, -5), wpe_vec3(0.5f, 2, 5));
    WPE_NavSettings point = wpe_nav_settings_default();
    point.agent_radius = 0.0f;
    grid = build(g, &point);
    WPE_TEST_ASSERT(cell_at(grid, -0.125f, 0, 0) != WPE_NAV_CELL_NONE);
    WPE_TEST_ASSERT(cell_at(grid, 0.625f, 0, 0) != WPE_NAV_CELL_NONE);
    WPE_TEST_ASSERT_EQ_UINT(WPE_NAV_CELL_NONE, cell_at(grid, 0.125f, 0, 0));
    WPE_TEST_ASSERT(cell_at(grid, 0.125f, 2, 0) != WPE_NAV_CELL_NONE); /* Its top. */
    WPE_TEST_ASSERT_EQ_UINT(40u * 40u,
                            wpe_nav_grid_info(grid).cell_count); /* Floor or wall top everywhere. */
    wpe_nav_grid_destroy(grid);
    wpe_nav_geometry_destroy(g);
}

static void test_nav_steps_up_to_step_height(void) {
    /* A 0.2 m platform is stepped onto; a 0.6 m one is a separate island on top of a block. */
    WPE_NavGeometry* g = new_geometry();
    add_floor(g);
    add_block(g, wpe_vec3(-4, 0, -1.5f), wpe_vec3(-1, 0.2f, 1.5f));
    add_block(g, wpe_vec3(1, 0, -1.5f), wpe_vec3(4, 0.6f, 1.5f));
    WPE_NavGrid* grid = build(g, NULL);
    const uint32_t floor = island_at(grid, 0, 0, 3.5f);
    WPE_TEST_ASSERT_EQ_UINT(floor, island_at(grid, -2.5f, 0.2f, 0));
    const uint32_t high = island_at(grid, 2.5f, 0.6f, 0);
    WPE_TEST_ASSERT(high != floor);
    WPE_TEST_ASSERT_EQ_UINT(2, wpe_nav_grid_info(grid).island_count);
    /* The low platform's edge needs no clearance (it is walked onto), the high one's does. */
    WPE_TEST_ASSERT(cell_at(grid, -1.1f, 0.2f, 0) != WPE_NAV_CELL_NONE);
    WPE_TEST_ASSERT_EQ_UINT(WPE_NAV_CELL_NONE, cell_at(grid, 1.1f, 0.6f, 0));
    WPE_TEST_ASSERT_EQ_UINT(WPE_NAV_CELL_NONE, cell_at(grid, 0.9f, 0.0f, 0)); /* Next to its wall. */
    wpe_nav_grid_destroy(grid);

    /* A taller step height climbs the high one too. */
    WPE_NavSettings climber = wpe_nav_settings_default();
    climber.step_height = 0.7f;
    grid = build(g, &climber);
    WPE_TEST_ASSERT_EQ_UINT(1, wpe_nav_grid_info(grid).island_count);
    wpe_nav_grid_destroy(grid);
    wpe_nav_geometry_destroy(g);
}

/* A floor plus a ramp (one quad, 3 m wide along Z) rising from x = 0 toward +X at `degrees`. */
static WPE_NavGeometry* ramp_geometry(float degrees) {
    WPE_NavGeometry* g = new_geometry();
    add_block(g, wpe_vec3(-5, -1, -3), wpe_vec3(0, 0, 3));
    const float run = 4.0f, rise = run * tanf(degrees * DEG);
    const WPE_Vec3 v[4] = {wpe_vec3(0, 0, -1.5f), wpe_vec3(0, 0, 1.5f), wpe_vec3(run, rise, 1.5f),
                           wpe_vec3(run, rise, -1.5f)};
    const uint32_t up[6] = {0, 1, 2, 0, 2, 3}; /* Counter-clockwise seen from above. */
    WPE_TEST_ASSERT_OK(wpe_nav_geometry_add_triangles(g, v, 4, up, 6, NULL));
    return g;
}

static void test_nav_slopes_up_to_max_slope(void) {
    WPE_NavGeometry* g = ramp_geometry(30.0f);
    WPE_NavGrid* grid = build(g, NULL);
    const float x = 2.0f, y = x * tanf(30.0f * DEG);
    const uint32_t on_ramp = wpe_nav_grid_find_cell(grid, wpe_vec3(x, y, 0), 0.2f);
    WPE_TEST_ASSERT(on_ramp != WPE_NAV_CELL_NONE);
    WPE_TEST_ASSERT_NEAR(y, info_of(grid, on_ramp).position.y, 0.2);
    WPE_TEST_ASSERT_EQ_UINT(island_at(grid, -2, 0, 0),
                            info_of(grid, on_ramp).island); /* Walked up from the floor. */
    wpe_nav_grid_destroy(grid);
    wpe_nav_geometry_destroy(g);

    g = ramp_geometry(60.0f);
    grid = build(g, NULL);
    WPE_TEST_ASSERT_EQ_UINT(WPE_NAV_CELL_NONE,
                            wpe_nav_grid_find_cell(grid, wpe_vec3(1.0f, tanf(60.0f * DEG), 0), 0.3f));
    WPE_TEST_ASSERT_EQ_UINT(1, wpe_nav_grid_info(grid).island_count); /* Only the floor. */
    wpe_nav_grid_destroy(grid);
    /* A steeper limit accepts it. */
    WPE_NavSettings goat = wpe_nav_settings_default();
    goat.max_slope = 65.0f;
    goat.step_height = 0.5f; /* Columns 0.25 apart rise 0.43 on a 60-degree slope. */
    grid = build(g, &goat);
    /* A column's floor is the top of the ramp's piece in it (its +X side). */
    const uint32_t steep = wpe_nav_grid_find_cell(grid, wpe_vec3(1.125f, 1.25f * tanf(60.0f * DEG), 0), 0.1f);
    WPE_TEST_ASSERT(steep != WPE_NAV_CELL_NONE);
    WPE_TEST_ASSERT_EQ_UINT(island_at(grid, -2, 0, 0), info_of(grid, steep).island);
    wpe_nav_grid_destroy(grid);
    wpe_nav_geometry_destroy(g);

    /* Seen from below (clockwise from above), the same quad is a ceiling, never a floor. */
    g = new_geometry();
    const WPE_Vec3 v[4] = {wpe_vec3(-2, 0, -2), wpe_vec3(-2, 0, 2), wpe_vec3(2, 0, 2), wpe_vec3(2, 0, -2)};
    const uint32_t down[6] = {0, 2, 1, 0, 3, 2};
    WPE_TEST_ASSERT_OK(wpe_nav_geometry_add_triangles(g, v, 4, down, 6, NULL));
    grid = build(g, NULL);
    WPE_TEST_ASSERT_EQ_UINT(0, wpe_nav_grid_info(grid).cell_count);
    wpe_nav_grid_destroy(grid);
    wpe_nav_geometry_destroy(g);
    /* Mirrored in X, a floor's corners turn clockwise, but it still faces up: still a floor. */
    const WPE_Mat4 mirror = wpe_mat4_from_trs(wpe_vec3(0, 0, 0), wpe_quat_identity(), wpe_vec3(-1, 1, 1));
    const uint32_t up[6] = {0, 1, 2, 0, 2, 3};
    g = new_geometry();
    WPE_TEST_ASSERT_OK(wpe_nav_geometry_add_triangles(g, v, 4, up, 6, &mirror));
    grid = build(g, NULL);
    WPE_TEST_ASSERT_EQ_UINT(12u * 12u, wpe_nav_grid_info(grid).cell_count);
    wpe_nav_grid_destroy(grid);
    wpe_nav_geometry_destroy(g);
    g = new_geometry();
    WPE_TEST_ASSERT_OK(
        wpe_nav_geometry_add_triangles(g, v, 4, down, 6, &mirror)); /* And a ceiling a ceiling. */
    grid = build(g, NULL);
    WPE_TEST_ASSERT_EQ_UINT(0, wpe_nav_grid_info(grid).cell_count);
    wpe_nav_grid_destroy(grid);
    wpe_nav_geometry_destroy(g);
}

static void test_nav_headroom_and_stacked_floors(void) {
    /* A slab 1 m above the floor: no room under it (1.8 m needed), but its top is walkable. */
    WPE_NavGeometry* g = new_geometry();
    add_floor(g);
    add_block(g, wpe_vec3(-2, 1.0f, -5), wpe_vec3(2, 1.2f, 5));
    WPE_NavGrid* grid = build(g, NULL);
    WPE_TEST_ASSERT_EQ_UINT(WPE_NAV_CELL_NONE, cell_at(grid, 0, 0, 0));
    WPE_TEST_ASSERT(cell_at(grid, 0, 1.2f, 0) != WPE_NAV_CELL_NONE);
    wpe_nav_grid_destroy(grid);
    /* A crawling agent fits under it. */
    WPE_NavSettings crawler = wpe_nav_settings_default();
    crawler.agent_height = 0.9f;
    grid = build(g, &crawler);
    const uint32_t under = cell_at(grid, 0, 0, 0);
    WPE_TEST_ASSERT(under != WPE_NAV_CELL_NONE);
    WPE_TEST_ASSERT_NEAR(1.0f, info_of(grid, under).ceiling, 1e-4);
    wpe_nav_grid_destroy(grid);
    wpe_nav_geometry_destroy(g);

    /* A bridge 3 m up: the floor under it and its deck are two cells of the same column. */
    g = new_geometry();
    add_floor(g);
    add_block(g, wpe_vec3(-1, 3.0f, -5), wpe_vec3(1, 3.2f, 5));
    grid = build(g, NULL);
    const uint32_t ground = cell_at(grid, 0, 0, 0), deck = cell_at(grid, 0, 3.2f, 0);
    WPE_TEST_ASSERT(ground != WPE_NAV_CELL_NONE && deck != WPE_NAV_CELL_NONE);
    const WPE_NavCellInfo a = info_of(grid, ground), b = info_of(grid, deck);
    WPE_TEST_ASSERT_EQ_UINT(a.x, b.x);
    WPE_TEST_ASSERT_EQ_UINT(a.z, b.z);
    WPE_TEST_ASSERT_EQ_UINT(0, a.layer);
    WPE_TEST_ASSERT_EQ_UINT(1, b.layer);
    WPE_TEST_ASSERT_NEAR(3.0f, a.ceiling, 1e-4);
    WPE_TEST_ASSERT(a.island != b.island);
    WPE_TEST_ASSERT_EQ_UINT(2, wpe_nav_grid_info(grid).max_layers);
    /* Points pick the nearer floor. */
    WPE_TEST_ASSERT_EQ_UINT(ground, wpe_nav_grid_find_cell(grid, wpe_vec3(0, 1.0f, 0), 5.0f));
    WPE_TEST_ASSERT_EQ_UINT(deck, wpe_nav_grid_find_cell(grid, wpe_vec3(0, 2.5f, 0), 5.0f));
    wpe_nav_grid_destroy(grid);
    wpe_nav_geometry_destroy(g);
}

static void test_nav_explicit_bounds_cut_the_region(void) {
    WPE_NavGeometry* g = new_geometry();
    add_floor(g);
    WPE_NavSettings s = wpe_nav_settings_default();
    s.bounds_min = wpe_vec3(0, -2, 0);
    s.bounds_max = wpe_vec3(2, 2, 2);
    WPE_NavGrid* grid = build(g, &s);
    const WPE_NavGridInfo info = wpe_nav_grid_info(grid);
    WPE_TEST_ASSERT_EQ_UINT(8, info.width);
    WPE_TEST_ASSERT_EQ_UINT(8, info.depth);
    WPE_TEST_ASSERT_EQ_UINT(4u * 4u, info.cell_count); /* The bounds' edge is an edge too. */
    WPE_TEST_ASSERT_EQ_UINT(WPE_NAV_CELL_NONE, cell_at(grid, -1, 0, 1));
    WPE_TEST_ASSERT(cell_at(grid, 1, 0, 1) != WPE_NAV_CELL_NONE);
    wpe_nav_grid_destroy(grid);
    wpe_nav_geometry_destroy(g);
}

/* A few levels' worth of shapes: the same input builds the same grid, cell for cell. */
static void test_nav_build_is_deterministic(void) {
    WPE_NavGeometry* g = new_geometry();
    add_floor(g);
    add_block(g, wpe_vec3(-4, 0, -4), wpe_vec3(-2, 0.25f, 4));
    add_block(g, wpe_vec3(1, 0, 1), wpe_vec3(3, 3, 3));
    const WPE_Mat4 tilted = wpe_mat4_from_trs(
        wpe_vec3(2, 0.5f, -2), wpe_quat_from_euler_xyz(wpe_vec3(0.3f, 0.7f, 0.2f)), wpe_vec3(1, 1, 1));
    WPE_TEST_ASSERT_OK(wpe_nav_geometry_add_box(g, wpe_vec3(0, 0, 0), wpe_vec3(1, 0.5f, 1), &tilted));
    WPE_NavGrid* a = build(g, NULL);
    WPE_NavGrid* b = build(g, NULL);
    const WPE_NavGridInfo ia = wpe_nav_grid_info(a), ib = wpe_nav_grid_info(b);
    WPE_TEST_ASSERT_EQ_UINT(ia.cell_count, ib.cell_count);
    WPE_TEST_ASSERT_EQ_UINT(ia.island_count, ib.island_count);
    for (uint32_t i = 0; i < ia.cell_count; ++i) {
        const WPE_NavCellInfo ca = info_of(a, i), cb = info_of(b, i);
        WPE_TEST_ASSERT(memcmp(&ca, &cb, sizeof ca) == 0);
        /* Links go both ways. */
        for (int dir = 0; dir < 4; ++dir) {
            if (ca.neighbors[dir] == WPE_NAV_CELL_NONE) continue;
            WPE_TEST_ASSERT_EQ_UINT(i, info_of(a, ca.neighbors[dir]).neighbors[(dir + 2) % 4]);
            WPE_TEST_ASSERT_EQ_UINT(ca.island, info_of(a, ca.neighbors[dir]).island);
        }
    }
    wpe_nav_grid_destroy(a);
    wpe_nav_grid_destroy(b);
    wpe_nav_geometry_destroy(g);
}

typedef struct LineCount {
    uint32_t lines;
    uint32_t colors[4];
    uint32_t color_count;
    float min_y, max_y;
} LineCount;

static void count_line(WPE_Vec3 a, WPE_Vec3 b, uint32_t color, void* user_data) {
    LineCount* c = user_data;
    ++c->lines;
    c->min_y = fminf(c->min_y, fminf(a.y, b.y));
    c->max_y = fmaxf(c->max_y, fmaxf(a.y, b.y));
    for (uint32_t i = 0; i < c->color_count; ++i) {
        if (c->colors[i] == color) return;
    }
    if (c->color_count < 4) c->colors[c->color_count++] = color;
}

static void test_nav_debug_draw_outlines_and_grids(void) {
    WPE_NavGeometry* g = new_geometry();
    add_floor(g);
    WPE_NavGrid* grid = build(g, NULL);
    LineCount count = {.min_y = INFINITY, .max_y = -INFINITY};
    wpe_nav_grid_debug_draw(grid, count_line, &count);
    /* The 36 x 36 cells: 4 * 36 outline pieces, and inner lines on every 4th column boundary (9 per
     * axis, 36 pieces each). */
    WPE_TEST_ASSERT_EQ_UINT(4u * 36u + 2u * 9u * 36u, count.lines);
    WPE_TEST_ASSERT_EQ_UINT(2, count.color_count);             /* Outline and dimmer grid of one island. */
    WPE_TEST_ASSERT(count.min_y > 0.0f && count.max_y < 0.1f); /* Just above the floor. */
    wpe_nav_grid_debug_draw(grid, NULL, NULL);
    wpe_nav_grid_debug_draw(NULL, count_line, &count);
    wpe_nav_grid_destroy(grid);
    wpe_nav_geometry_destroy(g);
}

/* ---- From a world's colliders ---------------------------------------------------------------------------
 */

typedef struct World {
    WPE_TestAssets assets;
    WPE_Ecs* ecs;
    WPE_Physics* physics;
} World;

static World make_world(void) {
    World w = {.assets = wpe_test_assets_create(NULL, 0)};
    WPE_TEST_ASSERT_OK(wpe_ecs_create(&w.ecs));
    WPE_TEST_ASSERT_OK(wpe_scene_register_components(w.ecs));
    WPE_TEST_ASSERT_OK(wpe_physics_create(
        &(WPE_PhysicsDesc){.ecs = w.ecs, .assets = w.assets.assets, .gravity = wpe_vec3(0, -9.81f, 0)},
        &w.physics));
    return w;
}

static void destroy_world(World* w) {
    wpe_physics_destroy(w->physics);
    wpe_ecs_destroy(w->ecs);
    wpe_test_assets_destroy(&w->assets);
}

static WPE_Entity spawn_collider(World* w, WPE_Vec3 position, WPE_Quat rotation, WPE_Vec3 scale,
                                 WPE_Collider collider) {
    const WPE_Entity e = wpe_ecs_spawn(w->ecs);
    WPE_Transform t = wpe_transform_identity();
    t.position = position;
    t.rotation = rotation;
    t.scale = scale;
    WPE_TEST_ASSERT_OK(wpe_ecs_add_component(w->ecs, e, WPE_COMP_TRANSFORM, &t, NULL));
    WPE_TEST_ASSERT_OK(wpe_ecs_add_component(w->ecs, e, WPE_COMP_PHYSICS_COLLIDER, &collider, NULL));
    return e;
}

static WPE_Collider box_collider(WPE_Vec3 size) {
    WPE_Collider c = wpe_collider_default(WPE_COLLIDER_BOX);
    c.size = size;
    return c;
}

static void add_body(World* w, WPE_Entity e, WPE_BodyType type) {
    const WPE_RigidBody body = wpe_rigidbody_default(type);
    WPE_TEST_ASSERT_OK(wpe_ecs_add_component(w->ecs, e, WPE_COMP_PHYSICS_RIGIDBODY, &body, NULL));
}

static void test_nav_world_colliders_are_level_geometry(void) {
    World w = make_world();
    const WPE_Vec3 one = wpe_vec3(1, 1, 1);
    /* The ground: a 1 x 1 x 1 box scaled to 10 x 1 x 10, top at y = 0. */
    spawn_collider(&w, wpe_vec3(0, -0.5f, 0), wpe_quat_identity(), wpe_vec3(10, 1, 10), box_collider(one));
    /* A static pillar (rigid body type static) in one corner. */
    const WPE_Entity pillar =
        spawn_collider(&w, wpe_vec3(3, 1, 3), wpe_quat_identity(), one, box_collider(wpe_vec3(1, 2, 1)));
    add_body(&w, pillar, WPE_BODY_STATIC);
    /* Moving things and triggers are not level geometry. */
    const WPE_Entity crate =
        spawn_collider(&w, wpe_vec3(-3, 0.5f, -3), wpe_quat_identity(), one, box_collider(one));
    add_body(&w, crate, WPE_BODY_DYNAMIC);
    const WPE_Entity door =
        spawn_collider(&w, wpe_vec3(-3, 1, 3), wpe_quat_identity(), one, box_collider(wpe_vec3(1, 2, 1)));
    add_body(&w, door, WPE_BODY_KINEMATIC);
    WPE_Collider trigger = box_collider(wpe_vec3(2, 2, 2));
    trigger.is_trigger = true;
    spawn_collider(&w, wpe_vec3(3, 1, -3), wpe_quat_identity(), one, trigger);
    /* A sphere boulder in the middle, offset up by its collider offset. */
    WPE_Collider boulder = wpe_collider_default(WPE_COLLIDER_SPHERE);
    boulder.size.x = 0.5f;
    boulder.offset = wpe_vec3(0, 0.25f, 0);
    spawn_collider(&w, wpe_vec3(0, 0, 0), wpe_quat_identity(), wpe_vec3(2, 2, 2), boulder); /* Radius 1. */

    WPE_NavGeometry* g = new_geometry();
    uint32_t added = 0;
    WPE_TEST_ASSERT_OK(wpe_nav_geometry_add_colliders(g, w.ecs, w.assets.assets, &added));
    WPE_TEST_ASSERT_EQ_UINT(3, added);
    WPE_NavGrid* grid = build(g, NULL);
    WPE_TEST_ASSERT(cell_at(grid, -3, 0, -3) != WPE_NAV_CELL_NONE);        /* Under the crate. */
    WPE_TEST_ASSERT(cell_at(grid, -3, 0, 3) != WPE_NAV_CELL_NONE);         /* Under the door. */
    WPE_TEST_ASSERT(cell_at(grid, 3, 0, -3) != WPE_NAV_CELL_NONE);         /* In the trigger. */
    WPE_TEST_ASSERT_EQ_UINT(WPE_NAV_CELL_NONE, cell_at(grid, 3, 0, 3));    /* The pillar. */
    WPE_TEST_ASSERT_EQ_UINT(WPE_NAV_CELL_NONE, cell_at(grid, 2.2f, 0, 3)); /* Next to it. */
    /* The boulder (center 0.5 up, radius 1) blocks its footprint plus the radius... */
    WPE_TEST_ASSERT_EQ_UINT(WPE_NAV_CELL_NONE, cell_at(grid, 0, 0, 0));
    WPE_TEST_ASSERT_EQ_UINT(WPE_NAV_CELL_NONE, cell_at(grid, 1.1f, 0, 0));
    WPE_TEST_ASSERT(cell_at(grid, 1.8f, 0, 0) != WPE_NAV_CELL_NONE);
    /* ...and the flat enough top of the sphere (1.4 m across) is a little island of its own. */
    const uint32_t ground = island_at(grid, -3, 0, -3);
    WPE_TEST_ASSERT_EQ_UINT(ground, island_at(grid, 3, 0, -3));
    WPE_TEST_ASSERT_EQ_UINT(ground, island_at(grid, 1.8f, 0, 0));
    const uint32_t top = wpe_nav_grid_find_cell(grid, wpe_vec3(0.1f, 1.5f, 0.1f), 0.2f);
    WPE_TEST_ASSERT(top != WPE_NAV_CELL_NONE);
    WPE_TEST_ASSERT(info_of(grid, top).island != ground);
    WPE_TEST_ASSERT_EQ_UINT(2,
                            wpe_nav_grid_info(grid).island_count); /* The pillar's top (1 m) is too small. */
    wpe_nav_grid_destroy(grid);
    wpe_nav_geometry_destroy(g);

    /* The same through the one-call builder. */
    WPE_TEST_ASSERT_OK(wpe_nav_grid_build_from_world(w.ecs, w.assets.assets, NULL, &grid));
    WPE_TEST_ASSERT(cell_at(grid, -3, 0, -3) != WPE_NAV_CELL_NONE);
    wpe_nav_grid_destroy(grid);
    destroy_world(&w);
}

static void test_nav_world_rotated_capsule_and_mesh_colliders(void) {
    World w = make_world();
    /* The floor is a mesh collider: a 10 m plane model. */
    WPE_Model* plane = NULL;
    WPE_TEST_ASSERT_OK(wpe_model_create_plane(10.0f, 2, NULL, &plane));
    const WPE_AssetHandle model = wpe_test_add_model(w.assets.assets, "plane", plane);
    WPE_Collider mesh = wpe_collider_default(WPE_COLLIDER_MESH);
    mesh.model = model;
    spawn_collider(&w, wpe_vec3(0, 0, 0), wpe_quat_identity(), wpe_vec3(1, 1, 1), mesh);
    /* A capsule log lying along X (rotated 90 degrees about Z), 0.5 thick and 4 long. */
    WPE_Collider log = wpe_collider_default(WPE_COLLIDER_CAPSULE);
    log.size = wpe_vec3(0.25f, 4.0f, 0);
    spawn_collider(&w, wpe_vec3(0, 0.25f, 2), wpe_quat_from_euler_xyz(wpe_vec3(0, 0, 90.0f * DEG)),
                   wpe_vec3(1, 1, 1), log);
    /* A mesh collider whose mesh index doesn't exist is skipped with a warning. */
    WPE_Collider broken = mesh;
    broken.mesh = 7;
    spawn_collider(&w, wpe_vec3(0, 5, 0), wpe_quat_identity(), wpe_vec3(1, 1, 1), broken);

    WPE_NavGeometry* g = new_geometry();
    uint32_t added = 0;
    WPE_TEST_ASSERT_OK(wpe_nav_geometry_add_colliders(g, w.ecs, w.assets.assets, &added));
    WPE_TEST_ASSERT_EQ_UINT(2, added);
    WPE_NavGrid* grid = build(g, NULL);
    WPE_TEST_ASSERT(cell_at(grid, 0, 0, -2) != WPE_NAV_CELL_NONE);
    /* The log is 0.5 tall: a step too high (0.3), so it blocks, along its length but not past its ends. */
    WPE_TEST_ASSERT_EQ_UINT(WPE_NAV_CELL_NONE, cell_at(grid, 1.5f, 0, 2));
    WPE_TEST_ASSERT_EQ_UINT(WPE_NAV_CELL_NONE, cell_at(grid, -1.5f, 0, 2.5f));
    WPE_TEST_ASSERT(cell_at(grid, 3.5f, 0, 2) != WPE_NAV_CELL_NONE);
    wpe_nav_grid_destroy(grid);
    wpe_nav_geometry_destroy(g);

    /* Without the asset manager, the mesh floor can't be added. */
    g = new_geometry();
    WPE_TEST_ASSERT_OK(wpe_nav_geometry_add_colliders(g, w.ecs, NULL, &added));
    WPE_TEST_ASSERT_EQ_UINT(1, added);
    wpe_nav_geometry_destroy(g);

    wpe_asset_release(w.assets.assets, model);
    destroy_world(&w);
}

static void test_nav_world_needs_physics_components(void) {
    WPE_Ecs* ecs = NULL;
    WPE_TEST_ASSERT_OK(wpe_ecs_create(&ecs));
    WPE_TEST_ASSERT_OK(wpe_scene_register_components(ecs));
    WPE_NavGeometry* g = new_geometry();
    WPE_TEST_ASSERT_EQ_INT(WPE_RESULT_ERROR_INVALID_STATE,
                           wpe_nav_geometry_add_colliders(g, ecs, NULL, NULL));
    WPE_TEST_ASSERT_EQ_INT(WPE_RESULT_ERROR_NULL_ARG, wpe_nav_geometry_add_colliders(g, NULL, NULL, NULL));
    WPE_NavGrid* grid = NULL;
    WPE_TEST_ASSERT_EQ_INT(WPE_RESULT_ERROR_INVALID_STATE,
                           wpe_nav_grid_build_from_world(ecs, NULL, NULL, &grid));
    wpe_nav_geometry_destroy(g);
    wpe_ecs_destroy(ecs);

    /* A world with physics but no colliders has nothing to build from. */
    World w = make_world();
    WPE_TEST_ASSERT_EQ_INT(WPE_RESULT_ERROR_NOT_FOUND,
                           wpe_nav_grid_build_from_world(w.ecs, w.assets.assets, NULL, &grid));
    WPE_TEST_ASSERT(grid == NULL);
    destroy_world(&w);
}

WPE_TEST_MAIN(WPE_TEST_CASE(test_nav_settings_are_validated),
              WPE_TEST_CASE(test_nav_geometry_validates_triangles),
              WPE_TEST_CASE(test_nav_build_needs_geometry),
              WPE_TEST_CASE(test_nav_flat_floor_is_one_island_kept_off_its_edges),
              WPE_TEST_CASE(test_nav_walls_split_and_doorways_join),
              WPE_TEST_CASE(test_nav_steps_up_to_step_height), WPE_TEST_CASE(test_nav_slopes_up_to_max_slope),
              WPE_TEST_CASE(test_nav_headroom_and_stacked_floors),
              WPE_TEST_CASE(test_nav_explicit_bounds_cut_the_region),
              WPE_TEST_CASE(test_nav_build_is_deterministic),
              WPE_TEST_CASE(test_nav_debug_draw_outlines_and_grids),
              WPE_TEST_CASE(test_nav_world_colliders_are_level_geometry),
              WPE_TEST_CASE(test_nav_world_rotated_capsule_and_mesh_colliders),
              WPE_TEST_CASE(test_nav_world_needs_physics_components))
