/**
 * @file code_text.h
 * @brief The script editor's text model (no UI): a UTF-8 buffer with lines, a cursor and selection, editing
 *        with auto-indent, undo/redo, search, and a Luau tokenizer for syntax highlighting.
 *
 * Offsets are byte offsets into the text; columns count characters (UTF-8 code points). Tabs are one
 * character (the widget draws them as several columns).
 *
 * - Dependencies: core (allocation)
 * - Thread affinity: owning thread
 *
 * @author WillPowerEngine Team
 */
#ifndef WPE_EDITOR_CODE_TEXT_H
#define WPE_EDITOR_CODE_TEXT_H

#include <stddef.h>
#include <stdint.h>

#define WPE_CODE_UNDO_MAX 100u /* Undo steps kept (each a copy of the text). */

typedef struct WPE_CodeUndo {
    char* text;
    size_t length;
    size_t cursor, anchor;
    uint64_t version;
} WPE_CodeUndo;

typedef struct WPE_CodeText {
    char* text; /* Always terminated. */
    size_t length;
    size_t capacity;
    uint32_t* line_starts; /* Offset of each line's first byte. */
    uint32_t line_count;
    uint32_t line_capacity;
    size_t cursor; /* Where typing goes; the selection runs from `anchor` to it. */
    size_t anchor;
    uint32_t preferred_column; /* Up/down keep to it across short lines. */
    uint64_t version;          /* Changes with every edit (undo brings an older one back). */
    uint64_t saved_version;    /* The version on disk: dirty while they differ. */
    uint64_t next_version;
    WPE_CodeUndo undo[WPE_CODE_UNDO_MAX]; /* States before each edit, newest last. */
    uint32_t undo_count;
    WPE_CodeUndo redo[WPE_CODE_UNDO_MAX]; /* States undone, newest last. */
    uint32_t redo_count;
    int last_edit; /* Typing and deleting coalesce into one undo step while the cursor stays put. */
    size_t last_edit_cursor;
} WPE_CodeText;

typedef enum WPE_CodeMove {
    WPE_CODE_LEFT = 0,
    WPE_CODE_RIGHT,
    WPE_CODE_UP,
    WPE_CODE_DOWN,
    WPE_CODE_WORD_LEFT,
    WPE_CODE_WORD_RIGHT,
    WPE_CODE_HOME, /* The line's first non-blank character, or its very start when already there. */
    WPE_CODE_END,
    WPE_CODE_PAGE_UP,
    WPE_CODE_PAGE_DOWN,
    WPE_CODE_START, /* Of the text. */
    WPE_CODE_FINISH,
} WPE_CodeMove;

/** Starts empty. @thread_safety Owning thread */
void wpe_code_text_init(WPE_CodeText* text);

/** Frees everything. @thread_safety Owning thread */
void wpe_code_text_free(WPE_CodeText* text);

/** Replaces the whole text (a file just opened): cursor at the start, no undo, saved. CRLF becomes LF.
 * @return false out of memory (the text is then empty). @thread_safety Owning thread */
bool wpe_code_text_set(WPE_CodeText* text, const char* content, size_t length);

/** Marks the current text as what is on disk. @thread_safety Owning thread */
void wpe_code_text_mark_saved(WPE_CodeText* text);

/** @return Whether the text differs from what was last saved. @thread_safety Owning thread */
bool wpe_code_text_dirty(const WPE_CodeText* text);

/** @return The line holding `offset` (0-based). @thread_safety Owning thread */
uint32_t wpe_code_text_line_of(const WPE_CodeText* text, size_t offset);

/** A line's text (without its newline). @thread_safety Owning thread */
const char* wpe_code_text_line(const WPE_CodeText* text, uint32_t line, size_t* out_length);

/** @return The character column of `offset` in its line. @thread_safety Owning thread */
uint32_t wpe_code_text_column(const WPE_CodeText* text, size_t offset);

