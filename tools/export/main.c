/*
 * wpe_export: turns a project directory into a standalone game executable.
 *
 *   wpe_export <project-dir> <output-executable> [--runtime <wpe_runtime>] [--level N]
 *
 * The output is the runtime with the project's files packed into an archive appended to it (see
 * engine/export.h). By default the runtime next to this tool is used.
 */
#define WPE_LOG_MODULE "wpe_export"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "core/core.h"
#include "engine/export.h"
#include "platform/platform.h"

#define OS_PATH_MAX 4096

static int usage(void) {
    fprintf(stderr, "usage: wpe_export <project-dir> <output-executable> [--runtime <wpe_runtime>] [--level N]\n");
    return EXIT_FAILURE;
}

/* The runtime installed next to this tool. */
static bool default_runtime(char* out, size_t capacity) {
    if (wpe_platform_executable_path(out, capacity) != WPE_RESULT_OK) return false;
    char* slash = strrchr(out, '/');
    char* backslash = strrchr(out, '\\');
    if (backslash && (!slash || backslash > slash)) slash = backslash;
    const size_t directory = slash ? (size_t)(slash - out) + 1 : 0;
    return snprintf(out + directory, capacity - directory, "wpe_runtime%s", WPE_EXE_SUFFIX) <
           (int)(capacity - directory);
}

int main(int argc, char** argv) {
    if (argc < 3) return usage();
    WPE_ExportDesc desc = {.project_dir = argv[1], .output_path = argv[2]};
    for (int i = 3; i < argc; ++i) {
        if (i + 1 < argc && strcmp(argv[i], "--runtime") == 0) {
            desc.runtime_path = argv[++i];
        } else if (i + 1 < argc && strcmp(argv[i], "--level") == 0) {
            desc.compression_level = atoi(argv[++i]);
            if (desc.compression_level < 1 || desc.compression_level > 19) {
                fprintf(stderr, "--level must be 1..19\n");
                return EXIT_FAILURE;
            }
        } else {
            return usage();
        }
    }
    static char runtime[OS_PATH_MAX];
    if (!desc.runtime_path) {
        if (!default_runtime(runtime, sizeof runtime)) {
            fprintf(stderr, "cannot locate wpe_runtime; give it with --runtime\n");
            return EXIT_FAILURE;
        }
        desc.runtime_path = runtime;
    }
    WPE_ExportStats stats;
    const WPE_Result result = wpe_export_game(&desc, &stats);
    if (result != WPE_RESULT_OK) {
        fprintf(stderr, "export failed: %s\n", wpe_result_to_string(result));
        return EXIT_FAILURE;
    }
    printf("exported %s: %u files (%llu bytes of assets), %llu bytes in total\n", desc.output_path, stats.files,
           (unsigned long long)stats.asset_bytes, (unsigned long long)stats.output_bytes);
    return EXIT_SUCCESS;
}
