#define WPE_LOG_MODULE "navigation"
#include <math.h>
#include <string.h>

#include "core/alloc.h"
#include "core/log.h"
#include "navigation/nav_internal.h"
#include "navigation/nav_path.h"

#define WPE__GOAL_SEARCH 1.0f /* How far (sideways) a goal may be from the walkable area. */
#define WPE__LOOKAHEAD 8u     /* Steps down the flow a direction may aim at. */

typedef struct wpe__FlowGoal {
    WPE_Vec3 point; /* On its cell's floor. */
    uint32_t cell;
} wpe__FlowGoal;

struct WPE_NavFlowField {
    const WPE_NavGrid* grid;
    wpe__NavNode*
        nodes; /* key: the distance to the nearest goal (INFINITY: unreached); parent: the next cell. */
    uint32_t* items;
    wpe__FlowGoal* goals;
    uint32_t goal_count, goal_capacity;
};

WPE_Result wpe_nav_flow_create(const WPE_NavGrid* grid, WPE_NavFlowField** out_field) {
    if (out_field) *out_field = NULL;
    if (!grid || !out_field) return WPE_RESULT_ERROR_NULL_ARG;
    WPE_NavFlowField* f = wpe_alloc_zeroed(1, sizeof *f);
    if (!f) return WPE_RESULT_ERROR_OUT_OF_MEMORY;
    f->grid = grid;
    const size_t n = grid->cell_count ? grid->cell_count : 1u;
    f->nodes = wpe_alloc(n * sizeof *f->nodes);
    f->items = wpe_alloc(n * sizeof *f->items);
    if (!f->nodes || !f->items) {
        wpe_nav_flow_destroy(f);
        return WPE_RESULT_ERROR_OUT_OF_MEMORY;
    }
    for (uint32_t i = 0; i < grid->cell_count; ++i)
        f->nodes[i] =
            (wpe__NavNode){.key = INFINITY, .pos = WPE__NAV_NOT_QUEUED, .parent = WPE_NAV_CELL_NONE};
    *out_field = f;
    return WPE_RESULT_OK;
}

void wpe_nav_flow_destroy(WPE_NavFlowField* field) {
    if (!field) return;
    wpe_free(field->nodes);
    wpe_free(field->items);
    wpe_free(field->goals);
    wpe_free(field);
}

static bool wpe__finite3(WPE_Vec3 v) {
    return isfinite(v.x) && isfinite(v.y) && isfinite(v.z);
}

WPE_Result wpe_nav_flow_build(WPE_NavFlowField* field, const WPE_Vec3* goals, uint32_t goal_count,
                              float max_distance) {
    if (!field || (goal_count && !goals)) return WPE_RESULT_ERROR_NULL_ARG;
    if (!(max_distance >= 0.0f)) return WPE_RESULT_ERROR_INVALID_ARG;
    for (uint32_t i = 0; i < goal_count; ++i) {
        if (!wpe__finite3(goals[i])) return WPE_RESULT_ERROR_INVALID_ARG;
    }
    const WPE_NavGrid* g = field->grid;
    if (goal_count > field->goal_capacity) {
        wpe__FlowGoal* bigger = wpe_realloc(field->goals, (size_t)goal_count * sizeof *bigger);
        if (!bigger) return WPE_RESULT_ERROR_OUT_OF_MEMORY;
        field->goals = bigger;
        field->goal_capacity = goal_count;
    }
    wpe__NavNode* nodes = field->nodes;
    for (uint32_t i = 0; i < g->cell_count; ++i)
        nodes[i] = (wpe__NavNode){.key = INFINITY, .pos = WPE__NAV_NOT_QUEUED, .parent = WPE_NAV_CELL_NONE};
    wpe__NavHeap heap = {.items = field->items, .nodes = nodes};
    field->goal_count = 0;
    const float standing = g->settings.agent_height * 0.5f;
    for (uint32_t i = 0; i < goal_count; ++i) {
        WPE_Vec3 point;
        const uint32_t cell = wpe_nav_grid_nearest(g, goals[i], WPE__GOAL_SEARCH, standing, &point);
        if (cell == WPE_NAV_CELL_NONE) continue;
        field->goals[field->goal_count++] = (wpe__FlowGoal){.point = point, .cell = cell};
        if (nodes[cell].key == 0.0f) continue; /* Two goals in one cell. */
        nodes[cell].key = 0.0f;
        nodes[cell].parent = cell;
        wpe__nav_heap_push(&heap, cell);
    }
    if (field->goal_count == 0) return WPE_RESULT_ERROR_NOT_FOUND;
    /* Dijkstra: cells leave the heap in order of distance, each final when it does. */
    while (heap.count > 0) {
        const uint32_t cell = wpe__nav_heap_pop(&heap);
        uint32_t moves[8], straight = 0;
        const uint32_t n = wpe__nav_moves(g, cell, moves, &straight);
        for (uint32_t m = 0; m < n; ++m) {
            const uint32_t other = moves[m];
            const float d = nodes[cell].key + wpe__nav_move_cost(g, cell, other, m >= straight);
            if (d >= nodes[other].key || (max_distance > 0.0f && d > max_distance)) continue;
            nodes[other].key = d;
            nodes[other].parent = cell; /* Walking back the way the search came leads to the goal. */
            wpe__nav_heap_push(&heap, other);
        }
    }
    return WPE_RESULT_OK;
}

