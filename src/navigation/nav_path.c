#define WPE_LOG_MODULE "navigation"
#include "navigation/nav_path.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "core/alloc.h"
#include "core/log.h"
#include "navigation/nav_internal.h"

#define WPE__CLOSED (UINT32_MAX - 1u) /* wpe__NavHeap.pos of a cell A* has finished with. */
#define WPE__INSET 1e-3f              /* Points clamped into a cell stay this fraction of a cell inside it. */

/* ---- Heap ----------------------------------------------------------------------------------------- */

static bool wpe__heap_less(const wpe__NavHeap* h, uint32_t a, uint32_t b) {
    const wpe__NavNode *na = &h->nodes[a], *nb = &h->nodes[b];
    if (na->key != nb->key) return na->key < nb->key;
    if (na->g != nb->g) return na->g > nb->g;
    return a < b;
}

static void wpe__heap_place(wpe__NavHeap* h, uint32_t slot, uint32_t cell) {
    h->items[slot] = cell;
    h->nodes[cell].pos = slot;
}

static void wpe__heap_up(wpe__NavHeap* h, uint32_t slot) {
    const uint32_t cell = h->items[slot];
    while (slot > 0) {
        const uint32_t parent = (slot - 1u) / 2u;
        if (!wpe__heap_less(h, cell, h->items[parent])) break;
        wpe__heap_place(h, slot, h->items[parent]);
        slot = parent;
    }
    wpe__heap_place(h, slot, cell);
}

void wpe__nav_heap_push(wpe__NavHeap* heap, uint32_t cell) {
    uint32_t slot = heap->nodes[cell].pos;
    if (slot == WPE__NAV_NOT_QUEUED || slot == WPE__CLOSED) {
        slot = heap->count++;
        heap->items[slot] = cell;
    }
    wpe__heap_up(heap, slot);
}

uint32_t wpe__nav_heap_pop(wpe__NavHeap* heap) {
    const uint32_t top = heap->items[0];
    heap->nodes[top].pos = WPE__NAV_NOT_QUEUED;
    const uint32_t last = heap->items[--heap->count];
    if (heap->count == 0) return top;
    uint32_t slot = 0;
    for (;;) {
        const uint32_t left = slot * 2u + 1u, right = left + 1u;
        uint32_t smallest = last;
        uint32_t target = slot;
        if (left < heap->count && wpe__heap_less(heap, heap->items[left], smallest)) {
            smallest = heap->items[left];
            target = left;
        }
        if (right < heap->count && wpe__heap_less(heap, heap->items[right], smallest)) target = right;
        if (target == slot) break;
        wpe__heap_place(heap, slot, heap->items[target]);
        slot = target;
    }
    wpe__heap_place(heap, slot, last);
    return top;
}

/* ---- Points and lines ------------------------------------------------------------------------------- */

static bool wpe__finite3(WPE_Vec3 v) {
    return isfinite(v.x) && isfinite(v.y) && isfinite(v.z);
}

/* How far below or above a floor a point still stands on it. */
static float wpe__standing(const WPE_NavGrid* g) {
    return g->settings.agent_height * 0.5f;
}

static int64_t wpe__abs64(int64_t v) {
    return v < 0 ? -v : v;
}

static float wpe__clampf(float v, float lo, float hi) {
    return v < lo ? lo : (v > hi ? hi : v);
}

/* The point of a cell's square nearest to `p` (slightly inside it), on its floor. */
static WPE_Vec3 wpe__clamp_to_cell(const WPE_NavGrid* g, uint32_t cell, WPE_Vec3 p) {
    const WPE_Vec3 c = wpe__nav_center(g, cell);
    const float half = g->settings.cell_size * (0.5f - WPE__INSET);
    return wpe_vec3(wpe__clampf(p.x, c.x - half, c.x + half), c.y, wpe__clampf(p.z, c.z - half, c.z + half));
}

