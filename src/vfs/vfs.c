#define WPE_LOG_MODULE "vfs"
#include "vfs/vfs.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "core/alloc.h"
#include "core/debug.h"
#include "core/log.h"
#include "platform/platform.h"

/* OS paths built from a mount root and an asset path. */
#define WPE__OS_PATH_MAX 1024u

typedef struct wpe__Mount {
    char* directory; /* Directory mount: OS path without a trailing separator. */
    WPE_Pak* pak;    /* Archive mount. */
    int64_t pak_time;
} wpe__Mount;

struct WPE_Vfs {
    wpe__Mount* mounts;
    uint32_t count, capacity;
};

struct WPE_VfsFile {
    WPE_File* file;        /* Directory file or the archive (NULL when decompressed into memory). */
    unsigned char* memory; /* Decompressed archive entry. */
    uint64_t base;         /* Offset of the file's first byte within `file`. */
    uint64_t size;
    uint64_t position;
};

bool wpe_vfs_path_is_valid(const char* path) {
    if (!path || path[0] == '\0') return false;
    size_t length = 0;
    size_t segment_start = 0;
    for (;; ++length) {
        const unsigned char c = (unsigned char)path[length];
        if (c == '/' || c == '\0') {
            const size_t segment = length - segment_start;
            if (segment == 0) return false; /* Leading '/', '//' or trailing '/'. */
            if (segment == 1 && path[segment_start] == '.') return false;
            if (segment == 2 && path[segment_start] == '.' && path[segment_start + 1] == '.') return false;
            if (c == '\0') break;
            segment_start = length + 1;
        } else if (c < 0x20 || c == 0x7F || c == '\\' || c == ':') {
            return false;
        }
        if (length + 1 >= WPE_VFS_PATH_MAX) return false;
    }
    return true;
}

bool wpe_vfs_path_normalize(const char* path, char* out, size_t capacity) {
    if (!out || capacity == 0) return false;
    out[0] = '\0';
    if (!path || capacity < WPE_VFS_PATH_MAX || path[0] == '/' || path[0] == '\\') return false;
    size_t length = 0;
    for (const char* p = path; *p;) {
        const char* end = p;
        while (*end && *end != '/' && *end != '\\')
            ++end;
        const size_t segment = (size_t)(end - p);
        if (segment == 2 && p[0] == '.' && p[1] == '.') {
            if (length == 0) { /* Escapes the root. */
                out[0] = '\0';
                return false;
            }
            while (length > 0 && out[length - 1] != '/')
                --length;
            if (length > 0) --length; /* The separator before the removed segment. */
        } else if (segment > 0 && !(segment == 1 && p[0] == '.')) {
            if (length + (length > 0) + segment >= WPE_VFS_PATH_MAX) {
                out[0] = '\0';
                return false;
            }
            if (length > 0) out[length++] = '/';
            memcpy(out + length, p, segment);
            length += segment;
        }
        p = *end ? end + 1 : end;
    }
    out[length] = '\0';
    if (!wpe_vfs_path_is_valid(out)) {
        out[0] = '\0';
        return false;
    }
    return true;
}

WPE_Result wpe_vfs_create(WPE_Vfs** out_vfs) {
    if (!out_vfs) return WPE_RESULT_ERROR_NULL_ARG;
    *out_vfs = wpe_alloc_zeroed(1, sizeof **out_vfs);
    return *out_vfs ? WPE_RESULT_OK : WPE_RESULT_ERROR_OUT_OF_MEMORY;
}

void wpe_vfs_destroy(WPE_Vfs* vfs) {
    if (!vfs) return;
    for (uint32_t i = 0; i < vfs->count; ++i) {
        wpe_free(vfs->mounts[i].directory);
        wpe_pak_close(vfs->mounts[i].pak);
    }
    wpe_free(vfs->mounts);
    wpe_free(vfs);
}

static wpe__Mount* wpe__add_mount(WPE_Vfs* vfs) {
    if (vfs->count == vfs->capacity) {
        const uint32_t capacity = vfs->capacity ? vfs->capacity * 2 : 4;
        wpe__Mount* mounts = wpe_realloc(vfs->mounts, (size_t)capacity * sizeof *mounts);
        if (!mounts) return NULL;
        vfs->mounts = mounts;
        vfs->capacity = capacity;
    }
    wpe__Mount* mount = &vfs->mounts[vfs->count];
    *mount = (wpe__Mount){0};
    return mount;
}

WPE_Result wpe_vfs_mount_directory(WPE_Vfs* vfs, const char* os_directory) {
    if (!vfs || !os_directory) return WPE_RESULT_ERROR_NULL_ARG;
    if (!wpe_directory_exists(os_directory)) {
        wpe_log_error("cannot mount '%s': not a directory", os_directory);
        return WPE_RESULT_ERROR_NOT_FOUND;
    }
    size_t length = strlen(os_directory);
    while (length > 1 && (os_directory[length - 1] == '/' || os_directory[length - 1] == '\\'))
        --length;
    char* directory = wpe_alloc(length + 1);
    wpe__Mount* mount = directory ? wpe__add_mount(vfs) : NULL;
    if (!mount) {
        wpe_free(directory);
        return WPE_RESULT_ERROR_OUT_OF_MEMORY;
    }
    memcpy(directory, os_directory, length);
    directory[length] = '\0';
    mount->directory = directory;
    ++vfs->count;
    return WPE_RESULT_OK;
}

