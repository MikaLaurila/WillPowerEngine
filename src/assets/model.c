#define WPE_LOG_MODULE "assets"
#include "assets/model.h"

#include <cgltf.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>

#include "assets/image.h"
#include "core/alloc.h"
#include "core/debug.h"
#include "core/log.h"
#include "platform/platform.h"

/* Required extensions the loader honors (fully or with a documented approximation). */
static const char* const wpe__supported_required_extensions[] = {
    "KHR_texture_transform", "KHR_materials_emissive_strength", "KHR_materials_pbrSpecularGlossiness",
    "KHR_materials_unlit",   "KHR_materials_transmission",      "KHR_mesh_quantization",
};

/* Transmission is approximated by alpha blending with this much opacity at full transmission. */
#define WPE__TRANSMISSION_MIN_ALPHA 0.25f

/* ---- cgltf hooks ---------------------------------------------------------------------------------- */

static void* wpe__cgltf_alloc(void* user, cgltf_size size) {
    (void)user;
    return wpe_alloc(size);
}

static void wpe__cgltf_free(void* user, void* ptr) {
    (void)user;
    wpe_free(ptr);
}

static cgltf_result wpe__cgltf_read(const cgltf_memory_options* memory, const cgltf_file_options* file,
                                    const char* path, cgltf_size* size, void** data) {
    (void)memory;
    (void)file;
    size_t read_size = 0;
    const WPE_Result result = wpe_file_read_all(path, data, &read_size);
    if (result == WPE_RESULT_OK) {
        *size = read_size;
        return cgltf_result_success;
    }
    return result == WPE_RESULT_ERROR_OUT_OF_MEMORY ? cgltf_result_out_of_memory
                                                    : cgltf_result_file_not_found;
}

static void wpe__cgltf_release(const cgltf_memory_options* memory, const cgltf_file_options* file,
                               void* data) {
    (void)memory;
    (void)file;
    wpe_free(data);
}

static const cgltf_options wpe__cgltf_options = {
    .memory = {.alloc_func = wpe__cgltf_alloc, .free_func = wpe__cgltf_free},
    .file = {.read = wpe__cgltf_read, .release = wpe__cgltf_release},
};

static const char* wpe__cgltf_result_name(cgltf_result result) {
    switch (result) {
    case cgltf_result_success:
        return "success";
    case cgltf_result_data_too_short:
        return "data too short";
    case cgltf_result_unknown_format:
        return "unknown format";
    case cgltf_result_invalid_json:
        return "invalid JSON";
    case cgltf_result_invalid_gltf:
        return "invalid glTF";
    case cgltf_result_invalid_options:
        return "invalid options";
    case cgltf_result_file_not_found:
        return "file not found";
    case cgltf_result_io_error:
        return "I/O error";
    case cgltf_result_out_of_memory:
        return "out of memory";
    case cgltf_result_legacy_gltf:
        return "legacy glTF 1.0";
    default:
        return "unknown error";
    }
}

static WPE_Result wpe__cgltf_to_result(cgltf_result result) {
    switch (result) {
    case cgltf_result_success:
        return WPE_RESULT_OK;
    case cgltf_result_file_not_found:
    case cgltf_result_io_error:
        return WPE_RESULT_ERROR_IO;
    case cgltf_result_out_of_memory:
        return WPE_RESULT_ERROR_OUT_OF_MEMORY;
    case cgltf_result_legacy_gltf:
        return WPE_RESULT_ERROR_UNSUPPORTED;
    default:
        return WPE_RESULT_ERROR_PARSE;
    }
}

/* ---- Loader state --------------------------------------------------------------------------------- */

/* One texture to decode: a glTF image interpreted as color (sRGB) or data. */
typedef struct wpe__TextureJob {
    const cgltf_image* image;
    bool srgb;
    WPE_Texture* texture; /* Output. */
    WPE_Result result;    /* Output. */
} wpe__TextureJob;

typedef struct wpe__Loader {
    const char* path;
    const cgltf_data* gltf;
    WPE_Model* model;
    WPE_Arena* arena;
    const WPE_ModelLoadOptions* options;

    wpe__TextureJob* texture_jobs; /* Indexed by model texture index; [image * 2 + srgb] lookup below. */
    uint32_t texture_job_count;
    uint32_t* image_texture_map; /* images_count * 2 entries: model texture index or WPE_MODEL_NONE. */
} wpe__Loader;

static void wpe__copy_name(char dst[WPE_MODEL_NAME_MAX], const char* name) {
    snprintf(dst, WPE_MODEL_NAME_MAX, "%s", name ? name : "");
}

static void* wpe__arena_array(wpe__Loader* loader, size_t count, size_t size) {
    if (count == 0) return NULL;
    return wpe_arena_alloc_zeroed(loader->arena, count, size, 0);
}

/* ---- Textures ------------------------------------------------------------------------------------- */

static WPE_Sampler wpe__convert_sampler(const cgltf_sampler* s) {
    WPE_Sampler out = {.mag_filter = WPE_FILTER_BILINEAR,
                       .min_filter = WPE_FILTER_BILINEAR,
                       .mip_filter = WPE_MIP_LINEAR,
                       .wrap_u = WPE_WRAP_REPEAT,
                       .wrap_v = WPE_WRAP_REPEAT};
    if (!s) return out;

    if (s->mag_filter == cgltf_filter_type_nearest) out.mag_filter = WPE_FILTER_NEAREST;
    switch (s->min_filter) {
    case cgltf_filter_type_nearest:
        out.min_filter = WPE_FILTER_NEAREST;
        out.mip_filter = WPE_MIP_NONE;
        break;
    case cgltf_filter_type_linear:
        out.mip_filter = WPE_MIP_NONE;
        break;
    case cgltf_filter_type_nearest_mipmap_nearest:
        out.min_filter = WPE_FILTER_NEAREST;
        out.mip_filter = WPE_MIP_NEAREST;
        break;
    case cgltf_filter_type_linear_mipmap_nearest:
        out.mip_filter = WPE_MIP_NEAREST;
        break;
    case cgltf_filter_type_nearest_mipmap_linear:
        out.min_filter = WPE_FILTER_NEAREST;
        break;
    case cgltf_filter_type_linear_mipmap_linear:
    case cgltf_filter_type_undefined:
    default:
        break;
    }
    const cgltf_wrap_mode wraps[2] = {s->wrap_s, s->wrap_t};
    WPE_TextureWrap* outs[2] = {&out.wrap_u, &out.wrap_v};
    for (int i = 0; i < 2; ++i) {
        if (wraps[i] == cgltf_wrap_mode_clamp_to_edge) *outs[i] = WPE_WRAP_CLAMP;
        if (wraps[i] == cgltf_wrap_mode_mirrored_repeat) *outs[i] = WPE_WRAP_MIRROR;
    }
    return out;
}

