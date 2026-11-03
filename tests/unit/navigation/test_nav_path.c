#include "wpe_test.h"

#include <math.h>
#include <string.h>

#include "core/alloc.h"
#include "navigation/nav_path.h"
#include "navigation/navigation.h"

/* ---- Levels ------------------------------------------------------------------------------------------- */

static void add_block(WPE_NavGeometry* g, WPE_Vec3 min, WPE_Vec3 max) {
    WPE_TEST_ASSERT_OK(wpe_nav_geometry_add_box(g, wpe_vec3_scale(wpe_vec3_add(min, max), 0.5f),
                                                wpe_vec3_scale(wpe_vec3_sub(max, min), 0.5f), NULL));
}

static WPE_NavGrid* build(WPE_NavGeometry* g) {
    WPE_NavGrid* grid = NULL;
    WPE_TEST_ASSERT_OK(wpe_nav_grid_build(g, NULL, &grid));
    wpe_nav_geometry_destroy(g);
    return grid;
}

/* A 10 x 10 floor (top at y = 0) with nothing on it. */
static WPE_NavGrid* open_floor(void) {
    WPE_NavGeometry* g = NULL;
    WPE_TEST_ASSERT_OK(wpe_nav_geometry_create(&g));
    add_block(g, wpe_vec3(-5, -1, -5), wpe_vec3(5, 0, 5));
    return build(g);
}

/* Two rooms split by a wall at x = 0, with a 2 m doorway at z = 3..5 (the +Z end). */
static WPE_NavGrid* two_rooms(bool doorway) {
    WPE_NavGeometry* g = NULL;
    WPE_TEST_ASSERT_OK(wpe_nav_geometry_create(&g));
    add_block(g, wpe_vec3(-5, -1, -5), wpe_vec3(5, 0, 5));
    add_block(g, wpe_vec3(-0.25f, 0, -5), wpe_vec3(0.25f, 2, doorway ? 3.0f : 5.0f));
    return build(g);
}

/* A ground floor and, at x = 2..5, a platform 1 m up, reached by a ramp (x = -2..2, z = -1..1). */
static WPE_NavGrid* ramp_level(void) {
    WPE_NavGeometry* g = NULL;
    WPE_TEST_ASSERT_OK(wpe_nav_geometry_create(&g));
    add_block(g, wpe_vec3(-5, -1, -5), wpe_vec3(5, 0, 5));
    add_block(g, wpe_vec3(2, 0, -5), wpe_vec3(5, 1, 5));
    const WPE_Vec3 v[4] = {wpe_vec3(-2, 0, -1), wpe_vec3(-2, 0, 1), wpe_vec3(2, 1, 1), wpe_vec3(2, 1, -1)};
    const uint32_t up[6] = {0, 1, 2, 0, 2, 3};
    WPE_TEST_ASSERT_OK(wpe_nav_geometry_add_triangles(g, v, 4, up, 6, NULL));
    return build(g);
}

static WPE_NavQuery* query_for(const WPE_NavGrid* grid) {
    WPE_NavQuery* q = NULL;
    WPE_TEST_ASSERT_OK(wpe_nav_query_create(grid, &q));
    WPE_TEST_ASSERT(wpe_nav_query_grid(q) == grid);
    return q;
}

/* Every segment of the path is walkable in a straight line, and the path ends where it says. */
static void expect_walkable(const WPE_NavGrid* grid, const WPE_NavPath* path) {
    WPE_TEST_ASSERT(path->count >= 1);
    for (uint32_t i = 1; i < path->count; ++i)
        WPE_TEST_ASSERT_MSG(wpe_nav_grid_line_walkable(grid, path->points[i - 1], path->points[i]),
                            "segment %u of %u is blocked", i, path->count);
}

static float distance_xz(WPE_Vec3 a, WPE_Vec3 b) {
    return sqrtf((a.x - b.x) * (a.x - b.x) + (a.z - b.z) * (a.z - b.z));
}

/* ---- Points and lines ------------------------------------------------------------------------------ */

