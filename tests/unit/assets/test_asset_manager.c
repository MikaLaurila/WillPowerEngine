#include "wpe_test.h"
#include "wpe_test_assets.h"

#include <stdio.h>
#include <string.h>

#include "assets/procedural.h"
#include "core/alloc.h"
#include "core/log.h"
#include "platform/platform.h"

/* Scratch files live below the test's working directory. */
#define ROOT "asset_scratch"

static void quiet(bool on) {
    wpe_log_set_stderr_enabled(!on);
}

static void write_os_file(const char* path, const void* data, size_t size) {
    WPE_File* file = NULL;
    WPE_TEST_ASSERT_OK(wpe_file_open(path, true, &file));
    WPE_TEST_ASSERT_OK(wpe_file_write(file, data, size));
    WPE_TEST_ASSERT_OK(wpe_file_close(file));
}

/* A one-triangle glTF whose buffer lives in a sibling directory ("../buffers/tri.bin"). */
static const char g_tri_gltf[] =
    "{\"asset\":{\"version\":\"2.0\"},"
    "\"buffers\":[{\"uri\":\"../buffers/tri.bin\",\"byteLength\":36}],"
    "\"bufferViews\":[{\"buffer\":0,\"byteLength\":36}],"
    "\"accessors\":[{\"bufferView\":0,\"componentType\":5126,\"count\":3,\"type\":\"VEC3\","
    "\"min\":[0,0,0],\"max\":[1,1,0]}],"
    "\"meshes\":[{\"primitives\":[{\"attributes\":{\"POSITION\":0}}]}],"
    "\"nodes\":[{\"mesh\":0}],\"scenes\":[{\"nodes\":[0]}],\"scene\":0}";
static const float g_tri_positions[9] = {0, 0, 0, 1, 0, 0, 0, 1, 0};

static void write_tri_files(void) {
    WPE_TEST_ASSERT_OK(wpe_directory_create(ROOT "/dir/models"));
    WPE_TEST_ASSERT_OK(wpe_directory_create(ROOT "/dir/buffers"));
    write_os_file(ROOT "/dir/models/tri.gltf", g_tri_gltf, strlen(g_tri_gltf));
    write_os_file(ROOT "/dir/buffers/tri.bin", g_tri_positions, sizeof g_tri_positions);
}

static void test_assets_load_caches_and_counts_references(void) {
    WPE_TestAssets t = wpe_test_assets_create(WPE_TEST_ASSETS_DIR, 16);
    WPE_AssetHandle a = WPE_ASSET_NONE, b = WPE_ASSET_NONE;
    WPE_TEST_ASSERT_OK(wpe_asset_load(t.assets, WPE_ASSET_MODEL, "meshes/RiggedFigure.glb", &a));
    /* Equivalent spellings share the asset. */
    WPE_TEST_ASSERT_OK(wpe_asset_load(t.assets, WPE_ASSET_MODEL, "meshes/./../meshes/RiggedFigure.glb", &b));
    WPE_TEST_ASSERT_EQ_UINT(a, b);
    WPE_TEST_ASSERT_EQ_UINT(2, wpe_asset_ref_count(t.assets, a));
    WPE_TEST_ASSERT_EQ_STR("meshes/RiggedFigure.glb", wpe_asset_path(t.assets, a));
    WPE_TEST_ASSERT_EQ_UINT(WPE_ASSET_MODEL, wpe_asset_type(t.assets, a));
    WPE_TEST_ASSERT(wpe_asset_model(t.assets, a) != NULL);
    WPE_TEST_ASSERT(wpe_asset_font(t.assets, a) == NULL); /* Wrong type. */
    WPE_TEST_ASSERT_EQ_UINT(a, wpe_asset_find(t.assets, WPE_ASSET_MODEL, "meshes/RiggedFigure.glb"));
    WPE_TEST_ASSERT_EQ_UINT(WPE_ASSET_NONE,
                            wpe_asset_find(t.assets, WPE_ASSET_FONT, "meshes/RiggedFigure.glb"));

    WPE_AssetStats stats = wpe_asset_manager_stats(t.assets);
    WPE_TEST_ASSERT_EQ_UINT(1, stats.assets);
    WPE_TEST_ASSERT_EQ_UINT(2, stats.references);
    WPE_TEST_ASSERT_EQ_UINT(1, stats.loads);
    WPE_TEST_ASSERT_EQ_UINT(1, stats.hits);

    wpe_asset_retain(t.assets, a);
    wpe_asset_release(t.assets, a);
    wpe_asset_release(t.assets, a);
    WPE_TEST_ASSERT(wpe_asset_is_valid(t.assets, a));
    wpe_asset_release(t.assets, b); /* Last reference: freed. */
    WPE_TEST_ASSERT(!wpe_asset_is_valid(t.assets, a));
    WPE_TEST_ASSERT(wpe_asset_model(t.assets, a) == NULL);
    WPE_TEST_ASSERT_EQ_UINT(0, wpe_asset_ref_count(t.assets, a));

    /* Loading again reuses the slot with a new generation: the stale handle stays invalid. */
    WPE_AssetHandle c = WPE_ASSET_NONE;
    WPE_TEST_ASSERT_OK(wpe_asset_load(t.assets, WPE_ASSET_MODEL, "meshes/RiggedFigure.glb", &c));
    WPE_TEST_ASSERT(c != a);
    WPE_TEST_ASSERT(!wpe_asset_is_valid(t.assets, a));
    WPE_TEST_ASSERT_EQ_UINT(2, wpe_asset_manager_stats(t.assets).loads);
    wpe_asset_release(t.assets, c);
    wpe_asset_release(t.assets, WPE_ASSET_NONE); /* No-op. */
    wpe_test_assets_destroy(&t);
}

