/**
 * @file material_shader.h
 * @brief Shader program that renders WPE_Model primitives with their WPE_Material.
 *
 * Lighting is hybrid:
 * - Per vertex: ambient, up to WPE_MAX_OBJECT_LIGHTS point/spot lights (diffuse + Blinn-Phong
 *   specular, smooth range falloff, spot cones) and the main directional light ("sun"). Combined
 *   with adaptive subdivision near the camera, this keeps vertex lighting smooth at close range.
 * - Per pixel: the sun's shadow (PCF-filtered shadow map) and, for normal-mapped materials, the
 *   sun's diffuse/specular evaluated with the tangent-space normal from the normal map.
 * Material inputs are sampled per pixel; metallic-roughness materials are converted per pixel
 * (diffuse = base * (1 - metallic), specular = mix(0.04, base, metallic), gloss = 1 - roughness).
 * Ambient light also reflects specularly (uniform environment). Fog is applied last.
 * Skinning (up to 4 weighted joints per vertex) runs in the vertex shader when a palette is given.
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

/** Point/spot lights evaluated per object (the most influential ones are chosen). */
#define WPE_MAX_OBJECT_LIGHTS 4u

typedef enum WPE_FogMode {
    WPE_FOG_NONE = 0,
    WPE_FOG_LINEAR,      /* 0 at fog_start, 1 at fog_end (view distance). */
    WPE_FOG_EXPONENTIAL, /* 1 - exp(-density * distance). */
} WPE_FogMode;

typedef struct WPE_FogParams {
    WPE_FogMode mode;
    WPE_Vec3 color;
    float start;
    float end;
    float density;
} WPE_FogParams;

/** A point or spot light in world space, as consumed by the shader. */
typedef struct WPE_ShaderLight {
    WPE_Vec3 position;
    WPE_Vec3 direction; /* Spot: direction the light travels (normalized). */
    WPE_Vec3 color;     /* Pre-multiplied by intensity. */
    float range;
    float cos_inner;
    float cos_outer;
    bool spot;
} WPE_ShaderLight;

/** Read-only view of a shadow map: depth in [0, 1] per texel, row 0 at the top. */
typedef struct WPE_ShadowSampler {
    const float* depth;
    uint32_t size; /* Square map, size x size texels. */
    float bias;    /* Subtracted from the receiver depth before comparison. */
} WPE_ShadowSampler;

typedef struct WPE_MaterialUniforms {
    WPE_Mat4 mvp;           /* Object (or, for skinned meshes, world) space to clip space. */
    WPE_Mat4 world;         /* Object to world (identity for skinned meshes). */
    WPE_Mat4 normal_matrix; /* Inverse-transpose of `world` (upper 3x3 used). */
    WPE_Vec3 camera_position;
    WPE_Vec3 ambient;

    bool has_sun;
    WPE_Vec3 sun_direction;              /* Direction the light travels (normalized). */
    WPE_Vec3 sun_color;                  /* Pre-multiplied by intensity. */
    const WPE_ShadowSampler* sun_shadow; /* NULL = unshadowed. Borrowed until the frame ends. */
    WPE_Mat4 shadow_matrix;              /* World -> (u, v, depth) in [0, 1] of the shadow map. */

    WPE_ShaderLight lights[WPE_MAX_OBJECT_LIGHTS];
    uint32_t light_count;
    WPE_FogParams fog;

    float shininess;               /* Blinn-Phong exponent derived from the material's gloss factor. */
    const WPE_Model* model;        /* Borrowed until the frame ends. */
    const WPE_Material* material;  /* Borrowed until the frame ends. */
    const WPE_Mat4* joint_palette; /* World-space skinning matrices, or NULL for static meshes. */
    uint32_t joint_count;
} WPE_MaterialUniforms;

/** @return The material program (static storage); supports subdivision. @thread_safety Any thread */
const WPE_ShaderProgram* wpe_shader_material(void);

/**
 * @param alpha_masked true for WPE_ALPHA_MASK materials: the program then runs a fragment shader
 *                     that discards cut-out texels; otherwise no fragment shader runs at all.
 * @return A depth-only program for shadow casters using WPE_MaterialUniforms (mvp, palette,
 *         material and model are used). Use with WPE_RenderState::depth_only. @thread_safety Any thread
 */
const WPE_ShaderProgram* wpe_shader_material_depth(bool alpha_masked);

/** @return How much fog covers a point `distance` from the camera, 0 (none) to 1. @thread_safety Any thread
 */
float wpe_fog_factor(const WPE_FogParams* fog, float distance);

/** @return Blinn-Phong exponent for a glossiness in [0, 1] (2 at 0, 2048 at 1). @thread_safety Any thread */
float wpe_material_shininess(float glossiness);

/**
 * Smooth distance falloff used for point/spot lights: inverse-square-like, reaching exactly zero
 * at `range` (windowed as in the glTF punctual-light recommendation, normalized so it is 1 at 0).
 *
 * @thread_safety Any thread
 */
float wpe_light_attenuation(float distance, float range);

/**
 * Shadow visibility in [0, 1] (1 = fully lit) at shadow-map coordinates, 3x3 PCF.
 * Coordinates outside the map are lit.
 *
 * @thread_safety Any thread
 */
float wpe_shadow_visibility(const WPE_ShadowSampler* shadow, float u, float v, float depth);

#endif /* WPE_GRAPHICS_MATERIAL_SHADER_H */
