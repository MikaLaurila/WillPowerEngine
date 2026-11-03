#define WPE_LOG_MODULE "navigation"
#include <math.h>
#include <string.h>

#include "core/alloc.h"
#include "core/log.h"
#include "navigation/nav_internal.h"
#include "navigation/navigation.h"

#define WPE__NO_SPAN UINT32_MAX
#define WPE__POLY_MAX 12 /* A triangle clipped by two slabs has at most 7 corners. */

/* ---- Settings ------------------------------------------------------------------------------------- */

WPE_NavSettings wpe_nav_settings_default(void) {
    return (WPE_NavSettings){.cell_size = 0.25f,
                             .cell_height = 0.1f,
                             .agent_radius = 0.4f,
                             .agent_height = 1.8f,
                             .max_slope = 45.0f,
                             .step_height = 0.3f};
}

static bool wpe__has_bounds(const WPE_NavSettings* s) {
    return s->bounds_max.x > s->bounds_min.x && s->bounds_max.y > s->bounds_min.y &&
           s->bounds_max.z > s->bounds_min.z;
}

WPE_Result wpe_nav_settings_validate(const WPE_NavSettings* s) {
    if (!s) return WPE_RESULT_ERROR_NULL_ARG;
    const struct {
        const char* name;
        float value;
        bool zero_ok;
    } lengths[] = {{"cell_size", s->cell_size, false},
                   {"cell_height", s->cell_height, false},
                   {"agent_radius", s->agent_radius, true},
                   {"agent_height", s->agent_height, false},
                   {"step_height", s->step_height, true}};
    for (size_t i = 0; i < sizeof lengths / sizeof lengths[0]; ++i) {
        const float v = lengths[i].value;
        if (!isfinite(v) || v < 0.0f || (v == 0.0f && !lengths[i].zero_ok) || v > 1e5f) {
            wpe_log_error("navigation setting %s must be a %s length (not %g)", lengths[i].name,
                          lengths[i].zero_ok ? "non-negative" : "positive", (double)v);
            return WPE_RESULT_ERROR_INVALID_ARG;
        }
    }
    if (!isfinite(s->max_slope) || s->max_slope < 0.0f || s->max_slope >= 90.0f) {
        wpe_log_error("navigation max_slope must be in [0, 90) degrees (not %g)", (double)s->max_slope);
        return WPE_RESULT_ERROR_INVALID_ARG;
    }
    if (s->step_height >= s->agent_height) {
        wpe_log_error("navigation step_height (%g) must be below agent_height (%g)", (double)s->step_height,
                      (double)s->agent_height);
        return WPE_RESULT_ERROR_INVALID_ARG;
    }
    const float bounds[6] = {s->bounds_min.x, s->bounds_min.y, s->bounds_min.z,
                             s->bounds_max.x, s->bounds_max.y, s->bounds_max.z};
    for (int i = 0; i < 6; ++i) {
        if (!isfinite(bounds[i])) {
            wpe_log_error("navigation bounds must be finite");
            return WPE_RESULT_ERROR_INVALID_ARG;
        }
    }
    return WPE_RESULT_OK;
}

/* ---- Heightfield: solid spans per column ------------------------------------------------------------ */

typedef struct wpe__Span {
    uint16_t smin, smax; /* Solid from smin to smax (cell heights above the origin). */
    bool walkable;       /* Its top can be stood on. */
    uint32_t next;       /* The next span up in the column, or WPE__NO_SPAN. */
} wpe__Span;

typedef struct wpe__Heightfield {
    uint32_t width, depth, height; /* height: cell heights the region spans. */
    WPE_Vec3 origin;
    float cell_size, cell_height;
    uint32_t* heads; /* Lowest span per column. */
    wpe__Span* spans;
    uint32_t span_count, span_capacity;
    uint32_t free_spans; /* Recycled spans, linked through `next`. */
    bool out_of_memory;
} wpe__Heightfield;

