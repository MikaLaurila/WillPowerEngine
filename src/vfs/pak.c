#define WPE_LOG_MODULE "vfs"
#include "vfs/pak.h"

#define ZSTD_STATIC_LINKING_ONLY /* ZSTD_customMem: route allocations through wpe_alloc. */
#include <stdlib.h>
#include <string.h>
#include <zstd.h>

#include "core/alloc.h"
#include "core/crc32.h"
#include "core/debug.h"
#include "core/log.h"
#include "platform/platform.h"
#include "vfs/vfs.h"

#define WPE__PAK_HEADER_SIZE 40u
#define WPE__PAK_RECORD_SIZE 36u /* Fixed part of an index record; the path follows. */
#define WPE__PAK_FLAG_ZSTD 0x1u
#define WPE__PAK_MAX_INDEX_SIZE (256ull << 20)

static const unsigned char wpe__pak_magic[8] = {'W', 'P', 'E', '_', 'P', 'A', 'K', '\0'};

typedef struct wpe__PakEntry {
    uint64_t offset;
    uint64_t stored_size;
    uint64_t size;
    uint32_t crc32;
    uint32_t path; /* Offset into the path pool. */
    bool compressed;
} wpe__PakEntry;

struct WPE_Pak {
    char* os_path;
    wpe__PakEntry* entries;
    uint32_t entry_count;
    char* paths; /* NUL-terminated paths, back to back. */
};

/* ---- Little-endian encoding ---------------------------------------------------------------------- */

static void wpe__put_u16(unsigned char* p, uint16_t v) {
    p[0] = (unsigned char)v;
    p[1] = (unsigned char)(v >> 8);
}

static void wpe__put_u32(unsigned char* p, uint32_t v) {
    for (int i = 0; i < 4; ++i)
        p[i] = (unsigned char)(v >> (8 * i));
}

static void wpe__put_u64(unsigned char* p, uint64_t v) {
    for (int i = 0; i < 8; ++i)
        p[i] = (unsigned char)(v >> (8 * i));
}

static uint16_t wpe__get_u16(const unsigned char* p) {
    return (uint16_t)(p[0] | (p[1] << 8));
}

static uint32_t wpe__get_u32(const unsigned char* p) {
    uint32_t v = 0;
    for (int i = 3; i >= 0; --i)
        v = (v << 8) | p[i];
    return v;
}

static uint64_t wpe__get_u64(const unsigned char* p) {
    uint64_t v = 0;
    for (int i = 7; i >= 0; --i)
        v = (v << 8) | p[i];
    return v;
}

/* ---- zstd allocation hooks ----------------------------------------------------------------------- */

static void* wpe__zstd_alloc(void* opaque, size_t size) {
    (void)opaque;
    return wpe_alloc(size);
}

static void wpe__zstd_free(void* opaque, void* address) {
    (void)opaque;
    wpe_free(address);
}

static const ZSTD_customMem wpe__zstd_memory = {wpe__zstd_alloc, wpe__zstd_free, NULL};

/* ---- Reading ------------------------------------------------------------------------------------- */

static char* wpe__strdup(const char* text) {
    const size_t length = strlen(text);
    char* copy = wpe_alloc(length + 1);
    if (copy) memcpy(copy, text, length + 1);
    return copy;
}

static bool wpe__read_exact(WPE_File* file, void* buffer, size_t size) {
    return wpe_file_read(file, buffer, size) == size;
}