static void test_nav_nearest_point(void) {
    WPE_NavGrid* grid = open_floor();
    WPE_Vec3 p;
    /* On the floor: the point itself, put on the floor. */
    uint32_t cell = wpe_nav_grid_nearest(grid, wpe_vec3(1.1f, 0.3f, -2.2f), 1.0f, 1.0f, &p);
    WPE_TEST_ASSERT(cell != WPE_NAV_CELL_NONE);
    WPE_TEST_ASSERT_EQ_UINT(wpe_nav_grid_find_cell(grid, wpe_vec3(1.1f, 0, -2.2f), 0.1f), cell);
    WPE_TEST_ASSERT_NEAR(1.1f, p.x, 1e-5);
    WPE_TEST_ASSERT_NEAR(0.0f, p.y, 1e-4);
    WPE_TEST_ASSERT_NEAR(-2.2f, p.z, 1e-5);
    /* Near the eroded edge (walkable from x = -4.5): moved inside. */
    cell = wpe_nav_grid_nearest(grid, wpe_vec3(-4.9f, 0, 0.3f), 1.0f, 1.0f, &p);
    WPE_TEST_ASSERT(cell != WPE_NAV_CELL_NONE);
    WPE_TEST_ASSERT_NEAR(-4.5f, p.x, 1e-3);
    WPE_TEST_ASSERT_NEAR(0.3f, p.z, 1e-5);
    /* Off the floor altogether, within the radius, and beyond it. */
    WPE_TEST_ASSERT(wpe_nav_grid_nearest(grid, wpe_vec3(-5.3f, 0, 0), 1.0f, 1.0f, NULL) != WPE_NAV_CELL_NONE);
    WPE_TEST_ASSERT_EQ_UINT(WPE_NAV_CELL_NONE,
                            wpe_nav_grid_nearest(grid, wpe_vec3(-6.0f, 0, 0), 1.0f, 1.0f, NULL));
    WPE_TEST_ASSERT_EQ_UINT(WPE_NAV_CELL_NONE,
                            wpe_nav_grid_nearest(grid, wpe_vec3(0, 2.0f, 0), 1.0f, 1.0f, NULL));
    /* Bad input. */
    WPE_TEST_ASSERT_EQ_UINT(WPE_NAV_CELL_NONE,
                            wpe_nav_grid_nearest(NULL, wpe_vec3(0, 0, 0), 1.0f, 1.0f, NULL));
    WPE_TEST_ASSERT_EQ_UINT(WPE_NAV_CELL_NONE,
                            wpe_nav_grid_nearest(grid, wpe_vec3(NAN, 0, 0), 1.0f, 1.0f, NULL));
    WPE_TEST_ASSERT_EQ_UINT(WPE_NAV_CELL_NONE,
                            wpe_nav_grid_nearest(grid, wpe_vec3(0, 0, 0), -1.0f, 1.0f, NULL));
    wpe_nav_grid_destroy(grid);
}

