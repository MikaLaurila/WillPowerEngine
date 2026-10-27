#include "wpe_test.h"

#include <stdio.h>
#include <string.h>

#include "core/alloc.h"
#include "core/crc32.h"
#include "core/log.h"
#include "platform/platform.h"
#include "vfs/vfs.h"

/* Scratch files live below the test's working directory. */
#define ROOT "vfs_scratch"

static void quiet(bool on) {
    wpe_log_set_stderr_enabled(!on);
}

static void write_os_file(const char* path, const void* data, size_t size) {
    WPE_File* file = NULL;
    WPE_TEST_ASSERT_OK(wpe_file_open(path, true, &file));
    WPE_TEST_ASSERT_OK(wpe_file_write(file, data, size));
    WPE_TEST_ASSERT_OK(wpe_file_close(file));
}

static void write_text(const char* path, const char* text) {
    write_os_file(path, text, strlen(text));
}

/* Deterministic incompressible bytes. */
static void fill_noise(unsigned char* data, size_t size, uint32_t seed) {
    uint32_t x = seed ? seed : 1;
    for (size_t i = 0; i < size; ++i) {
        x ^= x << 13;
        x ^= x >> 17;
        x ^= x << 5;
        data[i] = (unsigned char)x;
    }
}

static void assert_read(const WPE_Vfs* vfs, const char* path, const void* expected, size_t size) {
    void* data = NULL;
    size_t read = 0;
    WPE_TEST_ASSERT_OK(wpe_vfs_read_all(vfs, path, &data, &read));
    WPE_TEST_ASSERT_EQ_UINT(size, read);
    WPE_TEST_ASSERT(memcmp(data, expected, size) == 0);
    WPE_TEST_ASSERT_EQ_UINT(0, ((unsigned char*)data)[size]); /* NUL appended */
    wpe_free(data);
}

static void test_vfs_crc32_known_values(void) {
    WPE_TEST_ASSERT_EQ_UINT(0xCBF43926u, wpe_crc32(0, "123456789", 9));
    WPE_TEST_ASSERT_EQ_UINT(0u, wpe_crc32(0, NULL, 0));
    /* Incremental updates match one pass. */
    WPE_TEST_ASSERT_EQ_UINT(0xCBF43926u, wpe_crc32(wpe_crc32(0, "1234", 4), "56789", 5));
}

static void test_vfs_path_rules(void) {
    const char* valid[] = {"a", "meshes/helmet.glb", "a/b/c.d", ".hidden", "x..y/z", "UPPER/ä.txt"};
    for (size_t i = 0; i < sizeof valid / sizeof valid[0]; ++i)
        WPE_TEST_ASSERT_MSG(wpe_vfs_path_is_valid(valid[i]), "'%s' should be valid", valid[i]);
    const char* invalid[] = {"",     "/abs", "a//b", "a/",   "./a",      "a/./b",
                             "../a", "a/..", "a\\b", "c:/x", "tab\there"};
    for (size_t i = 0; i < sizeof invalid / sizeof invalid[0]; ++i)
        WPE_TEST_ASSERT_MSG(!wpe_vfs_path_is_valid(invalid[i]), "'%s' should be invalid", invalid[i]);
    WPE_TEST_ASSERT(!wpe_vfs_path_is_valid(NULL));
    char longest[WPE_VFS_PATH_MAX + 1];
    memset(longest, 'a', sizeof longest);
    longest[WPE_VFS_PATH_MAX - 1] = '\0';
    WPE_TEST_ASSERT(wpe_vfs_path_is_valid(longest)); /* 255 bytes */
    longest[WPE_VFS_PATH_MAX - 1] = 'a';
    longest[WPE_VFS_PATH_MAX] = '\0';
    WPE_TEST_ASSERT(!wpe_vfs_path_is_valid(longest)); /* 256 bytes */

    char out[WPE_VFS_PATH_MAX];
    const char* cases[][2] = {
        {"meshes/../textures//a.png", "textures/a.png"},
        {"./a/./b", "a/b"},
        {"a\\b\\c", "a/b/c"},
        {"a/b/../../c", "c"},
        {"x/y/", "x/y"},
        {"a/..b", "a/..b"},
    };
    for (size_t i = 0; i < sizeof cases / sizeof cases[0]; ++i) {
        WPE_TEST_ASSERT_MSG(wpe_vfs_path_normalize(cases[i][0], out, sizeof out), "'%s'", cases[i][0]);
        WPE_TEST_ASSERT_EQ_STR(cases[i][1], out);
    }
    const char* rejected[] = {"../a", "a/../..", "/abs", "", ".", "a/..", "c:/x"};
    for (size_t i = 0; i < sizeof rejected / sizeof rejected[0]; ++i) {
        WPE_TEST_ASSERT_MSG(!wpe_vfs_path_normalize(rejected[i], out, sizeof out), "'%s'", rejected[i]);
        WPE_TEST_ASSERT_EQ_STR("", out);
    }
    WPE_TEST_ASSERT(!wpe_vfs_path_normalize("a", out, 8)); /* Buffer smaller than WPE_VFS_PATH_MAX. */
}

