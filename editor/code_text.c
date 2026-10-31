#include "editor/code_text.h"

#include <string.h>

#include "core/alloc.h"

/* What the last edit was, for coalescing undo steps. */
enum { WPE__EDIT_NONE = 0, WPE__EDIT_TYPE, WPE__EDIT_BACKSPACE, WPE__EDIT_DELETE };

/* ---- Storage ----------------------------------------------------------------------------------------- */

static bool wpe__reserve(WPE_CodeText* t, size_t length) {
    if (length + 1 <= t->capacity) return true;
    size_t capacity = t->capacity ? t->capacity : 256;
    while (capacity < length + 1)
        capacity *= 2;
    char* text = wpe_realloc(t->text, capacity);
    if (!text) return false;
    t->text = text;
    t->capacity = capacity;
    return true;
}

/* Recomputes where each line starts (after every change: scripts are small). */
static bool wpe__index_lines(WPE_CodeText* t) {
    uint32_t count = 1;
    for (size_t i = 0; i < t->length; ++i)
        count += t->text[i] == '\n';
    if (count > t->line_capacity) {
        uint32_t* starts = wpe_realloc(t->line_starts, (size_t)count * sizeof *starts);
        if (!starts) return false;
        t->line_starts = starts;
        t->line_capacity = count;
    }
    uint32_t line = 0;
    t->line_starts[line++] = 0;
    for (size_t i = 0; i < t->length; ++i)
        if (t->text[i] == '\n') t->line_starts[line++] = (uint32_t)(i + 1);
    t->line_count = count;
    return true;
}

static void wpe__free_step(WPE_CodeUndo* step) {
    wpe_free(step->text);
    step->text = NULL;
}

/* A copy of the current state; false out of memory. */
static bool wpe__capture(const WPE_CodeText* t, WPE_CodeUndo* out) {
    char* copy = wpe_alloc(t->length + 1);
    if (!copy) return false;
    memcpy(copy, t->text, t->length + 1);
    *out = (WPE_CodeUndo){
        .text = copy, .length = t->length, .cursor = t->cursor, .anchor = t->anchor, .version = t->version};
    return true;
}

/* Pushes onto a stack of WPE_CODE_UNDO_MAX, dropping its oldest when full. */
static void wpe__push(WPE_CodeUndo* stack, uint32_t* count, WPE_CodeUndo step) {
    if (*count == WPE_CODE_UNDO_MAX) {
        wpe__free_step(&stack[0]);
        memmove(stack, stack + 1, (WPE_CODE_UNDO_MAX - 1) * sizeof *stack);
        --*count;
    }
    stack[(*count)++] = step;
}

static void wpe__clear_stack(WPE_CodeUndo* stack, uint32_t* count) {
    while (*count > 0)
        wpe__free_step(&stack[--*count]);
}

/* Puts a captured state back (taking its text). */
static void wpe__restore(WPE_CodeText* t, WPE_CodeUndo* step) {
    wpe_free(t->text);
    t->text = step->text;
    t->length = step->length;
    t->capacity = step->length + 1;
    t->cursor = step->cursor;
    t->anchor = step->anchor;
    t->version = step->version;
    step->text = NULL;
    t->last_edit = WPE__EDIT_NONE;
    (void)wpe__index_lines(t);
    t->preferred_column = wpe_code_text_column(t, t->cursor);
}

void wpe_code_text_init(WPE_CodeText* t) {
    memset(t, 0, sizeof *t);
    t->next_version = 1;
    (void)wpe_code_text_set(t, "", 0);
}

void wpe_code_text_free(WPE_CodeText* t) {
    wpe__clear_stack(t->undo, &t->undo_count);
    wpe__clear_stack(t->redo, &t->redo_count);
    wpe_free(t->text);
    wpe_free(t->line_starts);
    memset(t, 0, sizeof *t);
}

bool wpe_code_text_set(WPE_CodeText* t, const char* content, size_t length) {
    wpe__clear_stack(t->undo, &t->undo_count);
    wpe__clear_stack(t->redo, &t->redo_count);
    t->length = 0;
    t->cursor = t->anchor = 0;
    t->preferred_column = 0;
    t->last_edit = WPE__EDIT_NONE;
    t->version = t->saved_version = t->next_version++;
    bool ok = wpe__reserve(t, length);
    if (ok) {
        for (size_t i = 0; i < length; ++i) /* Windows line ends become plain newlines. */
            if (content[i] != '\r') t->text[t->length++] = content[i];
    }
    if (t->text) t->text[t->length] = '\0';
    return wpe__index_lines(t) && ok;
}