/* Returns the model texture index for (image, srgb), registering a decode job on first use. */
static uint32_t wpe__texture_for_image(wpe__Loader* loader, const cgltf_image* image, bool srgb) {
    const size_t image_index = cgltf_image_index(loader->gltf, image);
    uint32_t* slot = &loader->image_texture_map[image_index * 2 + (srgb ? 1 : 0)];
    if (*slot == WPE_MODEL_NONE) {
        *slot = loader->texture_job_count;
        loader->texture_jobs[loader->texture_job_count++] = (wpe__TextureJob){.image = image, .srgb = srgb};
    }
    return *slot;
}

static WPE_TextureRef wpe__convert_texture_view(wpe__Loader* loader, const cgltf_texture_view* view,
                                                bool srgb) {
    WPE_TextureRef ref = {.texture = WPE_MODEL_NONE, .scale = {{1.0f, 1.0f}}};
    if (!view->texture || !view->texture->image) return ref;

    ref.texture = wpe__texture_for_image(loader, view->texture->image, srgb);
    ref.sampler = wpe__convert_sampler(view->texture->sampler);
    ref.texcoord = view->texcoord > 0 ? 1u : 0u;
    if (view->texcoord > 1)
        wpe_log_warn("'%s': texture uses TEXCOORD_%d; only sets 0 and 1 are supported", loader->path,
                     view->texcoord);
    if (view->has_transform) {
        const cgltf_texture_transform* t = &view->transform;
        ref.offset = (WPE_Vec2){.x = t->offset[0], .y = t->offset[1]};
        ref.scale = (WPE_Vec2){.x = t->scale[0], .y = t->scale[1]};
        ref.rotation = t->rotation;
        if (t->has_texcoord) ref.texcoord = t->texcoord > 0 ? 1u : 0u;
    }
    return ref;
}

/* Fetches the encoded bytes of an image: from a buffer view, a data URI or an external file. */
static WPE_Result wpe__image_bytes(const char* model_path, const cgltf_image* image, const void** out_data,
                                   size_t* out_size, void** out_owned) {
    *out_owned = NULL;
    if (image->buffer_view) {
        *out_data = cgltf_buffer_view_data(image->buffer_view);
        *out_size = image->buffer_view->size;
        return *out_data ? WPE_RESULT_OK : WPE_RESULT_ERROR_PARSE;
    }
    if (!image->uri) return WPE_RESULT_ERROR_PARSE;

    if (strncmp(image->uri, "data:", 5) == 0) {
        const char* comma = strchr(image->uri, ',');
        if (!comma || !strstr(image->uri, ";base64,")) return WPE_RESULT_ERROR_UNSUPPORTED;
        const char* base64 = comma + 1;
        const size_t length = strlen(base64);
        size_t padding = 0;
        if (length >= 1 && base64[length - 1] == '=') ++padding;
        if (length >= 2 && base64[length - 2] == '=') ++padding;
        const size_t size = length / 4 * 3 - padding;
        void* decoded = NULL;
        if (size == 0 ||
            cgltf_load_buffer_base64(&wpe__cgltf_options, size, base64, &decoded) != cgltf_result_success) {
            return WPE_RESULT_ERROR_PARSE;
        }
        *out_owned = decoded;
        *out_data = decoded;
        *out_size = size;
        return WPE_RESULT_OK;
    }

    /* Relative file URI: resolve against the model's directory and percent-decode. */
    const char* slash = strrchr(model_path, '/');
    const char* backslash = strrchr(model_path, '\\');
    if (backslash && (!slash || backslash > slash)) slash = backslash;
    const size_t dir_length = slash ? (size_t)(slash - model_path) + 1 : 0;
    const size_t uri_length = strlen(image->uri);
    char* full = wpe_alloc(dir_length + uri_length + 1);
    if (!full) return WPE_RESULT_ERROR_OUT_OF_MEMORY;
    memcpy(full, model_path, dir_length);
    memcpy(full + dir_length, image->uri, uri_length + 1);
    cgltf_decode_uri(full + dir_length);

    size_t size = 0;
    void* data = NULL;
    const WPE_Result result = wpe_file_read_all(full, &data, &size);
    wpe_free(full);
    if (result != WPE_RESULT_OK) return result;
    *out_owned = data;
    *out_data = data;
    *out_size = size;
    return WPE_RESULT_OK;
}

typedef struct wpe__DecodeContext {
    wpe__Loader* loader;
} wpe__DecodeContext;

static void wpe__decode_textures(void* context, uint32_t begin, uint32_t end, uint32_t thread_index) {
    (void)thread_index;
    const wpe__Loader* loader = ((wpe__DecodeContext*)context)->loader;
    for (uint32_t i = begin; i < end; ++i) {
        wpe__TextureJob* job = &loader->texture_jobs[i];
        const char* label =
            job->image->name ? job->image->name : (job->image->uri ? job->image->uri : "embedded");

        const void* bytes = NULL;
        size_t size = 0;
        void* owned = NULL;
        job->result = wpe__image_bytes(loader->path, job->image, &bytes, &size, &owned);
        WPE_Image image = {0};
        if (job->result == WPE_RESULT_OK) job->result = wpe_image_decode(bytes, size, label, &image);
        wpe_free(owned);
        if (job->result != WPE_RESULT_OK) continue;

        job->result =
            wpe_texture_create(&(WPE_TextureDesc){.width = image.width,
                                                  .height = image.height,
                                                  .texels = image.pixels,
                                                  .generate_mips = true,
                                                  .srgb = job->srgb,
                                                  .max_dimension = loader->options->max_texture_size},
                               &job->texture);
        wpe_image_free(&image);
    }
}

/* ---- Materials ------------------------------------------------------------------------------------ */

static WPE_Material wpe__default_material(void) {
    const WPE_TextureRef none = {.texture = WPE_MODEL_NONE, .scale = {{1, 1}}};
    return (WPE_Material){
        .name = "default",
        .workflow = WPE_MATERIAL_METALLIC_ROUGHNESS,
        .diffuse_factor = {{1, 1, 1, 1}},
        .diffuse_texture = none,
        .specular_factor = {{1, 1, 1}},
        .glossiness_factor = 1.0f,
        .specular_glossiness_texture = none,
        .metallic_factor = 1.0f,
        .roughness_factor = 1.0f,
        .metallic_roughness_texture = none,
        .normal_texture = none,
        .normal_scale = 1.0f,
        .occlusion_texture = none,
        .occlusion_strength = 1.0f,
        .emissive_texture = none,
        .alpha_mode = WPE_ALPHA_OPAQUE,
        .alpha_cutoff = 0.5f,
    };
}