/* Writes ROOT/test.pak with a mix of entries; returns the noise buffer for comparisons. */
static unsigned char g_noise[50000];
static char g_text[20000];

static void write_test_pak(const char* path) {
    fill_noise(g_noise, sizeof g_noise, 7);
    for (size_t i = 0; i + 1 < sizeof g_text; ++i)
        g_text[i] = "the quick brown fox "[i % 20];
    g_text[sizeof g_text - 1] = '\0';

    WPE_PakWriter* writer = NULL;
    WPE_TEST_ASSERT_OK(wpe_pak_writer_create(path, 0, &writer));
    /* Added out of order: the index is sorted on finish. */
    WPE_TEST_ASSERT_OK(
        wpe_pak_writer_add(writer, "zeta/noise.bin", g_noise, sizeof g_noise, WPE_PAK_COMPRESS_AUTO));
    WPE_TEST_ASSERT_OK(
        wpe_pak_writer_add(writer, "alpha/text.txt", g_text, strlen(g_text), WPE_PAK_COMPRESS_AUTO));
    WPE_TEST_ASSERT_OK(wpe_pak_writer_add(writer, "empty", "", 0, WPE_PAK_COMPRESS_AUTO));
    WPE_TEST_ASSERT_OK(wpe_pak_writer_add(writer, "forced.txt", "hi", 2, WPE_PAK_COMPRESS_ALWAYS));
    WPE_TEST_ASSERT_OK(wpe_pak_writer_add(writer, "stored.txt", g_text, 1000, WPE_PAK_COMPRESS_NONE));
    WPE_TEST_ASSERT_OK(wpe_pak_writer_finish(writer));
    wpe_pak_writer_destroy(writer);
}

static void test_vfs_pak_roundtrip(void) {
    WPE_TEST_ASSERT_OK(wpe_directory_create(ROOT));
    write_test_pak(ROOT "/test.pak");

    WPE_Pak* pak = NULL;
    WPE_TEST_ASSERT_OK(wpe_pak_open(ROOT "/test.pak", &pak));
    WPE_TEST_ASSERT_EQ_UINT(5, wpe_pak_entry_count(pak));
    const char* order[] = {"alpha/text.txt", "empty", "forced.txt", "stored.txt", "zeta/noise.bin"};
    for (uint32_t i = 0; i < 5; ++i) {
        WPE_TEST_ASSERT_EQ_STR(order[i], wpe_pak_entry(pak, i).path);
        WPE_TEST_ASSERT_EQ_UINT(i, wpe_pak_find(pak, order[i]));
    }
    WPE_TEST_ASSERT_EQ_UINT(UINT32_MAX, wpe_pak_find(pak, "missing"));
    WPE_TEST_ASSERT_EQ_UINT(UINT32_MAX, wpe_pak_find(pak, "alpha"));

    /* Automatic compression only where it pays off. */
    const WPE_PakEntryInfo text = wpe_pak_entry(pak, 0);
    WPE_TEST_ASSERT(text.compressed && text.stored_size < text.size / 10);
    WPE_TEST_ASSERT_EQ_UINT(wpe_crc32(0, g_text, strlen(g_text)), text.crc32);
    const WPE_PakEntryInfo noise = wpe_pak_entry(pak, 4);
    WPE_TEST_ASSERT(!noise.compressed && noise.stored_size == sizeof g_noise);
    WPE_TEST_ASSERT(wpe_pak_entry(pak, 2).compressed);
    WPE_TEST_ASSERT(!wpe_pak_entry(pak, 3).compressed);

    void* data = NULL;
    size_t size = 0;
    for (uint32_t i = 0; i < 5; ++i) {
        WPE_TEST_ASSERT_OK(wpe_pak_read(pak, i, &data, &size));
        WPE_TEST_ASSERT_EQ_UINT(wpe_pak_entry(pak, i).size, size);
        WPE_TEST_ASSERT_EQ_UINT(wpe_pak_entry(pak, i).crc32, wpe_crc32(0, data, size));
        wpe_free(data);
    }
    WPE_TEST_ASSERT_EQ_INT(WPE_RESULT_ERROR_INVALID_ARG, wpe_pak_read(pak, 5, &data, &size));
    wpe_pak_close(pak);
}