/* Parses and validates the index; the pak's entries/paths are filled on success. */
static WPE_Result wpe__parse_index(WPE_Pak* pak, const unsigned char* index, uint64_t index_size,
                                   uint64_t data_end) {
    wpe__PakEntry* entries = wpe_alloc_zeroed(pak->entry_count ? pak->entry_count : 1, sizeof *entries);
    /* Path bytes plus terminators can never exceed the index size. */
    char* paths = wpe_alloc((size_t)index_size + 1);
    if (!entries || !paths) {
        wpe_free(entries);
        wpe_free(paths);
        return WPE_RESULT_ERROR_OUT_OF_MEMORY;
    }
    uint64_t at = 0;
    size_t pool = 0;
    const char* previous = NULL;
    for (uint32_t i = 0; i < pak->entry_count; ++i) {
        if (index_size - at < WPE__PAK_RECORD_SIZE) goto corrupt;
        const unsigned char* record = index + at;
        wpe__PakEntry* e = &entries[i];
        e->offset = wpe__get_u64(record);
        e->stored_size = wpe__get_u64(record + 8);
        e->size = wpe__get_u64(record + 16);
        e->crc32 = wpe__get_u32(record + 24);
        const uint16_t flags = wpe__get_u16(record + 28);
        const uint16_t path_length = wpe__get_u16(record + 30);
        /* Bytes 32..35 are reserved (zero). */
        at += WPE__PAK_RECORD_SIZE;
        if ((flags & ~WPE__PAK_FLAG_ZSTD) != 0 || wpe__get_u32(record + 32) != 0) goto corrupt;
        e->compressed = (flags & WPE__PAK_FLAG_ZSTD) != 0;
        if (path_length == 0 || path_length >= WPE_VFS_PATH_MAX || index_size - at < path_length)
            goto corrupt;
        if (e->offset < WPE__PAK_HEADER_SIZE || e->offset > data_end || e->stored_size > data_end - e->offset)
            goto corrupt;
        if (e->size >= SIZE_MAX || (!e->compressed && e->stored_size != e->size)) goto corrupt;

        char* path = paths + pool;
        memcpy(path, index + at, path_length);
        path[path_length] = '\0';
        at += path_length;
        if (strlen(path) != path_length || !wpe_vfs_path_is_valid(path)) goto corrupt;
        if (previous && strcmp(previous, path) >= 0) goto corrupt; /* Sorted and unique. */
        previous = path;
        e->path = (uint32_t)pool;
        pool += (size_t)path_length + 1;
    }
    if (at != index_size) goto corrupt;
    pak->entries = entries;
    pak->paths = paths;
    return WPE_RESULT_OK;

corrupt:
    wpe_free(entries);
    wpe_free(paths);
    return WPE_RESULT_ERROR_PARSE;
}

WPE_Result wpe_pak_open(const char* os_path, WPE_Pak** out_pak) {
    if (!os_path || !out_pak) return WPE_RESULT_ERROR_NULL_ARG;
    *out_pak = NULL;
    WPE_File* file = NULL;
    WPE_Result result = wpe_file_open(os_path, false, &file);
    if (result != WPE_RESULT_OK) return result;

    WPE_Pak* pak = wpe_alloc_zeroed(1, sizeof *pak);
    unsigned char* index = NULL;
    if (!pak) {
        result = WPE_RESULT_ERROR_OUT_OF_MEMORY;
        goto done;
    }
    unsigned char header[WPE__PAK_HEADER_SIZE];
    const uint64_t file_size = wpe_file_size(file);
    if (file_size == UINT64_MAX) {
        result = WPE_RESULT_ERROR_IO;
        goto done;
    }
    if (file_size < WPE__PAK_HEADER_SIZE || !wpe__read_exact(file, header, sizeof header) ||
        memcmp(header, wpe__pak_magic, sizeof wpe__pak_magic) != 0) {
        wpe_log_error("'%s' is not a .pak archive (or was not finished)", os_path);
        result = WPE_RESULT_ERROR_PARSE;
        goto done;
    }
    const uint32_t version = wpe__get_u32(header + 8);
    if (version != WPE_PAK_VERSION) {
        wpe_log_error("'%s' has .pak format version %u; this engine reads version %u", os_path, version,
                      WPE_PAK_VERSION);
        result = WPE_RESULT_ERROR_UNSUPPORTED;
        goto done;
    }
    pak->entry_count = wpe__get_u32(header + 12);
    const uint64_t index_offset = wpe__get_u64(header + 16);
    const uint64_t index_size = wpe__get_u64(header + 24);
    const uint32_t index_crc = wpe__get_u32(header + 32);
    /* The index is the last thing in the file. */
    if (pak->entry_count > WPE_PAK_MAX_ENTRIES || index_size > WPE__PAK_MAX_INDEX_SIZE ||
        index_offset < WPE__PAK_HEADER_SIZE || index_offset > file_size ||
        file_size - index_offset != index_size || wpe__get_u32(header + 36) != 0) {
        wpe_log_error("'%s': corrupt .pak header", os_path);
        result = WPE_RESULT_ERROR_PARSE;
        goto done;
    }
    index = wpe_alloc(index_size ? (size_t)index_size : 1);
    if (!index) {
        result = WPE_RESULT_ERROR_OUT_OF_MEMORY;
        goto done;
    }
    if (wpe_file_seek(file, index_offset) != WPE_RESULT_OK ||
        !wpe__read_exact(file, index, (size_t)index_size)) {
        wpe_log_error("'%s': cannot read the .pak index", os_path);
        result = WPE_RESULT_ERROR_IO;
        goto done;
    }
    if (wpe_crc32(0, index, (size_t)index_size) != index_crc) {
        wpe_log_error("'%s': .pak index checksum mismatch", os_path);
        result = WPE_RESULT_ERROR_PARSE;
        goto done;
    }
    result = wpe__parse_index(pak, index, index_size, index_offset);
    if (result == WPE_RESULT_ERROR_PARSE) wpe_log_error("'%s': corrupt .pak index", os_path);
    if (result == WPE_RESULT_OK) {
        pak->os_path = wpe__strdup(os_path);
        if (!pak->os_path) result = WPE_RESULT_ERROR_OUT_OF_MEMORY;
    }

done:
    wpe_free(index);
    wpe_file_close(file);
    if (result != WPE_RESULT_OK) {
        wpe_pak_close(pak);
        return result;
    }
    *out_pak = pak;
    return WPE_RESULT_OK;
}

