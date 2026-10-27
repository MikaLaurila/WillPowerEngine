#define WPE_LOG_MODULE "assets"
#include "assets/procedural.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

#include "core/alloc.h"
#include "core/json.h"
#include "core/log.h"

static bool wpe__material_has_textures(const WPE_Material* m) {
    const WPE_TextureRef* refs[] = {
        &m->diffuse_texture, &m->specular_glossiness_texture, &m->metallic_roughness_texture,
        &m->normal_texture,  &m->occlusion_texture,           &m->emissive_texture};
    for (size_t i = 0; i < sizeof refs / sizeof refs[0]; ++i) {
        if (refs[i]->texture != WPE_MODEL_NONE) return true;
    }
    return false;
}

WPE_Material wpe_material_solid(WPE_Vec4 color, float metallic, float roughness) {
    const WPE_TextureRef none = {.texture = WPE_MODEL_NONE, .scale = {{1, 1}}};
    return (WPE_Material){
        .name = "solid",
        .workflow = WPE_MATERIAL_METALLIC_ROUGHNESS,
        .diffuse_factor = color,
        .diffuse_texture = none,
        .specular_factor = {{1, 1, 1}},
        .glossiness_factor = 1.0f,
        .specular_glossiness_texture = none,
        .metallic_factor = metallic,
        .roughness_factor = roughness,
        .metallic_roughness_texture = none,
        .normal_texture = none,
        .normal_scale = 1.0f,
        .occlusion_texture = none,
        .occlusion_strength = 1.0f,
        .emissive_texture = none,
        .alpha_mode = color.w < 1.0f ? WPE_ALPHA_BLEND : WPE_ALPHA_OPAQUE,
        .alpha_cutoff = 0.5f,
    };
}

WPE_Result wpe_model_create_mesh(const WPE_MeshVertex* vertices, uint32_t vertex_count,
                                 const uint32_t* indices, uint32_t index_count, const WPE_Material* material,
                                 const char* name, WPE_Model** out_model) {
    if (!vertices || !indices || !out_model) return WPE_RESULT_ERROR_NULL_ARG;
    *out_model = NULL;
    if (vertex_count == 0 || index_count == 0 || index_count % 3 != 0) return WPE_RESULT_ERROR_INVALID_ARG;
    for (uint32_t i = 0; i < index_count; ++i) {
        if (indices[i] >= vertex_count) {
            wpe_log_error("procedural mesh index %u (value %u) out of range", i, indices[i]);
            return WPE_RESULT_ERROR_INVALID_ARG;
        }
    }
    const WPE_Material fallback = wpe_material_solid(wpe_vec4(1, 1, 1, 1), 0.0f, 0.6f);
    if (!material) material = &fallback;
    if (wpe__material_has_textures(material)) {
        wpe_log_error("procedural models cannot reference textures");
        return WPE_RESULT_ERROR_INVALID_ARG;
    }

    WPE_Model* model = wpe_alloc_zeroed(1, sizeof *model);
    if (!model) return WPE_RESULT_ERROR_OUT_OF_MEMORY;
    WPE_Arena* arena = &model->wpe__memory;
    wpe_arena_init(arena, 0);

    model->vertices = wpe_arena_alloc(arena, (size_t)vertex_count * sizeof(WPE_MeshVertex), 0);
    model->indices = wpe_arena_alloc(arena, (size_t)index_count * sizeof(uint32_t), 0);
    model->primitives = wpe_arena_alloc_zeroed(arena, 1, sizeof(WPE_MeshPrimitive), 0);
    model->meshes = wpe_arena_alloc_zeroed(arena, 1, sizeof(WPE_Mesh), 0);
    model->materials = wpe_arena_alloc_zeroed(arena, 2, sizeof(WPE_Material), 0);
    model->nodes = wpe_arena_alloc_zeroed(arena, 1, sizeof(WPE_ModelNode), 0);
    model->root_nodes = wpe_arena_alloc_zeroed(arena, 1, sizeof(uint32_t), 0);
    if (!model->vertices || !model->indices || !model->primitives || !model->meshes || !model->materials ||
        !model->nodes || !model->root_nodes) {
        wpe_model_destroy(model);
        return WPE_RESULT_ERROR_OUT_OF_MEMORY;
    }

    memcpy(model->vertices, vertices, (size_t)vertex_count * sizeof(WPE_MeshVertex));
    memcpy(model->indices, indices, (size_t)index_count * sizeof(uint32_t));
    model->vertex_count = vertex_count;
    model->index_count = index_count;

    WPE_Aabb bounds = {.min = vertices[0].position, .max = vertices[0].position};
    for (uint32_t v = 1; v < vertex_count; ++v) {
        const WPE_Vec3 p = vertices[v].position;
        bounds.min = wpe_vec3(fminf(bounds.min.x, p.x), fminf(bounds.min.y, p.y), fminf(bounds.min.z, p.z));
        bounds.max = wpe_vec3(fmaxf(bounds.max.x, p.x), fmaxf(bounds.max.y, p.y), fmaxf(bounds.max.z, p.z));
    }

    model->materials[0] = *material;
    model->materials[1] = wpe_material_solid(wpe_vec4(1, 1, 1, 1), 1.0f, 1.0f);
    snprintf(model->materials[1].name, WPE_MODEL_NAME_MAX, "default");
    model->material_count = 2;
    model->primitives[0] = (WPE_MeshPrimitive){
        .vertex_count = vertex_count, .index_count = index_count, .material = 0, .bounds = bounds};
    model->primitive_count = 1;
    model->meshes[0] = (WPE_Mesh){.first_primitive = 0, .primitive_count = 1, .bounds = bounds};
    snprintf(model->meshes[0].name, WPE_MODEL_NAME_MAX, "%s", name ? name : "mesh");
    model->mesh_count = 1;
    model->nodes[0] = (WPE_ModelNode){.parent = WPE_MODEL_NONE,
                                      .first_child = WPE_MODEL_NONE,
                                      .next_sibling = WPE_MODEL_NONE,
                                      .mesh = 0,
                                      .skin = WPE_MODEL_NONE,
                                      .rotation = wpe_quat_identity(),
                                      .scale = {{1, 1, 1}}};
    snprintf(model->nodes[0].name, WPE_MODEL_NAME_MAX, "%s", name ? name : "mesh");
    model->node_count = 1;
    model->root_nodes[0] = 0;
    model->root_count = 1;

    *out_model = model;
    return WPE_RESULT_OK;
}

