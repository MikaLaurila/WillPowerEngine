/**
 * @file api_docs.h
 * @brief Help for the `wpe` scripting API, read from the reference tables of docs/scripting.md (embedded in
 *        the editor at build time): each function's signature, its description and its section.
 *
 * A table row whose first cell holds `wpe.name(...)` (one or more, in backticks) documents those functions;
 * the second cell is the description. Markdown is reduced to plain text.
 *
 * - Dependencies: core (allocation)
 * - Thread affinity: any thread (a parsed set is read-only)
 *
 * @author WillPowerEngine Team
 */
#ifndef WPE_EDITOR_API_DOCS_H
#define WPE_EDITOR_API_DOCS_H

#include <stddef.h>
#include <stdint.h>

typedef struct WPE_ApiDoc {
    const char* name;      /* "tween" for wpe.tween. */
    const char* signature; /* "wpe.tween(entity, {position, rotation, scale, ...options}) -> id" */
    const char* summary;   /* The description, as plain text. */
    const char* section;   /* The heading it is under ("Tweens"). */
} WPE_ApiDoc;

typedef struct WPE_ApiDocs {
    WPE_ApiDoc* docs; /* Sorted by name. */
    uint32_t count;
    char* strings; /* Where the texts live. */
} WPE_ApiDocs;

/** Reads the reference tables of a Markdown document. @return false out of memory (the set is then empty).
 * Free with wpe_api_docs_free(). */
bool wpe_api_docs_parse(const char* markdown, size_t length, WPE_ApiDocs* out);

/** Parses the editor's embedded copy of docs/scripting.md. Free with wpe_api_docs_free(). */
bool wpe_api_docs_parse_builtin(WPE_ApiDocs* out);

void wpe_api_docs_free(WPE_ApiDocs* docs);

/** @return The entry for `wpe.<name>`, or NULL. `length` is the name's length (names need not be terminated).
 */
const WPE_ApiDoc* wpe_api_docs_find(const WPE_ApiDocs* docs, const char* name, size_t length);

#endif /* WPE_EDITOR_API_DOCS_H */
