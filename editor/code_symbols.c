/*
 * Names for the Script panel's completion: a small Luau lexer (comments and strings skipped) and scans over
 * its tokens. Scopes follow the block keywords: function, do, then and repeat open one; end, until, else and
 * elseif close one (else opens the next).
 */
#include "editor/code_symbols.h"

#include <stdio.h>
#include <string.h>

#include "core/alloc.h"

/* ---- Symbol lists ------------------------------------------------------------------------------------- */

const WPE_CodeSymbol* wpe_code_symbols_find(const WPE_CodeSymbols* symbols, const char* name, size_t length) {
    for (uint32_t i = 0; symbols && i < symbols->count; ++i) {
        const WPE_CodeSymbol* s = &symbols->items[i];
        if (strlen(s->name) == length && memcmp(s->name, name, length) == 0) return s;
    }
    return NULL;
}

bool wpe_code_symbols_add(WPE_CodeSymbols* out, const char* name, size_t length, WPE_CodeSymbolKind kind,
                          uint32_t line) {
    if (length == 0 || length >= WPE_CODE_SYMBOL_NAME_MAX || wpe_code_symbols_find(out, name, length))
        return true;
    if (out->count == out->capacity) {
        const uint32_t capacity = out->capacity ? out->capacity * 2 : 32;
        WPE_CodeSymbol* items = wpe_realloc(out->items, (size_t)capacity * sizeof *items);
        if (!items) return false;
        out->items = items;
        out->capacity = capacity;
    }
    WPE_CodeSymbol* s = &out->items[out->count++];
    memcpy(s->name, name, length);
    s->name[length] = '\0';
    s->kind = kind;
    s->line = line;
    s->module[0] = '\0';
    return true;
}

void wpe_code_symbols_free(WPE_CodeSymbols* symbols) {
    if (!symbols) return;
    wpe_free(symbols->items);
    *symbols = (WPE_CodeSymbols){0};
}

/* ---- Lexer -------------------------------------------------------------------------------------------- */

typedef enum wpe__TokKind { WPE__TOK_NAME, WPE__TOK_PUNCT, WPE__TOK_STRING, WPE__TOK_NUMBER } wpe__TokKind;

typedef struct wpe__Tok {
    size_t start, end;
    uint32_t line;
    wpe__TokKind kind;
} wpe__Tok;

typedef struct wpe__Lexed {
    const char* text;
    wpe__Tok* toks;
    uint32_t count, capacity;
    bool cursor_hidden; /* The cursor is inside a comment or a string. */
} wpe__Lexed;

static bool wpe__word_char(char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_';
}