static void test_nav_reachability_and_lines(void) {
    WPE_NavGrid* closed = two_rooms(false);
    const uint32_t left = wpe_nav_grid_find_cell(closed, wpe_vec3(-2, 0, 0), 0.1f);
    const uint32_t left2 = wpe_nav_grid_find_cell(closed, wpe_vec3(-3, 0, 3), 0.1f);
    const uint32_t right = wpe_nav_grid_find_cell(closed, wpe_vec3(2, 0, 0), 0.1f);
    WPE_TEST_ASSERT(wpe_nav_grid_reachable(closed, left, left2));
    WPE_TEST_ASSERT(!wpe_nav_grid_reachable(closed, left, right));
    WPE_TEST_ASSERT(!wpe_nav_grid_reachable(closed, left, 1u << 30));
    WPE_TEST_ASSERT(!wpe_nav_grid_reachable(NULL, left, left));
    WPE_TEST_ASSERT(!wpe_nav_grid_line_walkable(closed, wpe_vec3(-2, 0, 0), wpe_vec3(2, 0, 0)));
    wpe_nav_grid_destroy(closed);

    WPE_NavGrid* grid = two_rooms(true);
    WPE_TEST_ASSERT(
        wpe_nav_grid_line_walkable(grid, wpe_vec3(-2, 0, -2), wpe_vec3(-3, 0, 2))); /* In a room. */
    WPE_TEST_ASSERT(
        wpe_nav_grid_line_walkable(grid, wpe_vec3(-3, 0, 4), wpe_vec3(3, 0, 4))); /* The doorway. */
    WPE_TEST_ASSERT(!wpe_nav_grid_line_walkable(grid, wpe_vec3(-3, 0, 2), wpe_vec3(3, 0, 2))); /* The wall. */
    WPE_TEST_ASSERT(
        !wpe_nav_grid_line_walkable(grid, wpe_vec3(-3, 0, -4), wpe_vec3(3, 0, 4.5f)));       /* Cuts it. */
    WPE_TEST_ASSERT(wpe_nav_grid_line_walkable(grid, wpe_vec3(1, 0, 1), wpe_vec3(1, 0, 1))); /* No length. */
    /* Exactly through cell corners (a 45-degree line) on open floor. */
    WPE_TEST_ASSERT(wpe_nav_grid_line_walkable(grid, wpe_vec3(1.0f, 0, -4.0f), wpe_vec3(4.0f, 0, -1.0f)));
    /* Points not on the grid. */
    WPE_TEST_ASSERT(!wpe_nav_grid_line_walkable(grid, wpe_vec3(-2, 3, 0), wpe_vec3(-3, 0, 0)));
    WPE_TEST_ASSERT(!wpe_nav_grid_line_walkable(grid, wpe_vec3(-2, 0, 0), wpe_vec3(-9, 0, 0)));
    WPE_TEST_ASSERT(!wpe_nav_grid_line_walkable(NULL, wpe_vec3(-2, 0, 0), wpe_vec3(-3, 0, 0)));
    wpe_nav_grid_destroy(grid);

    /* Up a ramp onto a platform, and not up its 1 m side. */
    grid = ramp_level();
    WPE_TEST_ASSERT(wpe_nav_grid_line_walkable(grid, wpe_vec3(-4, 0, 0), wpe_vec3(4, 1, 0)));
    WPE_TEST_ASSERT(!wpe_nav_grid_line_walkable(grid, wpe_vec3(-4, 0, 3), wpe_vec3(4, 1, 3)));
    wpe_nav_grid_destroy(grid);
}

/* ---- Paths ------------------------------------------------------------------------------------------- */

static void test_nav_path_on_open_floor_is_straight(void) {
    WPE_NavGrid* grid = open_floor();
    WPE_NavQuery* q = query_for(grid);
    WPE_NavPath path;
    const WPE_Vec3 a = wpe_vec3(-3.3f, 0, -2.1f), b = wpe_vec3(3.7f, 0, 2.9f);
    WPE_TEST_ASSERT_OK(wpe_nav_find_path(q, a, b, NULL, &path));
    WPE_TEST_ASSERT_EQ_UINT(2, path.count);
    WPE_TEST_ASSERT(!path.partial);
    WPE_TEST_ASSERT_NEAR(distance_xz(a, b), path.length, 1e-3);
    WPE_TEST_ASSERT_NEAR(a.x, path.points[0].x, 1e-5);
    WPE_TEST_ASSERT_NEAR(b.z, path.points[1].z, 1e-5);
    WPE_TEST_ASSERT(path.visited > 0);

    /* Raw: the cells A* went through, longer than the pulled path but not by much (8 directions). */
    WPE_NavPathOptions raw = wpe_nav_path_options_default();
    raw.raw = true;
    WPE_NavPath zigzag;
    WPE_TEST_ASSERT_OK(wpe_nav_find_path(q, a, b, &raw, &zigzag));
    WPE_TEST_ASSERT(zigzag.count > 10);
    WPE_TEST_ASSERT(zigzag.length >= distance_xz(a, b) - 1e-3f && zigzag.length < distance_xz(a, b) * 1.1f);
    expect_walkable(grid, &zigzag);

    /* Start and goal in one cell; then at the same point. */
    WPE_TEST_ASSERT_OK(wpe_nav_find_path(q, wpe_vec3(0.05f, 0, 0.05f), wpe_vec3(0.2f, 0, 0.2f), NULL, &path));
    WPE_TEST_ASSERT_EQ_UINT(2, path.count);
    WPE_TEST_ASSERT_OK(wpe_nav_find_path(q, wpe_vec3(0.1f, 0, 0.1f), wpe_vec3(0.1f, 0, 0.1f), NULL, &path));
    WPE_TEST_ASSERT_EQ_UINT(1, path.count);
    WPE_TEST_ASSERT_NEAR(0.0f, path.length, 1e-6);
    wpe_nav_query_destroy(q);
    wpe_nav_grid_destroy(grid);
}