static void test_assets_load_errors(void) {
    WPE_TestAssets t = wpe_test_assets_create(WPE_TEST_ASSETS_DIR, 0);
    WPE_AssetHandle h = 7;
    quiet(true);
    WPE_TEST_ASSERT_EQ_INT(WPE_RESULT_ERROR_NOT_FOUND,
                           wpe_asset_load(t.assets, WPE_ASSET_MODEL, "missing.glb", &h));
    WPE_TEST_ASSERT_EQ_UINT(WPE_ASSET_NONE, h);
    WPE_TEST_ASSERT_EQ_INT(WPE_RESULT_ERROR_INVALID_ARG,
                           wpe_asset_load(t.assets, WPE_ASSET_MODEL, "../x.glb", &h));
    WPE_TEST_ASSERT_EQ_INT(WPE_RESULT_ERROR_INVALID_ARG,
                           wpe_asset_load(t.assets, WPE_ASSET_MODEL, "/abs.glb", &h));
    WPE_TEST_ASSERT_EQ_INT(
        WPE_RESULT_ERROR_NOT_FOUND,
        wpe_asset_load(t.assets, WPE_ASSET_SOUND, "meshes/RiggedFigure.glb", &h)); /* No loader. */
    /* A file of the wrong format fails in the loader and leaves nothing behind. */
    WPE_TEST_ASSERT_EQ_INT(WPE_RESULT_ERROR_PARSE,
                           wpe_asset_load(t.assets, WPE_ASSET_MODEL, "fonts/small.png", &h));
    WPE_TEST_ASSERT_EQ_INT(WPE_RESULT_ERROR_NULL_ARG, wpe_asset_load(t.assets, WPE_ASSET_MODEL, NULL, &h));
    quiet(false);
    WPE_TEST_ASSERT_EQ_UINT(0, wpe_asset_manager_stats(t.assets).assets);
    WPE_TEST_ASSERT(!wpe_asset_is_valid(t.assets, 0x12345678u));
    WPE_TEST_ASSERT(wpe_asset_get(t.assets, WPE_ASSET_NONE, WPE_ASSET_MODEL) == NULL);
    wpe_test_assets_destroy(&t);
}