static uint32_t wpe__new_span(wpe__Heightfield* hf) {
    if (hf->free_spans != WPE__NO_SPAN) {
        const uint32_t s = hf->free_spans;
        hf->free_spans = hf->spans[s].next;
        return s;
    }
    if (hf->span_count == hf->span_capacity) {
        const uint32_t grown = hf->span_capacity ? hf->span_capacity * 2u : 4096u;
        wpe__Span* bigger =
            grown > hf->span_capacity ? wpe_realloc(hf->spans, (size_t)grown * sizeof *bigger) : NULL;
        if (!bigger) {
            hf->out_of_memory = true;
            return WPE__NO_SPAN;
        }
        hf->spans = bigger;
        hf->span_capacity = grown;
    }
    return hf->span_count++;
}

/*
 * Adds solid [smin, smax] to a column, merging it with the spans it touches. The merged top is the
 * highest one; it is walkable as the span that reaches it, or as either when both tops are within
 * `merge` of each other (a floor triangle meeting a wall's top edge).
 */
static void wpe__add_span(wpe__Heightfield* hf, uint32_t column, uint16_t smin, uint16_t smax, bool walkable,
                          uint16_t merge) {
    uint32_t prev = WPE__NO_SPAN;
    uint32_t cur = hf->heads[column];
    while (cur != WPE__NO_SPAN) {
        wpe__Span* c = &hf->spans[cur];
        if (c->smin > smax) break; /* Above the new span: insert before it. */
        if (c->smax < smin) {      /* Below it. */
            prev = cur;
            cur = c->next;
            continue;
        }
        if (c->smax > smax + merge) {
            walkable = c->walkable;
        } else if (smax <= c->smax + merge) {
            walkable = walkable || c->walkable;
        }
        if (c->smin < smin) smin = c->smin;
        if (c->smax > smax) smax = c->smax;
        const uint32_t next = c->next;
        c->next = hf->free_spans; /* Absorbed into the new span. */
        hf->free_spans = cur;
        if (prev == WPE__NO_SPAN) {
            hf->heads[column] = next;
        } else {
            hf->spans[prev].next = next;
        }
        cur = next;
    }
    const uint32_t s = wpe__new_span(hf);
    if (s == WPE__NO_SPAN) return;
    hf->spans[s] = (wpe__Span){.smin = smin, .smax = smax, .walkable = walkable, .next = cur};
    if (prev == WPE__NO_SPAN) {
        hf->heads[column] = s;
    } else {
        hf->spans[prev].next = s;
    }
}

/* Splits a convex polygon at `line` on `axis`: the part below the line and the part at or above it. */
static void wpe__divide(const WPE_Vec3* in, int n, WPE_Vec3* below, int* below_n, WPE_Vec3* above,
                        int* above_n, float line, int axis) {
    float d[WPE__POLY_MAX];
    for (int i = 0; i < n; ++i)
        d[i] = line - in[i].v[axis]; /* Positive below the line. */
    int nb = 0, na = 0;
    for (int a = 0, b = n - 1; a < n; b = a, ++a) {
        if ((d[a] >= 0.0f) != (d[b] >= 0.0f)) {
            /* The edge crosses the line: both parts get the crossing point. */
            const float s = d[b] / (d[b] - d[a]);
            const WPE_Vec3 p = wpe_vec3_add(in[b], wpe_vec3_scale(wpe_vec3_sub(in[a], in[b]), s));
            below[nb++] = p;
            above[na++] = p;
            if (d[a] > 0.0f) {
                below[nb++] = in[a];
            } else if (d[a] < 0.0f) {
                above[na++] = in[a];
            }
            continue;
        }
        if (d[a] >= 0.0f) {
            below[nb++] = in[a];
            if (d[a] != 0.0f) continue;
        }
        above[na++] = in[a]; /* On the line: both. */
    }
    *below_n = nb;
    *above_n = na;
}

static int32_t wpe__clamp_i(int32_t v, int32_t lo, int32_t hi) {
    return v < lo ? lo : (v > hi ? hi : v);
}

/*
 * Whether a piece of a triangle belongs to the slab [start, end) on `axis`. A piece with no extent
 * on that axis lying on the slab's start or end (a face exactly on a grid line, common with snapped
 * level geometry) belongs to the side its solid is on, behind its front face: otherwise a wall
 * ending on a grid line would also fill the open column beside it. Pieces of faces that merely touch
 * the line (the edge of a floor) have no extent and no normal along the axis, and are dropped.
 */
