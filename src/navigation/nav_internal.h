/**
 * @file nav_internal.h
 * @brief Private layout of navigation geometry and grids, shared by the module's files.
 *
 * - Dependencies: core, math
 * - Thread affinity: as navigation.h
 */
#ifndef WPE_NAVIGATION_NAV_INTERNAL_H
#define WPE_NAVIGATION_NAV_INTERNAL_H

#include <math.h>
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
    uint32_t* cell_columns; /* The column of each cell. */
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

/* The cell `cell` (of `column`) links to in direction `dir`, or WPE_NAV_CELL_NONE. Links exist only toward
 * columns inside the grid, so the neighbor column is a plain offset (this is the hot path of searches). */
static inline uint32_t wpe__nav_neighbor(const WPE_NavGrid* g, uint32_t column, uint32_t cell, int dir) {
    const uint8_t link = g->cells[cell].links[dir];
    if (link == WPE__NAV_NO_LINK) return WPE_NAV_CELL_NONE;
    const uint32_t neighbor = dir == WPE_NAV_POS_X   ? column + 1u
                              : dir == WPE_NAV_POS_Z ? column + g->width
                              : dir == WPE_NAV_NEG_X ? column - 1u
                                                     : column - g->width;
    return g->columns[neighbor] + link;
}

/* The middle of a cell's floor. */
static inline WPE_Vec3 wpe__nav_center(const WPE_NavGrid* g, uint32_t cell) {
    const uint32_t column = g->cell_columns[cell];
    const uint32_t x = column % g->width, z = column / g->width; /* Whole columns, on purpose. */
    const float cs = g->settings.cell_size;
    return wpe_vec3(g->origin.x + ((float)x + 0.5f) * cs,
                    g->origin.y + (float)g->cells[cell].floor * g->settings.cell_height,
                    g->origin.z + ((float)z + 0.5f) * cs);
}

/* The length of a move between neighboring cells (straight or diagonal): 3D, between their floor centers. */
static inline float wpe__nav_move_cost(const WPE_NavGrid* g, uint32_t a, uint32_t b, bool diagonal) {
    const float flat = diagonal ? g->settings.cell_size * 1.41421356f : g->settings.cell_size;
    const int rise = (int)g->cells[b].floor - (int)g->cells[a].floor;
    if (rise == 0) return flat;
    const float dy = (float)rise * g->settings.cell_height;
    return sqrtf(flat * flat + dy * dy);
}

/* The moves out of a cell: its linked neighbors, then the diagonal cells reached both ways around the
 * corner (the same cell either way, so a move never cuts a wall's corner). @return Their count (<= 8); the
 * first *out_straight are the straight ones. */
static inline uint32_t wpe__nav_moves(const WPE_NavGrid* g, uint32_t cell, uint32_t out[8],
                                      uint32_t* out_straight) {
    const uint32_t column = g->cell_columns[cell];
    uint32_t straight[4];
    uint32_t n = 0;
    for (int dir = 0; dir < 4; ++dir) {
        straight[dir] = wpe__nav_neighbor(g, column, cell, dir);
        if (straight[dir] != WPE_NAV_CELL_NONE) out[n++] = straight[dir];
    }
    *out_straight = n;
    for (int dir = 0; dir < 4; ++dir) {
        const int turn = (dir + 1) % 4; /* +X with +Z, +Z with -X, -X with -Z, -Z with +X. */
        if (straight[dir] == WPE_NAV_CELL_NONE || straight[turn] == WPE_NAV_CELL_NONE) continue;
        const uint32_t a = wpe__nav_neighbor(g, g->cell_columns[straight[dir]], straight[dir], turn);
        const uint32_t b = wpe__nav_neighbor(g, g->cell_columns[straight[turn]], straight[turn], dir);
        if (a != WPE_NAV_CELL_NONE && a == b) out[n++] = a;
    }
    return n;
}

/* Search state of one cell, kept together so a search touches one cache line per cell. */
#define WPE__NAV_NOT_QUEUED UINT32_MAX
typedef struct wpe__NavNode {
    float key; /* Heap order: A*'s f (g + heuristic), a flow field's distance. */
    float g;   /* A*: cost from the start (ties in `key` go to the larger: deeper first). 0 in flow fields. */
    uint32_t pos;    /* Heap slot, WPE__NAV_NOT_QUEUED, or A*'s closed mark. */
    uint32_t parent; /* A*: where it was reached from; flow fields: the next cell toward the goal. */
    uint32_t stamp;  /* A*: the search that last touched it (older state is stale). */
} wpe__NavNode;

/* An indexed binary min-heap of cells ordered by node key, then the larger g, then the cell number
 * (deterministic). The caller owns both arrays (cell_count entries). */
typedef struct wpe__NavHeap {
    uint32_t* items;
    wpe__NavNode* nodes;
    uint32_t count;
} wpe__NavHeap;

/* Adds `cell`, or moves it up after its key decreased. */
void wpe__nav_heap_push(wpe__NavHeap* heap, uint32_t cell);
/* Removes and returns the smallest cell (the heap must not be empty). */
uint32_t wpe__nav_heap_pop(wpe__NavHeap* heap);

/* Walks the straight line (seen from above) from `from` (in `from_cell`'s square) to `to`, following
 * links. @return Whether it gets through to `to_cell`, or, when to_cell is WPE_NAV_CELL_NONE, to a cell in
 * `to`'s column whose floor is within max_vertical of to.y. */
bool wpe__nav_line(const WPE_NavGrid* g, uint32_t from_cell, WPE_Vec3 from, WPE_Vec3 to, uint32_t to_cell,
                   float max_vertical);

#endif /* WPE_NAVIGATION_NAV_INTERNAL_H */