/*
 * glTF color factors are linear while textures are sRGB-encoded, and the engine shades in gamma
 * space. Encoding the factor is exact for factor * texture products (pow distributes over
 * multiplication), apart from sRGB's tiny linear segment near black.
 */
static float wpe__linear_to_srgb(float c) {
    if (!(c > 0.0f)) return 0.0f;
    if (c >= 1.0f) return c; /* Keep HDR emissive intensities (emissive_strength) above 1. */
    return c <= 0.0031308f ? c * 12.92f : 1.055f * powf(c, 1.0f / 2.4f) - 0.055f;
}

static WPE_Vec3 wpe__srgb3(float r, float g, float b) {
    return wpe_vec3(wpe__linear_to_srgb(r), wpe__linear_to_srgb(g), wpe__linear_to_srgb(b));
}

static WPE_Material wpe__convert_material(wpe__Loader* loader, const cgltf_material* m) {
    WPE_Material out = wpe__default_material();
    wpe__copy_name(out.name, m->name);

    if (m->has_pbr_specular_glossiness) {
        const cgltf_pbr_specular_glossiness* sg = &m->pbr_specular_glossiness;
        out.workflow = WPE_MATERIAL_SPECULAR_GLOSSINESS;
        const WPE_Vec3 d = wpe__srgb3(sg->diffuse_factor[0], sg->diffuse_factor[1], sg->diffuse_factor[2]);
        out.diffuse_factor = wpe_vec4(d.x, d.y, d.z, sg->diffuse_factor[3]);
        out.diffuse_texture = wpe__convert_texture_view(loader, &sg->diffuse_texture, true);
        out.specular_factor =
            wpe__srgb3(sg->specular_factor[0], sg->specular_factor[1], sg->specular_factor[2]);
        out.glossiness_factor = sg->glossiness_factor;
        out.specular_glossiness_texture =
            wpe__convert_texture_view(loader, &sg->specular_glossiness_texture, true);
    } else {
        const cgltf_pbr_metallic_roughness* mr = &m->pbr_metallic_roughness;
        out.workflow = WPE_MATERIAL_METALLIC_ROUGHNESS;
        if (m->has_pbr_metallic_roughness) {
            const WPE_Vec3 base =
                wpe__srgb3(mr->base_color_factor[0], mr->base_color_factor[1], mr->base_color_factor[2]);
            out.diffuse_factor = wpe_vec4(base.x, base.y, base.z, mr->base_color_factor[3]);
            out.diffuse_texture = wpe__convert_texture_view(loader, &mr->base_color_texture, true);
            out.metallic_factor = mr->metallic_factor;
            out.roughness_factor = mr->roughness_factor;
            out.metallic_roughness_texture =
                wpe__convert_texture_view(loader, &mr->metallic_roughness_texture, false);
        }
    }
    if (m->unlit) out.workflow = WPE_MATERIAL_UNLIT;

    out.normal_texture = wpe__convert_texture_view(loader, &m->normal_texture, false);
    out.normal_scale = m->normal_texture.texture ? m->normal_texture.scale : 1.0f;
    out.occlusion_texture = wpe__convert_texture_view(loader, &m->occlusion_texture, false);
    out.occlusion_strength = m->occlusion_texture.texture ? m->occlusion_texture.scale : 1.0f;
    out.emissive_texture = wpe__convert_texture_view(loader, &m->emissive_texture, true);
    const float strength = m->has_emissive_strength ? m->emissive_strength.emissive_strength : 1.0f;
    out.emissive_factor = wpe_vec3_scale(
        wpe__srgb3(m->emissive_factor[0], m->emissive_factor[1], m->emissive_factor[2]), strength);

    switch (m->alpha_mode) {
    case cgltf_alpha_mode_mask:
        out.alpha_mode = WPE_ALPHA_MASK;
        break;
    case cgltf_alpha_mode_blend:
        out.alpha_mode = WPE_ALPHA_BLEND;
        break;
    case cgltf_alpha_mode_opaque:
    default:
        out.alpha_mode = WPE_ALPHA_OPAQUE;
        break;
    }
    out.alpha_cutoff = m->alpha_cutoff;
    out.double_sided = m->double_sided;

    /* NOTE(limitation): transmission (glass) has no refraction here; it becomes alpha blending. */
    if (m->has_transmission && m->transmission.transmission_factor > 0.0f) {
        const float t =
            m->transmission.transmission_factor > 1.0f ? 1.0f : m->transmission.transmission_factor;
        out.alpha_mode = WPE_ALPHA_BLEND;
        out.diffuse_factor.w *= 1.0f - t * (1.0f - WPE__TRANSMISSION_MIN_ALPHA);
    }
    return out;
}

/* ---- Meshes --------------------------------------------------------------------------------------- */

typedef struct wpe__PrimitivePlan {
    const cgltf_primitive* source;
    const cgltf_accessor* attributes[7]; /* Indexed by WPE__ATTR_*. */
    const cgltf_accessor* joints;
    const cgltf_accessor* weights;
    uint32_t source_vertex_count;
    uint32_t triangle_count;
    bool flat_normals; /* No NORMAL attribute: de-index and use face normals (glTF requirement). */
} wpe__PrimitivePlan;

enum {
    WPE__ATTR_POSITION,
    WPE__ATTR_NORMAL,
    WPE__ATTR_TANGENT,
    WPE__ATTR_UV0,
    WPE__ATTR_UV1,
    WPE__ATTR_COLOR,
    WPE__ATTR_COUNT
};

static_assert(WPE__ATTR_COUNT <= 7, "plan attribute array too small");

