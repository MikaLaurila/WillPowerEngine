/**
 * @file navigation.h
 * @brief Navigation grids: where a character of a given size can stand and walk, built from the
 *        level's static colliders.
 *
 * A nav grid is a 2.5D, multi-layer grid. Seen from above, the level is cut into square columns
 * (`cell_size` wide). Each column holds a list of *cells*: the floors in it that a character can
 * stand on, one above the other (the ground under a bridge and the bridge's deck are two cells of
 * the same column). A cell has a floor height, a ceiling (the next solid thing above it) and links
 * to the cells it can walk to in the four neighboring columns.
 *
 * Building one (wpe_nav_grid_build()) works like this:
 * 1. Rasterize: every solid triangle is cut by the column grid, and each piece becomes a solid
 *    *span* (a vertical interval, in steps of `cell_height`) in its column. Overlapping spans
 *    merge. A span's top is walkable when the triangle that made it faces up and is no steeper
 *    than `max_slope`.
 * 2. Low obstacles: a non-walkable top no more than `step_height` above a walkable one (a curb, a
 *    rail lying on the floor) becomes walkable, since a character steps onto it.
 * 3. Cells: each walkable span top with at least `agent_height` of free space above it becomes a
 *    cell.
 * 4. Links: two cells of neighboring columns are linked when their floors differ by at most
 *    `step_height` and the free space they share is at least `agent_height` tall.
 * 5. Erosion: cells whose centers are closer than `agent_radius` to an edge (a wall, a drop, the
 *    side of a cell without all four links) are removed, so a character's center can go anywhere
 *    that is left without its capsule touching a wall. Distances are measured along the grid, so a
 *    doorway needs to be about 2 * agent_radius + cell_size wide to stay open.
 * 6. Islands: cells connected through links get the same island number; two cells on different
 *    islands can't reach each other.
 *
 * The geometry comes from a WPE_NavGeometry: triangles added by hand, or collected from a world's
 * static colliders (wpe_nav_geometry_add_colliders()). The grid is immutable and holds no
 * references to the geometry or the world.
 *
 * The default settings match the default character controller (physics.h), so a character moved
 * by wpe_character_move() fits everywhere the grid says it can walk.
 *
 * - Dependencies: core, math, ecs, scene, assets, physics (component types only)
 * - Thread affinity: building takes the thread owning the world when collecting colliders, any
 *   thread otherwise; a built grid is read-only and its queries may run on any thread at once.
 *
 * @author WillPowerEngine Team
 */
#ifndef WPE_NAVIGATION_NAVIGATION_H
#define WPE_NAVIGATION_NAVIGATION_H

#include <stdint.h>

#include "assets/asset_manager.h"
#include "core/result.h"
#include "ecs/ecs.h"
#include "math/wpe_math.h"

/** No cell (failed lookups, missing links). */
#define WPE_NAV_CELL_NONE UINT32_MAX
/** Most cells in one column (floors stacked above each other); higher ones are dropped. */
#define WPE_NAV_MAX_LAYERS 254u
/** Most columns in a grid (width * depth), about 4096 x 4096; at most 65535 along either axis. */
#define WPE_NAV_MAX_COLUMNS (16u * 1024u * 1024u)

/** The four link directions, in this order: +X, +Z, -X, -Z. */
typedef enum WPE_NavDirection {
    WPE_NAV_POS_X = 0,
    WPE_NAV_POS_Z,
    WPE_NAV_NEG_X,
    WPE_NAV_NEG_Z,
} WPE_NavDirection;

/** How a grid is built. Lengths are in world units (meters), angles in degrees. */
typedef struct WPE_NavSettings {
    float cell_size;    /* Width of a column (default 0.25). Smaller is more precise and slower. */
    float cell_height;  /* Height resolution of floors and spans (default 0.1). */
    float agent_radius; /* Distance kept from walls and drops (default 0.4). */
    float agent_height; /* Free space needed above a floor (default 1.8). */
    float max_slope;    /* Steepest walkable surface (default 45). */
    float step_height;  /* Tallest step walked up or down between neighboring cells (default 0.3). */
    /* The region to build. When bounds_max is not above bounds_min on every axis (the default, all zero),
     * the geometry's bounds are used. */
    WPE_Vec3 bounds_min;
    WPE_Vec3 bounds_max;
} WPE_NavSettings;

/** Triangles to build a grid from. Opaque; owned by the caller. */
typedef struct WPE_NavGeometry WPE_NavGeometry;

