#include "wpe_test.h"

#include <stdio.h>

#include "assets/font.h"
#include "assets/image.h"
#include "assets/model.h"
#include "platform/platform.h"

#define ASSET(path) WPE_TEST_ASSETS_DIR "/" path

/* ---- Helpers --------------------------------------------------------------------------------------- */

static void write_file(const char* path, const char* text) {
    FILE* f = fopen(path, "wb");
    WPE_TEST_ASSERT(f != NULL);
    const size_t length = strlen(text);
    WPE_TEST_ASSERT_EQ_UINT(length, fwrite(text, 1, length, f));
    WPE_TEST_ASSERT(fclose(f) == 0);
}

static void base64_encode(const void* data, size_t size, char* out) {
    static const char alphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    const unsigned char* bytes = data;
    size_t o = 0;
    for (size_t i = 0; i < size; i += 3) {
        const uint32_t b0 = bytes[i], b1 = i + 1 < size ? bytes[i + 1] : 0,
                       b2 = i + 2 < size ? bytes[i + 2] : 0;
        const uint32_t triple = (b0 << 16) | (b1 << 8) | b2;
        out[o++] = alphabet[(triple >> 18) & 63];
        out[o++] = alphabet[(triple >> 12) & 63];
        out[o++] = i + 1 < size ? alphabet[(triple >> 6) & 63] : '=';
        out[o++] = i + 2 < size ? alphabet[triple & 63] : '=';
    }
    out[o] = '\0';
}

static WPE_Model* load_model(const char* path, uint32_t max_texture_size, WPE_JobSystem* jobs) {
    WPE_Model* model = NULL;
    const WPE_ModelLoadOptions options = {.jobs = jobs, .max_texture_size = max_texture_size};
    WPE_TEST_ASSERT_OK(wpe_model_load(path, &options, &model));
    WPE_TEST_ASSERT(model != NULL);
    return model;
}

/* Structural invariants every loaded model must satisfy. */
static void assert_model_consistent(const WPE_Model* m) {
    for (uint32_t p = 0; p < m->primitive_count; ++p) {
        const WPE_MeshPrimitive* prim = &m->primitives[p];
        WPE_TEST_ASSERT(prim->vertex_offset + prim->vertex_count <= m->vertex_count);
        WPE_TEST_ASSERT(prim->index_offset + prim->index_count <= m->index_count);
        WPE_TEST_ASSERT(prim->index_count % 3 == 0);
        WPE_TEST_ASSERT(prim->material < m->material_count);
        for (uint32_t i = 0; i < prim->index_count; ++i)
            WPE_TEST_ASSERT(m->indices[prim->index_offset + i] < prim->vertex_count);
        WPE_TEST_ASSERT(prim->bounds.min.x <= prim->bounds.max.x && prim->bounds.min.y <= prim->bounds.max.y);
    }
    for (uint32_t i = 0; i < m->mesh_count; ++i) {
        WPE_TEST_ASSERT(m->meshes[i].first_primitive + m->meshes[i].primitive_count <= m->primitive_count);
    }
    for (uint32_t i = 0; i < m->material_count; ++i) {
        const WPE_TextureRef* refs[] = {&m->materials[i].diffuse_texture, &m->materials[i].normal_texture,
                                        &m->materials[i].metallic_roughness_texture,
                                        &m->materials[i].emissive_texture,
                                        &m->materials[i].occlusion_texture};
        for (size_t r = 0; r < sizeof refs / sizeof refs[0]; ++r) {
            WPE_TEST_ASSERT(refs[r]->texture == WPE_MODEL_NONE || refs[r]->texture < m->texture_count);
        }
    }
    for (uint32_t n = 0; n < m->node_count; ++n) {
        const WPE_ModelNode* node = &m->nodes[n];
        WPE_TEST_ASSERT(node->parent == WPE_MODEL_NONE || node->parent < m->node_count);
        WPE_TEST_ASSERT(node->mesh == WPE_MODEL_NONE || node->mesh < m->mesh_count);
        WPE_TEST_ASSERT(node->skin == WPE_MODEL_NONE || node->skin < m->skin_count);
        for (uint32_t c = node->first_child; c != WPE_MODEL_NONE; c = m->nodes[c].next_sibling) {
            WPE_TEST_ASSERT(m->nodes[c].parent == n);
        }
    }
    for (uint32_t r = 0; r < m->root_count; ++r)
        WPE_TEST_ASSERT(m->root_nodes[r] < m->node_count);
    for (uint32_t c = 0; c < m->channel_count; ++c) {
        WPE_TEST_ASSERT(m->channels[c].node < m->node_count);
        WPE_TEST_ASSERT(m->channels[c].key_count > 0);
    }
}