void wpe_code_text_mark_saved(WPE_CodeText* t) {
    t->saved_version = t->version;
}

bool wpe_code_text_dirty(const WPE_CodeText* t) {
    return t->version != t->saved_version;
}

/* ---- Positions ----------------------------------------------------------------------------------------- */

static bool wpe__continuation(char c) {
    return ((unsigned char)c & 0xC0u) == 0x80u;
}

static size_t wpe__prev_char(const WPE_CodeText* t, size_t offset) {
    if (offset == 0) return 0;
    --offset;
    while (offset > 0 && wpe__continuation(t->text[offset]))
        --offset;
    return offset;
}

static size_t wpe__next_char(const WPE_CodeText* t, size_t offset) {
    if (offset >= t->length) return t->length;
    ++offset;
    while (offset < t->length && wpe__continuation(t->text[offset]))
        ++offset;
    return offset;
}

uint32_t wpe_code_text_line_of(const WPE_CodeText* t, size_t offset) {
    uint32_t low = 0, high = t->line_count - 1;
    while (low < high) { /* The last line starting at or before offset. */
        const uint32_t mid = (low + high + 1) / 2;
        if (t->line_starts[mid] <= offset) {
            low = mid;
        } else {
            high = mid - 1;
        }
    }
    return low;
}

const char* wpe_code_text_line(const WPE_CodeText* t, uint32_t line, size_t* out_length) {
    if (line >= t->line_count) line = t->line_count - 1;
    const size_t start = t->line_starts[line];
    const size_t end = line + 1 < t->line_count ? t->line_starts[line + 1] - 1 : t->length;
    if (out_length) *out_length = end - start;
    return t->text + start;
}

uint32_t wpe_code_text_column(const WPE_CodeText* t, size_t offset) {
    const size_t start = t->line_starts[wpe_code_text_line_of(t, offset)];
    uint32_t column = 0;
    for (size_t i = start; i < offset && i < t->length; ++i)
        column += !wpe__continuation(t->text[i]);
    return column;
}

size_t wpe_code_text_offset(const WPE_CodeText* t, uint32_t line, uint32_t column) {
    size_t length = 0;
    const char* text = wpe_code_text_line(t, line, &length);
    size_t i = 0;
    for (uint32_t c = 0; c < column && i < length; ++c) {
        ++i;
        while (i < length && wpe__continuation(text[i]))
            ++i;
    }
    return (size_t)(text - t->text) + i;
}

void wpe_code_text_selection(const WPE_CodeText* t, size_t* out_start, size_t* out_end) {
    *out_start = t->anchor < t->cursor ? t->anchor : t->cursor;
    *out_end = t->anchor < t->cursor ? t->cursor : t->anchor;
}

void wpe_code_text_set_cursor(WPE_CodeText* t, size_t offset, bool select) {
    if (offset > t->length) offset = t->length;
    while (offset > 0 && offset < t->length && wpe__continuation(t->text[offset]))
        --offset;
    t->cursor = offset;
    if (!select) t->anchor = offset;
    t->preferred_column = wpe_code_text_column(t, offset);
    t->last_edit = WPE__EDIT_NONE;
}

static bool wpe__word_char(char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' ||
           ((unsigned char)c & 0x80u);
}

void wpe_code_text_select_word(WPE_CodeText* t, size_t offset) {
    if (offset > t->length) offset = t->length;
    if (offset == t->length && offset > 0) offset = wpe__prev_char(t, offset);
    if (t->length == 0 || t->text[offset] == '\n') {
        wpe_code_text_set_cursor(t, offset, false);
        return;
    }
    const bool word = wpe__word_char(t->text[offset]);
    const bool space = t->text[offset] == ' ' || t->text[offset] == '\t';
    size_t start = offset, end = offset;
    while (start > 0) {
        const char c = t->text[start - 1];
        if (c == '\n' || wpe__word_char(c) != word || (!word && (c == ' ' || c == '\t') != space)) break;
        --start;
    }
    while (end < t->length) {
        const char c = t->text[end];
        if (c == '\n' || wpe__word_char(c) != word || (!word && (c == ' ' || c == '\t') != space)) break;
        ++end;
    }
    t->anchor = start;
    t->cursor = end;
    t->preferred_column = wpe_code_text_column(t, end);
    t->last_edit = WPE__EDIT_NONE;
}

