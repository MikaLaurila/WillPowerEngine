/**
 * @file nav_internal.h
 * @brief Private layout of navigation geometry and grids, shared by the module's files.
 *
 * - Dependencies: core, math
 * - Thread affinity: as navigation.h
 */
#ifndef WPE_NAVIGATION_NAV_INTERNAL_H
#define WPE_NAVIGATION_NAV_INTERNAL_H

#include <stddef.h>
#include <stdint.h>

#include "math/wpe_math.h"
#include "navigation/navigation.h"

#define WPE__NAV_NO_LINK 0xFFu        /* wpe__NavCell.links: no neighbor that way. */
#define WPE__NAV_OPEN_CEILING 0xFFFFu /* wpe__NavCell.ceiling: nothing above. */
#define WPE__NAV_MAX_HEIGHT 65000u    /* Most cell heights a grid spans. */

typedef struct wpe__NavTriangle {
    uint32_t v[3]; /* Into WPE_NavGeometry.vertices, counter-clockwise from the front. */
} wpe__NavTriangle;

struct WPE_NavGeometry {
    WPE_Vec3* vertices; /* World space. */
    uint32_t vertex_count;
    uint32_t vertex_capacity;
    wpe__NavTriangle* triangles;
    uint32_t triangle_count;
    uint32_t triangle_capacity;
    WPE_Vec3 min, max; /* Bounds of the triangles' corners. */
    bool has_bounds;
};

/* A walkable cell. Heights are in cell heights above the grid origin. */
typedef struct wpe__NavCell {
    uint16_t floor;
    uint16_t ceiling; /* WPE__NAV_OPEN_CEILING: open sky. */
    uint8_t links[4]; /* Per WPE_NavDirection: the neighbor's layer in its column, or WPE__NAV_NO_LINK. */
    uint32_t island;
} wpe__NavCell;

struct WPE_NavGrid {
    WPE_NavSettings settings;
    WPE_Vec3 origin;
    uint32_t width, depth;
    /* Column c = x + z * width holds cells columns[c] .. columns[c + 1] - 1, bottom first. */
    uint32_t* columns; /* width * depth + 1 entries. */
    wpe__NavCell* cells;
    uint32_t cell_count;
    uint32_t island_count;
    uint32_t max_layers;
};

/* The column next to `column` in direction `dir` (WPE_NavDirection), or UINT32_MAX past the edge. */
static inline uint32_t wpe__nav_neighbor_column(const WPE_NavGrid* g, uint32_t column, int dir) {
    const uint32_t x = column % g->width, z = column / g->width;
    switch (dir) {
    case WPE_NAV_POS_X:
        return x + 1 < g->width ? column + 1 : UINT32_MAX;
    case WPE_NAV_POS_Z:
        return z + 1 < g->depth ? column + g->width : UINT32_MAX;
    case WPE_NAV_NEG_X:
        return x > 0 ? column - 1 : UINT32_MAX;
    default:
        return z > 0 ? column - g->width : UINT32_MAX;
    }
}

/* The cell `cell` (of `column`) links to in direction `dir`, or WPE_NAV_CELL_NONE. */
static inline uint32_t wpe__nav_neighbor(const WPE_NavGrid* g, uint32_t column, uint32_t cell, int dir) {
    const uint8_t link = g->cells[cell].links[dir];
    if (link == WPE__NAV_NO_LINK) return WPE_NAV_CELL_NONE;
    return g->columns[wpe__nav_neighbor_column(g, column, dir)] + link;
}

#endif /* WPE_NAVIGATION_NAV_INTERNAL_H */