/* ---- Images ------------------------------------------------------------------------------------------ */

static void test_assets_image_decode_png_roundtrip(void) {
    const WPE_Color pixels[6] = {0xFFFF0000u, 0x8000FF00u, 0x000000FFu,
                                 0xFFFFFFFFu, 0xFF000000u, 0x7F123456u};
    WPE_TEST_ASSERT_OK(wpe_image_save_png("roundtrip.png", 3, 2, pixels));
    void* data = NULL;
    size_t size = 0;
    WPE_TEST_ASSERT_OK(wpe_file_read_all("roundtrip.png", &data, &size));
    WPE_Image image;
    const WPE_Result decoded = wpe_image_decode(data, size, "roundtrip", &image);
    wpe_free(data);
    WPE_TEST_ASSERT_OK(decoded);
    WPE_TEST_ASSERT_EQ_UINT(3, image.width);
    WPE_TEST_ASSERT_EQ_UINT(2, image.height);
    for (int i = 0; i < 6; ++i)
        WPE_TEST_ASSERT_EQ_UINT(pixels[i], image.pixels[i]);
    wpe_image_free(&image);
    remove("roundtrip.png");
}

static void test_assets_image_decode_rejects_garbage(void) {
    const char garbage[] = "definitely not an image";
    WPE_Image image;
    wpe_log_set_stderr_enabled(false);
    WPE_TEST_ASSERT_EQ_INT(WPE_RESULT_ERROR_UNSUPPORTED,
                           wpe_image_decode(garbage, sizeof garbage, "garbage", &image));
    /* A truncated PNG (valid header, missing data) must fail cleanly. */
    const WPE_Color pixels[4] = {0};
    WPE_TEST_ASSERT_OK(wpe_image_save_png("truncated.png", 2, 2, pixels));
    void* data = NULL;
    size_t size = 0;
    WPE_TEST_ASSERT_OK(wpe_file_read_all("truncated.png", &data, &size));
    const WPE_Result truncated = wpe_image_decode(data, size / 2, "truncated", &image);
    wpe_log_set_stderr_enabled(true);
    wpe_free(data);
    remove("truncated.png");
    WPE_TEST_ASSERT(truncated != WPE_RESULT_OK);
    WPE_TEST_ASSERT(image.pixels == NULL);
}

static void test_assets_file_read_missing(void) {
    void* data = (void*)&data;
    size_t size = 1;
    wpe_log_set_stderr_enabled(false);
    WPE_TEST_ASSERT_EQ_INT(WPE_RESULT_ERROR_IO, wpe_file_read_all("does/not/exist.bin", &data, &size));
    wpe_log_set_stderr_enabled(true);
    WPE_TEST_ASSERT(data == NULL && size == 0);
}

/* ---- Fonts ---------------------------------------------------------------------------------------- */

/* The built-in font is small.png compiled in: the same glyphs and the same atlas. */
static void test_assets_font_builtin_matches_small_png(void) {
    WPE_BitmapFont* file = NULL;
    WPE_BitmapFont* builtin = NULL;
    WPE_TEST_ASSERT_OK(wpe_font_load(ASSET("fonts/small.png"), &file));
    WPE_TEST_ASSERT_OK(wpe_font_create_builtin(&builtin));
    WPE_TEST_ASSERT_EQ_UINT(file->line_height, builtin->line_height);
    WPE_TEST_ASSERT(memcmp(file->glyphs, builtin->glyphs, sizeof file->glyphs) == 0);
    const WPE_TextureLevel* a = &file->atlas->levels[0];
    const WPE_TextureLevel* b = &builtin->atlas->levels[0];
    WPE_TEST_ASSERT(a->width == b->width && a->height == b->height);
    WPE_TEST_ASSERT(memcmp(a->texels, b->texels, (size_t)a->width * a->height * sizeof *a->texels) == 0);
    WPE_TEST_ASSERT_EQ_INT(WPE_RESULT_ERROR_NULL_ARG, wpe_font_create_builtin(NULL));
    wpe_font_destroy(builtin);
    wpe_font_destroy(file);
}