/** A built grid. Opaque, immutable. */
typedef struct WPE_NavGrid WPE_NavGrid;

/** Facts about a whole grid. */
typedef struct WPE_NavGridInfo {
    WPE_NavSettings settings; /* As built (the bounds are the ones used). */
    WPE_Vec3 origin;          /* World position of column (0, 0)'s lowest corner. */
    uint32_t width;           /* Columns along X. */
    uint32_t depth;           /* Columns along Z. */
    uint32_t cell_count;      /* Walkable cells. */
    uint32_t island_count;    /* Separately connected areas. */
    uint32_t max_layers;      /* The most cells any one column has. */
    size_t memory;            /* Bytes the grid holds. */
} WPE_NavGridInfo;

/** One walkable cell. */
typedef struct WPE_NavCellInfo {
    WPE_Vec3 position;     /* The middle of its floor (world space). */
    float ceiling;         /* World height of what is above it (INFINITY: nothing). */
    uint32_t x, z;         /* Its column. */
    uint32_t layer;        /* Its index among its column's cells, from the bottom. */
    uint32_t island;       /* Connected area, 0 .. island_count - 1. */
    uint32_t neighbors[4]; /* Linked cell per WPE_NavDirection, or WPE_NAV_CELL_NONE. */
} WPE_NavCellInfo;

/** Line callback for debug drawing (world-space segment, 0xRRGGBB color). */
typedef void (*WPE_NavLineFn)(WPE_Vec3 a, WPE_Vec3 b, uint32_t color, void* user_data);

/** @return The default settings (see WPE_NavSettings). @thread_safety Any thread */
WPE_NavSettings wpe_nav_settings_default(void);

/**
 * Checks settings: lengths finite and positive (agent_radius and step_height may be 0), max_slope
 * in [0, 90), step_height below agent_height, and the bounds finite. Problems are logged.
 *
 * @return WPE_RESULT_OK, WPE_RESULT_ERROR_NULL_ARG or WPE_RESULT_ERROR_INVALID_ARG.
 *
 * @thread_safety Any thread
 */
WPE_Result wpe_nav_settings_validate(const WPE_NavSettings* settings);

/** Creates an empty geometry. @return WPE_RESULT_OK, WPE_RESULT_ERROR_NULL_ARG or
 * WPE_RESULT_ERROR_OUT_OF_MEMORY. @thread_safety Any thread */
[[nodiscard]] WPE_Result wpe_nav_geometry_create(WPE_NavGeometry** out_geometry);

/** Destroys a geometry; NULL is a no-op. @thread_safety Thread using it */
void wpe_nav_geometry_destroy(WPE_NavGeometry* geometry);

/**
 * Adds an indexed triangle list, transformed by `transform` (NULL: as given). Front faces are
 * counter-clockwise (as in glTF and the renderer): only triangles whose front faces up can be
 * walked on. A transform that mirrors (negative determinant) is accounted for. Degenerate
 * triangles are skipped.
 *
 * @param indices `index_count` indices (a multiple of 3; extra ones are ignored) into `vertices`.
 * @return WPE_RESULT_OK; WPE_RESULT_ERROR_NULL_ARG; WPE_RESULT_ERROR_INVALID_ARG (an index out of
 *         range or a non-finite vertex); WPE_RESULT_ERROR_OUT_OF_MEMORY. Nothing is added on failure.
 *
 * @thread_safety Thread using the geometry
 */
WPE_Result wpe_nav_geometry_add_triangles(WPE_NavGeometry* geometry, const WPE_Vec3* vertices,
                                          uint32_t vertex_count, const uint32_t* indices,
                                          uint32_t index_count, const WPE_Mat4* transform);

/**
 * Adds a solid box: `half_extents` around `center`, then transformed by `transform` (NULL: none).
 *
 * @return As wpe_nav_geometry_add_triangles().
 *
 * @thread_safety Thread using the geometry
 */
WPE_Result wpe_nav_geometry_add_box(WPE_NavGeometry* geometry, WPE_Vec3 center, WPE_Vec3 half_extents,
                                    const WPE_Mat4* transform);