static void test_vfs_pak_writer_errors(void) {
    WPE_TEST_ASSERT_OK(wpe_directory_create(ROOT));
    quiet(true);
    WPE_PakWriter* writer = NULL;
    WPE_TEST_ASSERT_EQ_INT(WPE_RESULT_ERROR_INVALID_ARG, wpe_pak_writer_create(ROOT "/x.pak", 20, &writer));
    WPE_TEST_ASSERT_OK(wpe_pak_writer_create(ROOT "/dup.pak", 1, &writer));
    WPE_TEST_ASSERT_EQ_INT(WPE_RESULT_ERROR_INVALID_ARG,
                           wpe_pak_writer_add(writer, "../evil", "x", 1, WPE_PAK_COMPRESS_AUTO));
    WPE_TEST_ASSERT_OK(wpe_pak_writer_add(writer, "same", "a", 1, WPE_PAK_COMPRESS_AUTO));
    WPE_TEST_ASSERT_OK(wpe_pak_writer_add(writer, "same", "b", 1, WPE_PAK_COMPRESS_AUTO));
    WPE_TEST_ASSERT_EQ_INT(WPE_RESULT_ERROR_ALREADY_EXISTS, wpe_pak_writer_finish(writer));
    WPE_TEST_ASSERT_EQ_INT(WPE_RESULT_ERROR_INVALID_STATE, wpe_pak_writer_finish(writer));
    wpe_pak_writer_destroy(writer);

    /* An unfinished archive is rejected. */
    WPE_TEST_ASSERT_OK(wpe_pak_writer_create(ROOT "/unfinished.pak", 0, &writer));
    WPE_TEST_ASSERT_OK(wpe_pak_writer_add(writer, "a", "a", 1, WPE_PAK_COMPRESS_AUTO));
    wpe_pak_writer_destroy(writer);
    WPE_Pak* pak = NULL;
    WPE_TEST_ASSERT_EQ_INT(WPE_RESULT_ERROR_PARSE, wpe_pak_open(ROOT "/unfinished.pak", &pak));
    WPE_TEST_ASSERT_EQ_INT(WPE_RESULT_ERROR_NOT_FOUND, wpe_pak_open(ROOT "/missing.pak", &pak));
    quiet(false);
}

/* Copies the test archive, lets `mutate` damage it, and returns the open result. */
static WPE_Result open_damaged(void (*mutate)(unsigned char* bytes, size_t size), bool read_entries) {
    write_test_pak(ROOT "/source.pak");
    void* bytes = NULL;
    size_t size = 0;
    WPE_TEST_ASSERT_OK(wpe_file_read_all(ROOT "/source.pak", &bytes, &size));
    mutate(bytes, size);
    write_os_file(ROOT "/damaged.pak", bytes, size);
    wpe_free(bytes);

    WPE_Pak* pak = NULL;
    WPE_Result result = wpe_pak_open(ROOT "/damaged.pak", &pak);
    if (result == WPE_RESULT_OK && read_entries) {
        for (uint32_t i = 0; i < wpe_pak_entry_count(pak) && result == WPE_RESULT_OK; ++i) {
            void* data = NULL;
            size_t data_size = 0;
            result = wpe_pak_read(pak, i, &data, &data_size);
            wpe_free(data);
        }
    }
    wpe_pak_close(pak);
    return result;
}

