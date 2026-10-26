/**
 * @file scene_render.h
 * @brief Submits the visible WPE_MeshRenderer entities of a world to the rasterizer.
 *
 * Per call: builds the camera matrices, frustum-culls each primitive by its bounds (skinned
 * meshes are not culled), computes skinning palettes, chooses render state from the material
 * (double-sided, mirrored transforms, alpha mode) and submits draws with sort keys that render
 * opaque geometry front-to-back and blended geometry back-to-front afterwards.
 *
 * Usage per frame: wpe_renderer_begin_frame() → wpe_scene_render() → (2D, etc.) →
 * wpe_renderer_end_frame(). Per-frame data lives in the scene renderer until its next
 * wpe_scene_render() call, so end the frame before rendering the scene again.
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

typedef struct WPE_SceneRenderSettings {
    WPE_LightingParams light;
    bool dither_transparency; /* Render alpha-blended materials with WPE_BLEND_ALPHA_DITHER. */
    bool frustum_culling;
} WPE_SceneRenderSettings;

typedef struct WPE_SceneRenderStats {
    uint32_t primitives_submitted;
    uint32_t primitives_culled;
} WPE_SceneRenderStats;

typedef struct WPE_SceneRenderer WPE_SceneRenderer;

/** @return Defaults: a white key light from above-front-left, dim ambient, culling on. @thread_safety Any
 * thread */
WPE_SceneRenderSettings wpe_scene_render_settings_default(void);

/** @return WPE_RESULT_OK, WPE_RESULT_ERROR_NULL_ARG or WPE_RESULT_ERROR_OUT_OF_MEMORY. @thread_safety Any
 * thread */
[[nodiscard]] WPE_Result wpe_scene_renderer_create(WPE_SceneRenderer** out_scene_renderer);

/** Destroys the scene renderer; NULL is a no-op. @thread_safety Owning thread */
void wpe_scene_renderer_destroy(WPE_SceneRenderer* scene_renderer);

/**
 * Computes the view-projection matrix for a camera entity at the given aspect ratio.
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
 *         renderer submit error.
 *
 * @thread_safety Thread owning the world and the renderer
 */
WPE_Result wpe_scene_render(WPE_SceneRenderer* scene_renderer, WPE_Renderer* renderer, const WPE_Ecs* ecs,
                            WPE_Entity camera, const WPE_SceneRenderSettings* settings,
                            WPE_SceneRenderStats* out_stats);

#endif /* WPE_GRAPHICS_SCENE_RENDER_H */
