#include "editor/api_docs.h"

#include <stdlib.h>
#include <string.h>

#include "core/alloc.h"
#include "editor/scripting_docs_data.h"

/* Appends text to the string pool; returns its offset, or SIZE_MAX out of room. */
typedef struct wpe__Pool {
    char* data;
    size_t used, capacity;
} wpe__Pool;

static size_t wpe__pool_add(wpe__Pool* pool, const char* text, size_t length) {
    if (pool->used + length + 1 > pool->capacity) return SIZE_MAX;
    memcpy(pool->data + pool->used, text, length);
    pool->data[pool->used + length] = '\0';
    const size_t at = pool->used;
    pool->used += length + 1;
    return at;
}

static bool wpe__name_char(char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_';
}

/* Markdown to plain text: backticks and bold marks go, links keep their text, <br> and &nbsp; become spaces,
 * "\|" becomes "|", runs of spaces collapse. Writes at most `capacity` - 1 bytes. */
static size_t wpe__plain(const char* in, size_t length, char* out, size_t capacity) {
    size_t n = 0;
    for (size_t i = 0; i < length && n + 1 < capacity;) {
        const char c = in[i];
        if (c == '`' || (c == '*' && i + 1 < length && in[i + 1] == '*')) {
            i += c == '`' ? 1 : 2;
            continue;
        }
        if (c == '\\' && i + 1 < length && in[i + 1] == '|') {
            out[n++] = '|';
            i += 2;
            continue;
        }
        if (c == '<' && i + 3 < length && strncmp(in + i, "<br>", 4) == 0) {
            if (n > 0 && out[n - 1] != ' ') out[n++] = ' ';
            i += 4;
            continue;
        }
        if (c == '&' && i + 5 < length && strncmp(in + i, "&nbsp;", 6) == 0) {
            if (n > 0 && out[n - 1] != ' ') out[n++] = ' ';
            i += 6;
            continue;
        }
        if (c == ']' && i + 1 < length && in[i + 1] == '(') { /* A link's target is dropped. */
            size_t close = i + 2;
            while (close < length && in[close] != ')')
                ++close;
            i = close < length ? close + 1 : length;
            continue;
        }
        if (c == '[') {
            ++i;
            continue;
        }
        if (c == ' ' && n > 0 && out[n - 1] == ' ') {
            ++i;
            continue;
        }
        out[n++] = c;
        ++i;
    }
    while (n > 0 && out[n - 1] == ' ')
        --n;
    out[n] = '\0';
    return n;
}

/* By name; the same name in document order (its texts were added to the pool in that order), so the first
 * row naming a function is the one kept. */
static int wpe__compare(const void* a, const void* b) {
    const WPE_ApiDoc* x = a;
    const WPE_ApiDoc* y = b;
    const int order = strcmp(x->name, y->name);
    if (order != 0) return order;
    return x->signature < y->signature ? -1 : (x->signature > y->signature ? 1 : 0);
}

/* The cells of a table row: offsets of each cell's text (between unescaped '|'). */
static uint32_t wpe__cells(const char* line, size_t length, size_t starts[4], size_t ends[4]) {
    uint32_t count = 0;
    size_t i = 1; /* After the leading '|'. */
    while (i < length && count < 4) {
        size_t end = i;
        while (end < length && !(line[end] == '|' && line[end - 1] != '\\'))
            ++end;
        if (end >= length) break;
        size_t a = i, b = end;
        while (a < b && line[a] == ' ')
            ++a;
        while (b > a && line[b - 1] == ' ')
            --b;
        starts[count] = a;
        ends[count] = b;
        ++count;
        i = end + 1;
    }
    return count;
}