static WPE_MeshVertex wpe__vertex(WPE_Vec3 position, WPE_Vec3 normal, WPE_Vec3 tangent, float u, float v) {
    return (WPE_MeshVertex){.position = position,
                            .normal = normal,
                            .uv0 = {{u, v}},
                            .tangent = {tangent.x, tangent.y, tangent.z, 1.0f},
                            .color = {1, 1, 1, 1}};
}

WPE_Result wpe_model_create_plane(float size, uint32_t segments, const WPE_Material* material,
                                  WPE_Model** out_model) {
    if (!out_model) return WPE_RESULT_ERROR_NULL_ARG;
    if (!(size > 0.0f) || segments == 0 || segments > 1024) return WPE_RESULT_ERROR_INVALID_ARG;
    const uint32_t row = segments + 1;
    const uint32_t vertex_count = row * row, index_count = segments * segments * 6;
    WPE_MeshVertex* vertices = wpe_alloc((size_t)vertex_count * sizeof *vertices);
    uint32_t* indices = wpe_alloc((size_t)index_count * sizeof *indices);
    if (!vertices || !indices) {
        wpe_free(vertices);
        wpe_free(indices);
        return WPE_RESULT_ERROR_OUT_OF_MEMORY;
    }
    for (uint32_t z = 0; z < row; ++z) {
        for (uint32_t x = 0; x < row; ++x) {
            const float u = (float)x / (float)segments, v = (float)z / (float)segments;
            vertices[z * row + x] = wpe__vertex(wpe_vec3((u - 0.5f) * size, 0, (v - 0.5f) * size),
                                                wpe_vec3(0, 1, 0), wpe_vec3(1, 0, 0), u, v);
        }
    }
    uint32_t n = 0;
    for (uint32_t z = 0; z < segments; ++z) {
        for (uint32_t x = 0; x < segments; ++x) {
            const uint32_t a = z * row + x, b = a + 1, c = a + row, d = c + 1;
            /* Counter-clockwise when viewed from +Y. */
            const uint32_t quad[6] = {a, c, b, b, c, d};
            for (int i = 0; i < 6; ++i)
                indices[n++] = quad[i];
        }
    }
    const WPE_Result result =
        wpe_model_create_mesh(vertices, vertex_count, indices, index_count, material, "plane", out_model);
    wpe_free(vertices);
    wpe_free(indices);
    return result;
}