uint32_t wpe_nav_grid_nearest(const WPE_NavGrid* grid, WPE_Vec3 point, float max_horizontal,
                              float max_vertical, WPE_Vec3* out_point) {
    if (!grid || grid->cell_count == 0 || !wpe__finite3(point) || !(max_horizontal >= 0.0f) ||
        !(max_vertical >= 0.0f) || !isfinite(max_horizontal))
        return WPE_NAV_CELL_NONE;
    const float cs = grid->settings.cell_size;
    const double fx = floor(((double)point.x - grid->origin.x) / cs);
    const double fz = floor(((double)point.z - grid->origin.z) / cs);
    const int64_t rings = (int64_t)ceil((double)max_horizontal / cs) + 1;
    /* Search rings of columns outward; stop once a ring can't hold anything nearer than the best. */
    uint32_t best = WPE_NAV_CELL_NONE;
    float best_d2 = INFINITY;
    const float max_h2 = max_horizontal * max_horizontal;
    for (int64_t ring = 0; ring <= rings; ++ring) {
        const float closest = (float)(ring > 0 ? ring - 1 : 0) * cs; /* No column of this ring is nearer. */
        if (closest * closest > best_d2 || closest * closest > max_h2) break;
        for (int64_t dz = -ring; dz <= ring; ++dz) {
            const int64_t step = (dz == -ring || dz == ring) ? 1 : 2 * ring; /* Only the ring's border. */
            for (int64_t dx = -ring; dx <= ring; dx += step > 0 ? step : 1) {
                const double cx = fx + (double)dx, cz = fz + (double)dz;
                if (cx < 0 || cz < 0 || cx >= grid->width || cz >= grid->depth) continue;
                const uint32_t column = (uint32_t)cx + (uint32_t)cz * grid->width;
                for (uint32_t cell = grid->columns[column]; cell < grid->columns[column + 1]; ++cell) {
                    const WPE_Vec3 q = wpe__clamp_to_cell(grid, cell, point);
                    const float dy = q.y - point.y;
                    if (fabsf(dy) > max_vertical) continue;
                    const float h2 = (q.x - point.x) * (q.x - point.x) + (q.z - point.z) * (q.z - point.z);
                    if (h2 > max_h2) continue;
                    const float d2 = h2 + dy * dy;
                    if (d2 < best_d2) { /* Cells come in increasing order per ring, so ties keep the lower. */
                        best_d2 = d2;
                        best = cell;
                    }
                }
            }
        }
    }
    if (best != WPE_NAV_CELL_NONE && out_point) {
        const WPE_Vec3 q = wpe__clamp_to_cell(grid, best, point);
        /* A point already inside its cell keeps its exact position (on the floor). */
        *out_point = q;
    }
    return best;
}

bool wpe_nav_grid_reachable(const WPE_NavGrid* grid, uint32_t from_cell, uint32_t to_cell) {
    if (!grid || from_cell >= grid->cell_count || to_cell >= grid->cell_count) return false;
    return grid->cells[from_cell].island == grid->cells[to_cell].island;
}

/* The column coordinate of a world coordinate (in cells, fractional). */
static double wpe__cell_coordinate(float world, float origin, float cell_size) {
    return ((double)world - origin) / cell_size;
}