static void test_assets_add_takes_ownership(void) {
    WPE_TestAssets t = wpe_test_assets_create(NULL, 0);
    WPE_Model* box = NULL;
    WPE_TEST_ASSERT_OK(wpe_model_create_box(wpe_vec3(1, 1, 1), NULL, &box));
    WPE_AssetHandle handle = WPE_ASSET_NONE;
    WPE_TEST_ASSERT_OK(wpe_asset_add(t.assets, WPE_ASSET_MODEL, "procedural/box", box, &handle));
    WPE_TEST_ASSERT(wpe_asset_model(t.assets, handle) == box);
    /* Loading by the name returns the added asset (no file needed). */
    WPE_AssetHandle again = WPE_ASSET_NONE;
    WPE_TEST_ASSERT_OK(wpe_asset_load(t.assets, WPE_ASSET_MODEL, "procedural/box", &again));
    WPE_TEST_ASSERT_EQ_UINT(handle, again);
    wpe_asset_release(t.assets, again);

    /* Names are unique per type; on failure the caller keeps ownership. */
    WPE_Model* second = NULL;
    WPE_TEST_ASSERT_OK(wpe_model_create_box(wpe_vec3(2, 2, 2), NULL, &second));
    WPE_AssetHandle other = WPE_ASSET_NONE;
    quiet(true);
    WPE_TEST_ASSERT_EQ_INT(WPE_RESULT_ERROR_ALREADY_EXISTS,
                           wpe_asset_add(t.assets, WPE_ASSET_MODEL, "procedural/box", second, &other));
    WPE_TEST_ASSERT_EQ_INT(WPE_RESULT_ERROR_INVALID_ARG,
                           wpe_asset_add(t.assets, WPE_ASSET_MODEL, "a//../..", second, &other));
    WPE_TEST_ASSERT_EQ_INT(WPE_RESULT_ERROR_NOT_FOUND, wpe_asset_add(t.assets, 30, "x", second, &other));
    quiet(false);
    wpe_model_destroy(second);

    /* Destroying the manager with a live reference frees the asset (and warns). */
    quiet(true);
    wpe_test_assets_destroy(&t);
    quiet(false);
}

static void test_assets_fonts(void) {
    WPE_TestAssets t = wpe_test_assets_create(WPE_TEST_ASSETS_DIR, 0);
    WPE_AssetHandle font = WPE_ASSET_NONE;
    WPE_TEST_ASSERT_OK(wpe_asset_load(t.assets, WPE_ASSET_FONT, "fonts/small.png", &font));
    const WPE_BitmapFont* f = wpe_asset_font(t.assets, font);
    WPE_TEST_ASSERT(f != NULL && f->line_height > 0);
    /* The same file can be loaded as a different type independently. */
    WPE_TEST_ASSERT_EQ_UINT(WPE_ASSET_NONE, wpe_asset_find(t.assets, WPE_ASSET_MODEL, "fonts/small.png"));
    wpe_asset_release(t.assets, font);
    wpe_test_assets_destroy(&t);
}

/* A user asset type: the file's text, uppercased. */
typedef struct TextAsset {
    char text[64];
} TextAsset;

static WPE_Result load_text(WPE_AssetManager* assets, const char* path, void* user_data, void** out_asset) {
    ++*(int*)user_data;
    void* data = NULL;
    size_t size = 0;
    const WPE_Result result = wpe_vfs_read_all(wpe_asset_manager_vfs(assets), path, &data, &size);
    if (result != WPE_RESULT_OK) return result;
    TextAsset* text = wpe_alloc_zeroed(1, sizeof *text);
    if (text) {
        for (size_t i = 0; i < size && i + 1 < sizeof text->text; ++i) {
            const char c = ((const char*)data)[i];
            text->text[i] = c >= 'a' && c <= 'z' ? (char)(c - 32) : c;
        }
    }
    wpe_free(data);
    *out_asset = text;
    return text ? WPE_RESULT_OK : WPE_RESULT_ERROR_OUT_OF_MEMORY;
}

static void destroy_text(void* asset, void* user_data) {
    --*(int*)user_data;
    wpe_free(asset);
}

