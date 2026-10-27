/**
 * @file procedural.h
 * @brief Procedurally generated models (single mesh, single material) such as planes and boxes.
 *
 * The results are ordinary WPE_Model objects (one root node with one mesh), so they render,
 * instantiate and cast/receive shadows exactly like imported models.
 *
 * - Dependencies: assets/model
 * - Thread affinity: any thread
 *
 * @author WillPowerEngine Team
 */
#ifndef WPE_ASSETS_PROCEDURAL_H
#define WPE_ASSETS_PROCEDURAL_H

#include "assets/model.h"

/**
 * Creates a model from raw geometry with one primitive and the given material.
 *
 * @param vertices Copied; `vertex_count` > 0.
 * @param indices Copied; triangle list, every index < vertex_count, index_count a multiple of 3.
 * @param material Copied; texture references must be WPE_MODEL_NONE (procedural models own no
 *                 textures). NULL uses the default material.
 * @param name Mesh/node name (copied; may be NULL).
 * @param out_model Receives the model; release with wpe_model_destroy().
 * @return WPE_RESULT_OK, WPE_RESULT_ERROR_NULL_ARG, WPE_RESULT_ERROR_INVALID_ARG or
 *         WPE_RESULT_ERROR_OUT_OF_MEMORY.
 *
 * @thread_safety Any thread
 */
[[nodiscard]] WPE_Result wpe_model_create_mesh(const WPE_MeshVertex* vertices, uint32_t vertex_count,
                                               const uint32_t* indices, uint32_t index_count,
                                               const WPE_Material* material, const char* name,
                                               WPE_Model** out_model);

/** @return A copy of the default material with the given diffuse color, tuned roughness and no textures.
 * @thread_safety Any thread */
WPE_Material wpe_material_solid(WPE_Vec4 color, float metallic, float roughness);

/**
 * Creates a flat XZ plane centered at the origin facing +Y, split into `segments` x `segments`
 * quads (more segments give smoother per-vertex lighting without subdivision). UVs span [0, 1].
 *
 * @thread_safety Any thread
 */
[[nodiscard]] WPE_Result wpe_model_create_plane(float size, uint32_t segments, const WPE_Material* material,
                                                WPE_Model** out_model);

/** Creates an axis-aligned box centered at the origin with per-face normals and UVs. @thread_safety Any
 * thread */
[[nodiscard]] WPE_Result wpe_model_create_box(WPE_Vec3 size, const WPE_Material* material,
                                              WPE_Model** out_model);

/**
 * Creates a UV sphere centered at the origin (latitude rings from pole to pole, longitude segments
 * around +Y). UVs wrap once around and run from the top pole (v = 0) to the bottom (v = 1).
 *
 * @param rings 2..512. @param segments 3..1024.
 * @thread_safety Any thread
 */
[[nodiscard]] WPE_Result wpe_model_create_sphere(float radius, uint32_t rings, uint32_t segments,
                                                 const WPE_Material* material, WPE_Model** out_model);

/**
 * Builds a model from a primitive description (a ".prim" JSON file, see docs/assets.md), e.g.
 * `{"shape": "box", "size": [1, 2, 1], "color": [0.9, 0.5, 0.1], "roughness": 0.4}`.
 * Shapes: "plane" (size, segments), "box" (size [x, y, z]), "sphere" (radius, rings, segments).
 * Material keys: color (RGB or RGBA, sRGB), metallic, roughness, emissive (RGB), double_sided.
 * Unknown keys are errors.
 *
 * @param name Used in error messages (usually the asset path).
 * @return WPE_RESULT_OK, WPE_RESULT_ERROR_NULL_ARG, WPE_RESULT_ERROR_PARSE (logged with the reason)
 *         or WPE_RESULT_ERROR_OUT_OF_MEMORY.
 *
 * @thread_safety Any thread
 */
[[nodiscard]] WPE_Result wpe_model_parse_primitive(const char* text, size_t size, const char* name,
                                                   WPE_Model** out_model);

#endif /* WPE_ASSETS_PROCEDURAL_H */