static void test_assets_font_loads_sample_sheet(void) {
    WPE_BitmapFont* font = NULL;
    WPE_TEST_ASSERT_OK(wpe_font_load(ASSET("fonts/small.png"), &font));
    WPE_TEST_ASSERT_EQ_UINT(7, font->line_height); /* 6-texel glyphs + 1 leading */

    const WPE_FontGlyph* a = wpe_font_glyph(font, 'A');
    WPE_TEST_ASSERT_EQ_UINT(4, a->width);
    WPE_TEST_ASSERT_EQ_UINT(6, a->height);
    /* The '!' cell is 2 texels wide in the sheet (with magenta inside the cell, made transparent). */
    WPE_TEST_ASSERT_EQ_UINT(2, wpe_font_glyph(font, '!')->width);
    WPE_TEST_ASSERT_EQ_UINT(3, wpe_font_glyph(font, ' ')->width);
    WPE_TEST_ASSERT(wpe_font_glyph(font, '\t') == wpe_font_glyph(font, '?'));
    WPE_TEST_ASSERT(wpe_font_glyph(font, (char)200) == wpe_font_glyph(font, '?'));

    /* No magenta survives in the atlas; ink is white. */
    const WPE_TextureLevel* atlas = &font->atlas->levels[0];
    bool has_ink = false;
    for (uint32_t i = 0; i < atlas->width * atlas->height; ++i) {
        WPE_TEST_ASSERT(atlas->texels[i] != 0xFFFF00FFu);
        has_ink = has_ink || atlas->texels[i] == 0xFFFFFFFFu;
    }
    WPE_TEST_ASSERT(has_ink);

    uint32_t w = 0, h = 0;
    wpe_font_measure(font, "AA", &w, &h);
    WPE_TEST_ASSERT_EQ_UINT(4 + 1 + 4, w);
    WPE_TEST_ASSERT_EQ_UINT(6, h);
    wpe_font_measure(font, "A\nAAA\n", &w, &h);
    WPE_TEST_ASSERT_EQ_UINT(4 * 3 + 2, w);
    WPE_TEST_ASSERT_EQ_UINT(3 * 7 - 1, h);
    wpe_font_measure(font, "", &w, &h);
    WPE_TEST_ASSERT_EQ_UINT(0, w);
    wpe_font_destroy(font);
}

static void test_assets_font_rejects_wrong_glyph_count(void) {
    /* 5x3 sheet with two cells: M . M . M framed by magenta rows. */
    const uint32_t M = 0xFFFF00FFu, W = 0xFFFFFFFFu;
    const uint32_t sheet[15] = {M, M, M, M, M, M, W, M, W, M, M, M, M, M, M};
    WPE_BitmapFont* font = NULL;
    wpe_log_set_stderr_enabled(false);
    WPE_TEST_ASSERT_EQ_INT(WPE_RESULT_ERROR_PARSE, wpe_font_create(5, 3, sheet, &font));
    wpe_log_set_stderr_enabled(true);
    WPE_TEST_ASSERT(font == NULL);
}

/* ---- Models: real sample assets ------------------------------------------------------------------- */