/* At `text[i]`, a long bracket opening ("[[", "[=[" ...): returns its level, or -1. Sets *out_length. */
static int wpe__long_open(const char* text, size_t length, size_t i, size_t* out_length) {
    if (i >= length || text[i] != '[') return -1;
    size_t j = i + 1;
    int level = 0;
    while (j < length && text[j] == '=') {
        ++level;
        ++j;
    }
    if (j >= length || text[j] != '[') return -1;
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

static bool wpe__push(wpe__Lexed* lexed, size_t start, size_t end, uint32_t line, wpe__TokKind kind) {
    if (lexed->count == lexed->capacity) {
        const uint32_t capacity = lexed->capacity ? lexed->capacity * 2 : 256;
        wpe__Tok* toks = wpe_realloc(lexed->toks, (size_t)capacity * sizeof *toks);
        if (!toks) return false;
        lexed->toks = toks;
        lexed->capacity = capacity;
    }
    lexed->toks[lexed->count++] = (wpe__Tok){.start = start, .end = end, .line = line, .kind = kind};
    return true;
}

/* A comment or string from `start` to `end`: notes whether the cursor is in it (an unclosed one, or a line
 * comment, also holds the cursor at its end) and counts its lines. */
static void wpe__skipped(wpe__Lexed* lexed, size_t start, size_t end, bool closed, size_t cursor,
                         uint32_t* line) {
    if (cursor > start && (closed ? cursor < end : cursor <= end)) lexed->cursor_hidden = true;
    for (size_t i = start; i < end; ++i)
        if (lexed->text[i] == '\n') ++*line;
}

/* Lexes the tokens that end before `limit` (SIZE_MAX: all). */
static bool wpe__lex(const char* text, size_t length, size_t limit, wpe__Lexed* out) {
    *out = (wpe__Lexed){.text = text};
    const size_t cursor = limit;
    size_t i = 0;
    uint32_t line = 1;
    while (i < length && i < limit) {
        const char c = text[i];
        if (c == '\n') {
            ++line;
            ++i;
            continue;
        }
        if (c == ' ' || c == '\t' || c == '\r') {
            ++i;
            continue;
        }
        const size_t start = i;
        size_t open = 0;
        if (c == '-' && i + 1 < length && text[i + 1] == '-') {
            const int level = wpe__long_open(text, length, i + 2, &open);
            size_t end = i + 2;
            bool closed = false;
            if (level >= 0) {
                const size_t close = wpe__long_close(text, length, i + 2 + open, level);
                closed = close != SIZE_MAX;
                end = closed ? close : length;
            } else {
                while (end < length && text[end] != '\n')
                    ++end;
            }
            wpe__skipped(out, start, end, closed, cursor, &line);
            i = end;
            continue;
        }
        size_t end = i + 1;
        wpe__TokKind kind = WPE__TOK_PUNCT;
        const uint32_t token_line = line;
        if (c == '[' && wpe__long_open(text, length, i, &open) >= 0) {
            const size_t close =
                wpe__long_close(text, length, i + open, wpe__long_open(text, length, i, &open));
            end = close == SIZE_MAX ? length : close;
            wpe__skipped(out, start, end, close != SIZE_MAX, cursor, &line);
            kind = WPE__TOK_STRING;
        } else if (c == '"' || c == '\'' || c == '`') {
            /* An escape skips the character after it; a quoted string ends with its line. */
            while (end < length && text[end] != c && text[end] != '\n')
                end += text[end] == '\\' && end + 1 < length ? 2 : 1;
            const bool closed = end < length && text[end] == c;
            if (closed) ++end;
            wpe__skipped(out, start, end, closed, cursor, &line);
            kind = WPE__TOK_STRING;
        } else if ((c >= '0' && c <= '9') ||
                   (c == '.' && i + 1 < length && text[i + 1] >= '0' && text[i + 1] <= '9')) {
            while (end < length && (wpe__word_char(text[end]) || text[end] == '.' ||
                                    ((text[end] == '+' || text[end] == '-') &&
                                     (text[end - 1] == 'e' || text[end - 1] == 'E'))))
                ++end;
            kind = WPE__TOK_NUMBER;
        } else if (wpe__word_char(c)) {
            while (end < length && wpe__word_char(text[end]))
                ++end;
            kind = WPE__TOK_NAME;
        } else {
            static const char* const longer[] = {"...", "..", "==", "~=", "<=", ">=", "::", "->", "//"};
            for (size_t k = 0; k < sizeof longer / sizeof longer[0]; ++k) {
                const size_t n = strlen(longer[k]);
                if (i + n <= length && memcmp(text + i, longer[k], n) == 0) {
                    end = i + n;
                    break;
                }
            }
        }
        if (end >= limit) break; /* Touches the cursor: the name being typed isn't a declaration yet. */
        if (!wpe__push(out, start, end, token_line, kind)) return false;
        i = end;
    }
    return true;
}

/* ---- Token helpers ------------------------------------------------------------------------------------ */

static bool wpe__is(const wpe__Lexed* lexed, uint32_t k, const char* word) {
    if (k >= lexed->count) return false;
    const wpe__Tok* t = &lexed->toks[k];
    const size_t n = strlen(word);
    return t->end - t->start == n && memcmp(lexed->text + t->start, word, n) == 0;
}

static bool wpe__is_name(const wpe__Lexed* lexed, uint32_t k, const char* name, size_t length) {
    if (k >= lexed->count || lexed->toks[k].kind != WPE__TOK_NAME) return false;
    const wpe__Tok* t = &lexed->toks[k];
    return t->end - t->start == length && memcmp(lexed->text + t->start, name, length) == 0;
}

static const char* const wpe__keywords[] = {
    "and",   "break", "do",  "else", "elseif", "end",    "false", "for",  "function", "if",    "in",
    "local", "nil",   "not", "or",   "repeat", "return", "then",  "true", "until",    "while", "continue"};

static bool wpe__keyword(const wpe__Lexed* lexed, uint32_t k) {
    for (size_t i = 0; i < sizeof wpe__keywords / sizeof wpe__keywords[0]; ++i)
        if (wpe__is(lexed, k, wpe__keywords[i])) return true;
    return false;
}

/* A name that can be declared: a name token that isn't a keyword. */
static bool wpe__identifier(const wpe__Lexed* lexed, uint32_t k) {
    return k < lexed->count && lexed->toks[k].kind == WPE__TOK_NAME && !wpe__keyword(lexed, k);
}

static bool wpe__add_tok(WPE_CodeSymbols* out, const wpe__Lexed* lexed, uint32_t k, WPE_CodeSymbolKind kind) {
    const wpe__Tok* t = &lexed->toks[k];
    return wpe_code_symbols_add(out, lexed->text + t->start, t->end - t->start, kind, t->line);
}

/* Skips a type annotation (or a `<...>` list) from `k`: to the first `,`, `=` or `)` outside brackets, a
 * keyword, or a token on a later line than `line`. Returns where it stopped. */
static uint32_t wpe__skip_type(const wpe__Lexed* lexed, uint32_t k, uint32_t line) {
    int depth = 0;
    for (; k < lexed->count; ++k) {
        if (depth == 0 && (wpe__is(lexed, k, ",") || wpe__is(lexed, k, "=") || wpe__is(lexed, k, ")") ||
                           wpe__keyword(lexed, k) || lexed->toks[k].line > line))
            break;
        if (wpe__is(lexed, k, "(") || wpe__is(lexed, k, "{") || wpe__is(lexed, k, "[") ||
            wpe__is(lexed, k, "<"))
            ++depth;
        if (wpe__is(lexed, k, ")") || wpe__is(lexed, k, "}") || wpe__is(lexed, k, "]") ||
            wpe__is(lexed, k, ">"))
            --depth;
    }
    return k;
}

/* ---- Completion context ------------------------------------------------------------------------------- */

WPE_CodeCompletion wpe_code_completion_at(const char* text, size_t length, size_t cursor) {
    WPE_CodeCompletion none = {.kind = WPE_CODE_COMPLETE_NONE};
    if (!text || cursor > length) return none;
    size_t start = cursor;
    while (start > 0 && wpe__word_char(text[start - 1]))
        --start;
    if (start < cursor && text[start] >= '0' && text[start] <= '9') return none; /* A number. */
    wpe__Lexed lexed;
    if (!wpe__lex(text, length, cursor, &lexed)) {
        wpe_free(lexed.toks);
        return none;
    }
    const bool hidden = lexed.cursor_hidden;
    /* The token before the name: declaring a new name (local x, function f, for i) suggests nothing. */
    const uint32_t before = lexed.count;
    const bool declaring =
        before > 0 && (wpe__is(&lexed, before - 1, "local") || wpe__is(&lexed, before - 1, "function") ||
                       wpe__is(&lexed, before - 1, "for"));
    wpe_free(lexed.toks);
    if (hidden || declaring) return none;
    if (start > 0 && (text[start - 1] == '.' || text[start - 1] == ':')) {
        const size_t sep = start - 1;
        if (text[sep] == '.' && sep > 0 && text[sep - 1] == '.') return none; /* ".." concatenates. */
        size_t object = sep;
        while (object > 0 && wpe__word_char(text[object - 1]))
            --object;
        if (object == sep || (text[object] >= '0' && text[object] <= '9')) return none;
        if (object > 0 && (text[object - 1] == '.' || text[object - 1] == ':')) return none; /* a.b.c */
        return (WPE_CodeCompletion){.kind = WPE_CODE_COMPLETE_MEMBER,
                                    .start = start,
                                    .object_start = object,
                                    .object_length = sep - object,
                                    .separator = text[sep]};
    }
    return (WPE_CodeCompletion){.kind = WPE_CODE_COMPLETE_NAME, .start = start};
}

/* ---- Names in scope ----------------------------------------------------------------------------------- */

#define WPE__SCOPE_MAX 128u
#define WPE__PENDING_MAX 8u

typedef struct wpe__Scope {
    uint32_t base; /* Symbols below this index belong to outer scopes. */
    char self_class[WPE_CODE_SYMBOL_NAME_MAX];
} wpe__Scope;

typedef struct wpe__Scan {
    const wpe__Lexed* lexed;
    WPE_CodeSymbols stack; /* Declared names, innermost last; a closed scope drops its own. */
    wpe__Scope scopes[WPE__SCOPE_MAX];
    uint32_t depth;
    uint32_t pending[WPE__PENDING_MAX]; /* A for loop's variables, declared when its `do` opens. */
    uint32_t pending_count;
    bool ok;
} wpe__Scan;

static void wpe__open(wpe__Scan* scan) {
    if (scan->depth + 1 >= WPE__SCOPE_MAX) return;
    ++scan->depth;
    scan->scopes[scan->depth].base = scan->stack.count;
    memcpy(scan->scopes[scan->depth].self_class, scan->scopes[scan->depth - 1].self_class,
           WPE_CODE_SYMBOL_NAME_MAX);
}

static void wpe__close(wpe__Scan* scan) {
    if (scan->depth == 0) return;
    scan->stack.count = scan->scopes[scan->depth].base;
    --scan->depth;
}

/* Declares a name in the innermost scope; an inner one shadows an outer one of the same name. */
static WPE_CodeSymbol* wpe__declare(wpe__Scan* scan, uint32_t k, WPE_CodeSymbolKind kind) {
    const wpe__Tok* t = &scan->lexed->toks[k];
    const size_t n = t->end - t->start;
    if (n >= WPE_CODE_SYMBOL_NAME_MAX) return NULL;
    /* Declared again in the same scope: the newer declaration replaces it. */
    WPE_CodeSymbols* stack = &scan->stack;
    for (uint32_t i = scan->scopes[scan->depth].base; i < stack->count; ++i) {
        if (strlen(stack->items[i].name) == n &&
            memcmp(stack->items[i].name, scan->lexed->text + t->start, n) == 0) {
            stack->items[i].kind = kind;
            stack->items[i].line = t->line;
            return &stack->items[i];
        }
    }
    /* Not wpe_code_symbols_add: outer scopes may hold the same name. */
    if (stack->count == stack->capacity) {
        const uint32_t capacity = stack->capacity ? stack->capacity * 2 : 64;
        WPE_CodeSymbol* items = wpe_realloc(stack->items, (size_t)capacity * sizeof *items);
        if (!items) {
            scan->ok = false;
            return NULL;
        }
        stack->items = items;
        stack->capacity = capacity;
    }
    WPE_CodeSymbol* s = &stack->items[stack->count++];
    memcpy(s->name, scan->lexed->text + t->start, n);
    s->name[n] = '\0';
    s->kind = kind;
    s->line = t->line;
    s->module[0] = '\0';
    return s;
}

/* `function [name[.name]*[:name]] [<T>] (params)` from the `function` at `k`: declares a plain global name,
 * opens the body's scope with the parameters (and the method's `self`). `named_local` is the name a `local
 * function` already declared. Returns the token after the parameters. */
static uint32_t wpe__function(wpe__Scan* scan, uint32_t k, bool named_local) {
    const wpe__Lexed* lexed = scan->lexed;
    uint32_t j = k + 1;
    int64_t owner = -1;
    if (wpe__identifier(lexed, j)) {
        const uint32_t first = j++;
        bool path = false;
        while (wpe__is(lexed, j, ".") && wpe__identifier(lexed, j + 1)) {
            j += 2;
            path = true;
        }
        if (wpe__is(lexed, j, ":") && wpe__identifier(lexed, j + 1)) {
            owner = j - 1;
            j += 2;
            path = true;
        }
        if (!path && !named_local) wpe__declare(scan, first, WPE_CODE_SYMBOL_FUNCTION);
    }
    if (wpe__is(lexed, j, "<")) j = wpe__skip_type(lexed, j, lexed->toks[j].line);
    wpe__open(scan);
    if (owner >= 0) {
        const wpe__Tok* t = &lexed->toks[owner];
        const size_t n = t->end - t->start;
        if (n < WPE_CODE_SYMBOL_NAME_MAX) {
            memcpy(scan->scopes[scan->depth].self_class, lexed->text + t->start, n);
            scan->scopes[scan->depth].self_class[n] = '\0';
        }
    }
    if (!wpe__is(lexed, j, "(")) return j;
    ++j;
    while (j < lexed->count && !wpe__is(lexed, j, ")")) {
        if (wpe__identifier(lexed, j)) {
            wpe__declare(scan, j, WPE_CODE_SYMBOL_PARAMETER);
            const uint32_t line = lexed->toks[j].line;
            ++j;
            if (wpe__is(lexed, j, ":")) j = wpe__skip_type(lexed, j + 1, line);
        } else if (wpe__is(lexed, j, ",") || wpe__is(lexed, j, "...")) {
            ++j;
            if (wpe__is(lexed, j, ":")) j = wpe__skip_type(lexed, j + 1, lexed->toks[j].line);
        } else {
            break; /* Not a parameter list after all (unfinished code). */
        }
    }
    return j < lexed->count ? j + 1 : j;
}

/* `local a [: T], b ... [= values]` from the first name at `k`. Returns where the values start. */
static uint32_t wpe__local(wpe__Scan* scan, uint32_t k) {
    const wpe__Lexed* lexed = scan->lexed;
    WPE_CodeSymbol* only = NULL;
    uint32_t declared = 0;
    while (wpe__identifier(lexed, k)) {
        only = wpe__declare(scan, k, WPE_CODE_SYMBOL_LOCAL);
        ++declared;
        const uint32_t line = lexed->toks[k].line;
        ++k;
        if (wpe__is(lexed, k, "<")) k = wpe__skip_type(lexed, k, line); /* <const> */
        if (wpe__is(lexed, k, ":")) k = wpe__skip_type(lexed, k + 1, line);
        if (!wpe__is(lexed, k, ",")) break;
        ++k;
    }
    if (!wpe__is(lexed, k, "=") || declared != 1 || !only) return k;
    /* One name with a value: a function, or a module (require("path") or require "path"). */
    if (wpe__is(lexed, k + 1, "function")) only->kind = WPE_CODE_SYMBOL_FUNCTION;
    if (wpe__is(lexed, k + 1, "require")) {
        const uint32_t s = wpe__is(lexed, k + 2, "(") ? k + 3 : k + 2;
        if (s < lexed->count && lexed->toks[s].kind == WPE__TOK_STRING) {
            const wpe__Tok* t = &lexed->toks[s];
            const size_t n = t->end - t->start;
            if (n >= 2 && n - 2 < WPE_CODE_SYMBOL_PATH_MAX) {
                memcpy(only->module, lexed->text + t->start + 1, n - 2);
                only->module[n - 2] = '\0';
            }
        }
    }
    return k + 1;
}

bool wpe_code_symbols_in_scope(const char* text, size_t length, size_t cursor, WPE_CodeSymbols* out) {
    out->self_class[0] = '\0';
    if (!text) return true;
    wpe__Lexed lexed;
    wpe__Scan scan = {.lexed = &lexed, .ok = true};
    if (!wpe__lex(text, length, cursor, &lexed)) {
        wpe_free(lexed.toks);
        return false;
    }
    for (uint32_t k = 0; k < lexed.count && scan.ok;) {
        if (lexed.toks[k].kind != WPE__TOK_NAME) {
            ++k;
            continue;
        }
        if (wpe__is(&lexed, k, "local")) {
            if (wpe__is(&lexed, k + 1, "function")) {
                if (wpe__identifier(&lexed, k + 2)) wpe__declare(&scan, k + 2, WPE_CODE_SYMBOL_FUNCTION);
                k = wpe__function(&scan, k + 1, true);
            } else {
                k = wpe__local(&scan, k + 1);
            }
            continue;
        }
        if (wpe__is(&lexed, k, "function")) {
            k = wpe__function(&scan, k, false);
            continue;
        }
        if (wpe__is(&lexed, k, "for")) {
            scan.pending_count = 0;
            for (++k; wpe__identifier(&lexed, k); ++k) {
                if (scan.pending_count < WPE__PENDING_MAX) scan.pending[scan.pending_count++] = k;
                const uint32_t line = lexed.toks[k].line;
                if (wpe__is(&lexed, k + 1, ":")) k = wpe__skip_type(&lexed, k + 2, line) - 1;
                if (!wpe__is(&lexed, k + 1, ",")) break;
                ++k;
            }
            continue;
        }
        if (wpe__is(&lexed, k, "do")) {
            wpe__open(&scan);
            for (uint32_t i = 0; i < scan.pending_count; ++i)
                wpe__declare(&scan, scan.pending[i], WPE_CODE_SYMBOL_LOCAL);
            scan.pending_count = 0;
        } else if (wpe__is(&lexed, k, "then") || wpe__is(&lexed, k, "repeat")) {
            wpe__open(&scan);
        } else if (wpe__is(&lexed, k, "else")) {
            wpe__close(&scan);
            wpe__open(&scan);
        } else if (wpe__is(&lexed, k, "elseif") || wpe__is(&lexed, k, "end") || wpe__is(&lexed, k, "until")) {
            wpe__close(&scan);
        }
        ++k;
    }
    bool ok = scan.ok;
    for (uint32_t i = scan.stack.count; ok && i-- > 0;) { /* Innermost first. */
        const WPE_CodeSymbol* s = &scan.stack.items[i];
        if (wpe_code_symbols_find(out, s->name, strlen(s->name))) continue;
        ok = wpe_code_symbols_add(out, s->name, strlen(s->name), s->kind, s->line);
        if (ok) memcpy(out->items[out->count - 1].module, s->module, WPE_CODE_SYMBOL_PATH_MAX);
    }
    memcpy(out->self_class, scan.scopes[scan.depth].self_class, WPE_CODE_SYMBOL_NAME_MAX);
    wpe_free(scan.stack.items);
    wpe_free(lexed.toks);
    return ok;
}

/* ---- Members ------------------------------------------------------------------------------------------ */

/* The keys `name = ...` of the table constructor whose `{` is at `k` (only its own level). */
static bool wpe__table_keys(const wpe__Lexed* lexed, uint32_t k, WPE_CodeSymbols* out) {
    int depth = 0;
    for (; k < lexed->count; ++k) {
        if (wpe__is(lexed, k, "{") || wpe__is(lexed, k, "(") || wpe__is(lexed, k, "[")) ++depth;
        if (wpe__is(lexed, k, "}") || wpe__is(lexed, k, ")") || wpe__is(lexed, k, "]")) {
            if (--depth == 0) break;
        }
        if (depth == 1 && wpe__identifier(lexed, k) && wpe__is(lexed, k + 1, "=") &&
            (wpe__is(lexed, k - 1, "{") || wpe__is(lexed, k - 1, ",") || wpe__is(lexed, k - 1, ";"))) {
            const bool function = wpe__is(lexed, k + 2, "function");
            if (!wpe__add_tok(out, lexed, k, function ? WPE_CODE_SYMBOL_FUNCTION : WPE_CODE_SYMBOL_FIELD))
                return false;
        }
    }
    return true;
}

bool wpe_code_symbols_members(const char* text, size_t length, const char* object, size_t object_length,
                              WPE_CodeSymbols* out) {
    if (!text || !object || object_length == 0) return true;
    wpe__Lexed lexed;
    bool ok = wpe__lex(text, length, SIZE_MAX, &lexed);
    for (uint32_t k = 0; ok && k < lexed.count; ++k) {
        if (!wpe__is_name(&lexed, k, object, object_length)) continue;
        if (k > 0 && (wpe__is(&lexed, k - 1, ".") || wpe__is(&lexed, k - 1, ":"))) continue; /* x.object */
        const bool dot = wpe__is(&lexed, k + 1, "."), colon = wpe__is(&lexed, k + 1, ":");
        if ((dot || colon) && wpe__identifier(&lexed, k + 2)) {
            WPE_CodeSymbolKind kind = WPE_CODE_SYMBOL_FIELD;
            if (colon) {
                kind = WPE_CODE_SYMBOL_METHOD;
            } else if ((k > 0 && wpe__is(&lexed, k - 1, "function")) || wpe__is(&lexed, k + 3, "(") ||
                       (wpe__is(&lexed, k + 3, "=") && wpe__is(&lexed, k + 4, "function"))) {
                kind = WPE_CODE_SYMBOL_FUNCTION;
            }
            ok = wpe__add_tok(out, &lexed, k + 2, kind);
        } else if (wpe__is(&lexed, k + 1, "=") && wpe__is(&lexed, k + 2, "{")) {
            ok = wpe__table_keys(&lexed, k + 2, out);
        }
    }
    wpe_free(lexed.toks);
    return ok;
}

bool wpe_code_symbols_properties(const char* text, size_t length, const char* class_name, size_t class_length,
                                 WPE_CodeSymbols* out) {
    if (!text || !class_name || class_length == 0) return true;
    wpe__Lexed lexed;
    bool ok = wpe__lex(text, length, SIZE_MAX, &lexed);
    for (uint32_t k = 0; ok && k + 4 < lexed.count; ++k)
        if (wpe__is_name(&lexed, k, class_name, class_length) && wpe__is(&lexed, k + 1, ".") &&
            wpe__is(&lexed, k + 2, "properties") && wpe__is(&lexed, k + 3, "=") &&
            wpe__is(&lexed, k + 4, "{"))
            ok = wpe__table_keys(&lexed, k + 4, out);
    wpe_free(lexed.toks);
    return ok;
}

bool wpe_code_symbols_returned(const char* text, size_t length, char* out_name, size_t size) {
    if (size) out_name[0] = '\0';
    if (!text || size == 0) return false;
    wpe__Lexed lexed;
    bool found = false;
    if (wpe__lex(text, length, SIZE_MAX, &lexed) && lexed.count >= 2 &&
        wpe__is(&lexed, lexed.count - 2, "return") && wpe__identifier(&lexed, lexed.count - 1)) {
        const wpe__Tok* t = &lexed.toks[lexed.count - 1];
        found = (size_t)snprintf(out_name, size, "%.*s", (int)(t->end - t->start), text + t->start) < size;
    }
    wpe_free(lexed.toks);
    return found;
}

/* ---- Built-in names ----------------------------------------------------------------------------------- */

typedef struct wpe__Library {
    const char* name;
    const char* functions; /* Space-separated. */
    const char* constants;
} wpe__Library;

static const wpe__Library wpe__libraries[] = {
    {"math",
     "abs acos asin atan atan2 ceil clamp cos cosh deg exp floor fmod frexp ldexp lerp log log10 map max min "
     "modf noise pow rad random randomseed round sign sin sinh sqrt tan tanh",
     "pi huge"},
    {"string",
     "byte char find format gmatch gsub len lower match pack packsize rep reverse split sub unpack upper",
     ""},
    {"table", "clear clone concat create find freeze insert isfrozen maxn move pack remove sort unpack", ""},
    {"vector", "create magnitude normalize cross dot angle floor ceil abs sign clamp max min", "zero one"},
    {"os", "clock date difftime time", ""},
    {"coroutine", "close create isyieldable resume running status wrap yield", ""},
    {"bit32",
     "arshift band bnot bor btest bxor byteswap countlz countrz extract lrotate lshift replace rrotate "
     "rshift",
     ""},
    {"utf8", "char codepoint codes len offset", "charpattern"},
    {"buffer",
     "create fromstring tostring len copy fill readi8 readu8 readi16 readu16 readi32 readu32 readf32 readf64 "
     "writei8 writeu8 writei16 writeu16 writei32 writeu32 writef32 writef64 readstring writestring",
     ""},
};

/* Adds each word of a space-separated list. */
static bool wpe__add_words(WPE_CodeSymbols* out, const char* words, WPE_CodeSymbolKind kind) {
    for (const char* at = words; *at;) {
        const char* end = strchr(at, ' ');
        const size_t n = end ? (size_t)(end - at) : strlen(at);
        if (!wpe_code_symbols_add(out, at, n, kind, 0)) return false;
        at += n;
        while (*at == ' ')
            ++at;
    }
    return true;
}

bool wpe_code_symbols_library(const char* name, size_t length, WPE_CodeSymbols* out) {
    for (size_t i = 0; i < sizeof wpe__libraries / sizeof wpe__libraries[0]; ++i) {
        const wpe__Library* lib = &wpe__libraries[i];
        if (strlen(lib->name) != length || memcmp(lib->name, name, length) != 0) continue;
        return wpe__add_words(out, lib->functions, WPE_CODE_SYMBOL_FUNCTION) &&
               wpe__add_words(out, lib->constants, WPE_CODE_SYMBOL_CONSTANT);
    }
    return false;
}

bool wpe_code_symbols_globals(WPE_CodeSymbols* out) {
    if (!wpe_code_symbols_add(out, "wpe", 3, WPE_CODE_SYMBOL_LIBRARY, 0)) return false;
    for (size_t i = 0; i < sizeof wpe__libraries / sizeof wpe__libraries[0]; ++i)
        if (!wpe_code_symbols_add(out, wpe__libraries[i].name, strlen(wpe__libraries[i].name),
                                  WPE_CODE_SYMBOL_LIBRARY, 0))
            return false;
    if (!wpe__add_words(out,
                        "print error assert pcall xpcall pairs ipairs select tostring tonumber type typeof "
                        "require setmetatable getmetatable rawget rawset rawequal rawlen next",
                        WPE_CODE_SYMBOL_FUNCTION))
        return false;
    for (size_t i = 0; i < sizeof wpe__keywords / sizeof wpe__keywords[0]; ++i)
        if (!wpe_code_symbols_add(out, wpe__keywords[i], strlen(wpe__keywords[i]), WPE_CODE_SYMBOL_KEYWORD,
                                  0))
            return false;
    return true;
}
