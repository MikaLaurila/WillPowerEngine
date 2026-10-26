#define WPE_LOG_MODULE "renderer"
#include "renderer/renderer.h"

#include <stdlib.h>
#include <string.h>

#include "core/alloc.h"
#include "core/debug.h"
#include "core/log.h"
#include "renderer/renderer_internal.h"

/* ---- Framebuffer ---------------------------------------------------------------------------------- */

static bool wpe__renderer_valid_size(uint32_t width, uint32_t height) {
    return width > 0 && height > 0 && width <= WPE_RENDERER_MAX_DIMENSION &&
           height <= WPE_RENDERER_MAX_DIMENSION;
}

static WPE_Result wpe__renderer_allocate_targets(WPE_Renderer* renderer, uint32_t width, uint32_t height) {
    const size_t pixels = (size_t)width * height;
    WPE_Color* color = wpe_alloc(pixels * sizeof *color);
    float* depth = wpe_alloc(pixels * sizeof *depth);
    if (!color || !depth) {
        wpe_free(color);
        wpe_free(depth);
        return WPE_RESULT_ERROR_OUT_OF_MEMORY;
    }
    for (size_t i = 0; i < pixels; ++i) {
        color[i] = 0xFF000000u;
        depth[i] = 1.0f;
    }

    wpe_free(renderer->framebuffer.color);
    wpe_free(renderer->framebuffer.depth);
    renderer->framebuffer =
        (WPE_Framebuffer){.width = width, .height = height, .color = color, .depth = depth};
    renderer->tiles_x = (width + WPE_RENDERER_TILE_SIZE - 1) / WPE_RENDERER_TILE_SIZE;
    renderer->tiles_y = (height + WPE_RENDERER_TILE_SIZE - 1) / WPE_RENDERER_TILE_SIZE;
    return WPE_RESULT_OK;
}

/* ---- Lifecycle ------------------------------------------------------------------------------------ */

WPE_Result wpe_renderer_create(const WPE_RendererDesc* desc, WPE_Renderer** out_renderer) {
    if (!desc || !out_renderer) return WPE_RESULT_ERROR_NULL_ARG;
    *out_renderer = NULL;
    if (!wpe__renderer_valid_size(desc->width, desc->height)) {
        wpe_log_error("invalid framebuffer size %ux%u", desc->width, desc->height);
        return WPE_RESULT_ERROR_INVALID_ARG;
    }

    WPE_Renderer* renderer = wpe_alloc_zeroed(1, sizeof *renderer);
    if (!renderer) return WPE_RESULT_ERROR_OUT_OF_MEMORY;
    renderer->jobs = desc->jobs;
    wpe_arena_init(&renderer->frame_arena, (size_t)1 << 20);

    const WPE_Result result = wpe__renderer_allocate_targets(renderer, desc->width, desc->height);
    if (result != WPE_RESULT_OK) {
        wpe_renderer_destroy(renderer);
        return result;
    }
    *out_renderer = renderer;
    return WPE_RESULT_OK;
}

void wpe_renderer_destroy(WPE_Renderer* renderer) {
    if (!renderer) return;
    for (uint32_t i = 0; i < renderer->tri_output_capacity; ++i)
        wpe_free(renderer->tri_outputs[i].tris);
    wpe_free(renderer->tri_outputs);
    wpe_free(renderer->draws);
    wpe_arena_release(&renderer->frame_arena);
    wpe_free(renderer->framebuffer.color);
    wpe_free(renderer->framebuffer.depth);
    wpe_free(renderer);
}

WPE_Result wpe_renderer_resize(WPE_Renderer* renderer, uint32_t width, uint32_t height) {
    if (!renderer) return WPE_RESULT_ERROR_NULL_ARG;
    if (renderer->recording) return WPE_RESULT_ERROR_INVALID_STATE;
    if (!wpe__renderer_valid_size(width, height)) {
        wpe_log_error("invalid framebuffer size %ux%u", width, height);
        return WPE_RESULT_ERROR_INVALID_ARG;
    }
    if (width == renderer->framebuffer.width && height == renderer->framebuffer.height) return WPE_RESULT_OK;
    return wpe__renderer_allocate_targets(renderer, width, height);
}