WPE_Result wpe_vfs_mount_pak(WPE_Vfs* vfs, const char* os_path) {
    if (!vfs || !os_path) return WPE_RESULT_ERROR_NULL_ARG;
    WPE_Pak* pak = NULL;
    const WPE_Result result = wpe_pak_open(os_path, &pak);
    if (result != WPE_RESULT_OK) return result;
    wpe__Mount* mount = wpe__add_mount(vfs);
    if (!mount) {
        wpe_pak_close(pak);
        return WPE_RESULT_ERROR_OUT_OF_MEMORY;
    }
    mount->pak = pak;
    if (wpe_file_modified_time(os_path, &mount->pak_time) != WPE_RESULT_OK) mount->pak_time = 0;
    ++vfs->count;
    return WPE_RESULT_OK;
}

uint32_t wpe_vfs_mount_count(const WPE_Vfs* vfs) {
    return vfs ? vfs->count : 0;
}

/* Joins a directory mount and an asset path; false if it does not fit. */
static bool wpe__os_path(const wpe__Mount* mount, const char* path, char* out) {
    const int written = snprintf(out, WPE__OS_PATH_MAX, "%s/%s", mount->directory, path);
    return written > 0 && (size_t)written < WPE__OS_PATH_MAX;
}

/* Finds the highest-priority mount holding `path`; returns its index or UINT32_MAX. For archives,
 * *out_entry receives the entry index. */
static uint32_t wpe__locate(const WPE_Vfs* vfs, const char* path, uint32_t* out_entry) {
    for (uint32_t m = vfs->count; m-- > 0;) {
        const wpe__Mount* mount = &vfs->mounts[m];
        if (mount->pak) {
            const uint32_t entry = wpe_pak_find(mount->pak, path);
            if (entry != UINT32_MAX) {
                *out_entry = entry;
                return m;
            }
        } else {
            char full[WPE__OS_PATH_MAX];
            int64_t time = 0;
            if (wpe__os_path(mount, path, full) && wpe_file_modified_time(full, &time) == WPE_RESULT_OK)
                return m;
        }
    }
    return UINT32_MAX;
}

static WPE_Result wpe__check_path(const char* path) {
    if (wpe_vfs_path_is_valid(path)) return WPE_RESULT_OK;
    wpe_log_error("'%s' is not a valid asset path", path);
    return WPE_RESULT_ERROR_INVALID_ARG;
}

bool wpe_vfs_exists(const WPE_Vfs* vfs, const char* path) {
    uint32_t entry = 0;
    return vfs && wpe_vfs_path_is_valid(path) && wpe__locate(vfs, path, &entry) != UINT32_MAX;
}

WPE_Result wpe_vfs_read_all(const WPE_Vfs* vfs, const char* path, void** out_data, size_t* out_size) {
    if (!vfs || !path || !out_data || !out_size) return WPE_RESULT_ERROR_NULL_ARG;
    *out_data = NULL;
    *out_size = 0;
    const WPE_Result valid = wpe__check_path(path);
    if (valid != WPE_RESULT_OK) return valid;
    uint32_t entry = 0;
    const uint32_t m = wpe__locate(vfs, path, &entry);
    if (m == UINT32_MAX) return WPE_RESULT_ERROR_NOT_FOUND;
    const wpe__Mount* mount = &vfs->mounts[m];
    if (mount->pak) return wpe_pak_read(mount->pak, entry, out_data, out_size);
    char full[WPE__OS_PATH_MAX];
    wpe__os_path(mount, path, full);
    return wpe_file_read_all(full, out_data, out_size);
}

WPE_Result wpe_vfs_modified_time(const WPE_Vfs* vfs, const char* path, int64_t* out_time) {
    if (!vfs || !path || !out_time) return WPE_RESULT_ERROR_NULL_ARG;
    if (!wpe_vfs_path_is_valid(path)) return WPE_RESULT_ERROR_NOT_FOUND;
    uint32_t entry = 0;
    const uint32_t m = wpe__locate(vfs, path, &entry);
    if (m == UINT32_MAX) return WPE_RESULT_ERROR_NOT_FOUND;
    const wpe__Mount* mount = &vfs->mounts[m];
    if (mount->pak) {
        *out_time = mount->pak_time;
        return WPE_RESULT_OK;
    }
    char full[WPE__OS_PATH_MAX];
    wpe__os_path(mount, path, full);
    return wpe_file_modified_time(full, out_time);
}