bool wpe__nav_line(const WPE_NavGrid* g, uint32_t from_cell, WPE_Vec3 from, WPE_Vec3 to, uint32_t to_cell,
                   float max_vertical) {
    const float cs = g->settings.cell_size;
    const WPE_Vec3 start = wpe__clamp_to_cell(g, from_cell, from);
    const double u0 = wpe__cell_coordinate(start.x, g->origin.x, cs),
                 v0 = wpe__cell_coordinate(start.z, g->origin.z, cs);
    const double u1 = wpe__cell_coordinate(to.x, g->origin.x, cs),
                 v1 = wpe__cell_coordinate(to.z, g->origin.z, cs);
    int64_t ix = (int64_t)(g->cell_columns[from_cell] % g->width),
            iz = (int64_t)(g->cell_columns[from_cell] / g->width);
    int64_t tx, tz;
    if (to_cell != WPE_NAV_CELL_NONE) {
        tx = (int64_t)(g->cell_columns[to_cell] % g->width);
        tz = (int64_t)(g->cell_columns[to_cell] / g->width);
    } else {
        tx = (int64_t)floor(u1);
        tz = (int64_t)floor(v1);
        if (tx < 0 || tz < 0 || tx >= g->width || tz >= g->depth) return false;
    }
    /* Amanatides-Woo traversal: t of the next X and Z column boundary along the line (t in [0, 1]). */
    const double du = u1 - u0, dv = v1 - v0;
    const int step_x = du > 0 ? 1 : -1, step_z = dv > 0 ? 1 : -1;
    const double delta_x = du != 0 ? 1.0 / fabs(du) : INFINITY, delta_z = dv != 0 ? 1.0 / fabs(dv) : INFINITY;
    double next_x = du > 0 ? ((double)ix + 1.0 - u0) / du : du < 0 ? (u0 - (double)ix) / -du : INFINITY;
    double next_z = dv > 0 ? ((double)iz + 1.0 - v0) / dv : dv < 0 ? (v0 - (double)iz) / -dv : INFINITY;
    const int dir_x = step_x > 0 ? WPE_NAV_POS_X : WPE_NAV_NEG_X,
              dir_z = step_z > 0 ? WPE_NAV_POS_Z : WPE_NAV_NEG_Z;
    const double span = fmax(fabs(du), fabs(dv)); /* Cells per unit of t along the longer axis. */
    uint32_t cell = from_cell;
    const int64_t steps = wpe__abs64(tx - ix) + wpe__abs64(tz - iz);
    for (int64_t s = 0; s <= steps && (ix != tx || iz != tz); ++s) {
        const uint32_t column = g->cell_columns[cell];
        /* An axis whose target column is reached takes no more steps (an end exactly on a column line
         * would otherwise cross it at t = 1). */
        const bool x_done = ix == tx, z_done = iz == tz;
        /* Within a ten-thousandth of a cell of a corner counts as through it (cell centers carry float
         * rounding, and a line between diagonal neighbors' centers must not slip past the corner one way). */
        if (!x_done && !z_done && fabs(next_x - next_z) * span < 1e-4) {
            /* Through a corner: both ways around it must be open and meet. */
            const uint32_t a = wpe__nav_neighbor(g, column, cell, dir_x);
            const uint32_t b = wpe__nav_neighbor(g, column, cell, dir_z);
            if (a == WPE_NAV_CELL_NONE || b == WPE_NAV_CELL_NONE) return false;
            const uint32_t via_a = wpe__nav_neighbor(g, g->cell_columns[a], a, dir_z);
            const uint32_t via_b = wpe__nav_neighbor(g, g->cell_columns[b], b, dir_x);
            if (via_a == WPE_NAV_CELL_NONE || via_a != via_b) return false;
            cell = via_a;
            ix += step_x;
            iz += step_z;
            next_x += delta_x;
            next_z += delta_z;
            ++s; /* Two columns crossed. */
        } else if (z_done || (!x_done && next_x < next_z)) {
            cell = wpe__nav_neighbor(g, column, cell, dir_x);
            ix += step_x;
            next_x += delta_x;
        } else {
            cell = wpe__nav_neighbor(g, column, cell, dir_z);
            iz += step_z;
            next_z += delta_z;
        }
        if (cell == WPE_NAV_CELL_NONE) return false;
    }
    if (ix != tx || iz != tz) return false; /* Float trouble at the very end: say no. */
    if (to_cell != WPE_NAV_CELL_NONE) return cell == to_cell;
    return fabsf(wpe__nav_center(g, cell).y - to.y) <= max_vertical;
}