WPE_Result wpe_model_create_box(WPE_Vec3 size, const WPE_Material* material, WPE_Model** out_model) {
    if (!out_model) return WPE_RESULT_ERROR_NULL_ARG;
    if (!(size.x > 0.0f && size.y > 0.0f && size.z > 0.0f)) return WPE_RESULT_ERROR_INVALID_ARG;
    /* Per face: normal n and tangents u, v with u x v = n, so corners (-u-v, +u-v, +u+v, -u+v) are CCW. */
    static const float faces[6][3][3] = {
        {{1, 0, 0}, {0, 1, 0}, {0, 0, 1}}, {{-1, 0, 0}, {0, 0, 1}, {0, 1, 0}},
        {{0, 1, 0}, {0, 0, 1}, {1, 0, 0}}, {{0, -1, 0}, {1, 0, 0}, {0, 0, 1}},
        {{0, 0, 1}, {1, 0, 0}, {0, 1, 0}}, {{0, 0, -1}, {0, 1, 0}, {1, 0, 0}},
    };
    static const float corners[4][2] = {{-1, -1}, {1, -1}, {1, 1}, {-1, 1}};
    WPE_MeshVertex vertices[24];
    uint32_t indices[36];
    const WPE_Vec3 half = wpe_vec3_scale(size, 0.5f);
    for (uint32_t f = 0; f < 6; ++f) {
        const WPE_Vec3 n = wpe_vec3(faces[f][0][0], faces[f][0][1], faces[f][0][2]);
        const WPE_Vec3 u = wpe_vec3(faces[f][1][0], faces[f][1][1], faces[f][1][2]);
        const WPE_Vec3 v = wpe_vec3(faces[f][2][0], faces[f][2][1], faces[f][2][2]);
        for (uint32_t c = 0; c < 4; ++c) {
            const WPE_Vec3 unit = wpe_vec3_add(
                n, wpe_vec3_add(wpe_vec3_scale(u, corners[c][0]), wpe_vec3_scale(v, corners[c][1])));
            vertices[f * 4 + c] = wpe__vertex(wpe_vec3_mul(unit, half), n, u, (corners[c][0] + 1) * 0.5f,
                                              (1 - corners[c][1]) * 0.5f);
        }
        const uint32_t b = f * 4;
        const uint32_t face_indices[6] = {b, b + 1, b + 2, b, b + 2, b + 3};
        memcpy(&indices[(size_t)f * 6], face_indices, sizeof face_indices);
    }
    return wpe_model_create_mesh(vertices, 24, indices, 36, material, "box", out_model);
}

WPE_Result wpe_model_create_sphere(float radius, uint32_t rings, uint32_t segments,
                                   const WPE_Material* material, WPE_Model** out_model) {
    if (!out_model) return WPE_RESULT_ERROR_NULL_ARG;
    if (!(radius > 0.0f) || rings < 2 || rings > 512 || segments < 3 || segments > 1024)
        return WPE_RESULT_ERROR_INVALID_ARG;
    /* Latitude-longitude grid; the seam column is duplicated so UVs wrap cleanly. */
    const uint32_t row = segments + 1;
    const uint32_t vertex_count = (rings + 1) * row;
    const uint32_t index_count = (rings - 1) * segments * 6; /* Pole rows have one triangle per quad. */
    WPE_MeshVertex* vertices = wpe_alloc((size_t)vertex_count * sizeof *vertices);
    uint32_t* indices = wpe_alloc((size_t)index_count * sizeof *indices);
    if (!vertices || !indices) {
        wpe_free(vertices);
        wpe_free(indices);
        return WPE_RESULT_ERROR_OUT_OF_MEMORY;
    }
    const float pi = 3.14159265358979f;
    for (uint32_t r = 0; r <= rings; ++r) {
        const float v = (float)r / (float)rings, theta = v * pi; /* 0 at the top pole. */
        for (uint32_t s = 0; s <= segments; ++s) {
            const float u = (float)s / (float)segments, phi = u * 2.0f * pi;
            const WPE_Vec3 n = wpe_vec3(sinf(theta) * cosf(phi), cosf(theta), -sinf(theta) * sinf(phi));
            /* Tangent along increasing u (constant latitude). */
            vertices[r * row + s] =
                wpe__vertex(wpe_vec3_scale(n, radius), n, wpe_vec3(-sinf(phi), 0.0f, -cosf(phi)), u, v);
        }
    }
    uint32_t n = 0;
    for (uint32_t r = 0; r < rings; ++r) {
        for (uint32_t s = 0; s < segments; ++s) {
            const uint32_t a = r * row + s, b = a + row, c = a + 1, d = b + 1;
            /* Counter-clockwise seen from outside; skip the degenerate triangle at each pole. */
            if (r > 0) {
                indices[n++] = a;
                indices[n++] = b;
                indices[n++] = c;
            }
            if (r + 1 < rings) {
                indices[n++] = c;
                indices[n++] = b;
                indices[n++] = d;
            }
        }
    }
    const WPE_Result result =
        wpe_model_create_mesh(vertices, vertex_count, indices, n, material, "sphere", out_model);
    wpe_free(vertices);
    wpe_free(indices);
    return result;
}