void wpe_code_text_select_all(WPE_CodeText* t) {
    t->anchor = 0;
    t->cursor = t->length;
    t->last_edit = WPE__EDIT_NONE;
}

/* The first non-blank character of the line (its end when blank). */
static size_t wpe__line_text_start(const WPE_CodeText* t, uint32_t line) {
    size_t length = 0;
    const char* text = wpe_code_text_line(t, line, &length);
    size_t i = 0;
    while (i < length && (text[i] == ' ' || text[i] == '\t'))
        ++i;
    return (size_t)(text - t->text) + i;
}

void wpe_code_text_move(WPE_CodeText* t, WPE_CodeMove move, bool select, uint32_t page) {
    size_t start, end;
    wpe_code_text_selection(t, &start, &end);
    const uint32_t line = wpe_code_text_line_of(t, t->cursor);
    size_t to; /* Every move sets it. */
    bool vertical = false;
    switch (move) {
    case WPE_CODE_LEFT:
        to = !select && start != end ? start : wpe__prev_char(t, t->cursor);
        break;
    case WPE_CODE_RIGHT:
        to = !select && start != end ? end : wpe__next_char(t, t->cursor);
        break;
    case WPE_CODE_UP:
    case WPE_CODE_DOWN:
    case WPE_CODE_PAGE_UP:
    case WPE_CODE_PAGE_DOWN: {
        const int64_t step = move == WPE_CODE_UP        ? -1
                             : move == WPE_CODE_DOWN    ? 1
                             : move == WPE_CODE_PAGE_UP ? -(int64_t)(page ? page : 1)
                                                        : (int64_t)(page ? page : 1);
        int64_t target = (int64_t)line + step;
        if (target < 0) {
            to = 0; /* Past the first line: its start. */
        } else if (target >= (int64_t)t->line_count) {
            to = t->length;
        } else {
            to = wpe_code_text_offset(t, (uint32_t)target, t->preferred_column);
        }
        vertical = target >= 0 && target < (int64_t)t->line_count;
        break;
    }
    case WPE_CODE_WORD_LEFT:
        to = t->cursor;
        while (to > 0 && (t->text[to - 1] == ' ' || t->text[to - 1] == '\t'))
            --to;
        if (to > 0 && t->text[to - 1] == '\n') {
            --to;
        } else if (to > 0) {
            const bool word = wpe__word_char(t->text[to - 1]);
            while (to > 0 && t->text[to - 1] != '\n' && t->text[to - 1] != ' ' && t->text[to - 1] != '\t' &&
                   wpe__word_char(t->text[to - 1]) == word)
                --to;
        }
        break;
    case WPE_CODE_WORD_RIGHT:
        to = t->cursor;
        if (to < t->length && t->text[to] == '\n') {
            ++to;
        } else if (to < t->length) {
            const bool word = wpe__word_char(t->text[to]);
            while (to < t->length && t->text[to] != '\n' && t->text[to] != ' ' && t->text[to] != '\t' &&
                   wpe__word_char(t->text[to]) == word)
                ++to;
        }
        while (to < t->length && (t->text[to] == ' ' || t->text[to] == '\t'))
            ++to;
        break;
    case WPE_CODE_HOME: {
        const size_t text_start = wpe__line_text_start(t, line);
        to = t->cursor == text_start ? t->line_starts[line] : text_start;
        break;
    }
    case WPE_CODE_END: {
        size_t length = 0;
        to = (size_t)(wpe_code_text_line(t, line, &length) - t->text) + length;
        break;
    }
    case WPE_CODE_START:
        to = 0;
        break;
    case WPE_CODE_FINISH:
    default:
        to = t->length;
        break;
    }
    const uint32_t keep = t->preferred_column;
    wpe_code_text_set_cursor(t, to, select);
    if (vertical) t->preferred_column = keep; /* Up and down remember the column across short lines. */
}

/* ---- Editing ---------------------------------------------------------------------------------------------
 */