/* Classifies a primitive; returns false (with a warning) if it cannot be rendered. */
static bool wpe__plan_primitive(const wpe__Loader* loader, const cgltf_primitive* p,
                                wpe__PrimitivePlan* plan) {
    *plan = (wpe__PrimitivePlan){.source = p};
    if (p->type != cgltf_primitive_type_triangles && p->type != cgltf_primitive_type_triangle_strip &&
        p->type != cgltf_primitive_type_triangle_fan) {
        wpe_log_warn("'%s': skipping non-triangle primitive (points/lines are not rendered)", loader->path);
        return false;
    }
    if (p->has_draco_mesh_compression) {
        wpe_log_warn("'%s': skipping Draco-compressed primitive (unsupported)", loader->path);
        return false;
    }
    if (p->targets_count > 0) wpe_log_warn("'%s': morph targets are ignored", loader->path);

    for (cgltf_size i = 0; i < p->attributes_count; ++i) {
        const cgltf_attribute* a = &p->attributes[i];
        switch (a->type) {
        case cgltf_attribute_type_position:
            plan->attributes[WPE__ATTR_POSITION] = a->data;
            break;
        case cgltf_attribute_type_normal:
            plan->attributes[WPE__ATTR_NORMAL] = a->data;
            break;
        case cgltf_attribute_type_tangent:
            plan->attributes[WPE__ATTR_TANGENT] = a->data;
            break;
        case cgltf_attribute_type_texcoord:
            if (a->index == 0) plan->attributes[WPE__ATTR_UV0] = a->data;
            if (a->index == 1) plan->attributes[WPE__ATTR_UV1] = a->data;
            break;
        case cgltf_attribute_type_color:
            if (a->index == 0) plan->attributes[WPE__ATTR_COLOR] = a->data;
            break;
        case cgltf_attribute_type_joints:
            if (a->index == 0) plan->joints = a->data;
            break;
        case cgltf_attribute_type_weights:
            if (a->index == 0) plan->weights = a->data;
            break;
        default:
            break;
        }
    }
    const cgltf_accessor* position = plan->attributes[WPE__ATTR_POSITION];
    if (!position || position->count == 0) {
        wpe_log_warn("'%s': skipping primitive without positions", loader->path);
        return false;
    }
    if (position->count > UINT32_MAX / 4) {
        wpe_log_warn("'%s': skipping primitive with %zu vertices (too many)", loader->path,
                     (size_t)position->count);
        return false;
    }
    plan->source_vertex_count = (uint32_t)position->count;
    for (int i = 1; i < WPE__ATTR_COUNT; ++i) {
        if (plan->attributes[i] && plan->attributes[i]->count != position->count) {
            wpe_log_error("'%s': attribute count mismatch in primitive", loader->path);
            return false;
        }
    }

    const size_t element_count = p->indices ? p->indices->count : position->count;
    size_t triangles = 0;
    if (p->type == cgltf_primitive_type_triangles)
        triangles = element_count / 3;
    else if (element_count >= 3)
        triangles = element_count - 2;
    if (triangles == 0 || triangles > UINT32_MAX / 3) {
        wpe_log_warn("'%s': skipping primitive with no (or too many) triangles", loader->path);
        return false;
    }
    plan->triangle_count = (uint32_t)triangles;
    plan->flat_normals = plan->attributes[WPE__ATTR_NORMAL] == NULL;
    return true;
}

/* Element index of triangle t, corner c, before index-buffer lookup. */
static size_t wpe__primitive_element(cgltf_primitive_type type, size_t t, int c) {
    switch (type) {
    case cgltf_primitive_type_triangle_strip:
        /* Odd triangles swap the first two corners to keep a consistent winding. */
        if (t % 2 == 1 && c < 2) return t + (size_t)(1 - c);
        return t + (size_t)c;
    case cgltf_primitive_type_triangle_fan:
        return c == 0 ? 0 : t + (size_t)c;
    case cgltf_primitive_type_triangles:
    default:
        return t * 3 + (size_t)c;
    }
}

static bool wpe__unpack(const cgltf_accessor* accessor, float* out, size_t components, size_t count) {
    if (!accessor) return false;
    if (cgltf_num_components(accessor->type) != components) return false;
    return cgltf_accessor_unpack_floats(accessor, out, components * count) == components * count;
}

