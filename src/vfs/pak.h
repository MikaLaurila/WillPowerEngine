/**
 * @file pak.h
 * @brief `.pak` archives: a single file holding many asset files, each optionally zstd-compressed.
 *
 * Format version 1 (all integers little-endian; specified in docs/assets.md):
 *
 *     header   40 bytes   magic "WPE_PAK\0", version, entry count, index offset/size/CRC-32
 *     data     ...        file contents, stored or as one zstd frame each
 *     index    ...        one record per file, sorted by path (bytewise), paths unique
 *
 * Every file carries the CRC-32 of its uncompressed contents, verified by wpe_pak_read(). Paths
 * use the VFS path rules (see wpe_vfs_path_is_valid()).
 *
 * - Dependencies: core, platform (file I/O), zstd (private)
 * - Thread affinity: a WPE_Pak may be read from any number of threads at once (each read opens its
 *   own file stream); a WPE_PakWriter belongs to one thread
 *
 * @author WillPowerEngine Team
 */
#ifndef WPE_VFS_PAK_H
#define WPE_VFS_PAK_H

#include <stddef.h>
#include <stdint.h>

#include "core/result.h"

#define WPE_PAK_VERSION 1u
/** Maximum number of files in one archive. */
#define WPE_PAK_MAX_ENTRIES (1u << 20)

typedef struct WPE_Pak WPE_Pak;
typedef struct WPE_PakWriter WPE_PakWriter;

typedef struct WPE_PakEntryInfo {
    const char* path;     /* Borrowed from the archive (valid until wpe_pak_close()). */
    uint64_t size;        /* Uncompressed size. */
    uint64_t stored_size; /* Bytes in the archive. */
    uint64_t offset;      /* Archive byte offset of the stored data. */
    uint32_t crc32;       /* Of the uncompressed contents. */
    bool compressed;
} WPE_PakEntryInfo;

typedef enum WPE_PakCompression {
    WPE_PAK_COMPRESS_AUTO = 0, /* zstd when it saves at least 5 %, otherwise stored. */
    WPE_PAK_COMPRESS_NONE,     /* Always stored (seekable streaming without decompression). */
    WPE_PAK_COMPRESS_ALWAYS,   /* Always zstd. */
} WPE_PakCompression;

/**
 * Opens an archive and loads its index (file contents are read on demand).
 *
 * @return WPE_RESULT_OK; WPE_RESULT_ERROR_NULL_ARG; WPE_RESULT_ERROR_NOT_FOUND; WPE_RESULT_ERROR_IO;
 *         WPE_RESULT_ERROR_PARSE (not a pak, corrupt or inconsistent index; logged);
 *         WPE_RESULT_ERROR_UNSUPPORTED (newer format version; logged); WPE_RESULT_ERROR_OUT_OF_MEMORY.
 *
 * @thread_safety Any thread
 */
[[nodiscard]] WPE_Result wpe_pak_open(const char* os_path, WPE_Pak** out_pak);

/** Closes an archive (ownership consumed); NULL is a no-op. @thread_safety No reads may be in progress */
void wpe_pak_close(WPE_Pak* pak);

/** @return Number of files. @thread_safety Any thread */
uint32_t wpe_pak_entry_count(const WPE_Pak* pak);

/** @return Index of a file by exact path, or UINT32_MAX. O(log n). @thread_safety Any thread */
uint32_t wpe_pak_find(const WPE_Pak* pak, const char* path);

/** @return Information about entry `index` (< wpe_pak_entry_count()). @thread_safety Any thread */
WPE_PakEntryInfo wpe_pak_entry(const WPE_Pak* pak, uint32_t index);

/** @return The archive's OS path (borrowed). @thread_safety Any thread */
const char* wpe_pak_os_path(const WPE_Pak* pak);

/**
 * Reads and verifies a whole file.
 *
 * @param out_data Receives a wpe_alloc() buffer with a terminating NUL byte appended (not counted).
 * @return WPE_RESULT_OK; WPE_RESULT_ERROR_NULL_ARG; WPE_RESULT_ERROR_INVALID_ARG (bad index);
 *         WPE_RESULT_ERROR_IO; WPE_RESULT_ERROR_PARSE (decompression or CRC failure; logged);
 *         WPE_RESULT_ERROR_OUT_OF_MEMORY.
 *
 * @thread_safety Any thread
 */
WPE_Result wpe_pak_read(const WPE_Pak* pak, uint32_t index, void** out_data, size_t* out_size);

/**
 * Starts writing a new archive (an existing file is replaced when finished).
 *
 * @param compression_level zstd level 1..19 (0 = 9).
 * @return WPE_RESULT_OK, WPE_RESULT_ERROR_NULL_ARG, WPE_RESULT_ERROR_INVALID_ARG, WPE_RESULT_ERROR_IO or
 *         WPE_RESULT_ERROR_OUT_OF_MEMORY.
 *
 * @thread_safety Any thread
 */
[[nodiscard]] WPE_Result wpe_pak_writer_create(const char* os_path, int compression_level,
                                               WPE_PakWriter** out_writer);

/**
 * Appends a file.
 *
 * @param path Archive path (VFS path rules); must be unique within the archive (checked by
 *             wpe_pak_writer_finish()).
 * @return WPE_RESULT_OK; WPE_RESULT_ERROR_NULL_ARG; WPE_RESULT_ERROR_INVALID_ARG (bad path; logged);
 *         WPE_RESULT_ERROR_CAPACITY (WPE_PAK_MAX_ENTRIES reached); WPE_RESULT_ERROR_IO;
 *         WPE_RESULT_ERROR_OUT_OF_MEMORY. After an error the writer can only be destroyed.
 *
 * @thread_safety Owning thread
 */
WPE_Result wpe_pak_writer_add(WPE_PakWriter* writer, const char* path, const void* data, size_t size,
                              WPE_PakCompression compression);

/**
 * Writes the index and closes the archive. Without a successful finish the partial file is left
 * behind with an invalid header (it fails to open).
 *
 * @return WPE_RESULT_OK, WPE_RESULT_ERROR_NULL_ARG, WPE_RESULT_ERROR_INVALID_STATE (an earlier add
 *         failed, or already finished), WPE_RESULT_ERROR_ALREADY_EXISTS (two files share a path;
 *         logged), WPE_RESULT_ERROR_IO or WPE_RESULT_ERROR_OUT_OF_MEMORY.
 *
 * @thread_safety Owning thread
 */
WPE_Result wpe_pak_writer_finish(WPE_PakWriter* writer);

/** Frees a writer (ownership consumed; closes the file if not finished); NULL is a no-op. @thread_safety
 * Owning thread */
void wpe_pak_writer_destroy(WPE_PakWriter* writer);

#endif /* WPE_VFS_PAK_H */