static bool wpe__piece_belongs(const WPE_Vec3* poly, int n, int axis, float start, float end, float normal) {
    float lo = poly[0].v[axis], hi = poly[0].v[axis];
    for (int i = 1; i < n; ++i) {
        lo = fminf(lo, poly[i].v[axis]);
        hi = fmaxf(hi, poly[i].v[axis]);
    }
    const float eps = (end - start) * 1e-4f;
    if (hi - lo > eps) return true;
    if (fabsf(lo - start) <= eps) return normal < 0.0f; /* Facing toward lower coordinates: solid above. */
    if (fabsf(hi - end) <= eps) return normal > 0.0f;
    return true; /* A vertical face inside the slab. */
}

/* Cuts a triangle by the column grid and adds a span per column piece. */
static void wpe__rasterize_triangle(wpe__Heightfield* hf, WPE_Vec3 a, WPE_Vec3 b, WPE_Vec3 c, WPE_Vec3 normal,
                                    bool walkable, uint16_t merge) {
    const float cs = hf->cell_size, ch = hf->cell_height;
    const WPE_Vec3 o = hf->origin;
    const float min_x = fminf(a.x, fminf(b.x, c.x)), max_x = fmaxf(a.x, fmaxf(b.x, c.x));
    const float min_z = fminf(a.z, fminf(b.z, c.z)), max_z = fmaxf(a.z, fmaxf(b.z, c.z));
    const float extent_x = (float)hf->width * cs, extent_z = (float)hf->depth * cs;
    if (max_x < o.x || min_x > o.x + extent_x || max_z < o.z || min_z > o.z + extent_z) return;
    /* Rows and columns from -1: the piece before the grid's first line is cut off and dropped. Each
     * starts a row (column) early, which a face on the line between the two may belong to. */
    const int32_t z0 = wpe__clamp_i((int32_t)floorf((min_z - o.z) / cs) - 1, -1, (int32_t)hf->depth - 1);
    const int32_t z1 = wpe__clamp_i((int32_t)floorf((max_z - o.z) / cs), -1, (int32_t)hf->depth - 1);

    WPE_Vec3 buffers[4][WPE__POLY_MAX];
    WPE_Vec3* rest = buffers[0];
    int rest_n = 3;
    rest[0] = a;
    rest[1] = b;
    rest[2] = c;
    for (int32_t z = z0; z <= z1; ++z) {
        WPE_Vec3* row = buffers[1];
        WPE_Vec3* next_rest = buffers[2];
        int row_n = 0, next_n = 0;
        wpe__divide(rest, rest_n, row, &row_n, next_rest, &next_n, o.z + (float)(z + 1) * cs, 2);
        memcpy(buffers[0], next_rest, (size_t)next_n * sizeof *next_rest);
        rest_n = next_n;
        if (row_n < 3 || z < 0 ||
            !wpe__piece_belongs(row, row_n, 2, o.z + (float)z * cs, o.z + (float)(z + 1) * cs, normal.z))
            continue;

        float row_min_x = row[0].x, row_max_x = row[0].x;
        for (int i = 1; i < row_n; ++i) {
            row_min_x = fminf(row_min_x, row[i].x);
            row_max_x = fmaxf(row_max_x, row[i].x);
        }
        const int32_t x0 =
            wpe__clamp_i((int32_t)floorf((row_min_x - o.x) / cs) - 1, -1, (int32_t)hf->width - 1);
        const int32_t x1 = wpe__clamp_i((int32_t)floorf((row_max_x - o.x) / cs), -1, (int32_t)hf->width - 1);
        for (int32_t x = x0; x <= x1; ++x) {
            WPE_Vec3* cell = buffers[3];
            WPE_Vec3 right[WPE__POLY_MAX];
            int cell_n = 0, right_n = 0;
            wpe__divide(row, row_n, cell, &cell_n, right, &right_n, o.x + (float)(x + 1) * cs, 0);
            memcpy(row, right, (size_t)right_n * sizeof *right);
            row_n = right_n;
            if (cell_n < 3 || x < 0 ||
                !wpe__piece_belongs(cell, cell_n, 0, o.x + (float)x * cs, o.x + (float)(x + 1) * cs,
                                    normal.x))
                continue;
            float lo = cell[0].y, hi = cell[0].y;
            for (int i = 1; i < cell_n; ++i) {
                lo = fminf(lo, cell[i].y);
                hi = fmaxf(hi, cell[i].y);
            }
            lo -= o.y;
            hi -= o.y;
            const float top = (float)hf->height * ch;
            if (hi < 0.0f || lo > top) continue;
            /* A thousandth of a cell height of slack, so float error never adds a whole step. Flat pieces
             * make spans of no thickness: their height is what matters. */
            const int32_t smax = wpe__clamp_i((int32_t)ceilf(hi / ch - 1e-3f), 0, (int32_t)hf->height + 1);
            const int32_t smin = wpe__clamp_i((int32_t)floorf(lo / ch + 1e-3f), 0, smax);
            wpe__add_span(hf, (uint32_t)x + (uint32_t)z * hf->width, (uint16_t)smin, (uint16_t)smax, walkable,
                          merge);
        }
        if (rest_n < 3) break;
    }
}

