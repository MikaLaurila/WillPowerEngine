/*
 * wpe_pak: builds, inspects and unpacks .pak archives.
 *
 *   wpe_pak create <archive.pak> <directory> [--level N]   pack every file below a directory
 *   wpe_pak list <archive.pak>                             print the index
 *   wpe_pak verify <archive.pak>                           read every file and check its CRC-32
 *   wpe_pak extract <archive.pak> <directory>              unpack into a directory
 *
 * Files are added in sorted path order, so the same input always produces the same archive.
 * Files whose names are not valid asset paths (see wpe_vfs_path_is_valid) are reported and skipped.
 */
#define WPE_LOG_MODULE "wpe_pak"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "core/alloc.h"
#include "core/log.h"
#include "platform/platform.h"
#include "vfs/vfs.h"

#define OS_PATH_MAX 1024

static int usage(void) {
    fprintf(stderr, "usage:\n"
                    "  wpe_pak create <archive.pak> <directory> [--level N]\n"
                    "  wpe_pak list <archive.pak>\n"
                    "  wpe_pak verify <archive.pak>\n"
                    "  wpe_pak extract <archive.pak> <directory>\n");
    return EXIT_FAILURE;
}

typedef struct PathList {
    char** paths;
    uint32_t count, capacity;
    uint32_t skipped;
    bool out_of_memory;
} PathList;

static bool collect(const char* path, void* user_data) {
    PathList* list = user_data;
    if (!wpe_vfs_path_is_valid(path)) {
        fprintf(stderr, "skipping '%s': not a valid asset path\n", path);
        ++list->skipped;
        return true;
    }
    if (list->count == list->capacity) {
        const uint32_t capacity = list->capacity ? list->capacity * 2 : 256;
        char** paths = wpe_realloc(list->paths, (size_t)capacity * sizeof *paths);
        if (!paths) {
            list->out_of_memory = true;
            return false;
        }
        list->paths = paths;
        list->capacity = capacity;
    }
    const size_t length = strlen(path);
    char* copy = wpe_alloc(length + 1);
    if (!copy) {
        list->out_of_memory = true;
        return false;
    }
    memcpy(copy, path, length + 1);
    list->paths[list->count++] = copy;
    return true;
}

static int compare_paths(const void* a, const void* b) {
    return strcmp(*(char* const*)a, *(char* const*)b);
}

static int command_create(const char* archive, const char* directory, int level) {
    PathList list = {0};
    WPE_Result result = wpe_directory_walk(directory, collect, &list);
    if (result == WPE_RESULT_ERROR_NOT_FOUND) fprintf(stderr, "'%s' is not a directory\n", directory);
    if (list.out_of_memory) result = WPE_RESULT_ERROR_OUT_OF_MEMORY;
    if (list.count > 1) qsort(list.paths, list.count, sizeof *list.paths, compare_paths);

    WPE_PakWriter* writer = NULL;
    if (result == WPE_RESULT_OK) result = wpe_pak_writer_create(archive, level, &writer);
    uint64_t total = 0;
    for (uint32_t i = 0; i < list.count && result == WPE_RESULT_OK; ++i) {
        char full[OS_PATH_MAX];
        if (snprintf(full, sizeof full, "%s/%s", directory, list.paths[i]) >= (int)sizeof full) {
            fprintf(stderr, "path too long: %s\n", list.paths[i]);
            result = WPE_RESULT_ERROR_CAPACITY;
            break;
        }
        void* data = NULL;
        size_t size = 0;
        result = wpe_file_read_all(full, &data, &size);
        if (result == WPE_RESULT_OK) result = wpe_pak_writer_add(writer, list.paths[i], data, size, WPE_PAK_COMPRESS_AUTO);
        total += size;
        wpe_free(data);
    }
    if (result == WPE_RESULT_OK) result = wpe_pak_writer_finish(writer);
    wpe_pak_writer_destroy(writer);
    for (uint32_t i = 0; i < list.count; ++i)
        wpe_free(list.paths[i]);
    wpe_free(list.paths);
    if (result != WPE_RESULT_OK) {
        fprintf(stderr, "create failed: %s\n", wpe_result_to_string(result));
        return EXIT_FAILURE;
    }
    printf("packed %u files (%llu bytes, %u skipped) into %s\n", list.count, (unsigned long long)total, list.skipped,
           archive);
    return EXIT_SUCCESS;
}