/* ---- Primitive description files -------------------------------------------------------------------- */

/* Reads an optional array of `count` numbers; false (logged) if present but malformed. */
static bool wpe__prim_numbers(const WPE_JsonValue* value, uint32_t min_count, uint32_t max_count, float* out,
                              const char* name, const char* key) {
    if (!value) return true;
    const uint32_t count = wpe_json_count(value);
    if (wpe_json_type(value) != WPE_JSON_ARRAY || count < min_count || count > max_count) {
        wpe_log_error("'%s': \"%s\" must be an array of %u..%u numbers", name, key, min_count, max_count);
        return false;
    }
    for (uint32_t i = 0; i < count; ++i) {
        const WPE_JsonValue* item = wpe_json_at(value, i);
        if (wpe_json_type(item) != WPE_JSON_NUMBER) {
            wpe_log_error("'%s': \"%s\" must contain only numbers", name, key);
            return false;
        }
        out[i] = (float)wpe_json_number(item, 0.0);
    }
    return true;
}

static bool wpe__prim_number(const WPE_JsonValue* root, const char* key, float* io_value, const char* name) {
    const WPE_JsonValue* value = wpe_json_get(root, key);
    if (!value) return true;
    if (wpe_json_type(value) != WPE_JSON_NUMBER) {
        wpe_log_error("'%s': \"%s\" must be a number", name, key);
        return false;
    }
    *io_value = (float)wpe_json_number(value, 0.0);
    return true;
}