/* Builds a primitive's vertices/indices into the model arrays. Returns WPE_RESULT_ERROR_PARSE on bad data. */
static WPE_Result wpe__build_primitive(wpe__Loader* loader, const wpe__PrimitivePlan* plan,
                                       WPE_MeshPrimitive* out, uint32_t vertex_offset, uint32_t index_offset,
                                       float* scratch) {
    WPE_Model* model = loader->model;
    const cgltf_primitive* p = plan->source;
    const uint32_t n = plan->source_vertex_count;
    const uint32_t tri_count = plan->triangle_count;

    /* Fill source-order vertices into a temporary region of the model's vertex array (or final region). */
    WPE_MeshVertex* source_vertices =
        plan->flat_normals ? wpe_alloc_zeroed(n, sizeof(WPE_MeshVertex)) : &model->vertices[vertex_offset];
    if (!source_vertices) return WPE_RESULT_ERROR_OUT_OF_MEMORY;

    const struct {
        int attr;
        size_t components;
        size_t offset;
    } fields[] = {
        {WPE__ATTR_POSITION, 3, offsetof(WPE_MeshVertex, position)},
        {WPE__ATTR_NORMAL, 3, offsetof(WPE_MeshVertex, normal)},
        {WPE__ATTR_TANGENT, 4, offsetof(WPE_MeshVertex, tangent)},
        {WPE__ATTR_UV0, 2, offsetof(WPE_MeshVertex, uv0)},
        {WPE__ATTR_UV1, 2, offsetof(WPE_MeshVertex, uv1)},
    };
    WPE_Result result = WPE_RESULT_OK;
    for (size_t f = 0; f < sizeof fields / sizeof fields[0] && result == WPE_RESULT_OK; ++f) {
        const cgltf_accessor* accessor = plan->attributes[fields[f].attr];
        if (!accessor) continue;
        if (!wpe__unpack(accessor, scratch, fields[f].components, n)) {
            result = WPE_RESULT_ERROR_PARSE;
            break;
        }
        for (uint32_t v = 0; v < n; ++v) {
            memcpy((unsigned char*)&source_vertices[v] + fields[f].offset,
                   scratch + (size_t)v * fields[f].components, fields[f].components * sizeof(float));
        }
    }

    /* COLOR_0 may be RGB or RGBA. */
    const cgltf_accessor* color = plan->attributes[WPE__ATTR_COLOR];
    const size_t color_components = color ? cgltf_num_components(color->type) : 0;
    if (result == WPE_RESULT_OK && color && (color_components == 3 || color_components == 4)) {
        if (!wpe__unpack(color, scratch, color_components, n)) result = WPE_RESULT_ERROR_PARSE;
        for (uint32_t v = 0; v < n && result == WPE_RESULT_OK; ++v) {
            for (size_t c = 0; c < 4; ++c) {
                source_vertices[v].color[c] =
                    c < color_components ? scratch[(size_t)v * color_components + c] : 1.0f;
            }
        }
    } else {
        for (uint32_t v = 0; v < n; ++v) {
            for (int c = 0; c < 4; ++c)
                source_vertices[v].color[c] = 1.0f;
        }
    }

    if (result == WPE_RESULT_OK && plan->joints && plan->weights) {
        if (!wpe__unpack(plan->weights, scratch, 4, n)) result = WPE_RESULT_ERROR_PARSE;
        for (uint32_t v = 0; v < n && result == WPE_RESULT_OK; ++v) {
            cgltf_uint joints[4] = {0, 0, 0, 0};
            if (!cgltf_accessor_read_uint(plan->joints, v, joints, 4)) {
                result = WPE_RESULT_ERROR_PARSE;
                break;
            }
            float sum = 0.0f;
            for (int c = 0; c < 4; ++c) {
                const float w = scratch[(size_t)v * 4 + (size_t)c];
                source_vertices[v].weights[c] = w > 0.0f ? w : 0.0f;
                sum += source_vertices[v].weights[c];
                if (joints[c] > UINT16_MAX) result = WPE_RESULT_ERROR_PARSE;
                source_vertices[v].joints[c] = (uint16_t)joints[c];
            }
            /* Renormalize (quantized weights rarely sum to exactly 1). */
            if (sum > 0.0f) {
                for (int c = 0; c < 4; ++c)
                    source_vertices[v].weights[c] /= sum;
            }
        }
    }

    /* Bounds from positions. */
    WPE_Aabb bounds = {.min = source_vertices[0].position, .max = source_vertices[0].position};
    for (uint32_t v = 1; v < n; ++v) {
        const WPE_Vec3 pos = source_vertices[v].position;
        bounds.min =
            wpe_vec3(fminf(bounds.min.x, pos.x), fminf(bounds.min.y, pos.y), fminf(bounds.min.z, pos.z));
        bounds.max =
            wpe_vec3(fmaxf(bounds.max.x, pos.x), fmaxf(bounds.max.y, pos.y), fmaxf(bounds.max.z, pos.z));
    }

    /* Triangulate, validating every index. */
    uint32_t* indices = &model->indices[index_offset];
    for (uint32_t t = 0; t < tri_count && result == WPE_RESULT_OK; ++t) {
        for (int c = 0; c < 3; ++c) {
            const size_t element = wpe__primitive_element(p->type, t, c);
            const size_t index = p->indices ? cgltf_accessor_read_index(p->indices, element) : element;
            if (index >= n) {
                wpe_log_error("'%s': vertex index %zu out of range (%u vertices)", loader->path, index, n);
                result = WPE_RESULT_ERROR_PARSE;
                break;
            }
            indices[t * 3 + (uint32_t)c] = (uint32_t)index;
        }
    }

    uint32_t vertex_count = n;
    if (result == WPE_RESULT_OK && plan->flat_normals) {
        /* De-index: three unique vertices per triangle sharing the face normal. */
        WPE_MeshVertex* dst = &model->vertices[vertex_offset];
        for (uint32_t t = 0; t < tri_count; ++t) {
            WPE_MeshVertex corners[3];
            for (int c = 0; c < 3; ++c)
                corners[c] = source_vertices[indices[t * 3 + (uint32_t)c]];
            const WPE_Vec3 normal =
                wpe_vec3_normalize(wpe_vec3_cross(wpe_vec3_sub(corners[1].position, corners[0].position),
                                                  wpe_vec3_sub(corners[2].position, corners[0].position)));
            for (int c = 0; c < 3; ++c) {
                corners[c].normal = normal;
                dst[t * 3 + (uint32_t)c] = corners[c];
                indices[t * 3 + (uint32_t)c] = t * 3 + (uint32_t)c;
            }
        }
        vertex_count = tri_count * 3;
    }
    if (plan->flat_normals) wpe_free(source_vertices);
    if (result != WPE_RESULT_OK) return result;

    *out = (WPE_MeshPrimitive){
        .vertex_offset = vertex_offset,
        .vertex_count = vertex_count,
        .index_offset = index_offset,
        .index_count = tri_count * 3,
        .material = p->material ? (uint32_t)cgltf_material_index(loader->gltf, p->material)
                                : model->material_count - 1,
        .bounds = bounds,
    };
    return WPE_RESULT_OK;
}