bool wpe_nav_grid_line_walkable(const WPE_NavGrid* grid, WPE_Vec3 from, WPE_Vec3 to) {
    if (!grid || !wpe__finite3(from) || !wpe__finite3(to)) return false;
    const uint32_t a = wpe_nav_grid_find_cell(grid, from, wpe__standing(grid));
    const uint32_t b = wpe_nav_grid_find_cell(grid, to, wpe__standing(grid));
    if (a == WPE_NAV_CELL_NONE || b == WPE_NAV_CELL_NONE) return false;
    return wpe__nav_line(grid, a, from, to, b, 0.0f);
}

/* ---- Paths ----------------------------------------------------------------------------------------- */

struct WPE_NavQuery {
    const WPE_NavGrid* grid;
    wpe__NavNode* nodes; /* Per cell; a node whose stamp isn't the current search is untouched. */
    uint32_t* items;     /* The heap. */
    uint32_t* chain;     /* The found cells, start first. */
    uint32_t* xz;        /* Each cell's column x | z << 16 (for the heuristic). */
    uint32_t search;
    WPE_Vec3* points;
    uint32_t point_capacity;
};
WPE_NavPathOptions wpe_nav_path_options_default(void) {
    return (WPE_NavPathOptions){.search_radius = 1.0f};
}

WPE_Result wpe_nav_query_create(const WPE_NavGrid* grid, WPE_NavQuery** out_query) {
    if (out_query) *out_query = NULL;
    if (!grid || !out_query) return WPE_RESULT_ERROR_NULL_ARG;
    WPE_NavQuery* q = wpe_alloc_zeroed(1, sizeof *q);
    if (!q) return WPE_RESULT_ERROR_OUT_OF_MEMORY;
    q->grid = grid;
    const size_t n = grid->cell_count ? grid->cell_count : 1u;
    q->nodes = wpe_alloc_zeroed(n, sizeof *q->nodes); /* Stamp 0: no search has touched them. */
    q->items = wpe_alloc(n * sizeof *q->items);
    q->chain = wpe_alloc(n * sizeof *q->chain);
    q->xz = wpe_alloc(n * sizeof *q->xz);
    if (!q->nodes || !q->items || !q->chain || !q->xz) {
        wpe_nav_query_destroy(q);
        return WPE_RESULT_ERROR_OUT_OF_MEMORY;
    }
    for (uint32_t i = 0; i < grid->cell_count; ++i) /* Grids are at most 65535 columns each way. */
        q->xz[i] = (grid->cell_columns[i] % grid->width) | (grid->cell_columns[i] / grid->width) << 16;
    *out_query = q;
    return WPE_RESULT_OK;
}

void wpe_nav_query_destroy(WPE_NavQuery* query) {
    if (!query) return;
    wpe_free(query->nodes);
    wpe_free(query->items);
    wpe_free(query->chain);
    wpe_free(query->xz);
    wpe_free(query->points);
    wpe_free(query);
}

const WPE_NavGrid* wpe_nav_query_grid(const WPE_NavQuery* query) {
    return query ? query->grid : NULL;
}

/* Octile distance between two cells' columns: a lower bound of any path's length between them. */
static float wpe__heuristic(const WPE_NavQuery* q, uint32_t a, uint32_t b) {
    const uint32_t pa = q->xz[a], pb = q->xz[b];
    const int32_t dx = abs((int32_t)(pa & 0xFFFFu) - (int32_t)(pb & 0xFFFFu));
    const int32_t dz = abs((int32_t)(pa >> 16) - (int32_t)(pb >> 16));
    const int32_t lo = dx < dz ? dx : dz, hi = dx < dz ? dz : dx;
    return ((float)hi + 0.41421356f * (float)lo) * q->grid->settings.cell_size;
}