/* A non-walkable top at most `climb` above the walkable top below it becomes walkable: a character steps
 * onto it. The original flags decide, so this doesn't climb a stack of such tops. */
static void wpe__filter_low_obstacles(wpe__Heightfield* hf, uint16_t climb) {
    if (!hf->spans) return; /* Nothing was rasterized inside the region. */
    for (uint32_t column = 0; column < hf->width * hf->depth; ++column) {
        bool below_walkable = false;
        uint16_t below_top = 0;
        for (uint32_t s = hf->heads[column]; s != WPE__NO_SPAN; s = hf->spans[s].next) {
            wpe__Span* span = &hf->spans[s];
            const bool was_walkable = span->walkable;
            if (!span->walkable && below_walkable && span->smax - below_top <= climb) span->walkable = true;
            below_walkable = was_walkable;
            below_top = span->smax;
        }
    }
}

/* ---- Cells ----------------------------------------------------------------------------------------------
 */

typedef struct wpe__Build {
    WPE_NavGrid* grid;
    uint16_t climb; /* step_height in cell heights. */
    uint16_t clear; /* agent_height in cell heights. */
    uint32_t erode; /* Distance (in half cells, as wpe__erode measures) a cell must keep from an edge. */
} wpe__Build;

/* Turns the walkable span tops with room above them into cells. */
static WPE_Result wpe__make_cells(const wpe__Heightfield* hf, wpe__Build* b) {
    WPE_NavGrid* g = b->grid;
    const uint32_t columns = g->width * g->depth;
    g->columns = wpe_alloc_zeroed((size_t)columns + 1u, sizeof *g->columns);
    if (!g->columns) return WPE_RESULT_ERROR_OUT_OF_MEMORY;
    uint32_t total = 0;
    bool capped = false;
    for (int pass = 0; pass < 2; ++pass) {
        total = 0;
        for (uint32_t column = 0; column < columns; ++column) {
            if (pass == 0) g->columns[column] = total;
            if (!hf->spans) continue; /* Nothing was rasterized: every column is empty. */
            uint32_t layers = 0;
            for (uint32_t s = hf->heads[column]; s != WPE__NO_SPAN; s = hf->spans[s].next) {
                const wpe__Span* span = &hf->spans[s];
                if (!span->walkable) continue;
                const uint32_t ceiling =
                    span->next != WPE__NO_SPAN ? hf->spans[span->next].smin : WPE__NAV_OPEN_CEILING;
                if (ceiling - span->smax < b->clear) continue; /* No room to stand. */
                if (layers == WPE_NAV_MAX_LAYERS) {
                    capped = true;
                    break;
                }
                if (pass == 1) {
                    g->cells[total] = (wpe__NavCell){
                        .floor = span->smax,
                        .ceiling = (uint16_t)ceiling,
                        .links = {WPE__NAV_NO_LINK, WPE__NAV_NO_LINK, WPE__NAV_NO_LINK, WPE__NAV_NO_LINK}};
                }
                ++layers;
                ++total;
            }
        }
        if (pass == 0) {
            g->columns[columns] = total;
            g->cells = total ? wpe_alloc((size_t)total * sizeof *g->cells) : NULL;
            if (total && !g->cells) return WPE_RESULT_ERROR_OUT_OF_MEMORY;
        }
    }
    if (capped)
        wpe_log_warn("some navigation columns have more than %u floors; the highest are left out",
                     WPE_NAV_MAX_LAYERS);
    g->cell_count = total;
    return WPE_RESULT_OK;
}