static void test_nav_path_goes_through_the_doorway(void) {
    WPE_NavGrid* grid = two_rooms(true);
    WPE_NavQuery* q = query_for(grid);
    WPE_NavPath path;
    const WPE_Vec3 a = wpe_vec3(-3, 0, -3), b = wpe_vec3(3, 0, -3);
    WPE_TEST_ASSERT_OK(wpe_nav_find_path(q, a, b, NULL, &path));
    expect_walkable(grid, &path);
    /* Up to the doorway and back down: a few corners (on cell centers), through the doorway past the wall's
     * end at (±0.25, 3), with the agent's radius kept clear of it. */
    WPE_TEST_ASSERT(path.count >= 4 && path.count <= 7);
    bool through = false;
    for (uint32_t i = 1; i + 1 < path.count; ++i) {
        through |= path.points[i].z > 3.3f && fabsf(path.points[i].x) < 1.0f;
        WPE_TEST_ASSERT(path.points[i].z > 0.0f && path.points[i].z < 4.0f);
    }
    WPE_TEST_ASSERT(through);
    /* Nearly the shortest way: two legs from the corners (0.6 off the wall, z 3.4) to the ends. */
    const float shortest = 2.0f * sqrtf(2.4f * 2.4f + 6.4f * 6.4f) + 1.2f;
    WPE_TEST_ASSERT(path.length > 14.0f && path.length < shortest * 1.03f);
    /* The same search again gives the same path, and doesn't allocate. */
    WPE_AllocStats before, after;
    WPE_Vec3 first[8];
    memcpy(first, path.points, path.count * sizeof first[0]);
    wpe_alloc_get_stats(&before);
    WPE_TEST_ASSERT_OK(wpe_nav_find_path(q, a, b, NULL, &path));
    wpe_alloc_get_stats(&after);
    WPE_TEST_ASSERT_EQ_UINT(before.total_allocations, after.total_allocations);
    WPE_TEST_ASSERT(memcmp(first, path.points, path.count * sizeof first[0]) == 0);
    wpe_nav_query_destroy(q);
    wpe_nav_grid_destroy(grid);
}

static void test_nav_path_climbs_ramps_and_stacked_floors(void) {
    WPE_NavGrid* grid = ramp_level();
    WPE_NavQuery* q = query_for(grid);
    WPE_NavPath path;
    /* From the ground behind the platform's side onto it: around to the ramp, up, along the top. */
    WPE_TEST_ASSERT_OK(wpe_nav_find_path(q, wpe_vec3(0, 0, 4), wpe_vec3(4, 1, 4), NULL, &path));
    expect_walkable(grid, &path);
    WPE_TEST_ASSERT_NEAR(1.0f, path.points[path.count - 1].y, 0.11);
    bool on_ramp = false;
    for (uint32_t i = 0; i < path.count; ++i)
        on_ramp |= fabsf(path.points[i].z) < 1.0f && path.points[i].x > -2.0f && path.points[i].x < 2.5f;
    WPE_TEST_ASSERT(on_ramp);
    WPE_TEST_ASSERT(path.length > 8.0f);
    wpe_nav_query_destroy(q);
    wpe_nav_grid_destroy(grid);

    /* Under a bridge 3 m up: the floor's path is not blocked by the deck above. */
    WPE_NavGeometry* g = NULL;
    WPE_TEST_ASSERT_OK(wpe_nav_geometry_create(&g));
    add_block(g, wpe_vec3(-5, -1, -5), wpe_vec3(5, 0, 5));
    add_block(g, wpe_vec3(-1, 3, -5), wpe_vec3(1, 3.2f, 5));
    grid = build(g);
    q = query_for(grid);
    WPE_TEST_ASSERT_OK(wpe_nav_find_path(q, wpe_vec3(-3, 0, 0), wpe_vec3(3, 0, 0), NULL, &path));
    WPE_TEST_ASSERT_EQ_UINT(2, path.count);
    /* The deck is its own island: a goal on it is out of reach. */
    WPE_TEST_ASSERT_EQ_INT(WPE_RESULT_ERROR_NOT_FOUND,
                           wpe_nav_find_path(q, wpe_vec3(-3, 0, 0), wpe_vec3(0, 3.2f, 0), NULL, &path));
    wpe_nav_query_destroy(q);
    wpe_nav_grid_destroy(grid);
}