void wpe_pak_close(WPE_Pak* pak) {
    if (!pak) return;
    wpe_free(pak->os_path);
    wpe_free(pak->entries);
    wpe_free(pak->paths);
    wpe_free(pak);
}

uint32_t wpe_pak_entry_count(const WPE_Pak* pak) {
    return pak ? pak->entry_count : 0;
}

uint32_t wpe_pak_find(const WPE_Pak* pak, const char* path) {
    if (!pak || !path) return UINT32_MAX;
    uint32_t low = 0, high = pak->entry_count;
    while (low < high) {
        const uint32_t mid = low + (high - low) / 2;
        const int order = strcmp(pak->paths + pak->entries[mid].path, path);
        if (order == 0) return mid;
        if (order < 0) {
            low = mid + 1;
        } else {
            high = mid;
        }
    }
    return UINT32_MAX;
}

WPE_PakEntryInfo wpe_pak_entry(const WPE_Pak* pak, uint32_t index) {
    if (!wpe_check(pak != NULL && index < pak->entry_count)) return (WPE_PakEntryInfo){0};
    const wpe__PakEntry* e = &pak->entries[index];
    return (WPE_PakEntryInfo){.path = pak->paths + e->path,
                              .size = e->size,
                              .stored_size = e->stored_size,
                              .offset = e->offset,
                              .crc32 = e->crc32,
                              .compressed = e->compressed};
}

const char* wpe_pak_os_path(const WPE_Pak* pak) {
    return pak ? pak->os_path : NULL;
}

/* Decompresses one zstd frame of exactly `size` bytes into `out`. */
static bool wpe__decompress(const void* stored, size_t stored_size, void* out, size_t size) {
    if (ZSTD_findFrameCompressedSize(stored, stored_size) != stored_size) return false;
    if (ZSTD_getFrameContentSize(stored, stored_size) != (unsigned long long)size) return false;
    ZSTD_DCtx* context = ZSTD_createDCtx_advanced(wpe__zstd_memory);
    if (!context) return false;
    const size_t written = ZSTD_decompressDCtx(context, out, size, stored, stored_size);
    ZSTD_freeDCtx(context);
    return !ZSTD_isError(written) && written == size;
}

WPE_Result wpe_pak_read(const WPE_Pak* pak, uint32_t index, void** out_data, size_t* out_size) {
    if (!pak || !out_data || !out_size) return WPE_RESULT_ERROR_NULL_ARG;
    *out_data = NULL;
    *out_size = 0;
    if (index >= pak->entry_count) return WPE_RESULT_ERROR_INVALID_ARG;
    const wpe__PakEntry* e = &pak->entries[index];
    const char* path = pak->paths + e->path;

    unsigned char* data = wpe_alloc((size_t)e->size + 1);
    unsigned char* stored = e->compressed ? wpe_alloc(e->stored_size ? (size_t)e->stored_size : 1) : data;
    WPE_File* file = NULL;
    WPE_Result result =
        (!data || !stored) ? WPE_RESULT_ERROR_OUT_OF_MEMORY : wpe_file_open(pak->os_path, false, &file);
    if (result == WPE_RESULT_OK && (wpe_file_seek(file, e->offset) != WPE_RESULT_OK ||
                                    !wpe__read_exact(file, stored, (size_t)e->stored_size))) {
        wpe_log_error("'%s': cannot read '%s'", pak->os_path, path);
        result = WPE_RESULT_ERROR_IO;
    }
    wpe_file_close(file);
    if (result == WPE_RESULT_OK && e->compressed &&
        !wpe__decompress(stored, (size_t)e->stored_size, data, (size_t)e->size)) {
        wpe_log_error("'%s': '%s' fails to decompress", pak->os_path, path);
        result = WPE_RESULT_ERROR_PARSE;
    }
    if (result == WPE_RESULT_OK && wpe_crc32(0, data, (size_t)e->size) != e->crc32) {
        wpe_log_error("'%s': '%s' fails its checksum", pak->os_path, path);
        result = WPE_RESULT_ERROR_PARSE;
    }
    if (e->compressed) wpe_free(stored);
    if (result != WPE_RESULT_OK) {
        wpe_free(data);
        return result;
    }
    data[e->size] = '\0';
    *out_data = data;
    *out_size = (size_t)e->size;
    return WPE_RESULT_OK;
}