const WPE_Framebuffer* wpe_renderer_framebuffer(const WPE_Renderer* renderer) {
    return renderer ? &renderer->framebuffer : NULL;
}

WPE_RenderStats wpe_renderer_stats(const WPE_Renderer* renderer) {
    return renderer ? renderer->stats : (WPE_RenderStats){0};
}

/* ---- Recording ------------------------------------------------------------------------------------ */

WPE_Result wpe_renderer_begin_frame(WPE_Renderer* renderer, const WPE_ClearDesc* clear) {
    if (!renderer) return WPE_RESULT_ERROR_NULL_ARG;
    if (renderer->recording) {
        wpe_log_error("begin_frame called while a frame is already recording");
        return WPE_RESULT_ERROR_INVALID_STATE;
    }
    wpe_arena_reset(&renderer->frame_arena);
    renderer->draw_count = 0;
    renderer->clear = clear ? *clear : (WPE_ClearDesc){0};
    renderer->recording = true;
    return WPE_RESULT_OK;
}

static bool wpe__renderer_validate_draw(const WPE_DrawDesc* draw) {
    const WPE_ShaderProgram* program = draw->program;
    if (!program || !program->vertex || !program->fragment) {
        wpe_log_error("draw has no complete shader program");
        return false;
    }
    if (program->varying_count > WPE_MAX_VARYINGS) {
        wpe_log_error("shader program uses %u varyings (max %u)", program->varying_count, WPE_MAX_VARYINGS);
        return false;
    }
    if (program->uv_derivatives && (program->uv_varying >= program->varying_count ||
                                    program->uv_varying + 1 >= program->varying_count)) {
        wpe_log_error("shader program requests UV derivatives of varyings %u..%u but has only %u varyings",
                      program->uv_varying, program->uv_varying + 1, program->varying_count);
        return false;
    }
    if (draw->uniforms_size > 0 && !draw->uniforms) {
        wpe_log_error("draw declares %u uniform bytes but no uniform data", draw->uniforms_size);
        return false;
    }
    if (draw->vertex_count > 0 && (!draw->vertices || draw->vertex_stride == 0)) {
        wpe_log_error("draw has %u vertices but no vertex data or stride", draw->vertex_count);
        return false;
    }
    if ((unsigned)draw->state.cull > WPE_CULL_FRONT || (unsigned)draw->state.depth_func > WPE_DEPTH_LEQUAL ||
        (unsigned)draw->state.blend > WPE_BLEND_ALPHA_DITHER) {
        wpe_log_error("draw has an invalid render state");
        return false;
    }

    if (draw->indices) {
        if (draw->index_count % 3 != 0) {
            wpe_log_error("index count %u is not a multiple of 3", draw->index_count);
            return false;
        }
        for (uint32_t i = 0; i < draw->index_count; ++i) {
            if (draw->indices[i] >= draw->vertex_count) {
                wpe_log_error("index %u (value %u) is out of range for %u vertices", i, draw->indices[i],
                              draw->vertex_count);
                return false;
            }
        }
    } else if (draw->index_count != 0) {
        wpe_log_error("draw has an index count but no indices");
        return false;
    }
    return true;
}