static void test_nav_path_failures_and_partial_paths(void) {
    WPE_NavGrid* grid = two_rooms(false);
    WPE_NavQuery* q = query_for(grid);
    WPE_NavPath path;
    const WPE_Vec3 a = wpe_vec3(-3, 0, 0), b = wpe_vec3(3, 0, 1);
    WPE_TEST_ASSERT_EQ_INT(WPE_RESULT_ERROR_NOT_FOUND, wpe_nav_find_path(q, a, b, NULL, &path));
    WPE_TEST_ASSERT_EQ_UINT(0, path.count);
    /* Partial: as near as the left room gets, at the wall. */
    WPE_NavPathOptions o = wpe_nav_path_options_default();
    o.allow_partial = true;
    WPE_TEST_ASSERT_OK(wpe_nav_find_path(q, a, b, &o, &path));
    WPE_TEST_ASSERT(path.partial);
    const WPE_Vec3 end = path.points[path.count - 1];
    WPE_TEST_ASSERT(end.x < -0.5f && end.x > -1.0f);
    WPE_TEST_ASSERT_NEAR(1.0f, end.z, 0.15);
    expect_walkable(grid, &path);
    WPE_TEST_ASSERT(!wpe_nav_grid_reachable(grid, path.end_cell, wpe_nav_grid_find_cell(grid, b, 0.1f)));

    /* A search allowed too few cells. */
    o = wpe_nav_path_options_default();
    o.max_visited = 10;
    WPE_TEST_ASSERT_EQ_INT(WPE_RESULT_ERROR_NOT_FOUND,
                           wpe_nav_find_path(q, wpe_vec3(-3, 0, -3), wpe_vec3(-3, 0, 3), &o, &path));
    o.allow_partial = true;
    WPE_TEST_ASSERT_OK(wpe_nav_find_path(q, wpe_vec3(-3, 0, -3), wpe_vec3(-3, 0, 3), &o, &path));
    WPE_TEST_ASSERT(path.partial && path.visited == 10);

    /* Off the grid: within the search radius it starts at the nearest point, beyond it fails. */
    WPE_TEST_ASSERT_OK(wpe_nav_find_path(q, wpe_vec3(-5.2f, 0, 0), wpe_vec3(-3, 0, 0), NULL, &path));
    WPE_TEST_ASSERT_NEAR(-4.5f, path.points[0].x, 1e-3);
    WPE_TEST_ASSERT_EQ_INT(WPE_RESULT_ERROR_NOT_FOUND,
                           wpe_nav_find_path(q, wpe_vec3(-8, 0, 0), wpe_vec3(-3, 0, 0), NULL, &path));
    o = wpe_nav_path_options_default();
    o.search_radius = 4.0f;
    WPE_TEST_ASSERT_OK(wpe_nav_find_path(q, wpe_vec3(-8, 0, 0), wpe_vec3(-3, 0, 0), &o, &path));
    /* Bad arguments. */
    WPE_TEST_ASSERT_EQ_INT(WPE_RESULT_ERROR_INVALID_ARG,
                           wpe_nav_find_path(q, wpe_vec3(NAN, 0, 0), wpe_vec3(-3, 0, 0), NULL, &path));
    o.search_radius = -1.0f;
    WPE_TEST_ASSERT_EQ_INT(WPE_RESULT_ERROR_INVALID_ARG, wpe_nav_find_path(q, a, a, &o, &path));
    WPE_TEST_ASSERT_EQ_INT(WPE_RESULT_ERROR_NULL_ARG, wpe_nav_find_path(NULL, a, a, NULL, &path));
    WPE_TEST_ASSERT_EQ_INT(WPE_RESULT_ERROR_NULL_ARG, wpe_nav_find_path(q, a, a, NULL, NULL));
    WPE_NavQuery* none = NULL;
    WPE_TEST_ASSERT_EQ_INT(WPE_RESULT_ERROR_NULL_ARG, wpe_nav_query_create(NULL, &none));
    wpe_nav_query_destroy(NULL);
    wpe_nav_query_destroy(q);
    wpe_nav_grid_destroy(grid);
}

