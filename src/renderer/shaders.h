/**
 * @file shaders.h
 * @brief Built-in shader programs for common cases.
 *
 * - Dependencies: renderer, math
 * - Thread affinity: programs are immutable and may be used from any thread
 *
 * @author WillPowerEngine Team
 */
#ifndef WPE_RENDERER_SHADERS_H
#define WPE_RENDERER_SHADERS_H

#include "renderer/renderer.h"

/** Vertex layout for wpe_shader_vertex_color(): position plus linear RGBA color. */
typedef struct WPE_VertexColored {
    WPE_Vec3 position;
    float color[4];
} WPE_VertexColored;

/** Uniforms for wpe_shader_vertex_color(). */
typedef struct WPE_VertexColorUniforms {
    WPE_Mat4 mvp;  /* Object space to clip space. */
    WPE_Vec4 tint; /* Multiplied with the vertex color. */
} WPE_VertexColorUniforms;

/**
 * @return Program that transforms WPE_VertexColored by `mvp` and outputs the interpolated vertex
 *         color multiplied by `tint`. Static storage; never NULL.
 *
 * @thread_safety Any thread
 */
const WPE_ShaderProgram* wpe_shader_vertex_color(void);

#endif /* WPE_RENDERER_SHADERS_H */