WPE_Result wpe_renderer_submit(WPE_Renderer* renderer, const WPE_DrawDesc* draw) {
    if (!renderer || !draw) return WPE_RESULT_ERROR_NULL_ARG;
    if (!renderer->recording) {
        wpe_log_error("submit called outside begin_frame/end_frame");
        return WPE_RESULT_ERROR_INVALID_STATE;
    }
    if (!wpe__renderer_validate_draw(draw)) return WPE_RESULT_ERROR_INVALID_ARG;

    const uint32_t triangle_count = draw->indices ? draw->index_count / 3 : draw->vertex_count / 3;
    if (triangle_count == 0) return WPE_RESULT_OK;

    if (renderer->draw_count == renderer->draw_capacity) {
        const uint32_t capacity = renderer->draw_capacity ? renderer->draw_capacity * 2 : 64;
        wpe__Draw* draws = wpe_realloc(renderer->draws, (size_t)capacity * sizeof *draws);
        if (!draws) return WPE_RESULT_ERROR_OUT_OF_MEMORY;
        renderer->draws = draws;
        renderer->draw_capacity = capacity;
    }

    WPE_DrawDesc copy = *draw;
    if (draw->uniforms_size > 0) {
        void* uniforms = wpe_arena_alloc(&renderer->frame_arena, draw->uniforms_size, 0);
        if (!uniforms) return WPE_RESULT_ERROR_OUT_OF_MEMORY;
        memcpy(uniforms, draw->uniforms, draw->uniforms_size);
        copy.uniforms = uniforms;
    } else {
        copy.uniforms = NULL;
    }

    renderer->draws[renderer->draw_count] = (wpe__Draw){
        .desc = copy,
        .submit_index = renderer->draw_count,
        .triangle_count = triangle_count,
    };
    ++renderer->draw_count;
    return WPE_RESULT_OK;
}

/* ---- Execution ------------------------------------------------------------------------------------ */

static int wpe__renderer_compare_draws(const void* a, const void* b) {
    const wpe__Draw* da = *(const wpe__Draw* const*)a;
    const wpe__Draw* db = *(const wpe__Draw* const*)b;
    if (da->desc.sort_key != db->desc.sort_key) return da->desc.sort_key < db->desc.sort_key ? -1 : 1;
    return da->submit_index < db->submit_index ? -1 : (da->submit_index > db->submit_index);
}

/* Splits every draw into fixed-size jobs. Returns false on allocation failure. */
static bool wpe__renderer_plan_jobs(WPE_Renderer* renderer) {
    WPE_Arena* arena = &renderer->frame_arena;
    const uint32_t draw_count = renderer->draw_count;

    renderer->sorted = wpe_arena_alloc(arena, (size_t)draw_count * sizeof *renderer->sorted, 0);
    if (draw_count && !renderer->sorted) return false;
    for (uint32_t i = 0; i < draw_count; ++i)
        renderer->sorted[i] = &renderer->draws[i];
    if (draw_count > 1)
        qsort(renderer->sorted, draw_count, sizeof *renderer->sorted, wpe__renderer_compare_draws);

    uint32_t vertex_jobs = 0, tri_jobs = 0;
    uint32_t vertex_total = 0;
    size_t varying_total = 0;
    for (uint32_t i = 0; i < draw_count; ++i) {
        wpe__Draw* draw = renderer->sorted[i];
        draw->vertex_base = vertex_total;
        draw->varying_base = varying_total;
        vertex_total += draw->desc.vertex_count;
        varying_total += (size_t)draw->desc.vertex_count * draw->desc.program->varying_count;
        vertex_jobs += (draw->desc.vertex_count + WPE__VERTEX_JOB_SIZE - 1) / WPE__VERTEX_JOB_SIZE;
        tri_jobs += (draw->triangle_count + WPE__TRIANGLE_JOB_SIZE - 1) / WPE__TRIANGLE_JOB_SIZE;
    }

    renderer->positions = wpe_arena_alloc(arena, (size_t)vertex_total * sizeof(WPE_Vec4), 16);
    renderer->varyings = varying_total ? wpe_arena_alloc(arena, varying_total * sizeof(float), 0) : NULL;
    renderer->vertex_jobs = wpe_arena_alloc(arena, (size_t)vertex_jobs * sizeof(wpe__Job), 0);
    renderer->tri_jobs = wpe_arena_alloc(arena, (size_t)tri_jobs * sizeof(wpe__Job), 0);
    if ((vertex_total && !renderer->positions) || (varying_total && !renderer->varyings) ||
        (vertex_jobs && !renderer->vertex_jobs) || (tri_jobs && !renderer->tri_jobs)) {
        return false;
    }

    renderer->vertex_job_count = 0;
    renderer->tri_job_count = 0;
    for (uint32_t d = 0; d < draw_count; ++d) {
        const wpe__Draw* draw = renderer->sorted[d];
        for (uint32_t v = 0; v < draw->desc.vertex_count; v += WPE__VERTEX_JOB_SIZE) {
            const uint32_t end = v + WPE__VERTEX_JOB_SIZE < draw->desc.vertex_count ? v + WPE__VERTEX_JOB_SIZE
                                                                                    : draw->desc.vertex_count;
            renderer->vertex_jobs[renderer->vertex_job_count++] =
                (wpe__Job){.draw = d, .begin = v, .end = end};
        }
        for (uint32_t t = 0; t < draw->triangle_count; t += WPE__TRIANGLE_JOB_SIZE) {
            const uint32_t end = t + WPE__TRIANGLE_JOB_SIZE < draw->triangle_count
                                     ? t + WPE__TRIANGLE_JOB_SIZE
                                     : draw->triangle_count;
            renderer->tri_jobs[renderer->tri_job_count++] = (wpe__Job){.draw = d, .begin = t, .end = end};
        }
    }

    /* Triangle job outputs persist across frames; only the table of them grows here. */
    if (renderer->tri_job_count > renderer->tri_output_capacity) {
        wpe__TriOutput* outputs =
            wpe_realloc(renderer->tri_outputs, (size_t)renderer->tri_job_count * sizeof *outputs);
        if (!outputs) return false;
        memset(outputs + renderer->tri_output_capacity, 0,
               (size_t)(renderer->tri_job_count - renderer->tri_output_capacity) * sizeof *outputs);
        renderer->tri_outputs = outputs;
        renderer->tri_output_capacity = renderer->tri_job_count;
    }
    return true;
}

