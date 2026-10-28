/*
 * New projects: the files a game starts from (wpe_project_create, `wpe_editor <dir> --init`). They are
 * copied from the project templates, which CMake embeds from templates/<name>/ (templates_data.h).
 */
#define WPE_LOG_MODULE "engine"
#include <stdio.h>
#include <string.h>

#include "core/log.h"
#include "engine/project.h"
#include "engine/scene_file_internal.h"
#include "platform/platform.h"

typedef struct WPE__TemplateFile {
    const char* template_name;
    const char* path; /* Relative to the template's directory, with '/' separators. */
    const unsigned char* data;
    size_t size;
} WPE__TemplateFile;

#include "engine/templates_data.h"

#define WPE__TEMPLATE_FILE_COUNT (sizeof wpe__template_files / sizeof wpe__template_files[0])

uint32_t wpe_project_template_count(void) {
    uint32_t count = 0;
    for (size_t i = 0; i < WPE__TEMPLATE_FILE_COUNT; ++i) {
        if (i == 0 ||
            strcmp(wpe__template_files[i].template_name, wpe__template_files[i - 1].template_name) != 0)
            ++count;
    }
    return count;
}

const char* wpe_project_template_name(uint32_t index) {
    uint32_t seen = 0;
    for (size_t i = 0; i < WPE__TEMPLATE_FILE_COUNT; ++i) {
        if (i > 0 &&
            strcmp(wpe__template_files[i].template_name, wpe__template_files[i - 1].template_name) == 0)
            continue;
        if (seen++ == index) return wpe__template_files[i].template_name;
    }
    return NULL;
}

static bool wpe__any_file(const char* relative_path, void* user_data) {
    (void)relative_path;
    *(bool*)user_data = true;
    return false;
}

/* Writes a file of the new project, making its directory first. */
static WPE_Result wpe__write_project_file(const char* directory, const char* relative, const void* data,
                                          size_t size) {
    char path[1024];
    if (snprintf(path, sizeof path, "%s/%s", directory, relative) >= (int)sizeof path) {
        wpe_log_error("the project path '%s' is too long", directory);
        return WPE_RESULT_ERROR_INVALID_ARG;
    }
    char* slash = strrchr(path, '/');
    *slash = '\0';
    WPE_Result result = wpe_directory_create(path);
    *slash = '/';
    if (result == WPE_RESULT_OK) result = wpe__write_file_atomic(path, data, size);
    if (result != WPE_RESULT_OK) wpe_log_error("cannot write '%s': %s", path, wpe_result_to_string(result));
    return result;
}

/* The template's project.json with the new name, written back in the canonical form. */
static WPE_Result wpe__renamed_project(const WPE__TemplateFile* file, const char* name,
                                       WPE_JsonWriter* writer) {
    WPE_ProjectSettings settings;
    WPE_Result result = wpe_project_parse((const char*)file->data, file->size, file->path, &settings);
    if (result != WPE_RESULT_OK) {
        wpe_log_error("the template '%s' has an invalid %s", file->template_name, file->path);
        return result;
    }
    snprintf(settings.name, sizeof settings.name, "%s", name);
    return wpe_project_write(&settings, writer);
}

WPE_Result wpe_project_create(const char* os_directory, const char* name, const char* template_name) {
    if (!os_directory || !name) return WPE_RESULT_ERROR_NULL_ARG;
    if (!template_name) template_name = WPE_PROJECT_DEFAULT_TEMPLATE;
    if (!name[0] || strlen(name) >= WPE_PROJECT_NAME_MAX) {
        wpe_log_error("a project name is 1 to %u bytes", WPE_PROJECT_NAME_MAX - 1);
        return WPE_RESULT_ERROR_INVALID_ARG;
    }
    size_t first = 0;
    while (first < WPE__TEMPLATE_FILE_COUNT &&
           strcmp(wpe__template_files[first].template_name, template_name) != 0)
        ++first;
    if (first == WPE__TEMPLATE_FILE_COUNT) {
        char names[512] = "";
        size_t length = 0;
        for (uint32_t i = 0; i < wpe_project_template_count() && length < sizeof names; ++i)
            length += (size_t)snprintf(names + length, sizeof names - length, "%s%s", i ? ", " : "",
                                       wpe_project_template_name(i));
        wpe_log_error("there is no project template '%s' (templates: %s)", template_name, names);
        return WPE_RESULT_ERROR_NOT_FOUND;
    }
    bool occupied = false;
    if (wpe_directory_exists(os_directory)) {
        const WPE_Result walked = wpe_directory_walk(os_directory, wpe__any_file, &occupied);
        if (walked != WPE_RESULT_OK) return walked;
    }
    if (occupied) {
        wpe_log_error("'%s' already has files; a new project needs an empty or new directory", os_directory);
        return WPE_RESULT_ERROR_ALREADY_EXISTS;
    }

    WPE_JsonWriter* writer = NULL;
    WPE_Result result = wpe_json_writer_create(&writer);
    for (size_t i = first; result == WPE_RESULT_OK && i < WPE__TEMPLATE_FILE_COUNT &&
                           strcmp(wpe__template_files[i].template_name, template_name) == 0;
         ++i) {
        const WPE__TemplateFile* file = &wpe__template_files[i];
        if (strcmp(file->path, WPE_PROJECT_FILE) != 0) {
            result = wpe__write_project_file(os_directory, file->path, file->data, file->size);
            continue;
        }
        const char* project = NULL;
        size_t size = 0;
        result = wpe__renamed_project(file, name, writer);
        if (result == WPE_RESULT_OK) result = wpe_json_writer_finish(writer, &project, &size);
        if (result == WPE_RESULT_OK)
            result = wpe__write_project_file(os_directory, file->path, project, size);
    }
    wpe_json_writer_destroy(writer);
    if (result == WPE_RESULT_OK)
        wpe_log_info("created project '%s' in '%s' from the template '%s'", name, os_directory,
                     template_name);
    return result;
}