/* Links each cell to the cell of each neighboring column it can walk to: floors within a step, and room
 * for the agent where both columns are open. */
static void wpe__link_cells(wpe__Build* b) {
    WPE_NavGrid* g = b->grid;
    for (uint32_t column = 0; column < g->width * g->depth; ++column) {
        for (uint32_t cell = g->columns[column]; cell < g->columns[column + 1]; ++cell) {
            wpe__NavCell* c = &g->cells[cell];
            for (int dir = 0; dir < 4; ++dir) {
                c->links[dir] = WPE__NAV_NO_LINK;
                const uint32_t nc = wpe__nav_neighbor_column(g, column, dir);
                if (nc == UINT32_MAX) continue;
                uint32_t best_rise = UINT32_MAX;
                for (uint32_t n = g->columns[nc]; n < g->columns[nc + 1]; ++n) {
                    const wpe__NavCell* other = &g->cells[n];
                    const uint32_t bottom = c->floor > other->floor ? c->floor : other->floor;
                    const uint32_t top = c->ceiling < other->ceiling ? c->ceiling : other->ceiling;
                    const uint32_t rise = c->floor > other->floor ? (uint32_t)(c->floor - other->floor)
                                                                  : (uint32_t)(other->floor - c->floor);
                    if (top < bottom || top - bottom < b->clear || rise > b->climb || rise >= best_rise)
                        continue;
                    best_rise = rise;
                    c->links[dir] = (uint8_t)(n - g->columns[nc]);
                }
            }
        }
    }
}

static uint8_t wpe__add_dist(uint8_t d, uint8_t add) {
    return (uint8_t)(d + add > 255 ? 255 : d + add);
}

/* Distance (in half cells: 2 per straight step, 3 per diagonal one) from every cell to the nearest edge
 * cell (a cell missing a link), by two chamfer sweeps. */
static void wpe__distances(const WPE_NavGrid* g, uint8_t* dist) {
    for (uint32_t i = 0; i < g->cell_count; ++i) {
        const wpe__NavCell* c = &g->cells[i];
        const bool edge = c->links[0] == WPE__NAV_NO_LINK || c->links[1] == WPE__NAV_NO_LINK ||
                          c->links[2] == WPE__NAV_NO_LINK || c->links[3] == WPE__NAV_NO_LINK;
        dist[i] = edge ? 0 : 255;
    }
    /* Sweep 1 from the low corner: the -X and -Z neighbors (and the diagonals past them). Sweep 2 from the
     * high corner: +X and +Z. */
    for (int sweep = 0; sweep < 2; ++sweep) {
        const int first = sweep == 0 ? WPE_NAV_NEG_X : WPE_NAV_POS_X;
        const int second = sweep == 0 ? WPE_NAV_NEG_Z : WPE_NAV_POS_Z;
        const uint32_t columns = g->width * g->depth;
        for (uint32_t k = 0; k < columns; ++k) {
            const uint32_t column = sweep == 0 ? k : columns - 1u - k;
            for (uint32_t cell = g->columns[column]; cell < g->columns[column + 1]; ++cell) {
                uint8_t d = dist[cell];
                /* Each straight neighbor, then the diagonal one step further from it: sweep 1 reaches
                 * (-1, 0), (-1, -1), (0, -1) and (1, -1); sweep 2 the mirror images. */
                const int straight[2] = {first, second};
                for (int s = 0; s < 2; ++s) {
                    const int dir = straight[s];
                    const uint32_t a = wpe__nav_neighbor(g, column, cell, dir);
                    if (a == WPE_NAV_CELL_NONE) continue;
                    d = d < wpe__add_dist(dist[a], 2) ? d : wpe__add_dist(dist[a], 2);
                    const int turn = s == 0 ? second : (first + 2) % 4;
                    const uint32_t ac = wpe__nav_neighbor_column(g, column, dir);
                    const uint32_t diagonal = wpe__nav_neighbor(g, ac, a, turn);
                    if (diagonal != WPE_NAV_CELL_NONE)
                        d = d < wpe__add_dist(dist[diagonal], 3) ? d : wpe__add_dist(dist[diagonal], 3);
                }
                dist[cell] = d;
            }
        }
    }
}