/* Starts an edit: a new undo step unless it continues the same kind of edit at the same place. */
static bool wpe__begin(WPE_CodeText* t, int kind) {
    const bool continues = kind != WPE__EDIT_NONE && kind == t->last_edit &&
                           t->cursor == t->last_edit_cursor && t->anchor == t->cursor;
    if (!continues) {
        WPE_CodeUndo step;
        if (!wpe__capture(t, &step)) return false;
        wpe__push(t->undo, &t->undo_count, step);
    }
    wpe__clear_stack(t->redo, &t->redo_count);
    t->version = t->next_version++;
    return true;
}

static void wpe__end(WPE_CodeText* t, int kind) {
    (void)wpe__index_lines(t);
    t->anchor = t->cursor;
    t->last_edit = kind;
    t->last_edit_cursor = t->cursor;
    t->preferred_column = wpe_code_text_column(t, t->cursor);
}

/* Replaces [start, end) with `content`; the cursor goes after it. */
static bool wpe__replace(WPE_CodeText* t, size_t start, size_t end, const char* content, size_t length) {
    const size_t new_length = t->length - (end - start) + length;
    if (!wpe__reserve(t, new_length)) return false;
    memmove(t->text + start + length, t->text + end, t->length - end + 1);
    memcpy(t->text + start, content, length);
    t->length = new_length;
    t->cursor = start + length;
    return true;
}

bool wpe_code_text_insert(WPE_CodeText* t, const char* content, size_t length) {
    /* Pasted text loses carriage returns; one typed character (no newline) coalesces with the last. */
    size_t kept = 0;
    char stack[64];
    char* clean = length <= sizeof stack ? stack : wpe_alloc(length);
    if (!clean) return false;
    for (size_t i = 0; i < length; ++i)
        if (content[i] != '\r') clean[kept++] = content[i];
    size_t chars = 0;
    for (size_t i = 0; i < kept; ++i)
        chars += !wpe__continuation(clean[i]);
    const int kind = chars == 1 && clean[0] != '\n' ? WPE__EDIT_TYPE : WPE__EDIT_NONE;
    size_t start, end;
    wpe_code_text_selection(t, &start, &end);
    bool ok = wpe__begin(t, kind) && wpe__replace(t, start, end, clean, kept);
    if (clean != stack) wpe_free(clean);
    wpe__end(t, kind);
    return ok;
}

/* Whether the line (up to the cursor) opens a block, so the next line goes one tab deeper. */
static bool wpe__opens_block(const char* text, size_t length) {
    while (length > 0 && (text[length - 1] == ' ' || text[length - 1] == '\t'))
        --length;
    if (length == 0) return false;
    const char last = text[length - 1];
    if (last == '{' || last == '(') return true;
    static const char* const words[] = {"then", "do", "else", "repeat"};
    for (size_t w = 0; w < sizeof words / sizeof words[0]; ++w) {
        const size_t n = strlen(words[w]);
        if (length >= n && memcmp(text + length - n, words[w], n) == 0 &&
            (length == n || !wpe__word_char(text[length - n - 1])))
            return true;
    }
    /* "function name(args)" or "= function(args)": a function definition's header. */
    if (last == ')') {
        for (size_t i = 0; i + 8 <= length; ++i) {
            if (memcmp(text + i, "function", 8) == 0 && (i == 0 || !wpe__word_char(text[i - 1])) &&
                (i + 8 == length || !wpe__word_char(text[i + 8])))
                return true;
        }
    }
    return false;
}

bool wpe_code_text_newline(WPE_CodeText* t) {
    size_t start, end;
    wpe_code_text_selection(t, &start, &end);
    const uint32_t line = wpe_code_text_line_of(t, start);
    const char* text = t->text + t->line_starts[line];
    const size_t before = start - t->line_starts[line];
    size_t indent = 0;
    while (indent < before && (text[indent] == ' ' || text[indent] == '\t'))
        ++indent;
    const bool deeper = wpe__opens_block(text, before);
    char stack[128];
    const size_t length = 1 + indent + (deeper ? 1 : 0);
    char* insert = length <= sizeof stack ? stack : wpe_alloc(length);
    if (!insert) return false;
    insert[0] = '\n';
    memcpy(insert + 1, text, indent);
    if (deeper) insert[1 + indent] = '\t';
    bool ok = wpe__begin(t, WPE__EDIT_NONE) && wpe__replace(t, start, end, insert, length);
    if (insert != stack) wpe_free(insert);
    wpe__end(t, WPE__EDIT_NONE);
    return ok;
}