float wpe_nav_flow_distance(const WPE_NavFlowField* field, uint32_t cell) {
    if (!field || cell >= field->grid->cell_count) return INFINITY;
    return field->nodes[cell].key;
}

uint32_t wpe_nav_flow_next(const WPE_NavFlowField* field, uint32_t cell) {
    if (!field || cell >= field->grid->cell_count) return WPE_NAV_CELL_NONE;
    return field->nodes[cell].parent;
}

/* The goal point in a goal's cell. */
static WPE_Vec3 wpe__goal_point(const WPE_NavFlowField* field, uint32_t cell) {
    for (uint32_t i = 0; i < field->goal_count; ++i) {
        if (field->goals[i].cell == cell) return field->goals[i].point;
    }
    return wpe__nav_center(field->grid, cell); /* Not reached: every goal cell has its goal. */
}

bool wpe_nav_flow_direction(const WPE_NavFlowField* field, WPE_Vec3 position, WPE_Vec3* out_direction,
                            float* out_distance) {
    if (out_direction) *out_direction = wpe_vec3(0, 0, 0);
    if (out_distance) *out_distance = INFINITY;
    if (!field || !out_direction) return false;
    const WPE_NavGrid* g = field->grid;
    WPE_Vec3 here;
    const uint32_t cell =
        wpe_nav_grid_nearest(g, position, WPE__GOAL_SEARCH, g->settings.agent_height * 0.5f, &here);
    if (cell == WPE_NAV_CELL_NONE || field->nodes[cell].parent == WPE_NAV_CELL_NONE) return false;
    if (out_distance) *out_distance = field->nodes[cell].key;
    /* Aim at the farthest of the next few cells (or the goal point) in straight walkable sight. */
    WPE_Vec3 target = field->nodes[cell].parent == cell ? wpe__goal_point(field, cell)
                                                        : wpe__nav_center(g, field->nodes[cell].parent);
    uint32_t step = field->nodes[cell].parent;
    for (uint32_t i = 1; i < WPE__LOOKAHEAD && field->nodes[step].parent != step; ++i) {
        const uint32_t further = field->nodes[step].parent;
        const WPE_Vec3 point = field->nodes[further].parent == further ? wpe__goal_point(field, further)
                                                                       : wpe__nav_center(g, further);
        if (!wpe__nav_line(g, cell, here, point, further, 0.0f)) break;
        target = point;
        step = further;
    }
    if (field->nodes[step].parent == step && step != cell) {
        const WPE_Vec3 goal = wpe__goal_point(field, step); /* The goal itself, when in sight. */
        if (wpe__nav_line(g, cell, here, goal, step, 0.0f)) target = goal;
    }
    const WPE_Vec3 d = wpe_vec3(target.x - here.x, 0.0f, target.z - here.z);
    const float length = wpe_vec3_length(d);
    if (length > 1e-5f) *out_direction = wpe_vec3_scale(d, 1.0f / length);
    return true;
}
