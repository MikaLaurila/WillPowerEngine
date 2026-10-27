#define WPE_LOG_MODULE "vfs"
#include "vfs/pak.h"

#define ZSTD_STATIC_LINKING_ONLY /* ZSTD_customMem: route allocations through wpe_alloc. */
#include <stdio.h>
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

/* Trailer of a file carrying an embedded archive: archive offset and size, then this magic. */
#define WPE__EMBED_TRAILER_SIZE 24u
static const unsigned char wpe__embed_magic[8] = {'W', 'P', 'E', '_', 'E', 'M', 'B', 'D'};

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
    uint64_t base; /* File offset of the archive (non-zero when embedded in another file). */
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

/*
 * Finds the archive inside an open file: the whole file when it starts with the archive magic,
 * otherwise the range named by an embed trailer at its end. Returns false if neither is present.
 */
static bool wpe__locate_archive(WPE_File* file, uint64_t file_size, uint64_t* out_base, uint64_t* out_size) {
    unsigned char magic[sizeof wpe__pak_magic];
    if (file_size < WPE__PAK_HEADER_SIZE || wpe_file_seek(file, 0) != WPE_RESULT_OK ||
        !wpe__read_exact(file, magic, sizeof magic))
        return false;
    if (memcmp(magic, wpe__pak_magic, sizeof magic) == 0) {
        *out_base = 0;
        *out_size = file_size;
        return true;
    }
    unsigned char trailer[WPE__EMBED_TRAILER_SIZE];
    if (file_size < WPE__PAK_HEADER_SIZE + WPE__EMBED_TRAILER_SIZE ||
        wpe_file_seek(file, file_size - WPE__EMBED_TRAILER_SIZE) != WPE_RESULT_OK ||
        !wpe__read_exact(file, trailer, sizeof trailer) ||
        memcmp(trailer + 16, wpe__embed_magic, sizeof wpe__embed_magic) != 0)
        return false;
    const uint64_t base = wpe__get_u64(trailer);
    const uint64_t size = wpe__get_u64(trailer + 8);
    const uint64_t available = file_size - WPE__EMBED_TRAILER_SIZE;
    if (base > available || size != available - base || size < WPE__PAK_HEADER_SIZE) return false;
    *out_base = base;
    *out_size = size;
    return true;
}