static WPE_Result wpe__load_meshes(wpe__Loader* loader) {
    const cgltf_data* gltf = loader->gltf;
    WPE_Model* model = loader->model;

    size_t primitive_total = 0;
    for (cgltf_size m = 0; m < gltf->meshes_count; ++m)
        primitive_total += gltf->meshes[m].primitives_count;
    wpe__PrimitivePlan* plans = wpe_alloc_zeroed(primitive_total ? primitive_total : 1, sizeof *plans);
    bool* usable = wpe_alloc_zeroed(primitive_total ? primitive_total : 1, sizeof *usable);
    if (!plans || !usable) {
        wpe_free(plans);
        wpe_free(usable);
        return WPE_RESULT_ERROR_OUT_OF_MEMORY;
    }

    /* Pass 1: plan every primitive and size the shared arrays. */
    uint64_t vertex_total = 0, index_total = 0;
    uint32_t usable_count = 0, max_source_vertices = 0;
    size_t k = 0;
    for (cgltf_size m = 0; m < gltf->meshes_count; ++m) {
        for (cgltf_size p = 0; p < gltf->meshes[m].primitives_count; ++p, ++k) {
            usable[k] = wpe__plan_primitive(loader, &gltf->meshes[m].primitives[p], &plans[k]);
            if (!usable[k]) continue;
            ++usable_count;
            const uint64_t indices = (uint64_t)plans[k].triangle_count * 3;
            vertex_total += plans[k].flat_normals ? indices : plans[k].source_vertex_count;
            index_total += indices;
            if (plans[k].source_vertex_count > max_source_vertices)
                max_source_vertices = plans[k].source_vertex_count;
        }
    }
    WPE_Result result = WPE_RESULT_OK;
    if (vertex_total > UINT32_MAX || index_total > UINT32_MAX) {
        wpe_log_error("'%s': model too large (%llu vertices, %llu indices)", loader->path,
                      (unsigned long long)vertex_total, (unsigned long long)index_total);
        result = WPE_RESULT_ERROR_UNSUPPORTED;
    }

    float* scratch = NULL;
    if (result == WPE_RESULT_OK) {
        model->vertices = wpe__arena_array(loader, (size_t)vertex_total, sizeof(WPE_MeshVertex));
        model->indices = wpe__arena_array(loader, (size_t)index_total, sizeof(uint32_t));
        model->primitives = wpe__arena_array(loader, usable_count, sizeof(WPE_MeshPrimitive));
        model->meshes = wpe__arena_array(loader, gltf->meshes_count, sizeof(WPE_Mesh));
        scratch = wpe_alloc((size_t)(max_source_vertices ? max_source_vertices : 1) * 4 * sizeof(float));
        if ((vertex_total && !model->vertices) || (index_total && !model->indices) ||
            (usable_count && !model->primitives) || (gltf->meshes_count && !model->meshes) || !scratch) {
            result = WPE_RESULT_ERROR_OUT_OF_MEMORY;
        }
    }

    /* Pass 2: build. */
    uint32_t vertex_offset = 0, index_offset = 0;
    k = 0;
    for (cgltf_size m = 0; m < gltf->meshes_count && result == WPE_RESULT_OK; ++m) {
        WPE_Mesh* mesh = &model->meshes[m];
        wpe__copy_name(mesh->name, gltf->meshes[m].name);
        mesh->first_primitive = model->primitive_count;
        for (cgltf_size p = 0; p < gltf->meshes[m].primitives_count && result == WPE_RESULT_OK; ++p, ++k) {
            if (!usable[k]) continue;
            WPE_MeshPrimitive* primitive = &model->primitives[model->primitive_count];
            result = wpe__build_primitive(loader, &plans[k], primitive, vertex_offset, index_offset, scratch);
            if (result != WPE_RESULT_OK) break;
            vertex_offset += primitive->vertex_count;
            index_offset += primitive->index_count;
            if (mesh->primitive_count == 0) {
                mesh->bounds = primitive->bounds;
            } else {
                const WPE_Aabb b = primitive->bounds;
                mesh->bounds.min =
                    wpe_vec3(fminf(mesh->bounds.min.x, b.min.x), fminf(mesh->bounds.min.y, b.min.y),
                             fminf(mesh->bounds.min.z, b.min.z));
                mesh->bounds.max =
                    wpe_vec3(fmaxf(mesh->bounds.max.x, b.max.x), fmaxf(mesh->bounds.max.y, b.max.y),
                             fmaxf(mesh->bounds.max.z, b.max.z));
            }
            ++mesh->primitive_count;
            ++model->primitive_count;
        }
    }
    if (result == WPE_RESULT_OK) {
        model->mesh_count = (uint32_t)gltf->meshes_count;
        model->vertex_count = vertex_offset;
        model->index_count = index_offset;
    } else if (result == WPE_RESULT_ERROR_PARSE) {
        wpe_log_error("'%s': malformed vertex data", loader->path);
    }

    wpe_free(scratch);
    wpe_free(plans);
    wpe_free(usable);
    return result;
}

/* ---- Nodes, skins, animations --------------------------------------------------------------------- */

static WPE_Result wpe__load_nodes(wpe__Loader* loader) {
    const cgltf_data* gltf = loader->gltf;
    WPE_Model* model = loader->model;
    model->node_count = (uint32_t)gltf->nodes_count;
    model->nodes = wpe__arena_array(loader, gltf->nodes_count, sizeof(WPE_ModelNode));
    if (gltf->nodes_count && !model->nodes) return WPE_RESULT_ERROR_OUT_OF_MEMORY;

    for (cgltf_size i = 0; i < gltf->nodes_count; ++i) {
        const cgltf_node* src = &gltf->nodes[i];
        WPE_ModelNode* node = &model->nodes[i];
        wpe__copy_name(node->name, src->name);
        node->parent = src->parent ? (uint32_t)cgltf_node_index(gltf, src->parent) : WPE_MODEL_NONE;
        node->first_child = WPE_MODEL_NONE;
        node->next_sibling = WPE_MODEL_NONE;
        node->mesh = src->mesh ? (uint32_t)cgltf_mesh_index(gltf, src->mesh) : WPE_MODEL_NONE;
        node->skin = src->skin ? (uint32_t)cgltf_skin_index(gltf, src->skin) : WPE_MODEL_NONE;
        node->translation = wpe_vec3(0, 0, 0);
        node->rotation = wpe_quat_identity();
        node->scale = wpe_vec3(1, 1, 1);

        if (src->has_matrix) {
            WPE_Mat4 m;
            memcpy(m.m, src->matrix, sizeof m.m);
            if (!wpe_mat4_decompose(&m, &node->translation, &node->rotation, &node->scale)) {
                /* Degenerate (zero-scale) matrix: keep its translation and collapse the node. */
                node->translation = wpe_vec3(m.m[3][0], m.m[3][1], m.m[3][2]);
                node->scale = wpe_vec3(0, 0, 0);
            }
        } else {
            if (src->has_translation)
                node->translation = wpe_vec3(src->translation[0], src->translation[1], src->translation[2]);
            if (src->has_rotation) {
                node->rotation = wpe_quat_normalize((WPE_Quat){.x = src->rotation[0],
                                                               .y = src->rotation[1],
                                                               .z = src->rotation[2],
                                                               .w = src->rotation[3]});
            }
            if (src->has_scale) node->scale = wpe_vec3(src->scale[0], src->scale[1], src->scale[2]);
        }
    }

    /* Child lists in glTF order (built back to front so first_child is the first glTF child). */
    for (cgltf_size i = 0; i < gltf->nodes_count; ++i) {
        const cgltf_node* src = &gltf->nodes[i];
        for (cgltf_size c = src->children_count; c-- > 0;) {
            const uint32_t child = (uint32_t)cgltf_node_index(gltf, src->children[c]);
            model->nodes[child].next_sibling = model->nodes[i].first_child;
            model->nodes[i].first_child = child;
        }
    }

    /* Roots: the default scene (or the first scene), falling back to all parentless nodes. */
    const cgltf_scene* scene = gltf->scene ? gltf->scene : (gltf->scenes_count ? &gltf->scenes[0] : NULL);
    uint32_t root_count = 0;
    if (scene) {
        root_count = (uint32_t)scene->nodes_count;
    } else {
        for (uint32_t i = 0; i < model->node_count; ++i)
            root_count += model->nodes[i].parent == WPE_MODEL_NONE;
    }
    model->root_nodes = wpe__arena_array(loader, root_count, sizeof(uint32_t));
    if (root_count && !model->root_nodes) return WPE_RESULT_ERROR_OUT_OF_MEMORY;
    if (scene) {
        for (uint32_t r = 0; r < root_count; ++r)
            model->root_nodes[r] = (uint32_t)cgltf_node_index(gltf, scene->nodes[r]);
    } else {
        uint32_t r = 0;
        for (uint32_t i = 0; i < model->node_count; ++i) {
            if (model->nodes[i].parent == WPE_MODEL_NONE) model->root_nodes[r++] = i;
        }
    }
    model->root_count = root_count;
    return WPE_RESULT_OK;
}