/* Assigns every setup triangle to the tiles its bounds overlap, preserving draw order. */
static bool wpe__renderer_bin_triangles(WPE_Renderer* renderer) {
    WPE_Arena* arena = &renderer->frame_arena;
    const uint32_t tile_count = renderer->tiles_x * renderer->tiles_y;

    uint32_t tri_total = 0;
    for (uint32_t j = 0; j < renderer->tri_job_count; ++j)
        tri_total += renderer->tri_outputs[j].count;

    renderer->tile_offsets = wpe_arena_alloc_zeroed(arena, (size_t)tile_count + 1, sizeof(uint32_t), 0);
    renderer->tri_table =
        wpe_arena_alloc(arena, (size_t)(tri_total ? tri_total : 1) * sizeof *renderer->tri_table, 0);
    renderer->tile_fragments = wpe_arena_alloc_zeroed(arena, tile_count, sizeof(uint64_t), 0);
    if (!renderer->tile_offsets || !renderer->tri_table || !renderer->tile_fragments) return false;

    /* Pass 1: count references per tile (stored shifted by one for the prefix sum). */
    uint32_t global = 0;
    for (uint32_t j = 0; j < renderer->tri_job_count; ++j) {
        const wpe__TriOutput* output = &renderer->tri_outputs[j];
        for (uint32_t i = 0; i < output->count; ++i) {
            const wpe__SetupTri* tri = &output->tris[i];
            renderer->tri_table[global++] = tri;
            const uint32_t tx0 = (uint32_t)tri->min_px / WPE_RENDERER_TILE_SIZE;
            const uint32_t tx1 = (uint32_t)tri->max_px / WPE_RENDERER_TILE_SIZE;
            const uint32_t ty0 = (uint32_t)tri->min_py / WPE_RENDERER_TILE_SIZE;
            const uint32_t ty1 = (uint32_t)tri->max_py / WPE_RENDERER_TILE_SIZE;
            for (uint32_t ty = ty0; ty <= ty1; ++ty) {
                for (uint32_t tx = tx0; tx <= tx1; ++tx)
                    ++renderer->tile_offsets[ty * renderer->tiles_x + tx + 1];
            }
        }
    }
    for (uint32_t t = 0; t < tile_count; ++t)
        renderer->tile_offsets[t + 1] += renderer->tile_offsets[t];

    const uint32_t ref_total = renderer->tile_offsets[tile_count];
    renderer->tile_refs = wpe_arena_alloc(arena, (size_t)(ref_total ? ref_total : 1) * sizeof(uint32_t), 0);
    uint32_t* cursor = wpe_arena_alloc(arena, (size_t)tile_count * sizeof(uint32_t), 0);
    if (!renderer->tile_refs || !cursor) return false;
    memcpy(cursor, renderer->tile_offsets, (size_t)tile_count * sizeof(uint32_t));

    /* Pass 2: fill references in global (draw) order. */
    for (uint32_t g = 0; g < tri_total; ++g) {
        const wpe__SetupTri* tri = renderer->tri_table[g];
        const uint32_t tx0 = (uint32_t)tri->min_px / WPE_RENDERER_TILE_SIZE;
        const uint32_t tx1 = (uint32_t)tri->max_px / WPE_RENDERER_TILE_SIZE;
        const uint32_t ty0 = (uint32_t)tri->min_py / WPE_RENDERER_TILE_SIZE;
        const uint32_t ty1 = (uint32_t)tri->max_py / WPE_RENDERER_TILE_SIZE;
        for (uint32_t ty = ty0; ty <= ty1; ++ty) {
            for (uint32_t tx = tx0; tx <= tx1; ++tx)
                renderer->tile_refs[cursor[ty * renderer->tiles_x + tx]++] = g;
        }
    }

    renderer->stats.triangles_rasterized = tri_total;
    return true;
}

