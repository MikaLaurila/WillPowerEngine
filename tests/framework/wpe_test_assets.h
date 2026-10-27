/*
 * Test helpers for suites that need an asset manager: a VFS (optionally mounting one OS
 * directory) plus a manager on top, and asserting wrappers for loading and adding models.
 * Header-only so the core test framework does not depend on the assets module.
 */
#ifndef WPE_TEST_ASSETS_H
#define WPE_TEST_ASSETS_H

#include <math.h>
#include <string.h>

#include "assets/asset_manager.h"
#include "core/alloc.h"
#include "platform/platform.h"
#include "wpe_test.h"

typedef struct WPE_TestAssets {
    WPE_Vfs* vfs;
    WPE_AssetManager* assets;
} WPE_TestAssets;

/* Creates a VFS mounting `os_directory` (NULL mounts nothing) and an asset manager over it that
 * caps model textures at `max_texture_size` (0 = original size). */
static inline WPE_TestAssets wpe_test_assets_create(const char* os_directory, uint32_t max_texture_size) {
    WPE_TestAssets t = {0};
    WPE_TEST_ASSERT_OK(wpe_vfs_create(&t.vfs));
    if (os_directory) WPE_TEST_ASSERT_OK(wpe_vfs_mount_directory(t.vfs, os_directory));
    WPE_TEST_ASSERT_OK(wpe_asset_manager_create(
        &(WPE_AssetManagerDesc){.vfs = t.vfs, .max_texture_size = max_texture_size}, &t.assets));
    return t;
}

static inline void wpe_test_assets_destroy(WPE_TestAssets* t) {
    wpe_asset_manager_destroy(t->assets);
    wpe_vfs_destroy(t->vfs);
    *t = (WPE_TestAssets){0};
}

/* Loads a model by asset path; the caller owns one reference. */
static inline WPE_AssetHandle wpe_test_load_model(WPE_AssetManager* assets, const char* path) {
    WPE_AssetHandle handle = WPE_ASSET_NONE;
    WPE_TEST_ASSERT_OK(wpe_asset_load(assets, WPE_ASSET_MODEL, path, &handle));
    return handle;
}

/* Hands a model created in code to the manager under `name`; the caller owns one reference. */
static inline WPE_AssetHandle wpe_test_add_model(WPE_AssetManager* assets, const char* name,
                                                 WPE_Model* model) {
    WPE_TEST_ASSERT(model != NULL);
    WPE_AssetHandle handle = WPE_ASSET_NONE;
    WPE_TEST_ASSERT_OK(wpe_asset_add(assets, WPE_ASSET_MODEL, name, model, &handle));
    return handle;
}

static inline unsigned char* wpe_test__put_le(unsigned char* p, uint32_t value, int bytes) {
    for (int i = 0; i < bytes; ++i)
        *p++ = (unsigned char)(value >> (8 * i));
    return p;
}

/* Writes a 16-bit PCM WAV file holding a sine of `hz` at amplitude 0.5 on every channel. */
static inline void wpe_test_write_wav(const char* path, uint32_t channels, uint32_t rate, uint32_t frames,
                                      float hz) {
    const uint32_t data_size = frames * channels * 2u;
    unsigned char header[44];
    unsigned char* p = header;
    memcpy(p, "RIFF", 4);
    p = wpe_test__put_le(p + 4, 36u + data_size, 4);
    memcpy(p, "WAVEfmt ", 8);
    p = wpe_test__put_le(p + 8, 16u, 4);
    p = wpe_test__put_le(p, 1u, 2); /* PCM */
    p = wpe_test__put_le(p, channels, 2);
    p = wpe_test__put_le(p, rate, 4);
    p = wpe_test__put_le(p, rate * channels * 2u, 4);
    p = wpe_test__put_le(p, channels * 2u, 2);
    p = wpe_test__put_le(p, 16u, 2);
    memcpy(p, "data", 4);
    wpe_test__put_le(p + 4, data_size, 4);

    int16_t* samples = wpe_alloc(data_size ? data_size : 1);
    WPE_TEST_ASSERT(samples != NULL);
    for (uint32_t f = 0; f < frames; ++f) {
        const float s = 0.5f * sinf(2.0f * 3.14159265f * hz * (float)f / (float)rate);
        for (uint32_t c = 0; c < channels; ++c)
            samples[f * channels + c] = (int16_t)(s * 32767.0f);
    }
    WPE_File* file = NULL;
    WPE_TEST_ASSERT_OK(wpe_file_open(path, true, &file));
    WPE_TEST_ASSERT_OK(wpe_file_write(file, header, sizeof header));
    WPE_TEST_ASSERT_OK(wpe_file_write(file, samples, data_size));
    WPE_TEST_ASSERT_OK(wpe_file_close(file));
    wpe_free(samples);
}

#endif /* WPE_TEST_ASSETS_H */