static bool wpe__add_point(WPE_NavQuery* q, uint32_t* count, WPE_Vec3 p) {
    if (*count == q->point_capacity) {
        const uint32_t grown = q->point_capacity ? q->point_capacity * 2u : 64u;
        WPE_Vec3* bigger = wpe_realloc(q->points, (size_t)grown * sizeof *bigger);
        if (!bigger) return false;
        q->points = bigger;
        q->point_capacity = grown;
    }
    q->points[(*count)++] = p;
    return true;
}

/* A* from `start` to `goal`. @return The cell it ended at: the goal, or (when it can't be reached) the one
 * nearest to it by the heuristic; the chain is in q->chain[0 .. *out_length - 1]. */
static uint32_t wpe__search(WPE_NavQuery* q, uint32_t start, uint32_t goal, uint32_t max_visited,
                            uint32_t* out_length, uint32_t* out_visited) {
    const WPE_NavGrid* g = q->grid;
    wpe__NavNode* nodes = q->nodes;
    if (++q->search == 0) { /* Wrapped: every stamp is stale again. */
        for (uint32_t i = 0; i < g->cell_count; ++i)
            nodes[i].stamp = 0;
        q->search = 1;
    }
    const uint32_t id = q->search;
    wpe__NavHeap heap = {.items = q->items, .nodes = nodes};
    nodes[start] = (wpe__NavNode){.key = wpe__heuristic(q, start, goal),
                                  .g = 0.0f,
                                  .pos = WPE__NAV_NOT_QUEUED,
                                  .parent = WPE_NAV_CELL_NONE,
                                  .stamp = id};
    wpe__nav_heap_push(&heap, start);
    uint32_t best = start, visited = 0;
    float best_h = nodes[start].key;
    while (heap.count > 0) {
        const uint32_t cell = wpe__nav_heap_pop(&heap);
        wpe__NavNode* node = &nodes[cell];
        node->pos = WPE__CLOSED;
        ++visited;
        if (cell == goal) {
            best = goal;
            break;
        }
        const float h = node->key - node->g;
        if (h < best_h || (h == best_h && node->g < nodes[best].g)) {
            best_h = h;
            best = cell;
        }
        if (max_visited && visited >= max_visited) break;
        uint32_t moves[8], straight = 0;
        const uint32_t n = wpe__nav_moves(g, cell, moves, &straight);
        for (uint32_t m = 0; m < n; ++m) {
            const uint32_t next = moves[m];
            wpe__NavNode* other = &nodes[next];
            const float cost = node->g + wpe__nav_move_cost(g, cell, next, m >= straight);
            if (other->stamp == id) {
                if (other->pos == WPE__CLOSED || cost >= other->g) continue;
            } else {
                other->stamp = id;
                other->pos = WPE__NAV_NOT_QUEUED;
            }
            other->g = cost;
            other->key = cost + wpe__heuristic(q, next, goal);
            other->parent = cell;
            wpe__nav_heap_push(&heap, next);
        }
    } /* Walk back from where it ended, then reverse. */
    uint32_t length = 0;
    for (uint32_t c = best; c != WPE_NAV_CELL_NONE; c = nodes[c].parent)
        q->chain[length++] = c;
    for (uint32_t i = 0; i < length / 2u; ++i) {
        const uint32_t t = q->chain[i];
        q->chain[i] = q->chain[length - 1u - i];
        q->chain[length - 1u - i] = t;
    }
    *out_length = length;
    *out_visited = visited;
    return best;
}

/* The chain's point i: the start and end points at its ends, cell centers between. */
static WPE_Vec3 wpe__chain_point(const WPE_NavQuery* q, uint32_t i, uint32_t length, WPE_Vec3 start,
                                 WPE_Vec3 end) {
    if (i == 0) return start;
    if (i == length - 1u) return end;
    return wpe__nav_center(q->grid, q->chain[i]);
}

/* Pulls the chain straight: from each corner, goes on to the farthest chain point in straight walkable
 * sight. */
