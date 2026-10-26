/**
 * @file scene_render.h
 * @brief Submits the visible WPE_MeshRenderer entities of a world to the rasterizer, with lights,
 *        directional shadows, fog and adaptive subdivision.
 *
 * Per call:
 * 1. Builds the camera matrices and gathers WPE_Light entities. The main directional light (the
 *    "sun") is the first one with cast_shadows, else the first directional light. With no light
 *    entities at all, an optional default sun is used.
 * 2. If the sun casts shadows, renders a shadow map (depth-only, orthographic) fitted to the
 *    camera's view up to `shadows.distance`, texel-snapped so shadows do not shimmer.
 * 3. Frustum-culls each primitive by its bounds (skinned meshes are not culled), picks the most
 *    influential point/spot lights per object (WPE_MAX_OBJECT_LIGHTS), chooses render state from
 *    the material and submits draws that render opaque geometry front-to-back and blended
 *    geometry back-to-front afterwards.
 *
 * Usage per frame: wpe_renderer_begin_frame() → wpe_scene_render() → (2D, etc.) →
 * wpe_renderer_end_frame(). Per-frame data (palettes, shadow map) lives in the scene renderer
 * until its next wpe_scene_render() call, so end the frame before rendering the scene again.
 *
 * - Dependencies: graphics, renderer, scene, ecs
 * - Thread affinity: thread owning the world and the renderer
 *
 * @author WillPowerEngine Team
 */
#ifndef WPE_GRAPHICS_SCENE_RENDER_H
#define WPE_GRAPHICS_SCENE_RENDER_H

#include "graphics/graphics.h"
#include "graphics/material_shader.h"
#include "renderer/renderer.h"

typedef struct WPE_ShadowSettings {
    bool enabled;
    uint32_t map_size;      /* Shadow map resolution (square), e.g. 1024. */
    float distance;         /* Shadows cover the camera view up to this distance. */
    float caster_extension; /* Extra distance toward the light in which casters are included. */
    float bias;             /* Depth bias in shadow-map depth units. */
} WPE_ShadowSettings;

typedef struct WPE_SceneRenderSettings {
    WPE_Vec3 ambient;
    WPE_FogParams fog;
    WPE_ShadowSettings shadows;
    bool dither_transparency; /* Render alpha-blended materials with WPE_BLEND_ALPHA_DITHER. */
    bool frustum_culling;
    uint32_t subdivision_levels;   /* 0 disables adaptive subdivision. */
    float subdivision_edge_pixels; /* Split edges longer than this on screen. */
    bool default_sun;              /* Use default_sun_* when the world has no lights at all. */
    WPE_Vec3 default_sun_direction;
    WPE_Vec3 default_sun_color;
} WPE_SceneRenderSettings;

typedef struct WPE_SceneRenderStats {
    uint32_t primitives_submitted;
    uint32_t primitives_culled;
    uint32_t shadow_casters; /* Primitives drawn into the shadow map. */
    uint32_t lights;         /* Point/spot lights in the world. */
} WPE_SceneRenderStats;

typedef struct WPE_SceneRenderer WPE_SceneRenderer;

/**
 * @return Defaults: dim ambient, no fog, 1024 shadows over 30 units, culling on, subdivision of
 *         edges longer than 24 pixels (3 levels), a default sun when the world has no lights.
 * @thread_safety Any thread
 */
WPE_SceneRenderSettings wpe_scene_render_settings_default(void);

/** @return WPE_RESULT_OK, WPE_RESULT_ERROR_NULL_ARG or WPE_RESULT_ERROR_OUT_OF_MEMORY. @thread_safety Any
 * thread */
[[nodiscard]] WPE_Result wpe_scene_renderer_create(WPE_SceneRenderer** out_scene_renderer);

/** Destroys the scene renderer; NULL is a no-op. @thread_safety Owning thread */
void wpe_scene_renderer_destroy(WPE_SceneRenderer* scene_renderer);

/**
 * Computes the view and projection matrices for a camera entity at the given aspect ratio.
 *
 * @return false if the entity lacks a Transform or Camera, or the camera parameters are invalid.
 *
 * @thread_safety Thread owning the world
 */
bool wpe_scene_camera_matrices(const WPE_Ecs* ecs, WPE_Entity camera, float aspect, WPE_Mat4* out_view,
                               WPE_Mat4* out_projection);

/**
 * Submits all visible meshes as seen from `camera`. Requires world matrices to be current
 * (call wpe_scene_update_transforms() first) and a frame to be recording on `renderer`.
 *
 * @param settings Optional (NULL = defaults).
 * @param out_stats Optional.
 * @return WPE_RESULT_OK; WPE_RESULT_ERROR_NULL_ARG; WPE_RESULT_ERROR_INVALID_ARG (camera entity
 *         lacks Transform/Camera or has invalid parameters); WPE_RESULT_ERROR_OUT_OF_MEMORY; or a
 *         renderer error.
 *
 * @thread_safety Thread owning the world and the renderer
 */
WPE_Result wpe_scene_render(WPE_SceneRenderer* scene_renderer, WPE_Renderer* renderer, const WPE_Ecs* ecs,
                            WPE_Entity camera, const WPE_SceneRenderSettings* settings,
                            WPE_SceneRenderStats* out_stats);

/**
 * @return The shadow map rendered by the last wpe_scene_render() call, or NULL if none (for
 *         debugging/visualization). Valid until the next call.
 *
 * @thread_safety Owning thread
 */
const WPE_ShadowSampler* wpe_scene_renderer_shadow_map(const WPE_SceneRenderer* scene_renderer);

#endif /* WPE_GRAPHICS_SCENE_RENDER_H */