/* ---- Writing ------------------------------------------------------------------------------------- */

typedef struct wpe__PakRecord {
    char* path;
    uint64_t offset;
    uint64_t stored_size;
    uint64_t size;
    uint32_t crc32;
    bool compressed;
} wpe__PakRecord;

struct WPE_PakWriter {
    WPE_File* file;
    ZSTD_CCtx* zstd;
    int level;
    uint64_t offset; /* Where the next file's data goes. */
    wpe__PakRecord* records;
    uint32_t count, capacity;
    bool failed, finished;
};

WPE_Result wpe_pak_writer_create(const char* os_path, int compression_level, WPE_PakWriter** out_writer) {
    if (!os_path || !out_writer) return WPE_RESULT_ERROR_NULL_ARG;
    *out_writer = NULL;
    if (compression_level < 0 || compression_level > 19) return WPE_RESULT_ERROR_INVALID_ARG;
    WPE_PakWriter* writer = wpe_alloc_zeroed(1, sizeof *writer);
    if (!writer) return WPE_RESULT_ERROR_OUT_OF_MEMORY;
    writer->level = compression_level ? compression_level : 9;
    writer->offset = WPE__PAK_HEADER_SIZE;
    writer->zstd = ZSTD_createCCtx_advanced(wpe__zstd_memory);
    WPE_Result result =
        writer->zstd ? wpe_file_open(os_path, true, &writer->file) : WPE_RESULT_ERROR_OUT_OF_MEMORY;
    /* A zeroed header marks the archive invalid until finish writes the real one. */
    static const unsigned char placeholder[WPE__PAK_HEADER_SIZE] = {0};
    if (result == WPE_RESULT_OK) result = wpe_file_write(writer->file, placeholder, sizeof placeholder);
    if (result != WPE_RESULT_OK) {
        wpe_pak_writer_destroy(writer);
        return result;
    }
    *out_writer = writer;
    return WPE_RESULT_OK;
}

WPE_Result wpe_pak_writer_add(WPE_PakWriter* writer, const char* path, const void* data, size_t size,
                              WPE_PakCompression compression) {
    if (!writer || !path || (!data && size > 0)) return WPE_RESULT_ERROR_NULL_ARG;
    if (writer->failed || writer->finished) return WPE_RESULT_ERROR_INVALID_STATE;
    if (!wpe_vfs_path_is_valid(path)) {
        wpe_log_error("'%s' is not a valid archive path", path);
        return WPE_RESULT_ERROR_INVALID_ARG;
    }
    if (writer->count == WPE_PAK_MAX_ENTRIES) {
        writer->failed = true;
        return WPE_RESULT_ERROR_CAPACITY;
    }
    if (writer->count == writer->capacity) {
        const uint32_t capacity = writer->capacity ? writer->capacity * 2 : 64;
        wpe__PakRecord* records = wpe_realloc(writer->records, (size_t)capacity * sizeof *records);
        if (!records) {
            writer->failed = true;
            return WPE_RESULT_ERROR_OUT_OF_MEMORY;
        }
        writer->records = records;
        writer->capacity = capacity;
    }

    wpe__PakRecord record = {.offset = writer->offset, .size = size, .crc32 = wpe_crc32(0, data, size)};
    void* packed = NULL;
    size_t packed_size = 0;
    if (compression != WPE_PAK_COMPRESS_NONE && size > 0) {
        const size_t bound = ZSTD_compressBound(size);
        packed = wpe_alloc(bound);
        if (!packed) {
            writer->failed = true;
            return WPE_RESULT_ERROR_OUT_OF_MEMORY;
        }
        ZSTD_CCtx_reset(writer->zstd, ZSTD_reset_session_and_parameters);
        ZSTD_CCtx_setParameter(writer->zstd, ZSTD_c_compressionLevel, writer->level);
        ZSTD_CCtx_setParameter(writer->zstd, ZSTD_c_contentSizeFlag, 1);
        packed_size = ZSTD_compress2(writer->zstd, packed, bound, data, size);
        if (ZSTD_isError(packed_size)) {
            wpe_log_error("zstd failed on '%s': %s", path, ZSTD_getErrorName(packed_size));
            wpe_free(packed);
            writer->failed = true;
            return WPE_RESULT_ERROR_IO;
        }
        record.compressed = compression == WPE_PAK_COMPRESS_ALWAYS || packed_size < size - size / 20;
    }
    record.stored_size = record.compressed ? packed_size : size;
    record.path = wpe__strdup(path);
    WPE_Result result = record.path ? WPE_RESULT_OK : WPE_RESULT_ERROR_OUT_OF_MEMORY;
    if (result == WPE_RESULT_OK)
        result = wpe_file_write(writer->file, record.compressed ? packed : data, (size_t)record.stored_size);
    wpe_free(packed);
    if (result != WPE_RESULT_OK) {
        wpe_free(record.path);
        writer->failed = true;
        return result;
    }
    writer->offset += record.stored_size;
    writer->records[writer->count++] = record;
    return WPE_RESULT_OK;
}

