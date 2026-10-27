#define WPE_LOG_MODULE "export"
#include "engine/export.h"

#include <stdio.h>
#include <string.h>

#include "core/alloc.h"
#include "core/log.h"
#include "engine/project.h"
#include "platform/platform.h"
#include "vfs/pak.h"
#include "vfs/vfs.h"

/* Checks that the project parses and its start scene exists, through the same VFS view the game gets. */
static WPE_Result wpe__validate_project(const char* project_dir) {
    WPE_Vfs* vfs = NULL;
    WPE_Result result = wpe_vfs_create(&vfs);
    if (result == WPE_RESULT_OK) result = wpe_vfs_mount_directory(vfs, project_dir);
    WPE_ProjectSettings* settings = result == WPE_RESULT_OK ? wpe_alloc(sizeof *settings) : NULL;
    if (result == WPE_RESULT_OK && !settings) result = WPE_RESULT_ERROR_OUT_OF_MEMORY;
    if (result == WPE_RESULT_OK) result = wpe_project_load(vfs, settings);
    if (result == WPE_RESULT_OK && settings->start_scene[0] && !wpe_vfs_exists(vfs, settings->start_scene)) {
        wpe_log_error("the start scene '%s' does not exist", settings->start_scene);
        result = WPE_RESULT_ERROR_NOT_FOUND;
    }
    if (result == WPE_RESULT_OK && settings->font[0] && !wpe_vfs_exists(vfs, settings->font)) {
        wpe_log_error("the font '%s' does not exist", settings->font);
        result = WPE_RESULT_ERROR_NOT_FOUND;
    }
    wpe_free(settings);
    wpe_vfs_destroy(vfs);
    return result;
}

/* Whether `path` lies inside `directory` (compared textually after unifying separators and trailing slashes).
 */
static bool wpe__inside(const char* path, const char* directory) {
    size_t length = strlen(directory);
    while (length > 0 && (directory[length - 1] == '/' || directory[length - 1] == '\\'))
        --length;
    for (size_t i = 0; i < length; ++i) {
        const char a = path[i] == '\\' ? '/' : path[i], b = directory[i] == '\\' ? '/' : directory[i];
        if (a != b) return false;
    }
    return path[length] == '/' || path[length] == '\\';
}

WPE_Result wpe_export_game(const WPE_ExportDesc* desc, WPE_ExportStats* out_stats) {
    if (!desc || !desc->project_dir || !desc->runtime_path || !desc->output_path)
        return WPE_RESULT_ERROR_NULL_ARG;
    if (out_stats) *out_stats = (WPE_ExportStats){0};
    if (wpe__inside(desc->output_path, desc->project_dir)) {
        /* The game would pack itself (and its temporary archive) into its own assets. */
        wpe_log_error("the exported game must be written outside the project directory");
        return WPE_RESULT_ERROR_INVALID_ARG;
    }
    WPE_Result result = wpe__validate_project(desc->project_dir);
    if (result != WPE_RESULT_OK) {
        wpe_log_error("project '%s' is not exportable: %s", desc->project_dir, wpe_result_to_string(result));
        return result;
    }
    char temp[1024];
    if (snprintf(temp, sizeof temp, "%s.pak.tmp", desc->output_path) >= (int)sizeof temp)
        return WPE_RESULT_ERROR_INVALID_ARG;
    WPE_PakPackStats pack = {0};
    result = wpe_pak_create_from_directory(temp, desc->project_dir, desc->compression_level, &pack);
    if (result == WPE_RESULT_OK) result = wpe_pak_embed(desc->runtime_path, temp, desc->output_path);
    wpe_path_remove(temp);
    if (result == WPE_RESULT_OK) result = wpe_file_set_executable(desc->output_path);
    if (result != WPE_RESULT_OK) {
        wpe_path_remove(desc->output_path);
        wpe_log_error("export failed: %s", wpe_result_to_string(result));
        return result;
    }
    if (out_stats) {
        WPE_File* file = NULL;
        uint64_t size = 0;
        if (wpe_file_open(desc->output_path, false, &file) == WPE_RESULT_OK) {
            size = wpe_file_size(file);
            wpe_file_close(file);
        }
        *out_stats = (WPE_ExportStats){.files = pack.files, .asset_bytes = pack.bytes, .output_bytes = size};
    }
    return WPE_RESULT_OK;
}
