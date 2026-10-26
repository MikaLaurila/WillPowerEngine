/**
 * @file material_shader.h
 * @brief Shader program that renders WPE_Model primitives with their WPE_Material.
 *
 * Shading model: traditional diffuse + Blinn-Phong specular. Lighting is evaluated per vertex
 * (one directional light plus ambient for now); material inputs are sampled per pixel:
 * - Metallic-roughness materials are converted per pixel: diffuse = base * (1 - metallic),
 *   specular = mix(0.04, base, metallic), gloss = 1 - roughness.
 * - Occlusion darkens the diffuse term, emissive is added unlit.
 * - Alpha: OPAQUE forces alpha 1, MASK discards below the cutoff, BLEND outputs alpha.
 * Skinning (up to 4 weighted joints per vertex) is applied in the vertex shader when a joint
 * palette is provided.
 *
 * NOTE(limitation): normal maps are loaded but not yet applied (per-pixel lighting arrives with
 * the lighting milestone), and textures on UV set 1 reuse UV set 0's derivatives for mip selection.
 *
 * - Dependencies: renderer, assets
 * - Thread affinity: the program is immutable; shaders run concurrently on worker threads
 *
 * @author WillPowerEngine Team
 */
#ifndef WPE_GRAPHICS_MATERIAL_SHADER_H
#define WPE_GRAPHICS_MATERIAL_SHADER_H

#include "assets/model.h"
#include "renderer/renderer.h"

typedef struct WPE_LightingParams {
    WPE_Vec3 direction; /* Direction the light travels (normalized internally). */
    WPE_Vec3 color;
    WPE_Vec3 ambient;
} WPE_LightingParams;

typedef struct WPE_MaterialUniforms {
    WPE_Mat4 mvp;           /* Object (or, for skinned meshes, world) space to clip space. */
    WPE_Mat4 world;         /* Object to world (identity for skinned meshes). */
    WPE_Mat4 normal_matrix; /* Inverse-transpose of `world` (upper 3x3 used). */
    WPE_Vec3 camera_position;
    WPE_LightingParams light;
    float shininess;               /* Blinn-Phong exponent derived from the material's gloss factor. */
    const WPE_Model* model;        /* Borrowed until the frame ends. */
    const WPE_Material* material;  /* Borrowed until the frame ends. */
    const WPE_Mat4* joint_palette; /* World-space skinning matrices, or NULL for static meshes. */
    uint32_t joint_count;
} WPE_MaterialUniforms;

/** @return The material program (static storage). @thread_safety Any thread */
const WPE_ShaderProgram* wpe_shader_material(void);

/** @return Blinn-Phong exponent for a glossiness in [0, 1] (2 at 0, 2048 at 1). @thread_safety Any thread */
float wpe_material_shininess(float glossiness);

#endif /* WPE_GRAPHICS_MATERIAL_SHADER_H */