static void damage_magic(unsigned char* b, size_t s) {
    (void)s;
    b[0] = 'X';
}
static void damage_version(unsigned char* b, size_t s) {
    (void)s;
    b[8] = 2;
}
static void damage_index(unsigned char* b, size_t s) {
    b[s - 3] ^= 0x40; /* A path byte: caught by the index checksum. */
}
static void damage_stored_data(unsigned char* b, size_t s) {
    (void)s;
    b[40] ^= 0x01; /* First data byte: the first-written entry (stored noise). */
}
static void damage_truncate_header(unsigned char* b, size_t s) {
    (void)s;
    b[24] ^= 0x10; /* Index size no longer matches the file. */
}

static void test_vfs_pak_rejects_corruption(void) {
    WPE_TEST_ASSERT_OK(wpe_directory_create(ROOT));
    quiet(true);
    WPE_TEST_ASSERT_EQ_INT(WPE_RESULT_ERROR_PARSE, open_damaged(damage_magic, false));
    WPE_TEST_ASSERT_EQ_INT(WPE_RESULT_ERROR_UNSUPPORTED, open_damaged(damage_version, false));
    WPE_TEST_ASSERT_EQ_INT(WPE_RESULT_ERROR_PARSE, open_damaged(damage_index, false));
    WPE_TEST_ASSERT_EQ_INT(WPE_RESULT_ERROR_PARSE, open_damaged(damage_truncate_header, false));
    /* Data damage is found when the entry is read (CRC-32). */
    WPE_TEST_ASSERT_EQ_INT(WPE_RESULT_ERROR_PARSE, open_damaged(damage_stored_data, true));
    quiet(false);
}