WPE_Result wpe_vfs_open(const WPE_Vfs* vfs, const char* path, WPE_VfsFile** out_file) {
    if (!vfs || !path || !out_file) return WPE_RESULT_ERROR_NULL_ARG;
    *out_file = NULL;
    WPE_Result result = wpe__check_path(path);
    if (result != WPE_RESULT_OK) return result;
    uint32_t entry = 0;
    const uint32_t m = wpe__locate(vfs, path, &entry);
    if (m == UINT32_MAX) return WPE_RESULT_ERROR_NOT_FOUND;
    const wpe__Mount* mount = &vfs->mounts[m];

    WPE_VfsFile* file = wpe_alloc_zeroed(1, sizeof *file);
    if (!file) return WPE_RESULT_ERROR_OUT_OF_MEMORY;
    if (mount->pak && wpe_pak_entry(mount->pak, entry).compressed) {
        size_t size = 0;
        void* memory = NULL;
        result = wpe_pak_read(mount->pak, entry, &memory, &size);
        file->memory = memory;
        file->size = size;
    } else if (mount->pak) {
        /* Stored entries stream straight from the archive. CRC-32 is not verified when streaming. */
        const WPE_PakEntryInfo info = wpe_pak_entry(mount->pak, entry);
        result = wpe_file_open(wpe_pak_os_path(mount->pak), false, &file->file);
        file->size = info.size;
        file->base = info.offset;
        if (result == WPE_RESULT_OK) result = wpe_file_seek(file->file, file->base);
    } else {
        char full[WPE__OS_PATH_MAX];
        wpe__os_path(mount, path, full);
        result = wpe_file_open(full, false, &file->file);
        if (result == WPE_RESULT_OK) {
            file->size = wpe_file_size(file->file);
            if (file->size == UINT64_MAX) result = WPE_RESULT_ERROR_IO;
        }
    }
    if (result != WPE_RESULT_OK) {
        wpe_vfs_file_close(file);
        return result;
    }
    *out_file = file;
    return WPE_RESULT_OK;
}

void wpe_vfs_file_close(WPE_VfsFile* file) {
    if (!file) return;
    wpe_file_close(file->file);
    wpe_free(file->memory);
    wpe_free(file);
}

size_t wpe_vfs_file_read(WPE_VfsFile* file, void* buffer, size_t size) {
    if (!file || !buffer || size == 0 || file->position >= file->size) return 0;
    const uint64_t left = file->size - file->position;
    const size_t wanted = left < size ? (size_t)left : size;
    size_t read = 0;
    if (file->memory) {
        memcpy(buffer, file->memory + file->position, wanted);
        read = wanted;
    } else {
        read = wpe_file_read(file->file, buffer, wanted);
    }
    file->position += read;
    return read;
}

WPE_Result wpe_vfs_file_seek(WPE_VfsFile* file, uint64_t offset) {
    if (!file) return WPE_RESULT_ERROR_NULL_ARG;
    if (offset > file->size) return WPE_RESULT_ERROR_INVALID_ARG;
    if (!file->memory) {
        const WPE_Result result = wpe_file_seek(file->file, file->base + offset);
        if (result != WPE_RESULT_OK) return result;
    }
    file->position = offset;
    return WPE_RESULT_OK;
}

uint64_t wpe_vfs_file_tell(const WPE_VfsFile* file) {
    return file ? file->position : 0;
}

uint64_t wpe_vfs_file_size(const WPE_VfsFile* file) {
    return file ? file->size : 0;
}

/* ---- Listing ------------------------------------------------------------------------------------- */

typedef struct wpe__PathList {
    char** paths;
    uint32_t count, capacity;
    bool out_of_memory;
} wpe__PathList;

static bool wpe__collect(const char* path, void* user_data) {
    wpe__PathList* list = user_data;
    if (!wpe_vfs_path_is_valid(path)) return true; /* Not addressable through the VFS. */
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

static int wpe__compare_paths(const void* a, const void* b) {
    return strcmp(*(char* const*)a, *(char* const*)b);
}

WPE_Result wpe_vfs_list(const WPE_Vfs* vfs, WPE_VfsListFn fn, void* user_data) {
    if (!vfs || !fn) return WPE_RESULT_ERROR_NULL_ARG;
    wpe__PathList list = {0};
    WPE_Result result = WPE_RESULT_OK;
    for (uint32_t m = 0; m < vfs->count && result == WPE_RESULT_OK && !list.out_of_memory; ++m) {
        const wpe__Mount* mount = &vfs->mounts[m];
        if (mount->pak) {
            for (uint32_t e = 0; e < wpe_pak_entry_count(mount->pak) && !list.out_of_memory; ++e)
                wpe__collect(wpe_pak_entry(mount->pak, e).path, &list);
        } else {
            result = wpe_directory_walk(mount->directory, wpe__collect, &list);
        }
    }
    if (list.out_of_memory) result = WPE_RESULT_ERROR_OUT_OF_MEMORY;
    if (result == WPE_RESULT_OK) {
        if (list.count > 1) qsort(list.paths, list.count, sizeof *list.paths, wpe__compare_paths);
        for (uint32_t i = 0; i < list.count; ++i) {
            if (i > 0 && strcmp(list.paths[i - 1], list.paths[i]) == 0) continue; /* Shadowed duplicate. */
            if (!fn(list.paths[i], user_data)) break;
        }
    }
    for (uint32_t i = 0; i < list.count; ++i)
        wpe_free(list.paths[i]);
    wpe_free(list.paths);
    return result;
}
