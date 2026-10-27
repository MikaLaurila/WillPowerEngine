/**
 * @file export.h
 * @brief Game export: packages a project directory and the runtime executable into one
 *        distributable game executable (the runtime with the game's archive embedded).
 *
 * The exported file runs on the platform the runtime was built for and needs nothing else: on
 * start the runtime finds the archive appended to its own executable (wpe_pak_probe()) and plays
 * the game. Export validates the project first (project.json parses, its start scene exists) so a
 * broken project is not shipped.
 *
 * - Dependencies: vfs (archives), engine/project, platform
 * - Thread affinity: any thread
 *
 * @author WillPowerEngine Team
 */
#ifndef WPE_ENGINE_EXPORT_H
#define WPE_ENGINE_EXPORT_H

#include <stdint.h>

#include "core/result.h"

typedef struct WPE_ExportDesc {
    const char* project_dir;  /* Directory holding project.json and every asset of the game. */
    const char* runtime_path; /* The wpe_runtime executable to package. */
    const char* output_path;  /* Game executable to write (replaced if it exists). */
    int compression_level;    /* zstd level 1..19 (0 = default). */
} WPE_ExportDesc;

typedef struct WPE_ExportStats {
    uint32_t files;        /* Files packed. */
    uint64_t asset_bytes;  /* Their uncompressed size. */
    uint64_t output_bytes; /* Size of the exported executable. */
} WPE_ExportStats;

/**
 * Exports a game (see the file comment). A temporary archive "<output_path>.pak.tmp" is created
 * and removed again.
 *
 * @param out_stats Optional.
 * @return WPE_RESULT_OK; WPE_RESULT_ERROR_NULL_ARG; WPE_RESULT_ERROR_NOT_FOUND (project directory,
 *         project.json, start scene or runtime missing; logged); WPE_RESULT_ERROR_PARSE (invalid
 *         project.json; logged); WPE_RESULT_ERROR_INVALID_ARG (the runtime already carries a game, or
 *         the output lies inside the project directory; logged);
 *         archive or I/O errors. On failure no output file is left behind.
 *
 * @thread_safety Any thread
 */
WPE_Result wpe_export_game(const WPE_ExportDesc* desc, WPE_ExportStats* out_stats);

#endif /* WPE_ENGINE_EXPORT_H */