bool wpe_api_docs_parse(const char* markdown, size_t length, WPE_ApiDocs* out) {
    memset(out, 0, sizeof *out);
    /* Room: every entry's texts are shorter than the document; entries are fewer than its lines. */
    uint32_t lines = 1;
    for (size_t i = 0; i < length; ++i)
        lines += markdown[i] == '\n';
    wpe__Pool pool = {.data = wpe_alloc(length * 3 + 64), .capacity = length * 3 + 64};
    WPE_ApiDoc* docs = wpe_alloc((size_t)lines * 2 * sizeof *docs);
    size_t* offsets =
        wpe_alloc((size_t)lines * 2 * 4 * sizeof *offsets); /* name, signature, summary, section */
    char* scratch = wpe_alloc(length + 1);
    if (!pool.data || !docs || !offsets || !scratch) {
        wpe_free(pool.data);
        wpe_free(docs);
        wpe_free(offsets);
        wpe_free(scratch);
        return false;
    }
    uint32_t count = 0;
    size_t section = wpe__pool_add(&pool, "", 0);
    for (size_t at = 0; at < length;) {
        size_t end = at;
        while (end < length && markdown[end] != '\n')
            ++end;
        const char* line = markdown + at;
        const size_t line_length = end - at;
        if (line_length > 4 && strncmp(line, "### ", 4) == 0) {
            section = wpe__pool_add(&pool, line + 4, line_length - 4);
        } else if (line_length > 3 && strncmp(line, "## ", 3) == 0) {
            section = wpe__pool_add(&pool, line + 3, line_length - 3);
        } else if (line_length > 8 && strncmp(line, "| `wpe.", 7) == 0) {
            size_t starts[4], ends[4];
            if (wpe__cells(line, line_length, starts, ends) >= 2) {
                const size_t summary_length =
                    wpe__plain(line + starts[1], ends[1] - starts[1], scratch, length + 1);
                const size_t summary = wpe__pool_add(&pool, scratch, summary_length);
                /* Every `wpe.name...` span of the first cell is one function. */
                for (size_t i = starts[0]; i < ends[0];) {
                    if (line[i] != '`') {
                        ++i;
                        continue;
                    }
                    size_t close = i + 1;
                    while (close < ends[0] && line[close] != '`')
                        ++close;
                    if (close >= ends[0]) break;
                    const char* span = line + i + 1;
                    const size_t span_length = close - i - 1;
                    if (span_length > 4 && strncmp(span, "wpe.", 4) == 0) {
                        size_t name_end = 4;
                        while (name_end < span_length && wpe__name_char(span[name_end]))
                            ++name_end;
                        const size_t signature_length = wpe__plain(span, span_length, scratch, length + 1);
                        const size_t name = wpe__pool_add(&pool, span + 4, name_end - 4);
                        const size_t signature = wpe__pool_add(&pool, scratch, signature_length);
                        if (name != SIZE_MAX && signature != SIZE_MAX && summary != SIZE_MAX &&
                            name_end > 4) {
                            offsets[count * 4 + 0] = name;
                            offsets[count * 4 + 1] = signature;
                            offsets[count * 4 + 2] = summary;
                            offsets[count * 4 + 3] = section;
                            ++count;
                        }
                    }
                    i = close + 1;
                }
            }
        }
        at = end + 1;
    }
    /* Pointers once the pool is final; then sorted by name, and a name documented twice keeps its first. */
    for (uint32_t i = 0; i < count; ++i)
        docs[i] = (WPE_ApiDoc){.name = pool.data + offsets[i * 4 + 0],
                               .signature = pool.data + offsets[i * 4 + 1],
                               .summary = pool.data + offsets[i * 4 + 2],
                               .section = pool.data + offsets[i * 4 + 3]};
    if (count > 1) qsort(docs, count, sizeof *docs, wpe__compare);
    uint32_t kept = 0;
    for (uint32_t i = 0; i < count; ++i)
        if (kept == 0 || strcmp(docs[kept - 1].name, docs[i].name) != 0) docs[kept++] = docs[i];
    wpe_free(offsets);
    wpe_free(scratch);
    out->docs = docs;
    out->count = kept;
    out->strings = pool.data;
    return true;
}

void wpe_api_docs_free(WPE_ApiDocs* docs) {
    if (!docs) return;
    wpe_free(docs->docs);
    wpe_free(docs->strings);
    memset(docs, 0, sizeof *docs);
}

const WPE_ApiDoc* wpe_api_docs_find(const WPE_ApiDocs* docs, const char* name, size_t length) {
    uint32_t low = 0, high = docs ? docs->count : 0;
    while (low < high) {
        const uint32_t mid = (low + high) / 2;
        const char* entry = docs->docs[mid].name;
        const int order = strncmp(entry, name, length);
        if (order == 0 && entry[length] == '\0') return &docs->docs[mid];
        if (order < 0) {
            low = mid + 1;
        } else {
            high = mid; /* Greater, or longer with `name` as its prefix: after it either way. */
        }
    }
    return NULL;
}

bool wpe_api_docs_parse_builtin(WPE_ApiDocs* out) {
    return wpe_api_docs_parse((const char*)wpe__scripting_docs, sizeof wpe__scripting_docs - 1, out);
}