/* A character walking along a path at 0.1 m steps stays on the grid and arrives. */
static void test_nav_path_can_be_walked(void) {
    WPE_NavGrid* grid = two_rooms(true);
    WPE_NavQuery* q = query_for(grid);
    WPE_NavPath path;
    const WPE_Vec3 goal = wpe_vec3(4, 0, -4);
    WPE_TEST_ASSERT_OK(wpe_nav_find_path(q, wpe_vec3(-4, 0, -4), goal, NULL, &path));
    WPE_Vec3 at = path.points[0];
    uint32_t next = 1, steps = 0;
    while (next < path.count && steps < 1000) {
        const WPE_Vec3 d = wpe_vec3_sub(path.points[next], at);
        const float left = wpe_vec3_length(d);
        if (left <= 0.1f) {
            at = path.points[next++];
            continue;
        }
        at = wpe_vec3_add(at, wpe_vec3_scale(d, 0.1f / left));
        ++steps;
        WPE_TEST_ASSERT_MSG(wpe_nav_grid_find_cell(grid, at, 0.5f) != WPE_NAV_CELL_NONE,
                            "off the grid at %.2f %.2f", (double)at.x, (double)at.z);
    }
    WPE_TEST_ASSERT(distance_xz(at, goal) < 1e-3f);
    wpe_nav_query_destroy(q);
    wpe_nav_grid_destroy(grid);
}

/* ---- Flow fields -------------------------------------------------------------------------------------- */