/* The distance (as wpe__distances measures it) a cell needs from the edge cells for its center to be
 * agent_radius from the edge itself: an edge cell's center is half a cell inside, each straight step adds a
 * cell (2 units). */
static uint32_t wpe__erode_distance(const WPE_NavSettings* s) {
    const float units = 2.0f * s->agent_radius / s->cell_size - 1.0f;
    return units > 0.0f ? (uint32_t)ceilf(units - 1e-4f) : 0u;
}

/* Removes the cells too close to an edge for the agent's radius, keeping the others' links. */
static WPE_Result wpe__erode(wpe__Build* b) {
    WPE_NavGrid* g = b->grid;
    if (b->erode == 0 || g->cell_count == 0) return WPE_RESULT_OK;
    uint8_t* dist = wpe_alloc(g->cell_count);
    uint32_t* remap = wpe_alloc((size_t)g->cell_count * sizeof *remap);
    uint32_t* columns = wpe_alloc(((size_t)g->width * g->depth + 1u) * sizeof *columns);
    if (!dist || !remap || !columns) {
        wpe_free(dist);
        wpe_free(remap);
        wpe_free(columns);
        return WPE_RESULT_ERROR_OUT_OF_MEMORY;
    }
    wpe__distances(g, dist);
    const uint32_t column_count = g->width * g->depth;
    uint32_t kept = 0;
    for (uint32_t column = 0; column < column_count; ++column) {
        columns[column] = kept;
        for (uint32_t cell = g->columns[column]; cell < g->columns[column + 1]; ++cell)
            remap[cell] = dist[cell] >= b->erode ? kept++ : WPE_NAV_CELL_NONE;
    }
    columns[column_count] = kept;
    /* Compact in place (kept cells only move down), remapping links into the new numbering. */
    for (uint32_t column = 0; column < column_count; ++column) {
        for (uint32_t cell = g->columns[column]; cell < g->columns[column + 1]; ++cell) {
            if (remap[cell] == WPE_NAV_CELL_NONE) continue;
            wpe__NavCell c = g->cells[cell];
            for (int dir = 0; dir < 4; ++dir) {
                const uint32_t n = wpe__nav_neighbor(g, column, cell, dir);
                if (n == WPE_NAV_CELL_NONE) continue;
                const uint32_t nc = wpe__nav_neighbor_column(g, column, dir);
                c.links[dir] =
                    remap[n] == WPE_NAV_CELL_NONE ? WPE__NAV_NO_LINK : (uint8_t)(remap[n] - columns[nc]);
            }
            g->cells[remap[cell]] = c;
        }
    }
    wpe_free(g->columns);
    g->columns = columns;
    g->cell_count = kept;
    wpe_free(dist);
    wpe_free(remap);
    return WPE_RESULT_OK;
}