/** @return The offset at `column` characters into `line` (clamped to the line). @thread_safety Owning thread
 */
size_t wpe_code_text_offset(const WPE_CodeText* text, uint32_t line, uint32_t column);

/** The selection as [start, end); empty when they are equal. @thread_safety Owning thread */
void wpe_code_text_selection(const WPE_CodeText* text, size_t* out_start, size_t* out_end);

/** Puts the cursor at `offset` (clamped, kept off the middle of a character); `select` extends the selection.
 * @thread_safety Owning thread */
void wpe_code_text_set_cursor(WPE_CodeText* text, size_t offset, bool select);

/** Selects the word (or run of other characters) at `offset`. @thread_safety Owning thread */
void wpe_code_text_select_word(WPE_CodeText* text, size_t offset);

/** Selects everything. @thread_safety Owning thread */
void wpe_code_text_select_all(WPE_CodeText* text);

/** Moves the cursor; `select` extends the selection, else it collapses. `page` is the lines per page.
 * @thread_safety Owning thread */
void wpe_code_text_move(WPE_CodeText* text, WPE_CodeMove move, bool select, uint32_t page);

/** Types text over the selection (one undo step for a run of typing). @return false out of memory.
 * @thread_safety Owning thread */
bool wpe_code_text_insert(WPE_CodeText* text, const char* content, size_t length);

/** A new line keeping the current line's indentation, one tab deeper after a line opening a block (`then`,
 * `do`, `else`, `repeat`, `function(...)`, `{`, `(`). @thread_safety Owning thread */
bool wpe_code_text_newline(WPE_CodeText* text);

/** Deletes the selection, or the character before (backspace) / after (delete) the cursor.
 * @thread_safety Owning thread */
bool wpe_code_text_erase(WPE_CodeText* text, bool forward);

/** Tab: with a selection over lines, indents them all (or removes a tab from each, `outdent`); otherwise
 * inserts a tab (or outdents the line). @thread_safety Owning thread */
bool wpe_code_text_indent(WPE_CodeText* text, bool outdent);

/** Undo and redo. @return false when there was nothing to undo or redo. @thread_safety Owning thread */
bool wpe_code_text_undo(WPE_CodeText* text);
bool wpe_code_text_redo(WPE_CodeText* text);

/** Finds `needle` (case-insensitively for ASCII) after (or, backwards, before) `from`, wrapping around.
 * @return Its offset, or SIZE_MAX. @thread_safety Owning thread */
size_t wpe_code_text_find(const WPE_CodeText* text, const char* needle, size_t from, bool forward);

/* ---- Luau highlighting --------------------------------------------------------------------------------- */

typedef enum WPE_CodeTokenKind {
    WPE_CODE_TOKEN_TEXT = 0, /* Names, operators, spaces. */
    WPE_CODE_TOKEN_KEYWORD,  /* local, function, if, then, end, ... */
    WPE_CODE_TOKEN_CONSTANT, /* true, false, nil, self */
    WPE_CODE_TOKEN_NUMBER,
    WPE_CODE_TOKEN_STRING,
    WPE_CODE_TOKEN_COMMENT,
    WPE_CODE_TOKEN_LIBRARY, /* wpe, math, string, table, vector, ... */
} WPE_CodeTokenKind;

typedef struct WPE_CodeToken {
    uint32_t start; /* Byte offset in the line. */
    uint32_t length;
    WPE_CodeTokenKind kind;
} WPE_CodeToken;

/**
 * Splits one line into tokens. `state` carries a long comment or long string (`--[[ ]]`, `[==[ ]==]`) from
 * the line before; the state for the next line is returned (0: none open). Tokens past `max` are merged into
 * the last.
 *
 * @thread_safety Any thread
 */
uint8_t wpe_code_tokenize(const char* line, size_t length, uint8_t state, WPE_CodeToken* out_tokens,
                          uint32_t max, uint32_t* out_count);

#endif /* WPE_EDITOR_CODE_TEXT_H */