bool wpe_code_text_erase(WPE_CodeText* t, bool forward) {
    size_t start, end;
    wpe_code_text_selection(t, &start, &end);
    int kind = WPE__EDIT_NONE;
    if (start == end) {
        if (forward ? start >= t->length : start == 0) return false;
        if (forward) {
            end = wpe__next_char(t, start);
        } else {
            start = wpe__prev_char(t, start);
        }
        kind = forward ? WPE__EDIT_DELETE : WPE__EDIT_BACKSPACE;
    }
    const bool ok = wpe__begin(t, kind) && wpe__replace(t, start, end, "", 0);
    wpe__end(t, kind);
    return ok;
}

bool wpe_code_text_indent(WPE_CodeText* t, bool outdent) {
    size_t start, end;
    wpe_code_text_selection(t, &start, &end);
    const uint32_t first = wpe_code_text_line_of(t, start);
    uint32_t last = wpe_code_text_line_of(t, end);
    if (end > start && last > first && end == t->line_starts[last])
        --last; /* A selection ending at a line start. */
    if (first == last && !outdent && start == end) return wpe_code_text_insert(t, "\t", 1);
    if (first == last && !outdent && wpe_code_text_line_of(t, start) == wpe_code_text_line_of(t, end) &&
        !(start == t->line_starts[first] && end > start))
        return wpe_code_text_insert(t, "\t", 1); /* A selection within one line: replaced, as typing would. */
    const bool had_selection = start != end;
    const size_t cursor = t->cursor;
    if (!wpe__begin(t, WPE__EDIT_NONE)) return false;
    bool ok = true;
    size_t removed_first = 0; /* What came off the first line (for a cursor without a selection). */
    for (uint32_t line = last + 1; line-- > first && ok;) {
        const size_t at = t->line_starts[line];
        if (!outdent) {
            ok = wpe__replace(t, at, at, "\t", 1);
        } else {
            size_t remove = 0;
            if (at < t->length && t->text[at] == '\t') {
                remove = 1;
            } else {
                while (remove < 4 && at + remove < t->length && t->text[at + remove] == ' ')
                    ++remove;
            }
            if (remove) ok = wpe__replace(t, at, at + remove, "", 0);
            if (line == first) removed_first = remove;
        }
    }
    (void)wpe__index_lines(t);
    if (!had_selection) {
        /* Outdenting the cursor's line: the cursor stays on its text, without a selection. */
        const size_t line_start = t->line_starts[first];
        t->cursor = t->anchor = cursor - line_start >= removed_first ? cursor - removed_first : line_start;
        t->last_edit = WPE__EDIT_NONE;
        t->preferred_column = wpe_code_text_column(t, t->cursor);
        return ok;
    }
    /* The changed lines stay selected, whole. */
    size_t last_length = 0;
    const char* last_text = wpe_code_text_line(t, last, &last_length);
    t->anchor = t->line_starts[first];
    t->cursor = (size_t)(last_text - t->text) + last_length;
    t->last_edit = WPE__EDIT_NONE;
    t->preferred_column = wpe_code_text_column(t, t->cursor);
    return ok;
}

bool wpe_code_text_undo(WPE_CodeText* t) {
    if (t->undo_count == 0) return false;
    WPE_CodeUndo now;
    if (!wpe__capture(t, &now)) return false;
    wpe__push(t->redo, &t->redo_count, now);
    wpe__restore(t, &t->undo[--t->undo_count]);
    return true;
}

bool wpe_code_text_redo(WPE_CodeText* t) {
    if (t->redo_count == 0) return false;
    WPE_CodeUndo now;
    if (!wpe__capture(t, &now)) return false;
    wpe__push(t->undo, &t->undo_count, now);
    wpe__restore(t, &t->redo[--t->redo_count]);
    return true;
}

static char wpe__lower(char c) {
    return c >= 'A' && c <= 'Z' ? (char)(c - 'A' + 'a') : c;
}

static bool wpe__matches(const WPE_CodeText* t, size_t at, const char* needle, size_t length) {
    for (size_t i = 0; i < length; ++i)
        if (wpe__lower(t->text[at + i]) != wpe__lower(needle[i])) return false;
    return true;
}