static void test_vfs_mounts_and_priority(void) {
    WPE_TEST_ASSERT_OK(wpe_directory_create(ROOT "/dir/sub"));
    write_text(ROOT "/dir/only_dir.txt", "from dir");
    write_text(ROOT "/dir/sub/deep.txt", "deep");
    write_text(ROOT "/dir/stored.txt", "dir overrides pak"); /* Also in the archive. */
    write_test_pak(ROOT "/test.pak");

    WPE_Vfs* vfs = NULL;
    WPE_TEST_ASSERT_OK(wpe_vfs_create(&vfs));
    WPE_TEST_ASSERT_OK(wpe_vfs_mount_pak(vfs, ROOT "/test.pak"));
    WPE_TEST_ASSERT_OK(wpe_vfs_mount_directory(vfs, ROOT "/dir/")); /* Later mount wins. */
    WPE_TEST_ASSERT_EQ_UINT(2, wpe_vfs_mount_count(vfs));

    assert_read(vfs, "only_dir.txt", "from dir", 8);
    assert_read(vfs, "sub/deep.txt", "deep", 4);
    assert_read(vfs, "stored.txt", "dir overrides pak", 17);
    assert_read(vfs, "alpha/text.txt", g_text, strlen(g_text));
    assert_read(vfs, "zeta/noise.bin", g_noise, sizeof g_noise);
    assert_read(vfs, "empty", "", 0);
    WPE_TEST_ASSERT(wpe_vfs_exists(vfs, "sub/deep.txt"));
    WPE_TEST_ASSERT(!wpe_vfs_exists(vfs, "sub"));
    WPE_TEST_ASSERT(!wpe_vfs_exists(vfs, "nope.txt"));

    void* data = NULL;
    size_t size = 0;
    WPE_TEST_ASSERT_EQ_INT(WPE_RESULT_ERROR_NOT_FOUND, wpe_vfs_read_all(vfs, "nope.txt", &data, &size));
    quiet(true);
    WPE_TEST_ASSERT_EQ_INT(WPE_RESULT_ERROR_INVALID_ARG, wpe_vfs_read_all(vfs, "../test.pak", &data, &size));
    quiet(false);

    int64_t pak_time = 0, dir_time = 0;
    WPE_TEST_ASSERT_OK(wpe_vfs_modified_time(vfs, "alpha/text.txt", &pak_time));
    WPE_TEST_ASSERT_OK(wpe_vfs_modified_time(vfs, "only_dir.txt", &dir_time));
    WPE_TEST_ASSERT(dir_time != 0);
    WPE_TEST_ASSERT_EQ_INT(WPE_RESULT_ERROR_NOT_FOUND, wpe_vfs_modified_time(vfs, "nope", &dir_time));

    /* A later archive overrides the directory in turn. */
    WPE_PakWriter* writer = NULL;
    WPE_TEST_ASSERT_OK(wpe_pak_writer_create(ROOT "/patch.pak", 0, &writer));
    WPE_TEST_ASSERT_OK(wpe_pak_writer_add(writer, "only_dir.txt", "patched", 7, WPE_PAK_COMPRESS_AUTO));
    WPE_TEST_ASSERT_OK(wpe_pak_writer_finish(writer));
    wpe_pak_writer_destroy(writer);
    WPE_TEST_ASSERT_OK(wpe_vfs_mount_pak(vfs, ROOT "/patch.pak"));
    assert_read(vfs, "only_dir.txt", "patched", 7);
    quiet(true);
    WPE_TEST_ASSERT_EQ_INT(WPE_RESULT_ERROR_NOT_FOUND, wpe_vfs_mount_directory(vfs, ROOT "/missing"));
    quiet(false);
    wpe_vfs_destroy(vfs);
}

/* Streams a file in odd-sized chunks with a seek in the middle and compares with read_all. */
static void check_stream(const WPE_Vfs* vfs, const char* path) {
    void* whole = NULL;
    size_t size = 0;
    WPE_TEST_ASSERT_OK(wpe_vfs_read_all(vfs, path, &whole, &size));
    WPE_VfsFile* file = NULL;
    WPE_TEST_ASSERT_OK(wpe_vfs_open(vfs, path, &file));
    WPE_TEST_ASSERT_EQ_UINT(size, wpe_vfs_file_size(file));
    unsigned char* streamed = wpe_alloc(size + 1);
    size_t at = 0;
    unsigned char chunk[777];
    for (size_t got; (got = wpe_vfs_file_read(file, chunk, sizeof chunk)) > 0; at += got)
        memcpy(streamed + at, chunk, got);
    WPE_TEST_ASSERT_EQ_UINT(size, at);
    WPE_TEST_ASSERT(memcmp(streamed, whole, size) == 0);
    WPE_TEST_ASSERT_EQ_UINT(0, wpe_vfs_file_read(file, chunk, sizeof chunk)); /* At end. */

    if (size > 10) {
        WPE_TEST_ASSERT_OK(wpe_vfs_file_seek(file, size / 2));
        WPE_TEST_ASSERT_EQ_UINT(size / 2, wpe_vfs_file_tell(file));
        WPE_TEST_ASSERT_EQ_UINT(5, wpe_vfs_file_read(file, chunk, 5));
        WPE_TEST_ASSERT(memcmp(chunk, (unsigned char*)whole + size / 2, 5) == 0);
    }
    WPE_TEST_ASSERT_EQ_INT(WPE_RESULT_ERROR_INVALID_ARG, wpe_vfs_file_seek(file, size + 1));
    wpe_vfs_file_close(file);
    wpe_free(streamed);
    wpe_free(whole);
}

