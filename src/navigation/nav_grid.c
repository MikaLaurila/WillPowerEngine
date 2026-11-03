#define WPE_LOG_MODULE "navigation"
#include <math.h>

#include "navigation/nav_internal.h"
#include "navigation/navigation.h"

#define WPE__DEBUG_LIFT 0.04f /* Debug lines float this far above the floor. */

WPE_NavGridInfo wpe_nav_grid_info(const WPE_NavGrid* grid) {
    if (!grid) return (WPE_NavGridInfo){0};
    return (WPE_NavGridInfo){
        .settings = grid->settings,
        .origin = grid->origin,
        .width = grid->width,
        .depth = grid->depth,
        .cell_count = grid->cell_count,
        .island_count = grid->island_count,
        .max_layers = grid->max_layers,
        .memory = sizeof *grid + ((size_t)grid->width * grid->depth + 1u) * sizeof *grid->columns +
                  (size_t)grid->cell_count * sizeof *grid->cells,
    };
}

/* The column holding `cell`: the last column starting at or before it. */
static uint32_t wpe__column_of(const WPE_NavGrid* g, uint32_t cell) {
    uint32_t lo = 0, hi = g->width * g->depth; /* columns[lo] <= cell < columns[hi] */
    while (hi - lo > 1) {
        const uint32_t mid = lo + (hi - lo) / 2u;
        if (g->columns[mid] <= cell) {
            lo = mid;
        } else {
            hi = mid;
        }
    }
    return lo;
}

static float wpe__floor_y(const WPE_NavGrid* g, uint32_t cell) {
    return g->origin.y + (float)g->cells[cell].floor * g->settings.cell_height;
}

bool wpe_nav_grid_cell(const WPE_NavGrid* grid, uint32_t cell, WPE_NavCellInfo* out_info) {
    if (!grid || !out_info || cell >= grid->cell_count) return false;
    const uint32_t column = wpe__column_of(grid, cell);
    const uint32_t x = column % grid->width, z = column / grid->width;
    const wpe__NavCell* c = &grid->cells[cell];
    const float cs = grid->settings.cell_size;
    *out_info = (WPE_NavCellInfo){
        .position = wpe_vec3(grid->origin.x + ((float)x + 0.5f) * cs, wpe__floor_y(grid, cell),
                             grid->origin.z + ((float)z + 0.5f) * cs),
        .ceiling = c->ceiling == WPE__NAV_OPEN_CEILING
                       ? INFINITY
                       : grid->origin.y + (float)c->ceiling * grid->settings.cell_height,
        .x = x,
        .z = z,
        .layer = cell - grid->columns[column],
        .island = c->island,
    };
    for (int dir = 0; dir < 4; ++dir)
        out_info->neighbors[dir] = wpe__nav_neighbor(grid, column, cell, dir);
    return true;
}

uint32_t wpe_nav_grid_find_cell(const WPE_NavGrid* grid, WPE_Vec3 point, float max_vertical) {
    if (!grid || !isfinite(point.x) || !isfinite(point.y) || !isfinite(point.z) || !(max_vertical >= 0.0f))
        return WPE_NAV_CELL_NONE;
    const float cs = grid->settings.cell_size;
    const float fx = floorf((point.x - grid->origin.x) / cs), fz = floorf((point.z - grid->origin.z) / cs);
    if (fx < 0.0f || fz < 0.0f || fx >= (float)grid->width || fz >= (float)grid->depth)
        return WPE_NAV_CELL_NONE;
    const uint32_t column = (uint32_t)fx + (uint32_t)fz * grid->width;
    uint32_t best = WPE_NAV_CELL_NONE;
    float best_distance = max_vertical;
    for (uint32_t cell = grid->columns[column]; cell < grid->columns[column + 1]; ++cell) {
        const float distance = fabsf(point.y - wpe__floor_y(grid, cell));
        if (distance <= best_distance) {
            best_distance = distance;
            best = cell;
        }
    }
    return best;
}

/* Distinct, readable colors for islands (0xRRGGBB). */
static uint32_t wpe__island_color(uint32_t island) {
    static const uint32_t palette[] = {0x3CC8F0u, 0x50E070u, 0xF0C040u, 0xF07878u,
                                       0xB08CF0u, 0x40E0C0u, 0xF0A0D0u, 0xA0D040u};
    return palette[island % (sizeof palette / sizeof palette[0])];
}

static uint32_t wpe__dim(uint32_t rgb) {
    const uint32_t r = (rgb >> 16) & 0xFFu, g = (rgb >> 8) & 0xFFu, b = rgb & 0xFFu;
    return ((r * 3u / 5u) << 16) | ((g * 3u / 5u) << 8) | (b * 3u / 5u);
}

void wpe_nav_grid_debug_draw(const WPE_NavGrid* grid, WPE_NavLineFn line, void* user_data) {
    if (!grid || !line) return;
    const float cs = grid->settings.cell_size;
    /* Interior lines on column boundaries about a meter apart. */
    const uint32_t every = cs >= 1.0f ? 1u : (uint32_t)lroundf(1.0f / cs);
    for (uint32_t column = 0; column < grid->width * grid->depth; ++column) {
        const uint32_t x = column % grid->width, z = column / grid->width;
        const float x0 = grid->origin.x + (float)x * cs, x1 = x0 + cs;
        const float z0 = grid->origin.z + (float)z * cs, z1 = z0 + cs;
        for (uint32_t cell = grid->columns[column]; cell < grid->columns[column + 1]; ++cell) {
            const float y = wpe__floor_y(grid, cell) + WPE__DEBUG_LIFT;
            const uint32_t color = wpe__island_color(grid->cells[cell].island);
            for (int dir = 0; dir < 4; ++dir) {
                /* The cell's side facing `dir`. */
                WPE_Vec3 a, b;
                switch (dir) {
                case WPE_NAV_POS_X:
                    a = wpe_vec3(x1, y, z0), b = wpe_vec3(x1, y, z1);
                    break;
                case WPE_NAV_POS_Z:
                    a = wpe_vec3(x0, y, z1), b = wpe_vec3(x1, y, z1);
                    break;
                case WPE_NAV_NEG_X:
                    a = wpe_vec3(x0, y, z0), b = wpe_vec3(x0, y, z1);
                    break;
                default:
                    a = wpe_vec3(x0, y, z0), b = wpe_vec3(x1, y, z0);
                    break;
                }
                const uint32_t n = wpe__nav_neighbor(grid, column, cell, dir);
                if (n == WPE_NAV_CELL_NONE) {
                    line(a, b, color, user_data); /* The outline. */
                    continue;
                }
                /* A shared side is drawn once, from the -X/-Z cell, at the two floors' mean height. */
                const bool on_grid = dir == WPE_NAV_POS_X ? (x + 1u) % every == 0 : (z + 1u) % every == 0;
                if ((dir != WPE_NAV_POS_X && dir != WPE_NAV_POS_Z) || !on_grid) continue;
                const float mean = (y + wpe__floor_y(grid, n) + WPE__DEBUG_LIFT) * 0.5f;
                a.y = mean;
                b.y = mean;
                line(a, b, wpe__dim(color), user_data);
            }
        }
    }
}
