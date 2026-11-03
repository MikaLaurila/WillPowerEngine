/**
 * @file nav_path.h
 * @brief Finding the way on a navigation grid (navigation.h): nearest walkable points, straight-line
 *        walkability, shortest paths (A*) and flow fields.
 *
 * Moving between cells: a cell steps to its four linked neighbors, and diagonally when both ways
 * around the corner are linked and meet in the same cell (so paths never cut a wall's corner). A
 * step costs its 3D length between the cells' floor centers, so climbing costs a little more.
 *
 * Paths come out as a list of corner points: the A* chain of cells is pulled straight wherever a
 * straight line stays on walkable cells (wpe_nav_grid_line_walkable()), like a string pulled tight
 * around the corners. Since the grid was eroded by the agent's radius, a character whose center
 * follows the path keeps clear of the walls.
 *
 * Flow fields answer "which way to the goal?" for every cell at once (one Dijkstra pass), for many
 * agents chasing the same target.
 *
 * Positions "on the grid" are matched to cells as wpe_nav_grid_find_cell() does, within half the
 * agent's height vertically; points off the walkable area are moved to the nearest walkable point
 * within a search radius.
 *
 * - Dependencies: navigation.h
 * - Thread affinity: grids are read-only, so any number of queries and flow fields may run at once on
 *   different threads; each WPE_NavQuery / WPE_NavFlowField is used by one thread at a time. Destroy
 *   them before their grid.
 *
 * @author WillPowerEngine Team
 */
#ifndef WPE_NAVIGATION_NAV_PATH_H
#define WPE_NAVIGATION_NAV_PATH_H

#include <stdint.h>

#include "core/result.h"
#include "math/wpe_math.h"
#include "navigation/navigation.h"

/* ---- Points and lines ---------------------------------------------------------------------------- */

/**
 * Finds the walkable point nearest to `point`: the point itself, moved onto its cell's floor, when it
 * stands on the grid; otherwise the closest point of the nearest cell's square within the limits.
 *
 * @param max_horizontal How far to look sideways (world units).
 * @param max_vertical How far a floor may be above or below the point.
 * @param out_point Optional: the walkable point (on the cell's floor).
 * @return The cell, or WPE_NAV_CELL_NONE when nothing is walkable within the limits.
 *
 * @thread_safety Any thread
 */
uint32_t wpe_nav_grid_nearest(const WPE_NavGrid* grid, WPE_Vec3 point, float max_horizontal,
                              float max_vertical, WPE_Vec3* out_point);

/** @return Whether a path exists between two cells (they are on the same island). @thread_safety Any thread
 */
bool wpe_nav_grid_reachable(const WPE_NavGrid* grid, uint32_t from_cell, uint32_t to_cell);

/**
 * Whether a character can walk in a straight line (seen from above) from `from` to `to`: every cell
 * the line crosses is walkable and linked to the next, and corners are only passed where both ways
 * around them are open. Both points are matched to cells as the file comment says; the line follows
 * the floors in between, up and down steps and slopes.
 *
 * @return false when either point is not on the grid, or something blocks the line.
 *
 * @thread_safety Any thread
 */
bool wpe_nav_grid_line_walkable(const WPE_NavGrid* grid, WPE_Vec3 from, WPE_Vec3 to);

/* ---- Paths --------------------------------------------------------------------------------------- */

/** Scratch memory for path searches on one grid. Opaque. */
typedef struct WPE_NavQuery WPE_NavQuery;

typedef struct WPE_NavPathOptions {
    /* How far the start and the goal may be from the walkable area (sideways, world units; default 1).
     * Points further away fail with WPE_RESULT_ERROR_NOT_FOUND. */
    float search_radius;
    /* When the goal can't be reached (another island, or max_visited ran out), return the path to the
     * reachable point nearest to it instead of failing (default false). */
    bool allow_partial;
    /* Cell centers as A* found them, without pulling the path straight (default false). */
    bool raw;
    /* Most cells the search may expand before it gives up (0 = no limit, the default). */
    uint32_t max_visited;
} WPE_NavPathOptions;

/** A found path. `points` belong to the query and stay valid until its next search or destruction. */
typedef struct WPE_NavPath {
    const WPE_Vec3* points; /* From the start's walkable point to the goal's (or nearest reachable) point. */
    uint32_t count;         /* At least 1 (start and goal in one cell with the same point give 1). */
    float length;           /* Sum of the segments' 3D lengths. */
    bool partial;           /* The goal wasn't reached; the path ends as near to it as possible. */
    uint32_t start_cell, end_cell;
    uint32_t visited; /* Cells the search expanded (cost diagnostics). */
} WPE_NavPath;