static WPE_Result wpe__load_skins(wpe__Loader* loader) {
    const cgltf_data* gltf = loader->gltf;
    WPE_Model* model = loader->model;
    uint32_t joint_total = 0;
    for (cgltf_size s = 0; s < gltf->skins_count; ++s)
        joint_total += (uint32_t)gltf->skins[s].joints_count;

    model->skins = wpe__arena_array(loader, gltf->skins_count, sizeof(WPE_Skin));
    model->skin_joints = wpe__arena_array(loader, joint_total, sizeof(uint32_t));
    model->inverse_bind_matrices = wpe__arena_array(loader, joint_total, sizeof(WPE_Mat4));
    if ((gltf->skins_count && !model->skins) ||
        (joint_total && (!model->skin_joints || !model->inverse_bind_matrices))) {
        return WPE_RESULT_ERROR_OUT_OF_MEMORY;
    }

    uint32_t offset = 0;
    for (cgltf_size s = 0; s < gltf->skins_count; ++s) {
        const cgltf_skin* src = &gltf->skins[s];
        WPE_Skin* skin = &model->skins[s];
        wpe__copy_name(skin->name, src->name);
        skin->first_joint = offset;
        skin->joint_count = (uint32_t)src->joints_count;
        for (cgltf_size j = 0; j < src->joints_count; ++j) {
            model->skin_joints[offset + j] = (uint32_t)cgltf_node_index(gltf, src->joints[j]);
            model->inverse_bind_matrices[offset + j] = wpe_mat4_identity();
        }
        if (src->inverse_bind_matrices) {
            if (src->inverse_bind_matrices->count < src->joints_count ||
                !wpe__unpack(src->inverse_bind_matrices, (float*)&model->inverse_bind_matrices[offset], 16,
                             src->joints_count)) {
                wpe_log_error("'%s': invalid inverse bind matrices for skin %zu", loader->path, (size_t)s);
                return WPE_RESULT_ERROR_PARSE;
            }
        }
        offset += skin->joint_count;
    }
    model->skin_count = (uint32_t)gltf->skins_count;
    return WPE_RESULT_OK;
}

static WPE_Result wpe__load_animations(wpe__Loader* loader) {
    const cgltf_data* gltf = loader->gltf;
    WPE_Model* model = loader->model;
    uint32_t channel_total = 0;
    for (cgltf_size a = 0; a < gltf->animations_count; ++a)
        channel_total += (uint32_t)gltf->animations[a].channels_count;

    model->animations = wpe__arena_array(loader, gltf->animations_count, sizeof(WPE_Animation));
    model->channels = wpe__arena_array(loader, channel_total, sizeof(WPE_AnimationChannel));
    if ((gltf->animations_count && !model->animations) || (channel_total && !model->channels)) {
        return WPE_RESULT_ERROR_OUT_OF_MEMORY;
    }

    for (cgltf_size a = 0; a < gltf->animations_count; ++a) {
        const cgltf_animation* src = &gltf->animations[a];
        WPE_Animation* anim = &model->animations[a];
        wpe__copy_name(anim->name, src->name);
        anim->first_channel = model->channel_count;

        for (cgltf_size c = 0; c < src->channels_count; ++c) {
            const cgltf_animation_channel* ch = &src->channels[c];
            if (!ch->target_node || !ch->sampler || !ch->sampler->input || !ch->sampler->output) continue;
            if (ch->target_path == cgltf_animation_path_type_weights) {
                wpe_log_warn("'%s': morph-weight animation channels are ignored", loader->path);
                continue;
            }
            WPE_AnimationChannel out = {.node = (uint32_t)cgltf_node_index(gltf, ch->target_node)};
            size_t components = 3;
            switch (ch->target_path) {
            case cgltf_animation_path_type_translation:
                out.path = WPE_ANIM_TRANSLATION;
                break;
            case cgltf_animation_path_type_rotation:
                out.path = WPE_ANIM_ROTATION;
                components = 4;
                break;
            case cgltf_animation_path_type_scale:
                out.path = WPE_ANIM_SCALE;
                break;
            default:
                continue;
            }
            switch (ch->sampler->interpolation) {
            case cgltf_interpolation_type_step:
                out.interpolation = WPE_INTERP_STEP;
                break;
            case cgltf_interpolation_type_cubic_spline:
                out.interpolation = WPE_INTERP_CUBIC_SPLINE;
                break;
            case cgltf_interpolation_type_linear:
            default:
                out.interpolation = WPE_INTERP_LINEAR;
                break;
            }

            const size_t keys = ch->sampler->input->count;
            const size_t values_per_key = out.interpolation == WPE_INTERP_CUBIC_SPLINE ? 3 : 1;
            if (keys == 0 || keys > UINT32_MAX || ch->sampler->output->count != keys * values_per_key) {
                wpe_log_warn("'%s': skipping animation channel with inconsistent key counts", loader->path);
                continue;
            }
            float* times = wpe__arena_array(loader, keys, sizeof(float));
            float* values = wpe__arena_array(loader, keys * values_per_key * components, sizeof(float));
            if (!times || !values) return WPE_RESULT_ERROR_OUT_OF_MEMORY;
            if (!wpe__unpack(ch->sampler->input, times, 1, keys) ||
                !wpe__unpack(ch->sampler->output, values, components, keys * values_per_key)) {
                wpe_log_warn("'%s': skipping animation channel with unreadable data", loader->path);
                continue;
            }
            bool ascending = true;
            for (size_t k = 1; k < keys; ++k)
                ascending = ascending && times[k] >= times[k - 1];
            if (!ascending || !isfinite(times[0])) {
                wpe_log_warn("'%s': skipping animation channel with unsorted key times", loader->path);
                continue;
            }
            out.key_count = (uint32_t)keys;
            out.times = times;
            out.values = values;
            if (times[keys - 1] > anim->duration) anim->duration = times[keys - 1];
            model->channels[model->channel_count++] = out;
            ++anim->channel_count;
        }
    }
    model->animation_count = (uint32_t)gltf->animations_count;
    return WPE_RESULT_OK;
}

/* ---- Entry points --------------------------------------------------------------------------------- */