/* Numbers the connected areas by flood fill, in cell order (so the numbering is stable). */
static WPE_Result wpe__find_islands(WPE_NavGrid* g) {
    g->island_count = 0;
    if (g->cell_count == 0) return WPE_RESULT_OK;
    uint32_t* stack = wpe_alloc((size_t)g->cell_count * sizeof *stack);
    uint32_t* column_of = wpe_alloc((size_t)g->cell_count * sizeof *column_of);
    if (!stack || !column_of) {
        wpe_free(stack);
        wpe_free(column_of);
        return WPE_RESULT_ERROR_OUT_OF_MEMORY;
    }
    for (uint32_t column = 0; column < g->width * g->depth; ++column) {
        for (uint32_t cell = g->columns[column]; cell < g->columns[column + 1]; ++cell)
            column_of[cell] = column;
        const uint32_t layers = g->columns[column + 1] - g->columns[column];
        if (layers > g->max_layers) g->max_layers = layers;
    }
    for (uint32_t i = 0; i < g->cell_count; ++i)
        g->cells[i].island = UINT32_MAX;
    for (uint32_t start = 0; start < g->cell_count; ++start) {
        if (g->cells[start].island != UINT32_MAX) continue;
        const uint32_t island = g->island_count++;
        uint32_t top = 0;
        stack[top++] = start;
        g->cells[start].island = island;
        while (top > 0) {
            const uint32_t cell = stack[--top];
            for (int dir = 0; dir < 4; ++dir) {
                const uint32_t n = wpe__nav_neighbor(g, column_of[cell], cell, dir);
                if (n == WPE_NAV_CELL_NONE || g->cells[n].island != UINT32_MAX) continue;
                g->cells[n].island = island;
                stack[top++] = n; /* Each cell is pushed once, so the stack never overflows. */
            }
        }
    }
    wpe_free(stack);
    wpe_free(column_of);
    return WPE_RESULT_OK;
}

/* ---- Building ----------------------------------------------------------------------------------------- */

/* Sizes the grid from the bounds. */
static WPE_Result wpe__size_grid(WPE_NavGrid* g, WPE_Vec3 min, WPE_Vec3 max, uint32_t* out_height) {
    const WPE_NavSettings* s = &g->settings;
    const double width = ceil(((double)max.x - min.x) / s->cell_size);
    const double depth = ceil(((double)max.z - min.z) / s->cell_size);
    const double height = ceil(((double)max.y - min.y) / s->cell_height) + 1.0;
    const double w = width < 1.0 ? 1.0 : width, d = depth < 1.0 ? 1.0 : depth;
    if (w * d > (double)WPE_NAV_MAX_COLUMNS) {
        wpe_log_error(
            "the navigation region (%.1f x %.1f) needs %.0f columns of %g; the most is %u: use larger "
            "cells or smaller bounds",
            (double)(max.x - min.x), (double)(max.z - min.z), w * d, (double)s->cell_size,
            WPE_NAV_MAX_COLUMNS);
        return WPE_RESULT_ERROR_CAPACITY;
    }
    if (height > (double)WPE__NAV_MAX_HEIGHT) {
        wpe_log_error("the navigation region is %.1f tall: more than %u cell heights of %g",
                      (double)(max.y - min.y), WPE__NAV_MAX_HEIGHT, (double)s->cell_height);
        return WPE_RESULT_ERROR_CAPACITY;
    }
    g->origin = min;
    g->width = (uint32_t)w;
    g->depth = (uint32_t)d;
    *out_height = (uint32_t)height;
    return WPE_RESULT_OK;
}

static WPE_Result wpe__rasterize(const WPE_NavGeometry* geometry, const wpe__Build* b, wpe__Heightfield* hf) {
    const WPE_NavGrid* g = b->grid;
    const float walkable_y = cosf(g->settings.max_slope * 3.14159265358979f / 180.0f);
    for (uint32_t t = 0; t < geometry->triangle_count && !hf->out_of_memory; ++t) {
        const wpe__NavTriangle* tri = &geometry->triangles[t];
        const WPE_Vec3 a = geometry->vertices[tri->v[0]], bv = geometry->vertices[tri->v[1]],
                       c = geometry->vertices[tri->v[2]];
        const WPE_Vec3 normal = wpe_vec3_normalize(wpe_vec3_cross(wpe_vec3_sub(bv, a), wpe_vec3_sub(c, a)));
        /* A tiny tolerance, so a 45-degree ramp counts as 45 degrees. */
        wpe__rasterize_triangle(hf, a, bv, c, normal, normal.y >= walkable_y - 1e-5f, b->climb);
    }
    return hf->out_of_memory ? WPE_RESULT_ERROR_OUT_OF_MEMORY : WPE_RESULT_OK;
}