size_t wpe_code_text_find(const WPE_CodeText* t, const char* needle, size_t from, bool forward) {
    const size_t length = needle ? strlen(needle) : 0;
    if (length == 0 || length > t->length) return SIZE_MAX;
    const size_t last = t->length - length; /* The last offset a match can start at. */
    if (from > t->length) from = t->length;
    for (size_t step = 0; step <= last; ++step) {
        size_t at;
        if (forward) {
            at = (from + step) % (last + 1);
        } else {
            const size_t begin = from == 0 ? last : (from - 1 < last ? from - 1 : last);
            at = (begin + (last + 1) - step % (last + 1)) % (last + 1);
        }
        if (wpe__matches(t, at, needle, length)) return at;
    }
    return SIZE_MAX;
}

/* ---- Luau highlighting --------------------------------------------------------------------------------- */

/* Tokenizer states: 0 none; 1..63 inside a long comment of level state-1; 64..127 a long string, state-64. */
#define WPE__STATE_COMMENT 1u
#define WPE__STATE_STRING 64u

static bool wpe__in_list(const char* word, size_t length, const char* const* list, size_t count) {
    for (size_t i = 0; i < count; ++i)
        if (strlen(list[i]) == length && memcmp(list[i], word, length) == 0) return true;
    return false;
}

static const char* const wpe__keywords[] = {
    "and", "break", "do",     "else",   "elseif", "end",   "for",   "function", "if",     "in",   "local",
    "not", "or",    "repeat", "return", "then",   "until", "while", "continue", "export", "type", "typeof"};
static const char* const wpe__constants[] = {"true", "false", "nil", "self"};
static const char* const wpe__libraries[] = {
    "wpe",    "math",   "string",   "table",    "vector",   "bit32",   "coroutine",    "utf8",
    "os",     "buffer", "debug",    "print",    "error",    "assert",  "pcall",        "xpcall",
    "pairs",  "ipairs", "select",   "tostring", "tonumber", "require", "setmetatable", "getmetatable",
    "rawget", "rawset", "rawequal", "next",     "unpack"};

/* At `text[i]`, a long bracket opening ("[[", "[=[" ...): returns its level, or -1. Sets *out_length. */
static int wpe__long_open(const char* text, size_t length, size_t i, size_t* out_length) {
    if (i >= length || text[i] != '[') return -1;
    size_t j = i + 1;
    int level = 0;
    while (j < length && text[j] == '=') {
        ++level;
        ++j;
    }
    if (j >= length || text[j] != '[' || level > 62) return -1;
    *out_length = j + 1 - i;
    return level;
}

/* Where the long bracket of `level` closes at or after `i` (just past it), or SIZE_MAX. */
static size_t wpe__long_close(const char* text, size_t length, size_t i, int level) {
    for (; i < length; ++i) {
        if (text[i] != ']') continue;
        size_t j = i + 1;
        int seen = 0;
        while (j < length && text[j] == '=' && seen < level) {
            ++seen;
            ++j;
        }
        if (seen == level && j < length && text[j] == ']') return j + 1;
    }
    return SIZE_MAX;
}

typedef struct wpe__Tokens {
    WPE_CodeToken* out;
    uint32_t max;
    uint32_t count;
} wpe__Tokens;

static void wpe__emit(wpe__Tokens* tokens, size_t start, size_t end, WPE_CodeTokenKind kind) {
    if (end <= start || tokens->max == 0) return;
    WPE_CodeToken* last = tokens->count ? &tokens->out[tokens->count - 1] : NULL;
    /* Runs of plain text merge, and so does anything past the token limit. */
    if (last && (tokens->count == tokens->max || (last->kind == kind && kind == WPE_CODE_TOKEN_TEXT &&
                                                  last->start + last->length == start))) {
        last->length = (uint32_t)(end - last->start);
        return;
    }
    tokens->out[tokens->count++] =
        (WPE_CodeToken){.start = (uint32_t)start, .length = (uint32_t)(end - start), .kind = kind};
}