static void test_nav_flow_field_leads_to_the_goal(void) {
    WPE_NavGrid* grid = two_rooms(true);
    WPE_NavFlowField* field = NULL;
    WPE_TEST_ASSERT_OK(wpe_nav_flow_create(grid, &field));
    const WPE_Vec3 goal = wpe_vec3(3, 0, -3);
    WPE_TEST_ASSERT_OK(wpe_nav_flow_build(field, &goal, 1, 0.0f, NULL));
    const uint32_t goal_cell = wpe_nav_grid_find_cell(grid, goal, 0.1f);
    WPE_TEST_ASSERT_NEAR(0.0f, wpe_nav_flow_distance(field, goal_cell), 1e-6);
    WPE_TEST_ASSERT_EQ_UINT(goal_cell, wpe_nav_flow_next(field, goal_cell));

    /* From the other room: about as far as the path, and the chain of next cells ends at the goal. */
    WPE_NavQuery* q = query_for(grid);
    WPE_NavPath path;
    const WPE_Vec3 start = wpe_vec3(-3, 0, -3);
    WPE_TEST_ASSERT_OK(wpe_nav_find_path(q, start, goal, NULL, &path));
    const uint32_t start_cell = wpe_nav_grid_find_cell(grid, start, 0.1f);
    const float distance = wpe_nav_flow_distance(field, start_cell);
    WPE_TEST_ASSERT(distance >= path.length - 0.3f && distance < path.length * 1.1f);
    uint32_t cell = start_cell, hops = 0;
    while (cell != goal_cell && hops < 10000) {
        const uint32_t next = wpe_nav_flow_next(field, cell);
        WPE_TEST_ASSERT(wpe_nav_flow_distance(field, next) < wpe_nav_flow_distance(field, cell));
        cell = next;
        ++hops;
    }
    WPE_TEST_ASSERT_EQ_UINT(goal_cell, cell);

    /* Walking the field's directions arrives too, staying on the grid. */
    WPE_Vec3 at = start, dir;
    float left = 0.0f;
    for (int i = 0; i < 500 && distance_xz(at, goal) > 0.06f; ++i) {
        WPE_TEST_ASSERT(wpe_nav_flow_direction(field, at, &dir, &left));
        WPE_TEST_ASSERT_NEAR(0.0f, dir.y, 1e-6);
        at = wpe_vec3_add(at, wpe_vec3_scale(dir, fminf(0.1f, distance_xz(at, goal))));
        WPE_TEST_ASSERT(wpe_nav_grid_find_cell(grid, at, 0.5f) != WPE_NAV_CELL_NONE);
    }
    WPE_TEST_ASSERT(distance_xz(at, goal) <= 0.06f);
    WPE_TEST_ASSERT(wpe_nav_flow_direction(field, goal, &dir, &left));
    WPE_TEST_ASSERT_NEAR(0.0f, wpe_vec3_length(dir), 1e-6); /* At the goal: stand still. */
    WPE_TEST_ASSERT_NEAR(0.0f, left, 1e-6);
    WPE_TEST_ASSERT(!wpe_nav_flow_direction(field, wpe_vec3(0, 9, 0), &dir, NULL));

    /* Two goals: each cell goes to the nearer one. */
    const WPE_Vec3 goals[2] = {goal, wpe_vec3(-3, 0, 3)};
    WPE_TEST_ASSERT_OK(wpe_nav_flow_build(field, goals, 2, 0.0f, NULL));
    WPE_TEST_ASSERT(wpe_nav_flow_distance(field, start_cell) < 6.5f);
    /* A limited distance leaves the far room unreached. */
    WPE_TEST_ASSERT_OK(wpe_nav_flow_build(field, &goal, 1, 5.0f, NULL));
    WPE_TEST_ASSERT(isinf(wpe_nav_flow_distance(field, start_cell)));
    WPE_TEST_ASSERT_EQ_UINT(WPE_NAV_CELL_NONE, wpe_nav_flow_next(field, start_cell));
    WPE_TEST_ASSERT(!wpe_nav_flow_direction(field, start, &dir, NULL));

    /* Failures. */
    const WPE_Vec3 nowhere = wpe_vec3(50, 0, 0), broken = wpe_vec3(INFINITY, 0, 0);
    WPE_TEST_ASSERT_EQ_INT(WPE_RESULT_ERROR_NOT_FOUND, wpe_nav_flow_build(field, &nowhere, 1, 0.0f, NULL));
    WPE_TEST_ASSERT(isinf(wpe_nav_flow_distance(field, goal_cell)));
    WPE_TEST_ASSERT_EQ_INT(WPE_RESULT_ERROR_INVALID_ARG, wpe_nav_flow_build(field, &broken, 1, 0.0f, NULL));
    WPE_TEST_ASSERT_EQ_INT(WPE_RESULT_ERROR_INVALID_ARG, wpe_nav_flow_build(field, &goal, 1, -1.0f, NULL));
    WPE_TEST_ASSERT_EQ_INT(WPE_RESULT_ERROR_NULL_ARG, wpe_nav_flow_build(field, NULL, 1, 0.0f, NULL));
    WPE_TEST_ASSERT(isinf(wpe_nav_flow_distance(NULL, 0)));
    WPE_TEST_ASSERT(isinf(wpe_nav_flow_distance(field, 1u << 30)));
    WPE_TEST_ASSERT_EQ_UINT(WPE_NAV_CELL_NONE, wpe_nav_flow_next(NULL, 0));
    wpe_nav_flow_destroy(NULL);
    wpe_nav_flow_destroy(field);
    wpe_nav_query_destroy(q);
    wpe_nav_grid_destroy(grid);
}

WPE_TEST_MAIN(WPE_TEST_CASE(test_nav_nearest_point), WPE_TEST_CASE(test_nav_reachability_and_lines),
              WPE_TEST_CASE(test_nav_path_on_open_floor_is_straight),
              WPE_TEST_CASE(test_nav_path_goes_through_the_doorway),
              WPE_TEST_CASE(test_nav_path_climbs_ramps_and_stacked_floors),
              WPE_TEST_CASE(test_nav_path_failures_and_partial_paths),
              WPE_TEST_CASE(test_nav_path_can_be_walked),
              WPE_TEST_CASE(test_nav_flow_field_leads_to_the_goal))