WPE_Result wpe_model_parse_primitive(const char* text, size_t size, const char* name, WPE_Model** out_model) {
    if (!text || !out_model) return WPE_RESULT_ERROR_NULL_ARG;
    *out_model = NULL;
    if (!name) name = "primitive";
    WPE_JsonDocument* doc = NULL;
    WPE_JsonError error;
    WPE_Result result = wpe_json_parse(text, size, &doc, &error);
    if (result == WPE_RESULT_ERROR_PARSE)
        wpe_log_error("'%s':%u:%u: %s", name, error.line, error.column, error.message);
    if (result != WPE_RESULT_OK) return result;

    const WPE_JsonValue* root = wpe_json_root(doc);
    static const char* const known[] = {"shape", "size",     "segments",  "rings",    "radius",
                                        "color", "metallic", "roughness", "emissive", "double_sided"};
    result = wpe_json_type(root) == WPE_JSON_OBJECT ? WPE_RESULT_OK : WPE_RESULT_ERROR_PARSE;
    if (result != WPE_RESULT_OK) wpe_log_error("'%s': a primitive description must be a JSON object", name);
    for (uint32_t i = 0; result == WPE_RESULT_OK && i < wpe_json_count(root); ++i) {
        const char* key = NULL;
        wpe_json_member(root, i, &key);
        bool found = false;
        for (size_t k = 0; k < sizeof known / sizeof known[0] && !found; ++k)
            found = strcmp(key, known[k]) == 0;
        if (!found) {
            wpe_log_error("'%s': unknown key \"%s\"", name, key);
            result = WPE_RESULT_ERROR_PARSE;
        }
    }

    float color[4] = {1, 1, 1, 1}, emissive[3] = {0, 0, 0}, size3[3] = {1, 1, 1};
    float metallic = 0.0f, roughness = 0.6f, radius = 0.5f, segments = 0.0f, rings = 0.0f;
    const char* shape = wpe_json_string(wpe_json_get(root, "shape"));
    if (result == WPE_RESULT_OK &&
        (!wpe__prim_numbers(wpe_json_get(root, "color"), 3, 4, color, name, "color") ||
         !wpe__prim_numbers(wpe_json_get(root, "emissive"), 3, 3, emissive, name, "emissive") ||
         !wpe__prim_number(root, "metallic", &metallic, name) ||
         !wpe__prim_number(root, "roughness", &roughness, name) ||
         !wpe__prim_number(root, "radius", &radius, name) ||
         !wpe__prim_number(root, "segments", &segments, name) ||
         !wpe__prim_number(root, "rings", &rings, name)))
        result = WPE_RESULT_ERROR_PARSE;
    /* Counts must be whole numbers in range before they are converted. */
    if (result == WPE_RESULT_OK &&
        (!(segments >= 0.0f && segments <= 4096.0f) || segments != floorf(segments) ||
         !(rings >= 0.0f && rings <= 4096.0f) || rings != floorf(rings))) {
        wpe_log_error("'%s': \"segments\" and \"rings\" must be whole numbers from 1 to 4096", name);
        result = WPE_RESULT_ERROR_PARSE;
    }
    const WPE_JsonValue* size_value = wpe_json_get(root, "size");
    const WPE_JsonValue* double_sided = wpe_json_get(root, "double_sided");
    if (result == WPE_RESULT_OK && double_sided && wpe_json_type(double_sided) != WPE_JSON_BOOL) {
        wpe_log_error("'%s': \"double_sided\" must be true or false", name);
        result = WPE_RESULT_ERROR_PARSE;
    }

    WPE_Material material =
        wpe_material_solid(wpe_vec4(color[0], color[1], color[2], color[3]), metallic, roughness);
    material.emissive_factor = wpe_vec3(emissive[0], emissive[1], emissive[2]);
    material.double_sided = wpe_json_bool(double_sided, false);
    if (result != WPE_RESULT_OK) {
        /* Reported above. */
    } else if (!shape) {
        wpe_log_error("'%s': \"shape\" (\"plane\", \"box\" or \"sphere\") is required", name);
        result = WPE_RESULT_ERROR_PARSE;
    } else if (strcmp(shape, "plane") == 0) {
        float plane_size = 1.0f;
        if (size_value && wpe_json_type(size_value) != WPE_JSON_NUMBER) {
            wpe_log_error("'%s': a plane's \"size\" is one number", name);
            result = WPE_RESULT_ERROR_PARSE;
        }
        plane_size = (float)wpe_json_number(size_value, 1.0);
        if (result == WPE_RESULT_OK)
            result = wpe_model_create_plane(plane_size, segments > 0.0f ? (uint32_t)segments : 1u, &material,
                                            out_model);
    } else if (strcmp(shape, "box") == 0) {
        if (!wpe__prim_numbers(size_value, 3, 3, size3, name, "size")) result = WPE_RESULT_ERROR_PARSE;
        if (result == WPE_RESULT_OK)
            result = wpe_model_create_box(wpe_vec3(size3[0], size3[1], size3[2]), &material, out_model);
    } else if (strcmp(shape, "sphere") == 0) {
        result = wpe_model_create_sphere(radius, rings > 0.0f ? (uint32_t)rings : 12u,
                                         segments > 0.0f ? (uint32_t)segments : 24u, &material, out_model);
    } else {
        wpe_log_error("'%s': unknown shape \"%s\" (use \"plane\", \"box\" or \"sphere\")", name, shape);
        result = WPE_RESULT_ERROR_PARSE;
    }
    if (result == WPE_RESULT_ERROR_INVALID_ARG) {
        wpe_log_error("'%s': %s parameters out of range", name, shape);
        result = WPE_RESULT_ERROR_PARSE;
    }
    wpe_json_free(doc);
    return result;
}