static WPE_Result wpe__check_required_extensions(const char* path, const cgltf_data* gltf) {
    for (cgltf_size i = 0; i < gltf->extensions_required_count; ++i) {
        const char* name = gltf->extensions_required[i];
        bool supported = false;
        for (size_t s = 0;
             s < sizeof wpe__supported_required_extensions / sizeof wpe__supported_required_extensions[0];
             ++s) {
            supported = supported || strcmp(name, wpe__supported_required_extensions[s]) == 0;
        }
        if (!supported) {
            wpe_log_error("'%s' requires unsupported glTF extension %s", path, name);
            return WPE_RESULT_ERROR_UNSUPPORTED;
        }
    }
    return WPE_RESULT_OK;
}

static WPE_Result wpe__load_materials_and_textures(wpe__Loader* loader) {
    const cgltf_data* gltf = loader->gltf;
    WPE_Model* model = loader->model;

    /* Each image can produce at most two textures (sRGB and linear interpretations). */
    loader->texture_jobs = wpe_alloc_zeroed(gltf->images_count * 2 + 1, sizeof *loader->texture_jobs);
    loader->image_texture_map = wpe_alloc((gltf->images_count * 2 + 1) * sizeof(uint32_t));
    model->materials = wpe__arena_array(loader, gltf->materials_count + 1, sizeof(WPE_Material));
    if (!loader->texture_jobs || !loader->image_texture_map || !model->materials)
        return WPE_RESULT_ERROR_OUT_OF_MEMORY;
    for (size_t i = 0; i < gltf->images_count * 2 + 1; ++i)
        loader->image_texture_map[i] = WPE_MODEL_NONE;

    for (cgltf_size m = 0; m < gltf->materials_count; ++m) {
        model->materials[m] = wpe__convert_material(loader, &gltf->materials[m]);
    }
    model->materials[gltf->materials_count] = wpe__default_material();
    model->material_count = (uint32_t)gltf->materials_count + 1;

    wpe__DecodeContext context = {.loader = loader};
    wpe_jobs_parallel_for(loader->options->jobs, loader->texture_job_count, 1, wpe__decode_textures,
                          &context);

    model->textures = wpe__arena_array(loader, loader->texture_job_count, sizeof(WPE_Texture*));
    if (loader->texture_job_count && !model->textures) return WPE_RESULT_ERROR_OUT_OF_MEMORY;
    model->texture_count = loader->texture_job_count;

    bool any_failed = false;
    for (uint32_t i = 0; i < loader->texture_job_count; ++i) {
        model->textures[i] = loader->texture_jobs[i].texture; /* Ownership moves to the model. */
        loader->texture_jobs[i].texture = NULL;
        if (loader->texture_jobs[i].result == WPE_RESULT_ERROR_OUT_OF_MEMORY)
            return WPE_RESULT_ERROR_OUT_OF_MEMORY;
        any_failed = any_failed || loader->texture_jobs[i].result != WPE_RESULT_OK;
    }
    if (any_failed) {
        /* Fail soft: materials fall back to their factors where a texture could not be loaded. */
        wpe_log_warn("'%s': some textures failed to load and are ignored", loader->path);
    }
    return WPE_RESULT_OK;
}

WPE_Result wpe_model_load(const char* path, const WPE_ModelLoadOptions* options, WPE_Model** out_model) {
    if (!path || !out_model) return WPE_RESULT_ERROR_NULL_ARG;
    *out_model = NULL;
    const WPE_ModelLoadOptions default_options = {0};
    if (!options) options = &default_options;

    cgltf_data* gltf = NULL;
    cgltf_result parsed = cgltf_parse_file(&wpe__cgltf_options, path, &gltf);
    if (parsed == cgltf_result_success) parsed = cgltf_load_buffers(&wpe__cgltf_options, gltf, path);
    if (parsed == cgltf_result_success) parsed = cgltf_validate(gltf);
    if (parsed != cgltf_result_success) {
        wpe_log_error("failed to load glTF '%s': %s", path, wpe__cgltf_result_name(parsed));
        cgltf_free(gltf);
        return wpe__cgltf_to_result(parsed);
    }

    WPE_Result result = wpe__check_required_extensions(path, gltf);
    WPE_Model* model = NULL;
    if (result == WPE_RESULT_OK) {
        model = wpe_alloc_zeroed(1, sizeof *model);
        if (!model) result = WPE_RESULT_ERROR_OUT_OF_MEMORY;
    }

    wpe__Loader loader = {.path = path, .gltf = gltf, .model = model, .options = options};
    if (result == WPE_RESULT_OK) {
        wpe_arena_init(&model->wpe__memory, (size_t)1 << 20);
        loader.arena = &model->wpe__memory;
        result = wpe__load_materials_and_textures(&loader);
    }
    if (result == WPE_RESULT_OK) result = wpe__load_meshes(&loader);
    if (result == WPE_RESULT_OK) result = wpe__load_nodes(&loader);
    if (result == WPE_RESULT_OK) result = wpe__load_skins(&loader);
    if (result == WPE_RESULT_OK) result = wpe__load_animations(&loader);

    /* Textures still held by jobs exist only when loading failed before ownership moved. */
    for (uint32_t i = 0; i < loader.texture_job_count; ++i)
        wpe_texture_destroy(loader.texture_jobs[i].texture);
    wpe_free(loader.texture_jobs);
    wpe_free(loader.image_texture_map);
    cgltf_free(gltf);

    if (result != WPE_RESULT_OK) {
        if (result == WPE_RESULT_ERROR_OUT_OF_MEMORY) wpe_log_error("out of memory loading '%s'", path);
        wpe_model_destroy(model);
        return result;
    }
    wpe_log_debug(
        "loaded '%s': %u meshes, %u primitives, %u vertices, %u triangles, %u materials, %u textures, "
        "%u nodes, %u skins, %u animations",
        path, model->mesh_count, model->primitive_count, model->vertex_count, model->index_count / 3,
        model->material_count, model->texture_count, model->node_count, model->skin_count,
        model->animation_count);
    *out_model = model;
    return WPE_RESULT_OK;
}

void wpe_model_destroy(WPE_Model* model) {
    if (!model) return;
    for (uint32_t i = 0; i < model->texture_count; ++i)
        wpe_texture_destroy(model->textures[i]);
    wpe_arena_release(&model->wpe__memory);
    wpe_free(model);
}

WPE_Mat4 wpe_model_node_local_matrix(const WPE_ModelNode* node) {
    wpe_assert(node);
    return wpe_mat4_from_trs(node->translation, node->rotation, node->scale);
}