static void test_assets_model_damaged_helmet(void) {
    WPE_JobSystem* jobs = NULL;
    WPE_TEST_ASSERT_OK(wpe_jobs_create(3, &jobs));
    WPE_Model* m = load_model(ASSET("meshes/damaged_helmet.glb"), 256, jobs);
    assert_model_consistent(m);
    WPE_TEST_ASSERT_EQ_UINT(1, m->mesh_count);
    WPE_TEST_ASSERT_EQ_UINT(1, m->primitive_count);
    WPE_TEST_ASSERT_EQ_UINT(15452 * 3, m->index_count);
    WPE_TEST_ASSERT_EQ_UINT(2, m->material_count); /* helmet + default */

    const WPE_Material* mat = &m->materials[0];
    WPE_TEST_ASSERT_EQ_INT(WPE_MATERIAL_METALLIC_ROUGHNESS, mat->workflow);
    WPE_TEST_ASSERT(mat->diffuse_texture.texture != WPE_MODEL_NONE);
    WPE_TEST_ASSERT(mat->normal_texture.texture != WPE_MODEL_NONE);
    WPE_TEST_ASSERT(mat->metallic_roughness_texture.texture != WPE_MODEL_NONE);
    WPE_TEST_ASSERT(mat->emissive_texture.texture != WPE_MODEL_NONE);
    WPE_TEST_ASSERT(mat->occlusion_texture.texture != WPE_MODEL_NONE);
    /* Color textures are sRGB, data textures linear; all JPEGs decoded and capped at 256. */
    WPE_TEST_ASSERT(m->textures[mat->diffuse_texture.texture]->srgb);
    WPE_TEST_ASSERT(!m->textures[mat->normal_texture.texture]->srgb);
    for (uint32_t t = 0; t < m->texture_count; ++t) {
        WPE_TEST_ASSERT(m->textures[t] != NULL);
        WPE_TEST_ASSERT(m->textures[t]->levels[0].width <= 256 && m->textures[t]->levels[0].height <= 256);
    }
    /* Normals are unit length. */
    for (uint32_t v = 0; v < m->vertex_count; v += 97)
        WPE_TEST_ASSERT_NEAR(1.0, wpe_vec3_length(m->vertices[v].normal), 1e-3);
    wpe_model_destroy(m);
    wpe_jobs_destroy(jobs);
}

static void test_assets_model_car_concept(void) {
    WPE_JobSystem* jobs = NULL;
    WPE_TEST_ASSERT_OK(wpe_jobs_create(WPE_JOBS_AUTO_WORKERS, &jobs));
    WPE_Model* m = load_model(ASSET("meshes/car_concept.glb"), 128, jobs);
    assert_model_consistent(m);
    WPE_TEST_ASSERT_EQ_UINT(97, m->mesh_count);
    WPE_TEST_ASSERT_EQ_UINT(109, m->primitive_count);
    WPE_TEST_ASSERT_EQ_UINT(213347u * 3u, m->index_count);
    WPE_TEST_ASSERT_EQ_UINT(101, m->node_count);
    /* Tangents are imported when present. */
    bool has_tangent = false;
    for (uint32_t v = 0; v < m->vertex_count && !has_tangent; ++v)
        has_tangent = m->vertices[v].tangent[3] != 0.0f;
    WPE_TEST_ASSERT(has_tangent);
    wpe_model_destroy(m);
    wpe_jobs_destroy(jobs);
}

static void test_assets_model_fridge_with_required_transmission(void) {
    WPE_Model* m = load_model(ASSET("meshes/commercial_refrigerator.glb"), 64, NULL);
    assert_model_consistent(m);
    bool has_blended_glass = false;
    for (uint32_t i = 0; i + 1 < m->material_count; ++i) {
        if (m->materials[i].alpha_mode == WPE_ALPHA_BLEND) {
            has_blended_glass = true;
            WPE_TEST_ASSERT(m->materials[i].diffuse_factor.w < 1.0f);
        }
    }
    WPE_TEST_ASSERT(has_blended_glass);
    WPE_TEST_ASSERT_EQ_UINT(1, m->animation_count);
    wpe_model_destroy(m);
}