uint8_t wpe_code_tokenize(const char* text, size_t length, uint8_t state, WPE_CodeToken* out_tokens,
                          uint32_t max, uint32_t* out_count) {
    wpe__Tokens tokens = {.out = out_tokens, .max = max};
    size_t i = 0;
    /* A long comment or string carried over from the line before. */
    if (state >= WPE__STATE_COMMENT) {
        const bool comment = state < WPE__STATE_STRING;
        const int level = (int)state - (int)(comment ? WPE__STATE_COMMENT : WPE__STATE_STRING);
        const size_t close = wpe__long_close(text, length, 0, level);
        const WPE_CodeTokenKind kind = comment ? WPE_CODE_TOKEN_COMMENT : WPE_CODE_TOKEN_STRING;
        if (close == SIZE_MAX) {
            wpe__emit(&tokens, 0, length, kind);
            *out_count = tokens.count;
            return state;
        }
        wpe__emit(&tokens, 0, close, kind);
        i = close;
        state = 0;
    }
    while (i < length) {
        const char c = text[i];
        size_t open = 0;
        if (c == '-' && i + 1 < length && text[i + 1] == '-') {
            const int level = wpe__long_open(text, length, i + 2, &open);
            if (level >= 0) {
                const size_t close = wpe__long_close(text, length, i + 2 + open, level);
                if (close == SIZE_MAX) {
                    wpe__emit(&tokens, i, length, WPE_CODE_TOKEN_COMMENT);
                    state = (uint8_t)(WPE__STATE_COMMENT + level);
                    break;
                }
                wpe__emit(&tokens, i, close, WPE_CODE_TOKEN_COMMENT);
                i = close;
            } else {
                wpe__emit(&tokens, i, length, WPE_CODE_TOKEN_COMMENT);
                i = length;
            }
        } else if (c == '[' && wpe__long_open(text, length, i, &open) >= 0) {
            const int level = wpe__long_open(text, length, i, &open);
            const size_t close = wpe__long_close(text, length, i + open, level);
            if (close == SIZE_MAX) {
                wpe__emit(&tokens, i, length, WPE_CODE_TOKEN_STRING);
                state = (uint8_t)(WPE__STATE_STRING + level);
                break;
            }
            wpe__emit(&tokens, i, close, WPE_CODE_TOKEN_STRING);
            i = close;
        } else if (c == '"' || c == '\'' || c == '`') {
            size_t j = i + 1;
            while (j < length && text[j] != c) /* An escape skips the character after it. */
                j += text[j] == '\\' && j + 1 < length ? 2 : 1;
            j = j < length ? j + 1 : length;
            wpe__emit(&tokens, i, j, WPE_CODE_TOKEN_STRING);
            i = j;
        } else if ((c >= '0' && c <= '9') ||
                   (c == '.' && i + 1 < length && text[i + 1] >= '0' && text[i + 1] <= '9')) {
            size_t j = i + 1;
            while (j < length) {
                const char d = text[j];
                const bool exponent_sign =
                    (d == '+' || d == '-') &&
                    (text[j - 1] == 'e' || text[j - 1] == 'E' || text[j - 1] == 'p' || text[j - 1] == 'P');
                if (!wpe__word_char(d) && d != '.' && !exponent_sign) break;
                ++j;
            }
            wpe__emit(&tokens, i, j, WPE_CODE_TOKEN_NUMBER);
            i = j;
        } else if (wpe__word_char(c)) {
            size_t j = i + 1;
            while (j < length && wpe__word_char(text[j]))
                ++j;
            const char* word = text + i;
            const size_t n = j - i;
            /* After "." or ":" a name is a field or method, whatever it spells. */
            size_t p = i;
            while (p > 0 && (text[p - 1] == ' ' || text[p - 1] == '\t'))
                --p;
            const bool member = p > 0 && (text[p - 1] == '.' || text[p - 1] == ':') &&
                                !(p > 1 && text[p - 2] == '.'); /* ".." is concatenation */
            WPE_CodeTokenKind kind = WPE_CODE_TOKEN_TEXT;
            if (!member) {
                if (wpe__in_list(word, n, wpe__keywords, sizeof wpe__keywords / sizeof wpe__keywords[0])) {
                    kind = WPE_CODE_TOKEN_KEYWORD;
                } else if (wpe__in_list(word, n, wpe__constants,
                                        sizeof wpe__constants / sizeof wpe__constants[0])) {
                    kind = WPE_CODE_TOKEN_CONSTANT;
                } else if (wpe__in_list(word, n, wpe__libraries,
                                        sizeof wpe__libraries / sizeof wpe__libraries[0])) {
                    kind = WPE_CODE_TOKEN_LIBRARY;
                }
            }
            wpe__emit(&tokens, i, j, kind);
            i = j;
        } else {
            wpe__emit(&tokens, i, i + 1, WPE_CODE_TOKEN_TEXT);
            ++i;
        }
    }
    *out_count = tokens.count;
    return state;
}
