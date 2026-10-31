#ifndef WPE_EDITOR_CODE_SYMBOLS_H
#define WPE_EDITOR_CODE_SYMBOLS_H

/**
 * @file code_symbols.h
 * @brief The names a Luau script can use at a point, for the Script panel's completion: the locals in scope
 *        there, a table's fields and methods, a standard library's functions. A light scan of the tokens
 *        (scopes follow the block keywords), not a parser, so it also works on code that doesn't compile yet.
 *
 * Dependencies: core (allocation). No UI.
 * Thread affinity: any thread; the functions share no state.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define WPE_CODE_SYMBOL_NAME_MAX 48u
#define WPE_CODE_SYMBOL_PATH_MAX 128u

typedef enum WPE_CodeSymbolKind {
    WPE_CODE_SYMBOL_LOCAL = 0, /* A local variable. */
    WPE_CODE_SYMBOL_PARAMETER,
    WPE_CODE_SYMBOL_FUNCTION, /* A local, global or table function: completion adds "(". */
    WPE_CODE_SYMBOL_METHOD,   /* function T:name(...) */
    WPE_CODE_SYMBOL_FIELD,    /* A table field, or a declared script property. */
    WPE_CODE_SYMBOL_CONSTANT, /* math.pi, ... */
    WPE_CODE_SYMBOL_LIBRARY,  /* A standard library table (math, string, ...). */
    WPE_CODE_SYMBOL_KEYWORD,
} WPE_CodeSymbolKind;

typedef struct WPE_CodeSymbol {
    char name[WPE_CODE_SYMBOL_NAME_MAX];
    WPE_CodeSymbolKind kind;
    uint32_t line;                         /* 1-based line of its declaration; 0 for built-in names. */
    char module[WPE_CODE_SYMBOL_PATH_MAX]; /* For `local m = require("path")`: the path, else "". */
} WPE_CodeSymbol;

/** A list of symbols; zero-initialize it, free it with wpe_code_symbols_free(). Names are unique in it. */
typedef struct WPE_CodeSymbols {
    WPE_CodeSymbol* items;
    uint32_t count, capacity;
    /* Set by wpe_code_symbols_in_scope(): the table `self` belongs to at the point (the T of the
     * `function T:name` it is in), or "" outside methods. */
    char self_class[WPE_CODE_SYMBOL_NAME_MAX];
} WPE_CodeSymbols;

typedef enum WPE_CodeCompletionKind {
    WPE_CODE_COMPLETE_NONE = 0, /* In a comment, a string or a number, declaring a name, or after a chain. */
    WPE_CODE_COMPLETE_NAME,     /* A plain name (possibly nothing typed yet). */
    WPE_CODE_COMPLETE_MEMBER,   /* A name after `table.` or `table:`. */
} WPE_CodeCompletionKind;

typedef struct WPE_CodeCompletion {
    WPE_CodeCompletionKind kind;
    size_t start;                       /* Where the name being typed starts (it ends at the cursor). */
    size_t object_start, object_length; /* MEMBER: the table's name before the separator. */
    char separator;                     /* MEMBER: '.' or ':'. */
} WPE_CodeCompletion;

/**
 * What can be completed at `cursor` in `text`: a name, a member of a simple `name.` / `name:` (not a longer
 * chain), or nothing.
 *
 * @thread_safety Any thread
 */
WPE_CodeCompletion wpe_code_completion_at(const char* text, size_t length, size_t cursor);

/**
 * Appends the names declared before `cursor` that are still in scope there (locals, local and global
 * functions, parameters, loop variables), the innermost first, and sets `out->self_class`.
 *
 * @return false when out of memory (the list keeps what was added).
 * @thread_safety Any thread
 */
[[nodiscard]] bool wpe_code_symbols_in_scope(const char* text, size_t length, size_t cursor,
                                             WPE_CodeSymbols* out);

/**
 * Appends the members of the table named `object` that the file defines or uses: `function T.f`/`T:m`,
 * `T.name` and `T:name` anywhere, and the keys of a `T = { ... }` constructor.
 *
 * @return false when out of memory.
 * @thread_safety Any thread
 */
[[nodiscard]] bool wpe_code_symbols_members(const char* text, size_t length, const char* object,
                                            size_t object_length, WPE_CodeSymbols* out);

/**
 * Appends the keys of `class_name.properties = { ... }`: the script properties every instance (`self`) has.
 *
 * @return false when out of memory.
 * @thread_safety Any thread
 */
[[nodiscard]] bool wpe_code_symbols_properties(const char* text, size_t length, const char* class_name,
                                               size_t class_length, WPE_CodeSymbols* out);

/**
 * The name a module returns (its last `return name`), written into `out_name`.
 *
 * @return false when the file doesn't end by returning a name.
 * @thread_safety Any thread
 */
[[nodiscard]] bool wpe_code_symbols_returned(const char* text, size_t length, char* out_name, size_t size);

/**
 * Appends the functions and constants of the standard library table `name` (math, string, table, vector,
 * os, coroutine, bit32, utf8, buffer).
 *
 * @return false when `name` isn't one of them, or when out of memory.
 * @thread_safety Any thread
 */
[[nodiscard]] bool wpe_code_symbols_library(const char* name, size_t length, WPE_CodeSymbols* out);

/**
 * Appends the global names every script has (`wpe`, the libraries, `print`, `pairs`, ...) and the keywords.
 *
 * @return false when out of memory.
 * @thread_safety Any thread
 */
[[nodiscard]] bool wpe_code_symbols_globals(WPE_CodeSymbols* out);

/**
 * Appends one symbol unless the list already has its name (or the name is too long).
 *
 * @return false when out of memory.
 * @thread_safety Any thread
 */
[[nodiscard]] bool wpe_code_symbols_add(WPE_CodeSymbols* out, const char* name, size_t length,
                                        WPE_CodeSymbolKind kind, uint32_t line);

/** The symbol named `name`, or NULL. @thread_safety Any thread */
const WPE_CodeSymbol* wpe_code_symbols_find(const WPE_CodeSymbols* symbols, const char* name, size_t length);

/** Frees the list's memory and leaves it empty. @thread_safety Any thread */
void wpe_code_symbols_free(WPE_CodeSymbols* symbols);

#endif /* WPE_EDITOR_CODE_SYMBOLS_H */