static int wpe__compare_records(const void* a, const void* b) {
    return strcmp(((const wpe__PakRecord*)a)->path, ((const wpe__PakRecord*)b)->path);
}

WPE_Result wpe_pak_writer_finish(WPE_PakWriter* writer) {
    if (!writer) return WPE_RESULT_ERROR_NULL_ARG;
    if (writer->failed || writer->finished) return WPE_RESULT_ERROR_INVALID_STATE;
    writer->finished = true;
    if (writer->count > 1)
        qsort(writer->records, writer->count, sizeof *writer->records, wpe__compare_records);

    size_t index_size = 0;
    for (uint32_t i = 0; i < writer->count; ++i) {
        if (i > 0 && strcmp(writer->records[i - 1].path, writer->records[i].path) == 0) {
            wpe_log_error("duplicate archive path '%s'", writer->records[i].path);
            return WPE_RESULT_ERROR_ALREADY_EXISTS;
        }
        index_size += WPE__PAK_RECORD_SIZE + strlen(writer->records[i].path);
    }
    unsigned char* index = wpe_alloc(index_size ? index_size : 1);
    if (!index) return WPE_RESULT_ERROR_OUT_OF_MEMORY;
    unsigned char* at = index;
    for (uint32_t i = 0; i < writer->count; ++i) {
        const wpe__PakRecord* r = &writer->records[i];
        const size_t path_length = strlen(r->path);
        wpe__put_u64(at, r->offset);
        wpe__put_u64(at + 8, r->stored_size);
        wpe__put_u64(at + 16, r->size);
        wpe__put_u32(at + 24, r->crc32);
        wpe__put_u16(at + 28, r->compressed ? WPE__PAK_FLAG_ZSTD : 0);
        wpe__put_u16(at + 30, (uint16_t)path_length);
        wpe__put_u32(at + 32, 0);
        memcpy(at + WPE__PAK_RECORD_SIZE, r->path, path_length);
        at += WPE__PAK_RECORD_SIZE + path_length;
    }

    unsigned char header[WPE__PAK_HEADER_SIZE] = {0};
    memcpy(header, wpe__pak_magic, sizeof wpe__pak_magic);
    wpe__put_u32(header + 8, WPE_PAK_VERSION);
    wpe__put_u32(header + 12, writer->count);
    wpe__put_u64(header + 16, writer->offset);
    wpe__put_u64(header + 24, index_size);
    wpe__put_u32(header + 32, wpe_crc32(0, index, index_size));

    WPE_Result result = wpe_file_write(writer->file, index, index_size);
    wpe_free(index);
    if (result == WPE_RESULT_OK) result = wpe_file_seek(writer->file, 0);
    if (result == WPE_RESULT_OK) result = wpe_file_write(writer->file, header, sizeof header);
    const WPE_Result closed = wpe_file_close(writer->file);
    writer->file = NULL;
    return result != WPE_RESULT_OK ? result : closed;
}

void wpe_pak_writer_destroy(WPE_PakWriter* writer) {
    if (!writer) return;
    wpe_file_close(writer->file);
    for (uint32_t i = 0; i < writer->count; ++i)
        wpe_free(writer->records[i].path);
    wpe_free(writer->records);
    ZSTD_freeCCtx(writer->zstd);
    wpe_free(writer);
}