static int command_list(const WPE_Pak* pak) {
    uint64_t size = 0, stored = 0;
    for (uint32_t i = 0; i < wpe_pak_entry_count(pak); ++i) {
        const WPE_PakEntryInfo e = wpe_pak_entry(pak, i);
        printf("%12llu %12llu %-5s %08x  %s\n", (unsigned long long)e.size, (unsigned long long)e.stored_size,
               e.compressed ? "zstd" : "store", e.crc32, e.path);
        size += e.size;
        stored += e.stored_size;
    }
    printf("%12llu %12llu  %u files\n", (unsigned long long)size, (unsigned long long)stored, wpe_pak_entry_count(pak));
    return EXIT_SUCCESS;
}

static int command_verify(const WPE_Pak* pak) {
    uint32_t failures = 0;
    for (uint32_t i = 0; i < wpe_pak_entry_count(pak); ++i) {
        void* data = NULL;
        size_t size = 0;
        if (wpe_pak_read(pak, i, &data, &size) != WPE_RESULT_OK) ++failures;
        wpe_free(data);
    }
    printf("%u of %u files OK\n", wpe_pak_entry_count(pak) - failures, wpe_pak_entry_count(pak));
    return failures ? EXIT_FAILURE : EXIT_SUCCESS;
}

static int command_extract(const WPE_Pak* pak, const char* directory) {
    for (uint32_t i = 0; i < wpe_pak_entry_count(pak); ++i) {
        const WPE_PakEntryInfo e = wpe_pak_entry(pak, i);
        char full[OS_PATH_MAX];
        if (snprintf(full, sizeof full, "%s/%s", directory, e.path) >= (int)sizeof full) {
            fprintf(stderr, "path too long: %s\n", e.path);
            return EXIT_FAILURE;
        }
        /* Create the parent directory (asset paths never contain "..", so this stays inside). */
        char parent[OS_PATH_MAX];
        memcpy(parent, full, sizeof parent);
        char* slash = strrchr(parent, '/');
        if (slash) *slash = '\0';
        void* data = NULL;
        size_t size = 0;
        WPE_Result result = wpe_directory_create(parent);
        if (result == WPE_RESULT_OK) result = wpe_pak_read(pak, i, &data, &size);
        WPE_File* file = NULL;
        if (result == WPE_RESULT_OK) result = wpe_file_open(full, true, &file);
        if (result == WPE_RESULT_OK) result = wpe_file_write(file, data, size);
        const WPE_Result closed = wpe_file_close(file);
        wpe_free(data);
        if (result == WPE_RESULT_OK) result = closed;
        if (result != WPE_RESULT_OK) {
            fprintf(stderr, "extracting '%s' failed: %s\n", e.path, wpe_result_to_string(result));
            return EXIT_FAILURE;
        }
    }
    printf("extracted %u files into %s\n", wpe_pak_entry_count(pak), directory);
    return EXIT_SUCCESS;
}

int main(int argc, char** argv) {
    if (argc < 3) return usage();
    const char* command = argv[1];
    const char* archive = argv[2];

    if (strcmp(command, "create") == 0) {
        if (argc != 4 && !(argc == 6 && strcmp(argv[4], "--level") == 0)) return usage();
        const int level = argc == 6 ? atoi(argv[5]) : 0;
        if (level < 0 || level > 19) {
            fprintf(stderr, "--level must be 1..19\n");
            return EXIT_FAILURE;
        }
        return command_create(archive, argv[3], level);
    }

    const bool extract = strcmp(command, "extract") == 0;
    if (!extract && strcmp(command, "list") != 0 && strcmp(command, "verify") != 0) return usage();
    if (argc != (extract ? 4 : 3)) return usage();
    WPE_Pak* pak = NULL;
    const WPE_Result result = wpe_pak_open(archive, &pak);
    if (result != WPE_RESULT_OK) {
        fprintf(stderr, "cannot open '%s': %s\n", archive, wpe_result_to_string(result));
        return EXIT_FAILURE;
    }
    const int code = extract ? command_extract(pak, argv[3])
                     : strcmp(command, "list") == 0 ? command_list(pak)
                                                    : command_verify(pak);
    wpe_pak_close(pak);
    return code;
}
