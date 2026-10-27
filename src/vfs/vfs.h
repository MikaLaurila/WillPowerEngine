/**
 * @file vfs.h
 * @brief Virtual file system: one namespace of asset paths over directories and `.pak` archives.
 *
 * Asset paths are relative, '/'-separated UTF-8 ("meshes/helmet.glb"). Mounts are searched from
 * the most recently mounted to the first, so a later mount (a patch archive, or a development
 * directory over a shipped archive) overrides files of earlier ones.
 *
 * Paths are case-sensitive inside archives; inside mounted directories they follow the host file
 * system (case-insensitive on Windows). Ship lowercase paths to avoid surprises.
 *
 * - Dependencies: core, platform (file I/O), vfs/pak
 * - Thread affinity: mount/unmount/destroy on the owning thread while no reads are in progress;
 *   all read functions may run concurrently from any thread (each opens its own stream)
 *
 * @author WillPowerEngine Team
 */
#ifndef WPE_VFS_VFS_H
#define WPE_VFS_VFS_H

#include <stddef.h>
#include <stdint.h>

#include "core/result.h"
#include "vfs/pak.h"

/** Maximum asset path length in bytes, including the terminator. */
#define WPE_VFS_PATH_MAX 256u

typedef struct WPE_Vfs WPE_Vfs;
typedef struct WPE_VfsFile WPE_VfsFile;

/**
 * Called once per unique file by wpe_vfs_list().
 *
 * @param path Asset path (borrowed for the duration of the call).
 * @return false to stop listing.
 */
typedef bool (*WPE_VfsListFn)(const char* path, void* user_data);

/**
 * Checks an asset path: non-empty, shorter than WPE_VFS_PATH_MAX, relative, '/'-separated, with no
 * empty, "." or ".." segments, backslashes, colons or control characters.
 *
 * @thread_safety Any thread
 */
bool wpe_vfs_path_is_valid(const char* path);

/**
 * Normalizes a relative path into an asset path: '\\' becomes '/', empty and "." segments are
 * dropped and ".." removes the preceding segment ("meshes/../textures//a.png" -> "textures/a.png").
 *
 * @param out Receives the result; at least WPE_VFS_PATH_MAX bytes.
 * @return false if the path is absolute, would climb above the root, or the result is not a valid
 *         asset path (out is then empty).
 *
 * @thread_safety Any thread
 */
bool wpe_vfs_path_normalize(const char* path, char* out, size_t capacity);

/** @return WPE_RESULT_OK, WPE_RESULT_ERROR_NULL_ARG or WPE_RESULT_ERROR_OUT_OF_MEMORY. @thread_safety Any
 * thread */
[[nodiscard]] WPE_Result wpe_vfs_create(WPE_Vfs** out_vfs);

/** Unmounts everything and frees the VFS (ownership consumed); NULL is a no-op. @thread_safety Owning thread
 */
void wpe_vfs_destroy(WPE_Vfs* vfs);

/**
 * Mounts an OS directory (its files appear at paths relative to it).
 *
 * @return WPE_RESULT_OK, WPE_RESULT_ERROR_NULL_ARG, WPE_RESULT_ERROR_NOT_FOUND (not a directory;
 *         logged) or WPE_RESULT_ERROR_OUT_OF_MEMORY.
 *
 * @thread_safety Owning thread
 */
WPE_Result wpe_vfs_mount_directory(WPE_Vfs* vfs, const char* os_directory);

/**
 * Mounts a `.pak` archive (its index is loaded now).
 *
 * @return WPE_RESULT_OK, WPE_RESULT_ERROR_NULL_ARG or any wpe_pak_open() error.
 *
 * @thread_safety Owning thread
 */
WPE_Result wpe_vfs_mount_pak(WPE_Vfs* vfs, const char* os_path);

/** @return Number of mounts. @thread_safety Owning thread */
uint32_t wpe_vfs_mount_count(const WPE_Vfs* vfs);

/** @return Whether a file exists at `path` in any mount. @thread_safety Any thread */
bool wpe_vfs_exists(const WPE_Vfs* vfs, const char* path);

/**
 * Reads a whole file from the highest-priority mount that has it.
 *
 * @param out_data Receives a wpe_alloc() buffer owned by the caller, with a terminating NUL byte
 *                 appended (not counted in out_size).
 * @return WPE_RESULT_OK; WPE_RESULT_ERROR_NULL_ARG; WPE_RESULT_ERROR_INVALID_ARG (invalid path;
 *         logged); WPE_RESULT_ERROR_NOT_FOUND (not logged); WPE_RESULT_ERROR_IO;
 *         WPE_RESULT_ERROR_PARSE (corrupt archive data); WPE_RESULT_ERROR_OUT_OF_MEMORY.
 *
 * @thread_safety Any thread
 */
WPE_Result wpe_vfs_read_all(const WPE_Vfs* vfs, const char* path, void** out_data, size_t* out_size);

/**
 * Modification time of a file for change detection. Files inside archives report the archive's
 * modification time at mount, so they never appear to change.
 *
 * @return WPE_RESULT_OK, WPE_RESULT_ERROR_NULL_ARG or WPE_RESULT_ERROR_NOT_FOUND (not logged).
 *
 * @thread_safety Any thread
 */
WPE_Result wpe_vfs_modified_time(const WPE_Vfs* vfs, const char* path, int64_t* out_time);

/**
 * Opens a file for streaming. Files stored uncompressed in archives are streamed from the archive;
 * compressed ones are decompressed into memory when opened.
 *
 * @param out_file Receives the stream; close with wpe_vfs_file_close().
 * @return Same codes as wpe_vfs_read_all().
 *
 * @thread_safety Any thread; one stream must not be used by two threads at once
 */
[[nodiscard]] WPE_Result wpe_vfs_open(const WPE_Vfs* vfs, const char* path, WPE_VfsFile** out_file);

/** Closes a stream (ownership consumed); NULL is a no-op. @thread_safety See wpe_vfs_open() */
void wpe_vfs_file_close(WPE_VfsFile* file);

/** @return Bytes read (less than `size` only at end of file or on error). @thread_safety See wpe_vfs_open()
 */
size_t wpe_vfs_file_read(WPE_VfsFile* file, void* buffer, size_t size);

/** Seeks to an absolute offset (<= size). @return WPE_RESULT_OK, WPE_RESULT_ERROR_NULL_ARG,
 * WPE_RESULT_ERROR_INVALID_ARG or WPE_RESULT_ERROR_IO. @thread_safety See wpe_vfs_open() */
WPE_Result wpe_vfs_file_seek(WPE_VfsFile* file, uint64_t offset);

/** @return Current offset. @thread_safety See wpe_vfs_open() */
uint64_t wpe_vfs_file_tell(const WPE_VfsFile* file);

/** @return File size in bytes. @thread_safety See wpe_vfs_open() */
uint64_t wpe_vfs_file_size(const WPE_VfsFile* file);

/**
 * Lists every file visible through the VFS once, in ascending path order (files in mounted
 * directories with invalid asset paths are skipped).
 *
 * @return WPE_RESULT_OK (also when the callback stopped), WPE_RESULT_ERROR_NULL_ARG,
 *         WPE_RESULT_ERROR_IO or WPE_RESULT_ERROR_OUT_OF_MEMORY.
 *
 * @thread_safety Any thread
 */
WPE_Result wpe_vfs_list(const WPE_Vfs* vfs, WPE_VfsListFn fn, void* user_data);

#endif /* WPE_VFS_VFS_H */