static bool wpe__smooth(WPE_NavQuery* q, uint32_t length, WPE_Vec3 start, WPE_Vec3 end, uint32_t* count) {
    uint32_t anchor = 0;
    if (!wpe__add_point(q, count, start)) return false;
    while (anchor + 1u < length) {
        const WPE_Vec3 from = wpe__chain_point(q, anchor, length, start, end);
        uint32_t reach = anchor + 1u; /* A neighbor is always in sight. */
        for (uint32_t j = anchor + 2u; j < length; ++j) {
            if (!wpe__nav_line(q->grid, q->chain[anchor], from, wpe__chain_point(q, j, length, start, end),
                               q->chain[j], 0.0f))
                break;
            reach = j;
        }
        if (!wpe__add_point(q, count, wpe__chain_point(q, reach, length, start, end))) return false;
        anchor = reach;
    }
    return true;
}

WPE_Result wpe_nav_find_path(WPE_NavQuery* query, WPE_Vec3 start, WPE_Vec3 goal,
                             const WPE_NavPathOptions* options, WPE_NavPath* out_path) {
    if (out_path) *out_path = (WPE_NavPath){0};
    if (!query || !out_path) return WPE_RESULT_ERROR_NULL_ARG;
    const WPE_NavPathOptions o = options ? *options : wpe_nav_path_options_default();
    if (!wpe__finite3(start) || !wpe__finite3(goal) || !(o.search_radius >= 0.0f) ||
        !isfinite(o.search_radius))
        return WPE_RESULT_ERROR_INVALID_ARG;
    const WPE_NavGrid* g = query->grid;
    WPE_Vec3 start_point, goal_point;
    const uint32_t start_cell =
        wpe_nav_grid_nearest(g, start, o.search_radius, wpe__standing(g), &start_point);
    const uint32_t goal_cell = wpe_nav_grid_nearest(g, goal, o.search_radius, wpe__standing(g), &goal_point);
    if (start_cell == WPE_NAV_CELL_NONE || goal_cell == WPE_NAV_CELL_NONE) return WPE_RESULT_ERROR_NOT_FOUND;
    const bool same_island = wpe_nav_grid_reachable(g, start_cell, goal_cell);
    if (!same_island && !o.allow_partial) return WPE_RESULT_ERROR_NOT_FOUND; /* No search needed. */

    uint32_t length = 0, visited = 0;
    const uint32_t end_cell = wpe__search(query, start_cell, goal_cell, o.max_visited, &length, &visited);
    const bool partial = end_cell != goal_cell;
    if (partial && !o.allow_partial) return WPE_RESULT_ERROR_NOT_FOUND;
    /* A partial path ends at the point of its last cell nearest to the goal. */
    const WPE_Vec3 end_point = partial ? wpe__clamp_to_cell(g, end_cell, goal_point) : goal_point;

    uint32_t count = 0;
    bool ok = true;
    if (length == 1) {
        ok = wpe__add_point(query, &count, start_point);
        const WPE_Vec3 d = wpe_vec3_sub(end_point, start_point);
        if (ok && wpe_vec3_length_sq(d) > 0.0f) ok = wpe__add_point(query, &count, end_point);
    } else if (o.raw) {
        for (uint32_t i = 0; i < length && ok; ++i)
            ok = wpe__add_point(query, &count, wpe__chain_point(query, i, length, start_point, end_point));
    } else {
        ok = wpe__smooth(query, length, start_point, end_point, &count);
    }
    if (!ok) {
        wpe_log_error("out of memory storing a path of %u cells", length);
        return WPE_RESULT_ERROR_OUT_OF_MEMORY;
    }
    float total = 0.0f;
    for (uint32_t i = 1; i < count; ++i)
        total += wpe_vec3_length(wpe_vec3_sub(query->points[i], query->points[i - 1u]));
    *out_path = (WPE_NavPath){.points = query->points,
                              .count = count,
                              .length = total,
                              .partial = partial,
                              .start_cell = start_cell,
                              .end_cell = end_cell,
                              .visited = visited};
    return WPE_RESULT_OK;
}