static void test_assets_model_rigged_figure(void) {
    WPE_Model* m = load_model(ASSET("meshes/RiggedFigure.glb"), 0, NULL);
    assert_model_consistent(m);
    WPE_TEST_ASSERT_EQ_UINT(1, m->skin_count);
    WPE_TEST_ASSERT_EQ_UINT(19, m->skins[0].joint_count);
    WPE_TEST_ASSERT_EQ_UINT(1, m->animation_count);
    WPE_TEST_ASSERT_EQ_UINT(57, m->animations[0].channel_count);
    WPE_TEST_ASSERT(m->animations[0].duration > 0.0f);
    for (uint32_t v = 0; v < m->vertex_count; ++v) {
        const WPE_MeshVertex* vert = &m->vertices[v];
        WPE_TEST_ASSERT_NEAR(1.0, vert->weights[0] + vert->weights[1] + vert->weights[2] + vert->weights[3],
                             1e-4);
        for (int j = 0; j < 4; ++j)
            WPE_TEST_ASSERT(vert->joints[j] < 19);
    }
    for (uint32_t c = 0; c < m->channel_count; ++c) {
        const WPE_AnimationChannel* ch = &m->channels[c];
        for (uint32_t k = 1; k < ch->key_count; ++k)
            WPE_TEST_ASSERT(ch->times[k] >= ch->times[k - 1]);
    }
    wpe_model_destroy(m);
}

/* ---- Models: handwritten edge cases --------------------------------------------------------------- */

/*
 * Writes a .gltf whose single buffer is a data URI holding `float_count` floats followed by
 * optional uint16 indices. `rest_json` continues the top-level object after the "buffers" entry.
 */
static void write_gltf(const char* path, const char* rest_json, const float* floats, size_t float_count,
                       const uint16_t* indices, size_t index_count) {
    unsigned char buffer[1024];
    const size_t float_bytes = float_count * sizeof(float);
    memcpy(buffer, floats, float_bytes);
    if (indices) memcpy(buffer + float_bytes, indices, index_count * sizeof(uint16_t));
    const size_t total = float_bytes + index_count * sizeof(uint16_t);
    char base64[1500];
    base64_encode(buffer, total, base64);
    static char json[4096];
    snprintf(json, sizeof json,
             "{\"asset\":{\"version\":\"2.0\"},"
             "\"buffers\":[{\"byteLength\":%zu,\"uri\":\"data:application/octet-stream;base64,%s\"}],%s",
             total, base64, rest_json);
    write_file(path, json);
}

static void test_assets_model_strip_without_normals_gets_flat_normals(void) {
    /* Triangle strip of 4 vertices (2 triangles) in the XY plane, no NORMAL attribute. */
    const float positions[12] = {0, 0, 0, 1, 0, 0, 0, 1, 0, 1, 1, 0};
    write_gltf("strip.gltf",
               "\"bufferViews\":[{\"buffer\":0,\"byteLength\":48}],"
               "\"accessors\":[{\"bufferView\":0,\"componentType\":5126,\"count\":4,\"type\":\"VEC3\","
               "\"min\":[0,0,0],\"max\":[1,1,0]}],"
               "\"meshes\":[{\"primitives\":[{\"attributes\":{\"POSITION\":0},\"mode\":5}]}],"
               "\"nodes\":[{\"mesh\":0,\"translation\":[1,2,3]}],\"scenes\":[{\"nodes\":[0]}],\"scene\":0}",
               positions, 12, NULL, 0);
    WPE_Model* m = load_model("strip.gltf", 0, NULL);
    assert_model_consistent(m);
    WPE_TEST_ASSERT_EQ_UINT(1, m->primitive_count);
    WPE_TEST_ASSERT_EQ_UINT(6, m->primitives[0].index_count);
    WPE_TEST_ASSERT_EQ_UINT(6, m->primitives[0].vertex_count); /* de-indexed for flat normals */
    for (uint32_t t = 0; t < 2; ++t) {
        /* Both strip triangles keep counter-clockwise winding, so face normals point to +Z. */
        WPE_TEST_ASSERT_NEAR(1.0, m->vertices[t * 3].normal.z, 1e-6);
    }
    WPE_TEST_ASSERT_EQ_UINT(1, m->material_count); /* only the default material */
    WPE_TEST_ASSERT_NEAR(2.0, m->nodes[0].translation.y, 0);
    WPE_TEST_ASSERT_NEAR(1.0, m->primitives[0].bounds.max.x, 0);
    wpe_model_destroy(m);
    remove("strip.gltf");
}