static void test_vfs_streaming(void) {
    WPE_TEST_ASSERT_OK(wpe_directory_create(ROOT "/dir/sub"));
    write_text(ROOT "/dir/sub/deep.txt", "deep file contents for streaming");
    write_test_pak(ROOT "/test.pak");
    WPE_Vfs* vfs = NULL;
    WPE_TEST_ASSERT_OK(wpe_vfs_create(&vfs));
    WPE_TEST_ASSERT_OK(wpe_vfs_mount_pak(vfs, ROOT "/test.pak"));
    WPE_TEST_ASSERT_OK(wpe_vfs_mount_directory(vfs, ROOT "/dir"));
    check_stream(vfs, "sub/deep.txt");   /* directory */
    check_stream(vfs, "zeta/noise.bin"); /* stored in the archive */
    check_stream(vfs, "alpha/text.txt"); /* compressed in the archive */
    check_stream(vfs, "empty");
    WPE_VfsFile* file = NULL;
    WPE_TEST_ASSERT_EQ_INT(WPE_RESULT_ERROR_NOT_FOUND, wpe_vfs_open(vfs, "nope", &file));
    wpe_vfs_destroy(vfs);
}

typedef struct Listing {
    char paths[16][64];
    uint32_t count;
} Listing;

static bool collect(const char* path, void* user_data) {
    Listing* listing = user_data;
    if (listing->count < 16) snprintf(listing->paths[listing->count++], 64, "%s", path);
    return true;
}

static void test_vfs_list_and_directory_walk(void) {
    WPE_TEST_ASSERT_OK(wpe_directory_create(ROOT "/listing/a/b"));
    write_text(ROOT "/listing/top.txt", "1");
    write_text(ROOT "/listing/a/b/c.txt", "2");
    write_text(ROOT "/listing/empty", "");
    write_test_pak(ROOT "/test.pak");

    Listing walk = {0};
    WPE_TEST_ASSERT_OK(wpe_directory_walk(ROOT "/listing", collect, &walk));
    WPE_TEST_ASSERT_EQ_UINT(3, walk.count);
    WPE_TEST_ASSERT_EQ_INT(WPE_RESULT_ERROR_NOT_FOUND, wpe_directory_walk(ROOT "/missing", collect, &walk));

    WPE_Vfs* vfs = NULL;
    WPE_TEST_ASSERT_OK(wpe_vfs_create(&vfs));
    WPE_TEST_ASSERT_OK(wpe_vfs_mount_pak(vfs, ROOT "/test.pak"));
    WPE_TEST_ASSERT_OK(wpe_vfs_mount_directory(vfs, ROOT "/listing"));
    Listing listing = {0};
    WPE_TEST_ASSERT_OK(wpe_vfs_list(vfs, collect, &listing));
    /* "empty" exists in both mounts and is listed once; output is sorted. */
    const char* expected[] = {"a/b/c.txt",  "alpha/text.txt", "empty",         "forced.txt",
                              "stored.txt", "top.txt",        "zeta/noise.bin"};
    WPE_TEST_ASSERT_EQ_UINT(7, listing.count);
    for (uint32_t i = 0; i < 7; ++i)
        WPE_TEST_ASSERT_EQ_STR(expected[i], listing.paths[i]);
    wpe_vfs_destroy(vfs);

    WPE_TEST_ASSERT_OK(wpe_path_remove(ROOT "/listing/a/b/c.txt"));
    WPE_TEST_ASSERT_OK(wpe_path_remove(ROOT "/listing/a/b"));
    WPE_TEST_ASSERT(!wpe_directory_exists(ROOT "/listing/a/b"));
    WPE_TEST_ASSERT_OK(wpe_path_remove(ROOT "/listing/a/b")); /* Already gone. */
}

WPE_TEST_MAIN(WPE_TEST_CASE(test_vfs_crc32_known_values), WPE_TEST_CASE(test_vfs_path_rules),
              WPE_TEST_CASE(test_vfs_pak_roundtrip), WPE_TEST_CASE(test_vfs_pak_writer_errors),
              WPE_TEST_CASE(test_vfs_pak_rejects_corruption), WPE_TEST_CASE(test_vfs_mounts_and_priority),
              WPE_TEST_CASE(test_vfs_streaming), WPE_TEST_CASE(test_vfs_list_and_directory_walk))
