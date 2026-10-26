/**
 * @file cube_mesh.h
 * @brief Unit cube geometry (centered, edge length 1) with a distinct color per face, shared by
 *        samples and renderer tests.
 *
 * @author WillPowerEngine Team
 */
#ifndef WPE_SAMPLES_CUBE_MESH_H
#define WPE_SAMPLES_CUBE_MESH_H

#include "renderer/shaders.h"

#define WPE_CUBE_VERTEX_COUNT 24u
#define WPE_CUBE_INDEX_COUNT 36u

/**
 * Fills a cube with counter-clockwise outward-facing triangles.
 *
 * @param vertices Receives WPE_CUBE_VERTEX_COUNT vertices.
 * @param indices Receives WPE_CUBE_INDEX_COUNT indices.
 */
static inline void wpe_cube_mesh_build(WPE_VertexColored vertices[WPE_CUBE_VERTEX_COUNT],
                                       uint32_t indices[WPE_CUBE_INDEX_COUNT]) {
    /* Per face: normal n and tangents u, v with u x v = n, so (-u-v, +u-v, +u+v, -u+v) is CCW from outside. */
    static const float faces[6][3][3] = {
        {{1, 0, 0}, {0, 1, 0}, {0, 0, 1}},  {{-1, 0, 0}, {0, 0, 1}, {0, 1, 0}},
        {{0, 1, 0}, {0, 0, 1}, {1, 0, 0}},  {{0, -1, 0}, {1, 0, 0}, {0, 0, 1}},
        {{0, 0, 1}, {1, 0, 0}, {0, 1, 0}},  {{0, 0, -1}, {0, 1, 0}, {1, 0, 0}},
    };
    static const float colors[6][4] = {
        {0.90f, 0.25f, 0.20f, 1}, {0.25f, 0.75f, 0.30f, 1}, {0.25f, 0.45f, 0.90f, 1},
        {0.95f, 0.80f, 0.25f, 1}, {0.70f, 0.35f, 0.85f, 1}, {0.25f, 0.80f, 0.80f, 1},
    };
    static const float corners[4][2] = {{-1, -1}, {1, -1}, {1, 1}, {-1, 1}};

    for (uint32_t f = 0; f < 6; ++f) {
        for (uint32_t c = 0; c < 4; ++c) {
            WPE_VertexColored* v = &vertices[f * 4 + c];
            float p[3];
            for (int axis = 0; axis < 3; ++axis) {
                p[axis] = 0.5f * (faces[f][0][axis] + corners[c][0] * faces[f][1][axis] + corners[c][1] * faces[f][2][axis]);
            }
            v->position = wpe_vec3(p[0], p[1], p[2]);
            /* Darken one corner per face so rotation is visible on flat colors. */
            const float shade = c == 0 ? 0.6f : 1.0f;
            for (int k = 0; k < 3; ++k) v->color[k] = colors[f][k] * shade;
            v->color[3] = colors[f][3];
        }
        const uint32_t base = f * 4;
        const uint32_t face_indices[6] = {base, base + 1, base + 2, base, base + 2, base + 3};
        for (uint32_t i = 0; i < 6; ++i) indices[f * 6 + i] = face_indices[i];
    }
}

#endif /* WPE_SAMPLES_CUBE_MESH_H */