WPE_Result wpe_renderer_end_frame(WPE_Renderer* renderer) {
    if (!renderer) return WPE_RESULT_ERROR_NULL_ARG;
    if (!renderer->recording) {
        wpe_log_error("end_frame called without begin_frame");
        return WPE_RESULT_ERROR_INVALID_STATE;
    }
    renderer->recording = false;
    renderer->stats = (WPE_RenderStats){.draws = renderer->draw_count};
    for (uint32_t i = 0; i < renderer->draw_count; ++i) {
        renderer->stats.triangles_submitted += renderer->draws[i].triangle_count;
    }
    atomic_store(&renderer->out_of_memory, false);

    if (!wpe__renderer_plan_jobs(renderer)) goto out_of_memory;

    wpe_jobs_parallel_for(renderer->jobs, renderer->vertex_job_count, 1, wpe__renderer_shade_vertices,
                          renderer);
    wpe_jobs_parallel_for(renderer->jobs, renderer->tri_job_count, 1, wpe__renderer_setup_triangles,
                          renderer);
    if (atomic_load(&renderer->out_of_memory)) goto out_of_memory;

    for (uint32_t j = 0; j < renderer->tri_job_count; ++j) {
        renderer->stats.triangles_culled += renderer->tri_outputs[j].culled;
        renderer->stats.triangles_clipped += renderer->tri_outputs[j].clipped;
    }

    if (!wpe__renderer_bin_triangles(renderer)) goto out_of_memory;

    const uint32_t tile_count = renderer->tiles_x * renderer->tiles_y;
    wpe_jobs_parallel_for(renderer->jobs, tile_count, 1, wpe__renderer_raster_tiles, renderer);
    for (uint32_t t = 0; t < tile_count; ++t)
        renderer->stats.fragments_shaded += renderer->tile_fragments[t];
    return WPE_RESULT_OK;

out_of_memory:
    wpe_log_error("out of memory executing frame with %u draws; frame dropped", renderer->draw_count);
    return WPE_RESULT_ERROR_OUT_OF_MEMORY;
}