static WPE_Result wpe__build(const WPE_NavGeometry* geometry, wpe__Build* b) {
    WPE_NavGrid* g = b->grid;
    const WPE_NavSettings* s = &g->settings;
    WPE_Vec3 min = s->bounds_min, max = s->bounds_max;
    if (!wpe__has_bounds(s)) {
        min = geometry->min;
        max = geometry->max;
        g->settings.bounds_min = min;
        g->settings.bounds_max = max;
    }
    uint32_t height = 0;
    WPE_Result result = wpe__size_grid(g, min, max, &height);
    if (result != WPE_RESULT_OK) return result;

    wpe__Heightfield hf = {.width = g->width,
                           .depth = g->depth,
                           .height = height,
                           .origin = g->origin,
                           .cell_size = s->cell_size,
                           .cell_height = s->cell_height,
                           .free_spans = WPE__NO_SPAN};
    hf.heads = wpe_alloc((size_t)g->width * g->depth * sizeof *hf.heads);
    if (!hf.heads) return WPE_RESULT_ERROR_OUT_OF_MEMORY;
    memset(hf.heads, 0xFF, (size_t)g->width * g->depth * sizeof *hf.heads); /* WPE__NO_SPAN everywhere. */
    result = wpe__rasterize(geometry, b, &hf);
    if (result == WPE_RESULT_OK) {
        wpe__filter_low_obstacles(&hf, b->climb);
        result = wpe__make_cells(&hf, b);
    }
    wpe_free(hf.heads);
    wpe_free(hf.spans);
    if (result != WPE_RESULT_OK) return result;
    wpe__link_cells(b);
    result = wpe__erode(b);
    if (result == WPE_RESULT_OK) result = wpe__find_islands(g);
    return result;
}

WPE_Result wpe_nav_grid_build(const WPE_NavGeometry* geometry, const WPE_NavSettings* settings,
                              WPE_NavGrid** out_grid) {
    if (out_grid) *out_grid = NULL;
    if (!geometry || !out_grid) return WPE_RESULT_ERROR_NULL_ARG;
    const WPE_NavSettings s = settings ? *settings : wpe_nav_settings_default();
    WPE_Result result = wpe_nav_settings_validate(&s);
    if (result != WPE_RESULT_OK) return result;
    if (geometry->triangle_count == 0) {
        wpe_log_error("no geometry to build navigation from (no static colliders?)");
        return WPE_RESULT_ERROR_NOT_FOUND;
    }
    WPE_NavGrid* grid = wpe_alloc_zeroed(1, sizeof *grid);
    if (!grid) return WPE_RESULT_ERROR_OUT_OF_MEMORY;
    grid->settings = s;
    wpe__Build b = {.grid = grid,
                    .climb = (uint16_t)fminf(floorf(s.step_height / s.cell_height + 1e-4f), 65535.0f),
                    .clear = (uint16_t)fminf(ceilf(s.agent_height / s.cell_height - 1e-4f), 65535.0f),
                    .erode = wpe__erode_distance(&s)};
    result = wpe__build(geometry, &b);
    if (result != WPE_RESULT_OK) {
        if (result == WPE_RESULT_ERROR_OUT_OF_MEMORY) wpe_log_error("out of memory building navigation");
        wpe_nav_grid_destroy(grid);
        return result;
    }
    wpe_log_info("navigation grid %u x %u: %u walkable cells in %u islands", grid->width, grid->depth,
                 grid->cell_count, grid->island_count);
    *out_grid = grid;
    return WPE_RESULT_OK;
}

WPE_Result wpe_nav_grid_build_from_world(WPE_Ecs* ecs, const WPE_AssetManager* assets,
                                         const WPE_NavSettings* settings, WPE_NavGrid** out_grid) {
    if (out_grid) *out_grid = NULL;
    if (!ecs || !out_grid) return WPE_RESULT_ERROR_NULL_ARG;
    WPE_NavGeometry* geometry = NULL;
    WPE_Result result = wpe_nav_geometry_create(&geometry);
    if (result == WPE_RESULT_OK) result = wpe_nav_geometry_add_colliders(geometry, ecs, assets, NULL);
    if (result == WPE_RESULT_OK) result = wpe_nav_grid_build(geometry, settings, out_grid);
    wpe_nav_geometry_destroy(geometry);
    return result;
}

void wpe_nav_grid_destroy(WPE_NavGrid* grid) {
    if (!grid) return;
    wpe_free(grid->columns);
    wpe_free(grid->cells);
    wpe_free(grid);
}