static void test_assets_custom_type_from_pak(void) {
    WPE_TEST_ASSERT_OK(wpe_directory_create(ROOT));
    WPE_PakWriter* writer = NULL;
    WPE_TEST_ASSERT_OK(wpe_pak_writer_create(ROOT "/text.pak", 0, &writer));
    WPE_TEST_ASSERT_OK(wpe_pak_writer_add(writer, "greeting.txt", "hello", 5, WPE_PAK_COMPRESS_ALWAYS));
    WPE_TEST_ASSERT_OK(wpe_pak_writer_finish(writer));
    wpe_pak_writer_destroy(writer);

    WPE_TestAssets t = wpe_test_assets_create(NULL, 0);
    WPE_TEST_ASSERT_OK(wpe_vfs_mount_pak(t.vfs, ROOT "/text.pak"));
    int live = 0;
    const WPE_AssetType text_type = WPE_ASSET_USER_FIRST;
    const WPE_AssetLoader loader = {
        .name = "text", .load = load_text, .destroy = destroy_text, .user_data = &live};
    WPE_TEST_ASSERT_OK(wpe_asset_manager_register_type(t.assets, text_type, &loader));
    WPE_TEST_ASSERT_EQ_INT(WPE_RESULT_ERROR_ALREADY_EXISTS,
                           wpe_asset_manager_register_type(t.assets, text_type, &loader));
    WPE_TEST_ASSERT_EQ_INT(WPE_RESULT_ERROR_INVALID_ARG,
                           wpe_asset_manager_register_type(t.assets, WPE_ASSET_MAX_TYPES, &loader));

    WPE_AssetHandle h = WPE_ASSET_NONE;
    WPE_TEST_ASSERT_OK(wpe_asset_load(t.assets, text_type, "greeting.txt", &h));
    WPE_TEST_ASSERT_EQ_STR("HELLO", ((const TextAsset*)wpe_asset_get(t.assets, h, text_type))->text);
    WPE_TEST_ASSERT_EQ_INT(1, live);
    quiet(true);
    WPE_TEST_ASSERT_EQ_INT(WPE_RESULT_ERROR_INVALID_STATE,
                           wpe_asset_manager_unregister_type(t.assets, text_type));
    quiet(false);
    wpe_asset_release(t.assets, h);
    WPE_TEST_ASSERT_EQ_INT(0, live);
    WPE_TEST_ASSERT_OK(wpe_asset_manager_unregister_type(t.assets, text_type));
    WPE_TEST_ASSERT_EQ_INT(WPE_RESULT_ERROR_NOT_FOUND,
                           wpe_asset_manager_unregister_type(t.assets, text_type));
    wpe_test_assets_destroy(&t);
}

/* Loads models/tri.gltf (with its "../buffers/tri.bin") and checks the triangle. */
static void check_tri_model(WPE_AssetManager* assets) {
    WPE_AssetHandle h = WPE_ASSET_NONE;
    WPE_TEST_ASSERT_OK(wpe_asset_load(assets, WPE_ASSET_MODEL, "models/tri.gltf", &h));
    const WPE_Model* model = wpe_asset_model(assets, h);
    WPE_TEST_ASSERT_EQ_UINT(3, model->vertex_count);
    WPE_TEST_ASSERT_NEAR(1.0, model->vertices[1].position.x, 0);
    WPE_TEST_ASSERT_NEAR(1.0, model->vertices[2].position.y, 0);
    wpe_asset_release(assets, h);
}

static void test_assets_gltf_with_relative_buffer_from_directory_and_pak(void) {
    write_tri_files();
    WPE_TestAssets dir = wpe_test_assets_create(ROOT "/dir", 0);
    check_tri_model(dir.assets);
    wpe_test_assets_destroy(&dir);

    /* The same files packed into an archive. */
    WPE_PakWriter* writer = NULL;
    WPE_TEST_ASSERT_OK(wpe_pak_writer_create(ROOT "/tri.pak", 0, &writer));
    WPE_TEST_ASSERT_OK(
        wpe_pak_writer_add(writer, "models/tri.gltf", g_tri_gltf, strlen(g_tri_gltf), WPE_PAK_COMPRESS_AUTO));
    WPE_TEST_ASSERT_OK(wpe_pak_writer_add(writer, "buffers/tri.bin", g_tri_positions, sizeof g_tri_positions,
                                          WPE_PAK_COMPRESS_NONE));
    WPE_TEST_ASSERT_OK(wpe_pak_writer_finish(writer));
    wpe_pak_writer_destroy(writer);
    WPE_TestAssets pak = wpe_test_assets_create(NULL, 0);
    WPE_TEST_ASSERT_OK(wpe_vfs_mount_pak(pak.vfs, ROOT "/tri.pak"));
    check_tri_model(pak.assets);
    wpe_test_assets_destroy(&pak);
}

WPE_TEST_MAIN(WPE_TEST_CASE(test_assets_load_caches_and_counts_references),
              WPE_TEST_CASE(test_assets_load_errors), WPE_TEST_CASE(test_assets_add_takes_ownership),
              WPE_TEST_CASE(test_assets_fonts), WPE_TEST_CASE(test_assets_custom_type_from_pak),
              WPE_TEST_CASE(test_assets_gltf_with_relative_buffer_from_directory_and_pak))
