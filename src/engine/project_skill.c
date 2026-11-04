/*
 * The `willpowerengine` Claude skill in game projects (wpe_project_install_skill, `wpe_editor --init` and
 * `--update-skill`): the skill and the docs it points to, embedded at build time by cmake/WpeEmbedSkill.cmake
 * (skill_data.h), so a project's copy always matches the engine build that wrote it.
 */
#define WPE_LOG_MODULE "engine"
#include <stdio.h>
#include <string.h>

#include "core/alloc.h"
#include "core/log.h"
#include "engine/project.h"
#include "engine/scene_file_internal.h"
#include "platform/platform.h"

typedef struct WPE__SkillFile {
    const char* path; /* Relative to the skill's directory, with '/' separators. */
    const unsigned char* data;
    size_t size;
} WPE__SkillFile;

#include "engine/skill_data.h"

#define WPE__SKILL_FILE_COUNT (sizeof wpe__skill_files / sizeof wpe__skill_files[0])
#define WPE__PATH_MAX 1024

const char* wpe_project_skill_id(void) {
    return WPE__SKILL_ID;
}

/* <project>/WPE_PROJECT_SKILL_DIR[/relative]; false (logged) when it doesn't fit. */
static bool wpe__skill_path(const char* project, const char* relative, char* out, size_t capacity) {
    const int written = relative
                            ? snprintf(out, capacity, "%s/%s/%s", project, WPE_PROJECT_SKILL_DIR, relative)
                            : snprintf(out, capacity, "%s/%s", project, WPE_PROJECT_SKILL_DIR);
    if (written < 0 || (size_t)written >= capacity) {
        wpe_log_error("the project path '%s' is too long", project);
        return false;
    }
    return true;
}

typedef struct wpe__Stale {
    char paths[16][WPE__PATH_MAX];
    uint32_t count;
} wpe__Stale;

/* Files of an older copy that this one doesn't have (at most 16 per install; the rest go next time). */
static bool wpe__find_stale(const char* relative_path, void* user_data) {
    wpe__Stale* stale = user_data;
    for (size_t i = 0; i < WPE__SKILL_FILE_COUNT; ++i) {
        if (strcmp(wpe__skill_files[i].path, relative_path) == 0) return true;
    }
    if (stale->count < 16) snprintf(stale->paths[stale->count++], WPE__PATH_MAX, "%s", relative_path);
    return true;
}

WPE_Result wpe_project_install_skill(const char* os_directory) {
    if (!os_directory) return WPE_RESULT_ERROR_NULL_ARG;
    if (!wpe_directory_exists(os_directory)) {
        wpe_log_error("'%s' is not a directory", os_directory);
        return WPE_RESULT_ERROR_NOT_FOUND;
    }
    char path[WPE__PATH_MAX];
    if (!wpe__skill_path(os_directory, NULL, path, sizeof path)) return WPE_RESULT_ERROR_INVALID_ARG;
    if (wpe_directory_exists(path)) {
        wpe__Stale* stale = wpe_alloc_zeroed(1, sizeof *stale);
        if (!stale) return WPE_RESULT_ERROR_OUT_OF_MEMORY;
        WPE_Result result = wpe_directory_walk(path, wpe__find_stale, stale);
        for (uint32_t i = 0; result == WPE_RESULT_OK && i < stale->count; ++i) {
            char doomed[WPE__PATH_MAX];
            if (wpe__skill_path(os_directory, stale->paths[i], doomed, sizeof doomed)) {
                result = wpe_path_remove(doomed);
                if (result != WPE_RESULT_OK) wpe_log_error("cannot remove the old '%s'", doomed);
            }
        }
        wpe_free(stale);
        if (result != WPE_RESULT_OK) return result;
    }
    for (size_t i = 0; i < WPE__SKILL_FILE_COUNT; ++i) {
        const WPE__SkillFile* file = &wpe__skill_files[i];
        if (!wpe__skill_path(os_directory, file->path, path, sizeof path))
            return WPE_RESULT_ERROR_INVALID_ARG;
        char* slash = strrchr(path, '/');
        *slash = '\0';
        WPE_Result result = wpe_directory_create(path);
        *slash = '/';
        if (result == WPE_RESULT_OK)
            result = wpe__write_file_atomic(path, (const char*)file->data, file->size);
        if (result != WPE_RESULT_OK) {
            wpe_log_error("cannot write '%s': %s", path, wpe_result_to_string(result));
            return result;
        }
    }
    wpe_log_info("installed the willpowerengine skill (%s) in '%s/%s'", WPE__SKILL_ID, os_directory,
                 WPE_PROJECT_SKILL_DIR);
    return WPE_RESULT_OK;
}

WPE_SkillStatus wpe_project_skill_status(const char* os_directory) {
    char path[WPE__PATH_MAX];
    if (!os_directory || !wpe__skill_path(os_directory, "SKILL.md", path, sizeof path))
        return WPE_SKILL_MISSING;
    int64_t modified = 0;
    if (wpe_file_modified_time(path, &modified) != WPE_RESULT_OK) return WPE_SKILL_MISSING;
    void* text = NULL;
    size_t size = 0;
    if (wpe_file_read_all(path, &text, &size) != WPE_RESULT_OK) return WPE_SKILL_OUTDATED;
    /* The stamp the install wrote into the front matter: skill_id: "<id>". */
    const char* stamp = strstr(text, "skill_id: \"");
    const bool current = stamp && strncmp(stamp + 11, WPE__SKILL_ID, strlen(WPE__SKILL_ID)) == 0 &&
                         stamp[11 + strlen(WPE__SKILL_ID)] == '"';
    wpe_free(text);
    return current ? WPE_SKILL_CURRENT : WPE_SKILL_OUTDATED;
}