static void test_assets_model_rejects_unsupported_required_extension(void) {
    write_file("required.gltf",
               "{\"asset\":{\"version\":\"2.0\"},\"extensionsUsed\":[\"KHR_draco_mesh_compression\"],"
               "\"extensionsRequired\":[\"KHR_draco_mesh_compression\"]}");
    WPE_Model* m = NULL;
    wpe_log_set_stderr_enabled(false);
    WPE_TEST_ASSERT_EQ_INT(WPE_RESULT_ERROR_UNSUPPORTED, wpe_model_load("required.gltf", NULL, &m));
    wpe_log_set_stderr_enabled(true);
    WPE_TEST_ASSERT(m == NULL);
    remove("required.gltf");
}

static void test_assets_model_rejects_out_of_range_indices(void) {
    const float positions[9] = {0, 0, 0, 1, 0, 0, 0, 1, 0};
    const uint16_t indices[4] = {0, 1, 7, 0}; /* index 7 >= 3 vertices; padded to 4-byte multiple */
    write_gltf("badindex.gltf",
               "\"bufferViews\":[{\"buffer\":0,\"byteLength\":36},{\"buffer\":0,\"byteOffset\":36,"
               "\"byteLength\":6}],"
               "\"accessors\":[{\"bufferView\":0,\"componentType\":5126,\"count\":3,\"type\":\"VEC3\","
               "\"min\":[0,0,0],\"max\":[1,1,0]},{\"bufferView\":1,\"componentType\":5123,\"count\":3,"
               "\"type\":\"SCALAR\"}],"
               "\"meshes\":[{\"primitives\":[{\"attributes\":{\"POSITION\":0},\"indices\":1}]}],"
               "\"nodes\":[{\"mesh\":0}]}",
               positions, 9, indices, 4);
    WPE_Model* m = NULL;
    wpe_log_set_stderr_enabled(false);
    const WPE_Result result = wpe_model_load("badindex.gltf", NULL, &m);
    wpe_log_set_stderr_enabled(true);
    WPE_TEST_ASSERT(result == WPE_RESULT_ERROR_PARSE);
    WPE_TEST_ASSERT(m == NULL);
    remove("badindex.gltf");
}

static void test_assets_model_missing_and_malformed_files(void) {
    WPE_Model* m = NULL;
    wpe_log_set_stderr_enabled(false);
    WPE_TEST_ASSERT_EQ_INT(WPE_RESULT_ERROR_IO, wpe_model_load("nope/missing.glb", NULL, &m));
    write_file("broken.gltf", "{\"asset\":{\"version\":\"2.0\"}, this is not json");
    WPE_TEST_ASSERT_EQ_INT(WPE_RESULT_ERROR_PARSE, wpe_model_load("broken.gltf", NULL, &m));
    write_file("legacy.gltf", "{\"asset\":{\"version\":\"1.0\"}}");
    WPE_TEST_ASSERT(wpe_model_load("legacy.gltf", NULL, &m) != WPE_RESULT_OK);
    wpe_log_set_stderr_enabled(true);
    WPE_TEST_ASSERT(m == NULL);
    WPE_TEST_ASSERT_EQ_INT(WPE_RESULT_ERROR_NULL_ARG, wpe_model_load(NULL, NULL, &m));
    wpe_model_destroy(NULL);
    remove("broken.gltf");
    remove("legacy.gltf");
}

WPE_TEST_MAIN(WPE_TEST_CASE(test_assets_image_decode_png_roundtrip),
              WPE_TEST_CASE(test_assets_image_decode_rejects_garbage),
              WPE_TEST_CASE(test_assets_file_read_missing),
              WPE_TEST_CASE(test_assets_font_loads_sample_sheet),
              WPE_TEST_CASE(test_assets_font_builtin_matches_small_png),
              WPE_TEST_CASE(test_assets_font_rejects_wrong_glyph_count),
              WPE_TEST_CASE(test_assets_model_damaged_helmet), WPE_TEST_CASE(test_assets_model_car_concept),
              WPE_TEST_CASE(test_assets_model_fridge_with_required_transmission),
              WPE_TEST_CASE(test_assets_model_rigged_figure),
              WPE_TEST_CASE(test_assets_model_strip_without_normals_gets_flat_normals),
              WPE_TEST_CASE(test_assets_model_rejects_unsupported_required_extension),
              WPE_TEST_CASE(test_assets_model_rejects_out_of_range_indices),
              WPE_TEST_CASE(test_assets_model_missing_and_malformed_files))