bool wpe_pak_probe(const char* os_path) {
    if (!os_path) return false;
    WPE_File* file = NULL;
    if (wpe_file_open(os_path, false, &file) != WPE_RESULT_OK) return false;
    uint64_t base = 0, size = 0;
    const uint64_t file_size = wpe_file_size(file);
    const bool found = file_size != UINT64_MAX && wpe__locate_archive(file, file_size, &base, &size);
    wpe_file_close(file);
    return found;
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
    uint64_t pak_size = 0;
    if (!wpe__locate_archive(file, file_size, &pak->base, &pak_size) ||
        wpe_file_seek(file, pak->base) != WPE_RESULT_OK || !wpe__read_exact(file, header, sizeof header) ||
        memcmp(header, wpe__pak_magic, sizeof wpe__pak_magic) != 0) {
        wpe_log_error("'%s' is not a .pak archive and carries none (or it was not finished)", os_path);
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
        index_offset < WPE__PAK_HEADER_SIZE || index_offset > pak_size ||
        pak_size - index_offset != index_size || wpe__get_u32(header + 36) != 0) {
        wpe_log_error("'%s': corrupt .pak header", os_path);
        result = WPE_RESULT_ERROR_PARSE;
        goto done;
    }
    index = wpe_alloc(index_size ? (size_t)index_size : 1);
    if (!index) {
        result = WPE_RESULT_ERROR_OUT_OF_MEMORY;
        goto done;
    }
    if (wpe_file_seek(file, pak->base + index_offset) != WPE_RESULT_OK ||
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
                              .offset = pak->base + e->offset,
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
    if (result == WPE_RESULT_OK && (wpe_file_seek(file, pak->base + e->offset) != WPE_RESULT_OK ||
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

/* ---- Embedding ----------------------------------------------------------------------------------- */

/* Appends the whole of `path` to `out`; returns the number of bytes copied via out_size. */
static WPE_Result wpe__append_file(WPE_File* out, const char* path, unsigned char* buffer, size_t buffer_size,
                                   uint64_t* out_size) {
    WPE_File* in = NULL;
    WPE_Result result = wpe_file_open(path, false, &in);
    if (result == WPE_RESULT_ERROR_NOT_FOUND) wpe_log_error("'%s' not found", path);
    if (result != WPE_RESULT_OK) return result;
    const uint64_t size = wpe_file_size(in);
    if (size == UINT64_MAX) result = WPE_RESULT_ERROR_IO;
    uint64_t copied = 0;
    while (result == WPE_RESULT_OK && copied < size) {
        const size_t chunk = size - copied < buffer_size ? (size_t)(size - copied) : buffer_size;
        if (wpe_file_read(in, buffer, chunk) != chunk) {
            wpe_log_error("cannot read '%s'", path);
            result = WPE_RESULT_ERROR_IO;
            break;
        }
        result = wpe_file_write(out, buffer, chunk);
        copied += chunk;
    }
    wpe_file_close(in);
    *out_size = copied;
    return result;
}

WPE_Result wpe_pak_embed(const char* host_path, const char* pak_path, const char* out_path) {
    if (!host_path || !pak_path || !out_path) return WPE_RESULT_ERROR_NULL_ARG;
    if (strcmp(out_path, host_path) == 0 || strcmp(out_path, pak_path) == 0) {
        wpe_log_error("the embedded output must be a new file");
        return WPE_RESULT_ERROR_INVALID_ARG;
    }
    if (wpe_pak_probe(host_path)) {
        wpe_log_error("'%s' already carries an archive; embed into the original executable", host_path);
        return WPE_RESULT_ERROR_INVALID_ARG;
    }
    /* Only a valid, stand-alone archive may be embedded (opening it verifies the header and index). */
    WPE_Pak* pak = NULL;
    WPE_Result result = wpe_pak_open(pak_path, &pak);
    if (result != WPE_RESULT_OK) return result;
    const bool standalone = pak->base == 0;
    wpe_pak_close(pak);
    if (!standalone) {
        wpe_log_error("'%s' is itself embedded in another file", pak_path);
        return WPE_RESULT_ERROR_INVALID_ARG;
    }

    enum { BUFFER_SIZE = 1 << 16 };
    unsigned char* buffer = wpe_alloc(BUFFER_SIZE);
    if (!buffer) return WPE_RESULT_ERROR_OUT_OF_MEMORY;
    WPE_File* out = NULL;
    result = wpe_file_open(out_path, true, &out);
    uint64_t host_size = 0, pak_size = 0;
    if (result == WPE_RESULT_OK) result = wpe__append_file(out, host_path, buffer, BUFFER_SIZE, &host_size);
    if (result == WPE_RESULT_OK) result = wpe__append_file(out, pak_path, buffer, BUFFER_SIZE, &pak_size);
    if (result == WPE_RESULT_OK) {
        unsigned char trailer[WPE__EMBED_TRAILER_SIZE];
        wpe__put_u64(trailer, host_size);
        wpe__put_u64(trailer + 8, pak_size);
        memcpy(trailer + 16, wpe__embed_magic, sizeof wpe__embed_magic);
        result = wpe_file_write(out, trailer, sizeof trailer);
    }
    const WPE_Result closed = out ? wpe_file_close(out) : WPE_RESULT_OK;
    if (result == WPE_RESULT_OK) result = closed;
    wpe_free(buffer);
    if (result != WPE_RESULT_OK && out) wpe_path_remove(out_path);
    return result;
}

/* ---- Packing a directory -------------------------------------------------------------------------- */

typedef struct wpe__PathList {
    char** paths;
    uint32_t count, capacity;
    uint32_t skipped;
    bool out_of_memory;
} wpe__PathList;

static bool wpe__collect_path(const char* path, void* user_data) {
    wpe__PathList* list = user_data;
    if (!wpe_vfs_path_is_valid(path)) {
        wpe_log_warn("skipping '%s': not a valid asset path", path);
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
    char* copy = wpe__strdup(path);
    if (!copy) {
        list->out_of_memory = true;
        return false;
    }
    list->paths[list->count++] = copy;
    return true;
}

static int wpe__compare_paths(const void* a, const void* b) {
    return strcmp(*(char* const*)a, *(char* const*)b);
}

WPE_Result wpe_pak_create_from_directory(const char* os_path, const char* directory, int compression_level,
                                         WPE_PakPackStats* out_stats) {
    if (!os_path || !directory) return WPE_RESULT_ERROR_NULL_ARG;
    if (out_stats) *out_stats = (WPE_PakPackStats){0};
    wpe__PathList list = {0};
    WPE_Result result = wpe_directory_walk(directory, wpe__collect_path, &list);
    if (result == WPE_RESULT_ERROR_NOT_FOUND) wpe_log_error("'%s' is not a directory", directory);
    if (list.out_of_memory) result = WPE_RESULT_ERROR_OUT_OF_MEMORY;
    /* Sorted input makes the archive reproducible. */
    if (list.count > 1) qsort(list.paths, list.count, sizeof *list.paths, wpe__compare_paths);

    WPE_PakWriter* writer = NULL;
    if (result == WPE_RESULT_OK) result = wpe_pak_writer_create(os_path, compression_level, &writer);
    uint64_t total = 0;
    for (uint32_t i = 0; i < list.count && result == WPE_RESULT_OK; ++i) {
        char full[1024];
        if (snprintf(full, sizeof full, "%s/%s", directory, list.paths[i]) >= (int)sizeof full) {
            wpe_log_error("path too long: %s", list.paths[i]);
            result = WPE_RESULT_ERROR_CAPACITY;
            break;
        }
        void* data = NULL;
        size_t size = 0;
        result = wpe_file_read_all(full, &data, &size);
        if (result == WPE_RESULT_OK)
            result = wpe_pak_writer_add(writer, list.paths[i], data, size, WPE_PAK_COMPRESS_AUTO);
        total += size;
        wpe_free(data);
    }
    if (result == WPE_RESULT_OK) result = wpe_pak_writer_finish(writer);
    wpe_pak_writer_destroy(writer);
    if (out_stats)
        *out_stats = (WPE_PakPackStats){.files = list.count, .skipped = list.skipped, .bytes = total};
    for (uint32_t i = 0; i < list.count; ++i)
        wpe_free(list.paths[i]);
    wpe_free(list.paths);
    return result;
}