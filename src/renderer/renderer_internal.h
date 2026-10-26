/**
 * @file renderer_internal.h
 * @brief Private data structures shared by the renderer's translation units. Not a public API.
 *
 * Pipeline stage ownership:
 * - renderer.c: frame lifecycle, draw sorting, job planning, binning (serial stages)
 * - geometry.c: vertex shading and clip/cull/setup (parallel stages)
 * - raster.c: per-tile rasterization, depth test, fragment shading, blending (parallel stage)
 *
 * @author WillPowerEngine Team
 */
#ifndef WPE_RENDERER_RENDERER_INTERNAL_H
#define WPE_RENDERER_RENDERER_INTERNAL_H

#include <stdatomic.h>

#include "core/arena.h"
#include "renderer/renderer.h"

/* Sub-pixel precision of screen-space vertex positions (4 bits = 1/16 pixel). */
#define WPE__RASTER_SUBPIXEL_BITS 4
#define WPE__RASTER_SUBPIXEL_ONE (1 << WPE__RASTER_SUBPIXEL_BITS)

/*
 * Guard band: geometry is clipped to |x|, |y| <= WPE__RASTER_GUARD_BAND * w instead of the
 * viewport. With WPE_RENDERER_MAX_DIMENSION this bounds fixed-point coordinates to about
 * +-2^19, so edge functions fit comfortably in int64.
 */
#define WPE__RASTER_GUARD_BAND 4.0f

/* Work granularity of the parallel stages. Fixed values keep the output thread-count independent. */
#define WPE__VERTEX_JOB_SIZE 1024u
#define WPE__TRIANGLE_JOB_SIZE 512u

typedef struct wpe__Draw {
    WPE_DrawDesc desc;     /* desc.uniforms points at the frame arena copy. */
    uint32_t submit_index; /* Tie-breaker that makes the key sort stable. */
    uint32_t triangle_count;
    uint32_t vertex_base; /* Index of this draw's first vertex in the shaded vertex arrays. */
    size_t varying_base;  /* Index of this draw's first varying float. */
} wpe__Draw;

/* A contiguous range of one draw's vertices or triangles processed by a single parallel chunk. */
typedef struct wpe__Job {
    uint32_t draw; /* Index into the sorted draw order. */
    uint32_t begin;
    uint32_t end;
} wpe__Job;

/* A triangle ready for rasterization (screen space, positive area, varyings divided by w). */
typedef struct wpe__SetupTri {
    int32_t x[3], y[3];                     /* Fixed point, WPE__RASTER_SUBPIXEL_BITS fractional bits. */
    int32_t min_px, min_py, max_px, max_py; /* Inclusive pixel bounds, clamped to the framebuffer. */
    int64_t area;                           /* Twice the signed area in fixed-point units; > 0. */
    float z[3];                             /* Window depth in [0, 1]. */
    float inv_w[3];
    uint32_t draw;
    bool front_facing;
    float varyings[3][WPE_MAX_VARYINGS];
} wpe__SetupTri;

/* Output of one triangle job. Buffers persist across frames to avoid per-frame allocation. */
typedef struct wpe__TriOutput {
    wpe__SetupTri* tris;
    uint32_t count;
    uint32_t capacity;
    uint32_t culled;
    uint32_t clipped;
    uint32_t subdivided;
} wpe__TriOutput;

/*
 * Adaptive subdivision bounds. A fully refined triangle at WPE_MAX_SUBDIVISION_LEVELS has
 * (2^L + 1)(2^L + 2) / 2 = 153 vertices; the cap leaves headroom for the 3 originals.
 */
#define WPE__SUBDIV_MAX_VERTICES 160u
#define WPE__SUBDIV_STACK 64u

/* A vertex produced (or reused) during subdivision of one source triangle. */
typedef struct wpe__SubdivVertex {
    const void* input; /* Input vertex bytes (source buffer or the thread's scratch storage). */
    WPE_Vec4 position;
    float varyings[WPE_MAX_VARYINGS];
} wpe__SubdivVertex;

/* Per-thread scratch for subdivision; allocated during planning, never in parallel stages. */
typedef struct wpe__ThreadScratch {
    wpe__SubdivVertex vertices[WPE__SUBDIV_MAX_VERTICES];
    uint16_t midpoint_keys[WPE__SUBDIV_MAX_VERTICES][3]; /* (endpoint a, endpoint b, midpoint) */
    uint32_t midpoint_count;
    uint32_t vertex_count;
    unsigned char* inputs; /* WPE__SUBDIV_MAX_VERTICES * stride bytes of generated input vertices. */
    size_t inputs_capacity;
} wpe__ThreadScratch;

struct WPE_Renderer {
    WPE_Framebuffer framebuffer;
    WPE_JobSystem* jobs;
    uint32_t tiles_x;
    uint32_t tiles_y;

    bool recording;
    WPE_ClearDesc clear;
    WPE_Arena frame_arena;

    wpe__Draw* draws; /* Submission order; grows across frames. */
    uint32_t draw_count;
    uint32_t draw_capacity;

    wpe__TriOutput* tri_outputs; /* One per triangle job; persists across frames. */
    uint32_t tri_output_capacity;

    wpe__ThreadScratch* thread_scratch; /* Indexed by job thread index; persists across frames. */
    uint32_t thread_scratch_count;

    /* Per-frame execution state (frame arena), valid during end_frame. */
    wpe__Draw** sorted; /* Draws in execution order (points into `draws`). */
    wpe__Job* vertex_jobs;
    uint32_t vertex_job_count;
    wpe__Job* tri_jobs;
    uint32_t tri_job_count;
    WPE_Vec4* positions;             /* Shaded clip-space positions, all draws. */
    float* varyings;                 /* Shaded varyings, all draws. */
    const wpe__SetupTri** tri_table; /* Global triangle index -> setup triangle. */
    uint32_t* tile_offsets;          /* tiles + 1 prefix offsets into tile_refs. */
    uint32_t* tile_refs;             /* Global triangle indices per tile, in draw order. */
    uint64_t* tile_fragments;        /* Fragment shader invocations per tile. */
    atomic_bool out_of_memory;       /* Set by parallel stages that fail to grow a buffer. */

    WPE_RenderStats stats;
};

/* Parallel stage callbacks (context is the renderer). */
void wpe__renderer_shade_vertices(void* context, uint32_t begin, uint32_t end, uint32_t thread_index);
void wpe__renderer_setup_triangles(void* context, uint32_t begin, uint32_t end, uint32_t thread_index);
/* Ensures per-thread subdivision scratch for `threads` threads and vertices of `max_stride` bytes. */
bool wpe__renderer_reserve_scratch(WPE_Renderer* renderer, uint32_t threads, uint32_t max_stride);
void wpe__renderer_raster_tiles(void* context, uint32_t begin, uint32_t end, uint32_t thread_index);

#endif /* WPE_RENDERER_RENDERER_INTERNAL_H */