/** @return The default options (see WPE_NavPathOptions). @thread_safety Any thread */
WPE_NavPathOptions wpe_nav_path_options_default(void);

/**
 * Creates the scratch memory for searching `grid` (about 32 bytes per cell, allocated here; path points
 * grow as needed, so searches stop allocating once warmed up).
 *
 * @return WPE_RESULT_OK, WPE_RESULT_ERROR_NULL_ARG or WPE_RESULT_ERROR_OUT_OF_MEMORY.
 *
 * @thread_safety Any thread
 */
[[nodiscard]] WPE_Result wpe_nav_query_create(const WPE_NavGrid* grid, WPE_NavQuery** out_query);

/** Destroys a query; NULL is a no-op. @thread_safety Thread using it */
void wpe_nav_query_destroy(WPE_NavQuery* query);

/** @return The grid a query searches. @thread_safety Thread using it */
const WPE_NavGrid* wpe_nav_query_grid(const WPE_NavQuery* query);

/**
 * Finds the shortest path from `start` to `goal` (A* with an octile distance heuristic, ties broken
 * by cell number, so results are the same every time), then pulls it straight unless `raw`.
 *
 * @param options NULL: the defaults.
 * @return WPE_RESULT_OK; WPE_RESULT_ERROR_NULL_ARG; WPE_RESULT_ERROR_INVALID_ARG (non-finite points or a
 *         negative search radius); WPE_RESULT_ERROR_NOT_FOUND (start or goal off the grid, or no path
 *         without allow_partial; not logged, since callers expect it); WPE_RESULT_ERROR_OUT_OF_MEMORY.
 *         `out_path` is zeroed on failure.
 *
 * @thread_safety Thread using the query
 */
WPE_Result wpe_nav_find_path(WPE_NavQuery* query, WPE_Vec3 start, WPE_Vec3 goal,
                             const WPE_NavPathOptions* options, WPE_NavPath* out_path);

/* ---- Flow fields --------------------------------------------------------------------------------- */

/** Distances to a set of goals for every cell, and the way there. Opaque. */
typedef struct WPE_NavFlowField WPE_NavFlowField;

/** Creates an empty field for `grid` (about 24 bytes per cell). @return WPE_RESULT_OK,
 * WPE_RESULT_ERROR_NULL_ARG or WPE_RESULT_ERROR_OUT_OF_MEMORY. @thread_safety Any thread */
[[nodiscard]] WPE_Result wpe_nav_flow_create(const WPE_NavGrid* grid, WPE_NavFlowField** out_field);

/** Destroys a field; NULL is a no-op. @thread_safety Thread using it */
void wpe_nav_flow_destroy(WPE_NavFlowField* field);

/**
 * Computes the walking distance from every cell to the nearest goal (Dijkstra over the same moves
 * paths use). Goals off the grid (beyond 1 unit sideways) are skipped. Cells further than
 * `max_distance` (0: no limit) are left unreached, which bounds the work around a goal.
 *
 * @return WPE_RESULT_OK; WPE_RESULT_ERROR_NULL_ARG; WPE_RESULT_ERROR_INVALID_ARG (a non-finite goal or a
 *         negative max_distance); WPE_RESULT_ERROR_NOT_FOUND (no goal is on the grid; every cell is then
 *         unreached).
 *
 * @thread_safety Thread using the field
 */
WPE_Result wpe_nav_flow_build(WPE_NavFlowField* field, const WPE_Vec3* goals, uint32_t goal_count,
                              float max_distance);

/** @return A cell's walking distance to the nearest goal, or INFINITY when unreached. @thread_safety Any
 * thread (no build meanwhile) */
float wpe_nav_flow_distance(const WPE_NavFlowField* field, uint32_t cell);

/** @return The cell to go to next from `cell` (itself for a goal's cell), or WPE_NAV_CELL_NONE when
 * unreached.
 * @thread_safety Any thread (no build meanwhile) */
uint32_t wpe_nav_flow_next(const WPE_NavFlowField* field, uint32_t cell);

/**
 * Which way to walk from `position` toward the nearest goal: a unit vector in the XZ plane, toward the
 * farthest cell a few steps down the flow that is in straight walkable sight (so movement doesn't zig
 * zag along the grid), or toward the goal point itself in the goal's cell.
 *
 * @param out_direction Receives the direction ({0, 0, 0} at the goal point).
 * @param out_distance Optional: the walking distance left (from the position's cell).
 * @return false when the position isn't on the grid or its cell wasn't reached.
 *
 * @thread_safety Any thread (no build meanwhile)
 */
bool wpe_nav_flow_direction(const WPE_NavFlowField* field, WPE_Vec3 position, WPE_Vec3* out_direction,
                            float* out_distance);

#endif /* WPE_NAVIGATION_NAV_PATH_H */