/**
 * Adds the shapes of a world's static colliders, exactly as the physics world builds them (world
 * scale baked in, offsets, the same minimum sizes):
 * - Colliders with no rigid body or a static one count; kinematic and dynamic bodies, triggers and
 *   character controllers don't (they are moving obstacles, not level geometry).
 * - Spheres and capsules are tessellated finely enough for walking (their tops are walkable where
 *   they are flat enough).
 * - Mesh colliders add the mesh's triangles; convex colliders add their mesh's triangles too,
 *   which is the hull for a convex mesh.
 *
 * Colliders that can't be added (a missing model, a bad mesh index) are logged and skipped.
 *
 * @param ecs A world with the physics components registered (wpe_physics_create()).
 * @param assets Needed for convex and mesh colliders (may be NULL when there are none).
 * @param out_added Optional: how many colliders were added.
 * @return WPE_RESULT_OK; WPE_RESULT_ERROR_NULL_ARG; WPE_RESULT_ERROR_INVALID_STATE (no physics
 *         components registered); WPE_RESULT_ERROR_OUT_OF_MEMORY.
 *
 * @thread_safety Thread owning the world
 */
WPE_Result wpe_nav_geometry_add_colliders(WPE_NavGeometry* geometry, WPE_Ecs* ecs,
                                          const WPE_AssetManager* assets, uint32_t* out_added);

/** @return The number of triangles added. @thread_safety Thread using the geometry */
uint32_t wpe_nav_geometry_triangle_count(const WPE_NavGeometry* geometry);

/** Bounds of every vertex added. @return false when there are none. @thread_safety Thread using the
 * geometry */
bool wpe_nav_geometry_bounds(const WPE_NavGeometry* geometry, WPE_Vec3* out_min, WPE_Vec3* out_max);

/**
 * Builds a grid (see the file comment for the steps). Building reads the geometry only, so the
 * same geometry can be built with different settings (agent sizes).
 *
 * @param settings NULL: the defaults.
 * @return WPE_RESULT_OK; WPE_RESULT_ERROR_NULL_ARG; WPE_RESULT_ERROR_INVALID_ARG (bad settings);
 *         WPE_RESULT_ERROR_NOT_FOUND (no triangles; a grid with no walkable cells is not an error);
 *         WPE_RESULT_ERROR_CAPACITY (more than WPE_NAV_MAX_COLUMNS columns, or a region taller
 *         than 65000 cell heights: use larger cells or bounds); WPE_RESULT_ERROR_OUT_OF_MEMORY.
 *         Errors are logged.
 *
 * @thread_safety Any thread (the geometry must not change meanwhile)
 */
[[nodiscard]] WPE_Result wpe_nav_grid_build(const WPE_NavGeometry* geometry, const WPE_NavSettings* settings,
                                            WPE_NavGrid** out_grid);

/**
 * Builds a grid from a world's static colliders: wpe_nav_geometry_add_colliders() and then
 * wpe_nav_grid_build().
 *
 * @return As those two.
 *
 * @thread_safety Thread owning the world
 */
[[nodiscard]] WPE_Result wpe_nav_grid_build_from_world(WPE_Ecs* ecs, const WPE_AssetManager* assets,
                                                       const WPE_NavSettings* settings,
                                                       WPE_NavGrid** out_grid);

/** Destroys a grid; NULL is a no-op. @thread_safety No queries may run meanwhile */
void wpe_nav_grid_destroy(WPE_NavGrid* grid);

/** @return Facts about the grid (all zero for NULL). @thread_safety Any thread */
WPE_NavGridInfo wpe_nav_grid_info(const WPE_NavGrid* grid);

/**
 * Describes a cell.
 *
 * @param cell 0 .. cell_count - 1 (cells are numbered column by column, X fastest, then by layer).
 * @return false for NULL arguments or a cell out of range.
 *
 * @thread_safety Any thread
 */
bool wpe_nav_grid_cell(const WPE_NavGrid* grid, uint32_t cell, WPE_NavCellInfo* out_info);

/**
 * Finds the cell a point is standing on: in the point's column, the cell whose floor is nearest
 * the point's height, if that is within `max_vertical` (above or below).
 *
 * @return The cell, or WPE_NAV_CELL_NONE (outside the grid, nothing walkable in the column, or every
 *         floor too far).
 *
 * @thread_safety Any thread
 */
uint32_t wpe_nav_grid_find_cell(const WPE_NavGrid* grid, WPE_Vec3 point, float max_vertical);

/**
 * Emits debug lines for the walkable area: its outline (edges without a link: walls, drops, the
 * end of the grid) and a grid of lines about every meter across it, both slightly above the floor
 * and colored by island.
 *
 * @thread_safety Any thread
 */
void wpe_nav_grid_debug_draw(const WPE_NavGrid* grid, WPE_NavLineFn line, void* user_data);

#endif /* WPE_NAVIGATION_NAVIGATION_H */
