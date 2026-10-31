/*
 * The Script panel: a small code editor drawn with ImGui's draw lists over editor/code_text (the text model).
 * Reading, checking and saving go through the editor's commands.
 */
#define WPE_LOG_MODULE "editor"
#include "editor/script_panel.h"

#include <cimgui.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

#include "core/alloc.h"
#include "core/json.h"
#include "core/log.h"
#include "editor/api_docs.h"
#include "editor/code_symbols.h"
#include "editor/code_text.h"
#include "editor/editor_internal.h"

#define WPE__MAX_DOCS 16u
#define WPE__TAB_COLUMNS 4u
#define WPE__CHECK_DELAY                                                                                     \
    0.35f /* Seconds after the last keystroke before the code is compiled to check it.                       \
           */
#define WPE__MESSAGE_MAX 256u

/* ImGui colors are 0xAABBGGRR. */
#define WPE__COL(r, g, b, a)                                                                                 \
    (((uint32_t)(a) << 24) | ((uint32_t)(b) << 16) | ((uint32_t)(g) << 8) | (uint32_t)(r))

static const uint32_t wpe__token_colors[] = {[WPE_CODE_TOKEN_TEXT] = WPE__COL(224, 226, 234, 255),
                                             [WPE_CODE_TOKEN_KEYWORD] = WPE__COL(238, 153, 56, 255),
                                             [WPE_CODE_TOKEN_CONSTANT] = WPE__COL(236, 120, 110, 255),
                                             [WPE_CODE_TOKEN_NUMBER] = WPE__COL(130, 190, 240, 255),
                                             [WPE_CODE_TOKEN_STRING] = WPE__COL(150, 205, 120, 255),
                                             [WPE_CODE_TOKEN_COMMENT] = WPE__COL(112, 120, 134, 255),
                                             [WPE_CODE_TOKEN_LIBRARY] = WPE__COL(110, 205, 215, 255)};

typedef struct wpe__Doc {
    char path[WPE_VFS_PATH_MAX];
    WPE_CodeText text;
    uint8_t* states; /* The tokenizer's state at each line's start (for long comments and strings). */
    uint32_t state_capacity;
    uint64_t states_version; /* The text version they were computed for (0: never). */
    uint32_t widest;         /* Columns of the widest line (for the horizontal scroll). */
    /* Compile check: run WPE__CHECK_DELAY after the last change. */
    uint64_t checked_version;
    float check_wait;
    uint32_t error_line; /* 1-based; 0: compiles. */
    char error[WPE__MESSAGE_MAX];
    /* The last runtime error the log reported in this file (cleared on save). */
    uint32_t runtime_line;
    char runtime_error[WPE__MESSAGE_MAX];
    int64_t scroll_to_line; /* A line to bring into view next frame (0-based), or -1. */
    bool follow_cursor;     /* Keep the cursor in view after this frame's keys. */
} wpe__Doc;

#define WPE__COMPLETION_MAX 64u /* Suggestions listed at once. */
#define WPE__API_NAME_MAX 48u

/* A name of the `wpe` table, as the running VM has it. */
typedef struct wpe__ApiName {
    char name[WPE__API_NAME_MAX];
    bool function;
} wpe__ApiName;

struct WPE_ScriptPanel {
    WPE_Editor* editor;
    WPE_ApiDocs api_docs; /* Signatures and descriptions from docs/scripting.md. */
    wpe__ApiName* api;    /* The wpe table's names (read once from the script host). */
    uint32_t api_count;
    bool api_read;
    /* Completion: what is being completed, its candidates, the shown matches (indices into the candidates),
     * the highlighted one; `exact`: what is typed is already a whole candidate (Enter then starts a line). */
    bool completing;
    WPE_CodeCompletion context;
    bool api_list; /* The candidates are the wpe table's (their help comes from the docs). */
    WPE_CodeSymbols candidates;
    bool exact;
    uint32_t matches[WPE__COMPLETION_MAX];
    uint32_t match_count;
    uint32_t selected;
    uint32_t list_first; /* The first suggestion shown (the wheel scrolls the list). */
    bool list_follow;    /* Scroll the list to the highlighted entry (after the keyboard moved it). */
    float cursor_x, cursor_y, line_h; /* Where the cursor was drawn (screen), for the popups. */
    wpe__Doc* docs[WPE__MAX_DOCS];
    uint32_t count;
    int active;         /* Index of the shown tab, or -1. */
    int select_tab;     /* A tab to bring to the front next frame, or -1. */
    bool focus_code;    /* Give the code the keyboard next frame. */
    bool focused;       /* The code had the keyboard this frame. */
    bool dragging;      /* A mouse selection is being dragged. */
    bool show_window;   /* Bring the panel to the front next frame. */
    bool new_requested; /* The New script button was pressed. */
    uint64_t log_seen;  /* Log lines already scanned for runtime errors. */
    /* Find bar. */
    bool find_open, find_focus;
    char find[128];
    /* Prompts: closing one unsaved tab, and every unsaved one when the editor quits. */
    int closing; /* The tab asked about, or -1. */
    bool close_all_asked, close_all_popup;
    int close_all_result;
};

/* ---- Commands ------------------------------------------------------------------------------------------ */

/* Runs a command and parses the reply; *out_result is its "result" (NULL when the command failed). The
 * document must be freed. */
static WPE_JsonDocument* wpe__run(WPE_ScriptPanel* panel, WPE_JsonWriter* command,
                                  const WPE_JsonValue** out_result) {
    *out_result = NULL;
    if (!command) return NULL;
    wpe_json_write_object_end(command);
    const char* text = NULL;
    WPE_JsonDocument* doc = NULL;
    WPE_JsonWriter* reply = NULL;
    if (wpe_json_writer_finish(command, &text, NULL) == WPE_RESULT_OK &&
        wpe_json_writer_create(&reply) == WPE_RESULT_OK) {
        const WPE_Result result = wpe_editor_execute(panel->editor, text, strlen(text), reply);
        const char* reply_text = NULL;
        size_t size = 0;
        if (wpe_json_writer_finish(reply, &reply_text, &size) == WPE_RESULT_OK &&
            wpe_json_parse(reply_text, size, &doc, NULL) == WPE_RESULT_OK && result == WPE_RESULT_OK)
            *out_result = wpe_json_get(wpe_json_root(doc), "result");
    }
    wpe_json_writer_destroy(reply);
    wpe_json_writer_destroy(command);
    return doc;
}

static WPE_JsonWriter* wpe__command(const char* name) {
    WPE_JsonWriter* w = NULL;
    if (wpe_json_writer_create(&w) != WPE_RESULT_OK) return NULL;
    wpe_json_writer_set_compact(w, true);
    wpe_json_write_object_begin(w);
    wpe_json_write_key(w, "cmd");
    wpe_json_write_string(w, name);
    return w;
}

static bool wpe__save(WPE_ScriptPanel* panel, wpe__Doc* doc) {
    WPE_JsonWriter* w = wpe__command("write_file");
    if (!w) return false;
    wpe_json_write_key(w, "path");
    wpe_json_write_string(w, doc->path);
    wpe_json_write_key(w, "text");
    wpe_json_write_string(w, doc->text.text);
    const WPE_JsonValue* result = NULL;
    WPE_JsonDocument* reply = wpe__run(panel, w, &result);
    const bool ok = result != NULL;
    wpe_json_free(reply);
    if (ok) {
        wpe_code_text_mark_saved(&doc->text);
        doc->runtime_line = 0; /* The old run's error belongs to the old code. */
        doc->runtime_error[0] = '\0';
    }
    return ok;
}

/* Compiles the text to check it (the check_script command). */
static void wpe__check(WPE_ScriptPanel* panel, wpe__Doc* doc) {
    doc->checked_version = doc->text.version;
    WPE_JsonWriter* w = wpe__command("check_script");
    if (!w) return;
    wpe_json_write_key(w, "text");
    wpe_json_write_string(w, doc->text.text);
    const WPE_JsonValue* result = NULL;
    WPE_JsonDocument* reply = wpe__run(panel, w, &result);
    if (result) {
        const bool ok = wpe_json_bool(wpe_json_get(result, "ok"), true);
        doc->error_line = ok ? 0 : (uint32_t)wpe_json_number(wpe_json_get(result, "line"), 0.0);
        const char* message = wpe_json_string(wpe_json_get(result, "message"));
        snprintf(doc->error, sizeof doc->error, "%s", ok || !message ? "" : message);
    }
    wpe_json_free(reply);
}

/* ---- Documents ---------------------------------------------------------------------------------------- */

static void wpe__free_doc(wpe__Doc* doc) {
    if (!doc) return;
    wpe_code_text_free(&doc->text);
    wpe_free(doc->states);
    wpe_free(doc);
}

static int wpe__find_doc(const WPE_ScriptPanel* panel, const char* path) {
    for (uint32_t i = 0; i < panel->count; ++i)
        if (strcmp(panel->docs[i]->path, path) == 0) return (int)i;
    return -1;
}

static void wpe__close_doc(WPE_ScriptPanel* panel, int index) {
    if (index < 0 || (uint32_t)index >= panel->count) return;
    wpe__free_doc(panel->docs[index]);
    memmove(&panel->docs[index], &panel->docs[index + 1],
            (panel->count - (uint32_t)index - 1) * sizeof *panel->docs);
    --panel->count;
    if (panel->active >= (int)panel->count) panel->active = (int)panel->count - 1;
}

bool wpe_script_panel_open(WPE_ScriptPanel* panel, const char* path, uint32_t line, uint32_t column) {
    if (!panel || !path) return false;
    int index = wpe__find_doc(panel, path);
    if (index < 0) {
        if (panel->count == WPE__MAX_DOCS) {
            wpe_log_warn("the Script panel has %u files open: close one first", WPE__MAX_DOCS);
            return false;
        }
        WPE_JsonWriter* w = wpe__command("read_file");
        if (!w) return false;
        wpe_json_write_key(w, "path");
        wpe_json_write_string(w, path);
        const WPE_JsonValue* result = NULL;
        WPE_JsonDocument* reply = wpe__run(panel, w, &result);
        const char* text = wpe_json_string(wpe_json_get(result, "text"));
        wpe__Doc* doc = text ? wpe_alloc_zeroed(1, sizeof *doc) : NULL;
        if (doc) {
            snprintf(doc->path, sizeof doc->path, "%s", path);
            wpe_code_text_init(&doc->text);
            if (!wpe_code_text_set(&doc->text, text, strlen(text))) {
                wpe__free_doc(doc);
                doc = NULL;
            }
        }
        wpe_json_free(reply);
        if (!doc) {
            wpe_log_error("cannot open '%s' in the Script panel", path);
            return false;
        }
        doc->scroll_to_line = -1;
        panel->docs[panel->count] = doc;
        index = (int)panel->count++;
        wpe__check(panel, doc);
    }
    wpe__Doc* doc = panel->docs[index];
    if (line > 0) {
        const uint32_t target = line - 1 < doc->text.line_count ? line - 1 : doc->text.line_count - 1;
        wpe_code_text_set_cursor(&doc->text,
                                 wpe_code_text_offset(&doc->text, target, column ? column - 1 : 0), false);
        /* Without a column: from the line's start, Home goes to its first non-blank character. */
        if (!column && doc->text.cursor != doc->text.length && doc->text.text[doc->text.cursor] != '\n')
            wpe_code_text_move(&doc->text, WPE_CODE_HOME, false, 0);
        doc->scroll_to_line = target;
    }
    panel->active = panel->select_tab = index;
    panel->focus_code = true;
    panel->show_window = true;
    return true;
}

/* ---- Lifecycle --------------------------------------------------------------------------------------------
 */

WPE_ScriptPanel* wpe_script_panel_create(WPE_Editor* editor) {
    WPE_ScriptPanel* panel = wpe_alloc_zeroed(1, sizeof *panel);
    if (!panel) return NULL;
    panel->editor = editor;
    panel->active = panel->select_tab = panel->closing = -1;
    panel->log_seen = wpe__editor_log_position(editor);
    if (!wpe_api_docs_parse_builtin(&panel->api_docs))
        wpe_log_warn("the Script panel has no API help (out of memory)");
    return panel;
}

void wpe_script_panel_destroy(WPE_ScriptPanel* panel) {
    if (!panel) return;
    for (uint32_t i = 0; i < panel->count; ++i)
        wpe__free_doc(panel->docs[i]);
    wpe_api_docs_free(&panel->api_docs);
    wpe_free(panel->api);
    wpe_code_symbols_free(&panel->candidates);
    wpe_free(panel);
}

bool wpe_script_panel_focused(const WPE_ScriptPanel* panel) {
    return panel && panel->focused;
}

bool wpe_script_panel_take_new_request(WPE_ScriptPanel* panel) {
    if (!panel || !panel->new_requested) return false;
    panel->new_requested = false;
    return true;
}

bool wpe_script_panel_dirty(const WPE_ScriptPanel* panel) {
    for (uint32_t i = 0; panel && i < panel->count; ++i)
        if (wpe_code_text_dirty(&panel->docs[i]->text)) return true;
    return false;
}

void wpe_script_panel_request_close_all(WPE_ScriptPanel* panel) {
    if (!panel) return;
    panel->close_all_asked = true;
    panel->close_all_popup = true;
    panel->close_all_result = 0;
}

int wpe_script_panel_close_all_result(WPE_ScriptPanel* panel) {
    if (!panel) return 0;
    const int result = panel->close_all_result;
    panel->close_all_result = 0;
    return result;
}

/* ---- Runtime errors from the log --------------------------------------------------------------------------
 */

/* Finds "<path>:<line>:" in an error line and records it on that document. */
static void wpe__scan_line(const wpe__LogLine* line, void* user_data) {
    WPE_ScriptPanel* panel = user_data;
    if (line->level < WPE_LOG_LEVEL_ERROR) return;
    for (uint32_t i = 0; i < panel->count; ++i) {
        wpe__Doc* doc = panel->docs[i];
        const size_t path_length = strlen(doc->path);
        for (const char* at = strstr(line->text, doc->path); at; at = strstr(at + 1, doc->path)) {
            const char* p = at + path_length;
            if (*p != ':' || p[1] < '0' || p[1] > '9') continue;
            uint32_t number = 0;
            ++p;
            while (*p >= '0' && *p <= '9')
                number = number * 10u + (uint32_t)(*p++ - '0');
            if (*p != ':') continue;
            ++p;
            while (*p == ' ')
                ++p;
            const char* end = strchr(p, '\n');
            doc->runtime_line = number;
            snprintf(doc->runtime_error, sizeof doc->runtime_error, "%.*s",
                     (int)(end ? end - p : (ptrdiff_t)strlen(p)), p);
            break;
        }
    }
}

/* ---- Drawing helpers --------------------------------------------------------------------------------------
 */

/* The visual column (tabs expanded) at byte `offset` of a line. */
static uint32_t wpe__visual_column(const char* line, size_t offset) {
    uint32_t column = 0;
    for (size_t i = 0; i < offset; ++i) {
        if (((unsigned char)line[i] & 0xC0u) == 0x80u) continue;
        column = line[i] == '\t' ? (column / WPE__TAB_COLUMNS + 1) * WPE__TAB_COLUMNS : column + 1;
    }
    return column;
}

/* The byte offset in a line nearest to a visual column. */
static size_t wpe__offset_at_column(const char* line, size_t length, float column) {
    uint32_t at = 0;
    for (size_t i = 0; i < length;) {
        size_t next = i + 1;
        while (next < length && ((unsigned char)line[next] & 0xC0u) == 0x80u)
            ++next;
        const uint32_t after = line[i] == '\t' ? (at / WPE__TAB_COLUMNS + 1) * WPE__TAB_COLUMNS : at + 1;
        if (column < ((float)at + (float)after) * 0.5f) return i;
        at = after;
        i = next;
    }
    return length;
}

/* Recomputes each line's starting tokenizer state and the widest line, when the text changed. */
static void wpe__update_states(wpe__Doc* doc) {
    const WPE_CodeText* t = &doc->text;
    if (doc->states_version == t->version && doc->states) return;
    if (t->line_count > doc->state_capacity) {
        uint8_t* states = wpe_realloc(doc->states, t->line_count);
        if (!states) return;
        doc->states = states;
        doc->state_capacity = t->line_count;
    }
    if (!doc->states) return;
    uint8_t state = 0;
    uint32_t widest = 0;
    WPE_CodeToken tokens[2];
    for (uint32_t i = 0; i < t->line_count; ++i) {
        doc->states[i] = state;
        size_t length = 0;
        const char* line = wpe_code_text_line(t, i, &length);
        uint32_t count = 0;
        state = wpe_code_tokenize(line, length, state, tokens, 2, &count);
        const uint32_t columns = wpe__visual_column(line, length);
        if (columns > widest) widest = columns;
    }
    doc->widest = widest;
    doc->states_version = t->version;
}

/* Draws a run of a line from visual column `column`, expanding tabs; returns the column after it. */
static uint32_t wpe__draw_run(ImDrawList* draw, float x, float y, float char_w, const char* text,
                              size_t length, uint32_t column, uint32_t color) {
    size_t start = 0;
    for (size_t i = 0; i <= length; ++i) {
        if (i < length && text[i] != '\t') continue;
        if (i > start) {
            ImDrawList_AddText_Vec2(draw, (ImVec2_c){x + (float)column * char_w, y}, color, text + start,
                                    text + i);
            for (size_t k = start; k < i; ++k)
                column += ((unsigned char)text[k] & 0xC0u) != 0x80u;
        }
        if (i < length) column = (column / WPE__TAB_COLUMNS + 1) * WPE__TAB_COLUMNS;
        start = i + 1;
    }
    return column;
}

static void wpe__copy_selection(const WPE_CodeText* t) {
    size_t start, end;
    wpe_code_text_selection(t, &start, &end);
    if (start == end) return;
    char* copy = wpe_alloc(end - start + 1);
    if (!copy) return;
    memcpy(copy, t->text + start, end - start);
    copy[end - start] = '\0';
    igSetClipboardText(copy);
    wpe_free(copy);
}

/* Selects the next (or previous) match of the find text. */
static void wpe__find_next(WPE_ScriptPanel* panel, wpe__Doc* doc, bool forward) {
    if (!panel->find[0]) return;
    size_t start, end;
    wpe_code_text_selection(&doc->text, &start, &end);
    const size_t at = wpe_code_text_find(&doc->text, panel->find, forward ? end : start, forward);
    if (at == SIZE_MAX) return;
    wpe_code_text_set_cursor(&doc->text, at, false);
    wpe_code_text_set_cursor(&doc->text, at + strlen(panel->find), true);
    doc->follow_cursor = true;
}

/* ---- Keyboard --------------------------------------------------------------------------------------------
 */

/* Completion and API help. */

static void wpe__collect_api(const char* name, bool function, void* user_data) {
    WPE_ScriptPanel* panel = user_data;
    if (strlen(name) >= WPE__API_NAME_MAX) return;
    wpe__ApiName* api = wpe_realloc(panel->api, (size_t)(panel->api_count + 1) * sizeof *api);
    if (!api) return;
    panel->api = api;
    snprintf(api[panel->api_count].name, WPE__API_NAME_MAX, "%s", name);
    api[panel->api_count].function = function;
    ++panel->api_count;
}

/* The wpe table's names, read from the running script host the first time they are needed. */
static void wpe__read_api(WPE_ScriptPanel* panel) {
    if (panel->api_read) return;
    panel->api_read = true;
    wpe_script_host_list_api(wpe_engine_scripts(panel->editor->engine), wpe__collect_api, panel);
}

static bool wpe__ident(char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_';
}

/* A project text file through the read_file command (NULL when it can't be read); free it. */
static char* wpe__read_text(WPE_ScriptPanel* panel, const char* path) {
    WPE_JsonWriter* w = wpe__command("read_file");
    if (!w) return NULL;
    wpe_json_write_key(w, "path");
    wpe_json_write_string(w, path);
    const WPE_JsonValue* result = NULL;
    WPE_JsonDocument* reply = wpe__run(panel, w, &result);
    const char* text = wpe_json_string(wpe_json_get(result, "text"));
    char* copy = NULL;
    if (text) {
        const size_t n = strlen(text);
        copy = wpe_alloc(n + 1);
        if (copy) memcpy(copy, text, n + 1);
    }
    wpe_json_free(reply);
    return copy;
}

/* The members of a `require`d module: the fields and functions of the table its file returns. */
static bool wpe__module_members(WPE_ScriptPanel* panel, const char* module, WPE_CodeSymbols* out) {
    char path[WPE_VFS_PATH_MAX];
    const size_t n = strlen(module);
    snprintf(path, sizeof path, "%s%s", module, n > 5 && strcmp(module + n - 5, ".luau") == 0 ? "" : ".luau");
    char* text = wpe__read_text(panel, path);
    if (!text) return true;
    char name[WPE_CODE_SYMBOL_NAME_MAX];
    bool ok = true;
    if (wpe_code_symbols_returned(text, strlen(text), name, sizeof name))
        ok = wpe_code_symbols_members(text, strlen(text), name, strlen(name), out);
    wpe_free(text);
    return ok;
}

/* Fills the candidates for the completion context: the wpe table's names after "wpe."; self's properties,
 * fields and methods after "self."; a library's, a module's or a local table's members after "name."; and
 * otherwise the names in scope, the globals and the keywords. */
static bool wpe__collect_candidates(WPE_ScriptPanel* panel, const wpe__Doc* doc) {
    const WPE_CodeText* t = &doc->text;
    const WPE_CodeCompletion* c = &panel->context;
    WPE_CodeSymbols* out = &panel->candidates;
    out->count = 0;
    panel->api_list = false;
    if (c->kind == WPE_CODE_COMPLETE_NAME) {
        if (!wpe_code_symbols_in_scope(t->text, t->length, t->cursor, out)) return false;
        if (out->self_class[0] && !wpe_code_symbols_add(out, "self", 4, WPE_CODE_SYMBOL_LOCAL, 0))
            return false;
        return wpe_code_symbols_globals(out);
    }
    const char* object = t->text + c->object_start;
    const size_t length = c->object_length;
    if (length == 3 && memcmp(object, "wpe", 3) == 0) {
        wpe__read_api(panel);
        panel->api_list = true;
        for (uint32_t i = 0; i < panel->api_count; ++i) {
            const WPE_CodeSymbolKind kind =
                panel->api[i].function ? WPE_CODE_SYMBOL_FUNCTION : WPE_CODE_SYMBOL_CONSTANT;
            if (!wpe_code_symbols_add(out, panel->api[i].name, strlen(panel->api[i].name), kind, 0))
                return false;
        }
        return true;
    }
    if (wpe_code_symbols_library(object, length, out)) return true;
    /* The members are read from the file without the separator and what is typed after it: the unfinished
     * name isn't a member yet, and neither is what happens to follow on the next line. */
    const size_t cut_start = c->object_start + c->object_length;
    const size_t text_length = t->length - (t->cursor - cut_start);
    char* text = wpe_alloc(text_length + 1);
    if (!text) return false;
    memcpy(text, t->text, cut_start);
    memcpy(text + cut_start, t->text + t->cursor, t->length - t->cursor);
    text[text_length] = '\0';
    WPE_CodeSymbols scope = {0};
    bool ok = wpe_code_symbols_in_scope(t->text, t->length, t->cursor, &scope);
    if (length == 4 && memcmp(object, "self", 4) == 0) {
        /* An instance: its entity, the class's declared properties, what the file does with self, and the
         * class's own functions. */
        const char* cls = scope.self_class;
        ok = ok && wpe_code_symbols_add(out, "entity", 6, WPE_CODE_SYMBOL_FIELD, 0) &&
             wpe_code_symbols_properties(text, text_length, cls, strlen(cls), out) &&
             wpe_code_symbols_members(text, text_length, "self", 4, out) &&
             wpe_code_symbols_members(text, text_length, cls, strlen(cls), out);
    } else {
        ok = ok && wpe_code_symbols_members(text, text_length, object, length, out);
        const WPE_CodeSymbol* local = wpe_code_symbols_find(&scope, object, length);
        if (ok && local && local->module[0]) ok = wpe__module_members(panel, local->module, out);
    }
    wpe_code_symbols_free(&scope);
    wpe_free(text);
    return ok;
}

static bool wpe__starts_with(const char* name, const char* prefix, size_t length) {
    for (size_t i = 0; i < length; ++i)
        if (name[i] == '\0' || (name[i] | 0x20) != (prefix[i] | 0x20)) return false;
    return true;
}

static bool wpe__contains(const char* name, const char* part, size_t length) {
    for (const char* at = name; *at; ++at)
        if (wpe__starts_with(at, part, length)) return true;
    return false;
}

/* The suggestions for what is typed: candidates that start with it first, then those containing it. */
static void wpe__update_matches(WPE_ScriptPanel* panel, const wpe__Doc* doc) {
    panel->match_count = 0;
    panel->exact = false;
    if (!wpe__collect_candidates(panel, doc)) wpe_log_warn("completion: out of memory");
    const WPE_CodeSymbols* candidates = &panel->candidates;
    const char* typed = doc->text.text + panel->context.start;
    const size_t length = doc->text.cursor - panel->context.start;
    for (int pass = 0; pass < 2; ++pass) {
        for (uint32_t i = 0; i < candidates->count && panel->match_count < WPE__COMPLETION_MAX; ++i) {
            const WPE_CodeSymbol* s = &candidates->items[i];
            /* After ":" only functions can be called. */
            if (panel->context.separator == ':' && s->kind != WPE_CODE_SYMBOL_METHOD &&
                s->kind != WPE_CODE_SYMBOL_FUNCTION)
                continue;
            const bool starts = wpe__starts_with(s->name, typed, length);
            if (pass == 0 ? starts : (!starts && length > 0 && wpe__contains(s->name, typed, length)))
                panel->matches[panel->match_count++] = i;
            if (pass == 0 && strlen(s->name) == length && memcmp(s->name, typed, length) == 0)
                panel->exact = true;
        }
    }
    /* Exactly what is typed: nothing left to suggest. */
    if (panel->match_count == 1 && panel->exact) panel->match_count = 0;
    if (panel->match_count == 0) panel->completing = false;
    if (panel->selected >= panel->match_count) panel->selected = 0;
    panel->list_follow = true; /* New matches: show the highlighted one. */
}

/* Puts the highlighted suggestion in place of what was typed (with "(" after a function). */
static void wpe__accept(WPE_ScriptPanel* panel, wpe__Doc* doc) {
    if (!panel->completing || panel->match_count == 0) return;
    const WPE_CodeSymbol* s = &panel->candidates.items[panel->matches[panel->selected]];
    WPE_CodeText* t = &doc->text;
    size_t end = t->cursor;
    while (end < t->length && wpe__ident(t->text[end])) /* The rest of a name the cursor is inside. */
        ++end;
    wpe_code_text_set_cursor(t, panel->context.start, false);
    wpe_code_text_set_cursor(t, end, true);
    wpe_code_text_insert(t, s->name, strlen(s->name));
    const bool call = s->kind == WPE_CODE_SYMBOL_FUNCTION || s->kind == WPE_CODE_SYMBOL_METHOD;
    if (call && (t->cursor >= t->length || t->text[t->cursor] != '(')) wpe_code_text_insert(t, "(", 1);
    panel->completing = false;
    doc->check_wait = WPE__CHECK_DELAY;
    doc->follow_cursor = true;
}

/* The documented `wpe.` function at `offset` (in a name preceded by "wpe."), or NULL. */
static const WPE_ApiDoc* wpe__doc_at(const WPE_ScriptPanel* panel, const WPE_CodeText* t, size_t offset) {
    size_t start = offset, end = offset;
    while (start > 0 && wpe__ident(t->text[start - 1]))
        --start;
    while (end < t->length && wpe__ident(t->text[end]))
        ++end;
    if (end == start || start < 4 || memcmp(t->text + start - 4, "wpe.", 4) != 0) return NULL;
    return wpe_api_docs_find(&panel->api_docs, t->text + start, end - start);
}

/* The `wpe.` function whose call's parentheses the cursor is inside (on its line), or NULL. */
static const WPE_ApiDoc* wpe__call_at_cursor(const WPE_ScriptPanel* panel, const WPE_CodeText* t) {
    const size_t line_start = t->line_starts[wpe_code_text_line_of(t, t->cursor)];
    int depth = 0;
    for (size_t i = t->cursor; i > line_start; --i) {
        const char c = t->text[i - 1];
        if (c == ')') {
            ++depth;
        } else if (c == '(' && depth-- == 0) {
            return i - 1 > line_start ? wpe__doc_at(panel, t, i - 2) : NULL;
        }
    }
    return NULL;
}

/* A help box: the signature in the accent color, then the description, wrapped. */
static void wpe__doc_text(const WPE_ApiDoc* doc) {
    igPushTextWrapPos(igGetFontSize() * 34.0f);
    igTextColored((ImVec4_c){0.93f, 0.60f, 0.22f, 1.0f}, "%s", doc->signature);
    igTextUnformatted(doc->summary, NULL);
    if (doc->section[0]) igTextDisabled("%s (docs/scripting.md)", doc->section);
    igPopTextWrapPos();
}

/* The popups' look: lighter than the code, with a border. Pair with wpe__pop_style(). */
static void wpe__push_style(void) {
    igPushStyleColor_U32(ImGuiCol_WindowBg, WPE__COL(34, 37, 45, 250));
    igPushStyleColor_U32(ImGuiCol_Border, WPE__COL(238, 153, 56, 120));
    igPushStyleVar_Float(ImGuiStyleVar_WindowBorderSize, 1.0f);
    igPushStyleVar_Vec2(ImGuiStyleVar_WindowPadding, (ImVec2_c){8.0f, 6.0f});
}

static void wpe__pop_style(void) {
    igPopStyleVar(2);
    igPopStyleColor(2);
}

static const ImGuiWindowFlags wpe__popup_flags =
    ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings |
    ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav;

/* The suggestion list under the cursor, with the highlighted entry's help beside it. */
static void wpe__completion_popup(WPE_ScriptPanel* panel, wpe__Doc* doc) {
    if (!panel->completing || panel->match_count == 0) return;
    igSetNextWindowPos((ImVec2_c){panel->cursor_x, panel->cursor_y + panel->line_h}, 0, (ImVec2_c){0, 0});
    wpe__push_style();
    if (igBegin("##wpe_completion", NULL, wpe__popup_flags)) {
        /* Twelve at a time: the wheel scrolls them, the keyboard brings the highlighted one into view. */
        const uint32_t shown = panel->match_count < 12 ? panel->match_count : 12;
        const uint32_t last_first = panel->match_count - shown;
        if (panel->list_follow) {
            if (panel->selected < panel->list_first) panel->list_first = panel->selected;
            if (panel->selected >= panel->list_first + shown) panel->list_first = panel->selected - shown + 1;
            panel->list_follow = false;
        }
        const float wheel = igGetIO_Nil()->MouseWheel;
        if (wheel != 0.0f && igIsWindowHovered(ImGuiHoveredFlags_ChildWindows)) {
            const int64_t moved = (int64_t)panel->list_first - (int64_t)(wheel * 3.0f);
            panel->list_first = moved < 0 ? 0 : (uint32_t)moved;
        }
        if (panel->list_first > last_first) panel->list_first = last_first;
        uint32_t help_index = panel->selected; /* The hovered entry's help, else the highlighted one's. */
        igBeginGroup();
        for (uint32_t i = panel->list_first; i < panel->list_first + shown && i < panel->match_count; ++i) {
            const WPE_CodeSymbol* s = &panel->candidates.items[panel->matches[i]];
            const bool call = s->kind == WPE_CODE_SYMBOL_FUNCTION || s->kind == WPE_CODE_SYMBOL_METHOD;
            char label[WPE_CODE_SYMBOL_NAME_MAX + 16];
            snprintf(label, sizeof label, "%s%s##%u", s->name, call ? "()" : "", i);
            if (igSelectable_Bool(label, i == panel->selected, 0, (ImVec2_c){200, 0})) {
                panel->selected = i;
                wpe__accept(panel, doc);
                panel->focus_code = true;
            }
            if (igIsItemHovered(0)) help_index = i;
        }
        if (panel->match_count > shown)
            igTextDisabled("%u-%u of %u (wheel scrolls)", panel->list_first + 1, panel->list_first + shown,
                           panel->match_count);
        igEndGroup();
        if (panel->completing && help_index < panel->match_count) {
            const WPE_CodeSymbol* s = &panel->candidates.items[panel->matches[help_index]];
            const WPE_ApiDoc* help =
                panel->api_list ? wpe_api_docs_find(&panel->api_docs, s->name, strlen(s->name)) : NULL;
            igSameLine(0, -1);
            igBeginGroup();
            if (help) {
                wpe__doc_text(help);
            } else {
                static const char* const kinds[] = {
                    [WPE_CODE_SYMBOL_LOCAL] = "local",       [WPE_CODE_SYMBOL_PARAMETER] = "parameter",
                    [WPE_CODE_SYMBOL_FUNCTION] = "function", [WPE_CODE_SYMBOL_METHOD] = "method",
                    [WPE_CODE_SYMBOL_FIELD] = "field",       [WPE_CODE_SYMBOL_CONSTANT] = "constant",
                    [WPE_CODE_SYMBOL_LIBRARY] = "library",   [WPE_CODE_SYMBOL_KEYWORD] = "keyword"};
                const WPE_CodeCompletion* c = &panel->context;
                if (c->kind == WPE_CODE_COMPLETE_MEMBER) {
                    igTextColored((ImVec4_c){0.93f, 0.60f, 0.22f, 1.0f}, "%.*s%c%s", (int)c->object_length,
                                  doc->text.text + c->object_start, c->separator, s->name);
                } else {
                    igTextColored((ImVec4_c){0.93f, 0.60f, 0.22f, 1.0f}, "%s", s->name);
                }
                if (strcmp(s->name, "self") == 0 && panel->candidates.self_class[0]) {
                    igTextDisabled("this instance of %s", panel->candidates.self_class);
                } else if (s->line) {
                    igTextDisabled("%s, line %u", kinds[s->kind], s->line);
                } else {
                    igTextDisabled("%s", kinds[s->kind]);
                }
                if (s->module[0]) igTextDisabled("require(\"%s\")", s->module);
            }
            igEndGroup();
        }
    }
    igEnd();
    wpe__pop_style();
}

/* The signature of the call the cursor is in, just above its line. */
static void wpe__signature_popup(WPE_ScriptPanel* panel, const wpe__Doc* doc) {
    if (panel->completing || !panel->focused) return;
    const WPE_ApiDoc* help = wpe__call_at_cursor(panel, &doc->text);
    if (!help) return;
    igSetNextWindowPos((ImVec2_c){panel->cursor_x, panel->cursor_y - 4.0f}, 0, (ImVec2_c){0.0f, 1.0f});
    wpe__push_style();
    if (igBegin("##wpe_signature", NULL, wpe__popup_flags | ImGuiWindowFlags_NoInputs))
        igTextColored((ImVec4_c){0.93f, 0.60f, 0.22f, 1.0f}, "%s", help->signature);
    igEnd();
    wpe__pop_style();
}

/* Keyboard. */

static bool wpe__key(ImGuiKey key) {
    return igIsKeyPressed_Bool(key, true);
}

static void wpe__keyboard(WPE_ScriptPanel* panel, wpe__Doc* doc, uint32_t page) {
    ImGuiIO* io = igGetIO_Nil();
    WPE_CodeText* t = &doc->text;
    const bool ctrl = io->KeyCtrl, shift = io->KeyShift;
    const uint64_t before = t->version;
    const size_t cursor_before = t->cursor;
    /* The suggestion list takes up/down, Enter, Tab and Esc while it is open. When what is typed is already
     * a whole name (`end`, a local), Enter starts a new line instead; Tab still takes the suggestion. */
    const bool listing = panel->completing && panel->match_count > 0;
    const bool enter = wpe__key(ImGuiKey_Enter) || wpe__key(ImGuiKey_KeypadEnter);
    const bool enter_accepts = listing && !panel->exact;
    if (listing) {
        if (wpe__key(ImGuiKey_DownArrow) || wpe__key(ImGuiKey_UpArrow)) panel->list_follow = true;
        if (wpe__key(ImGuiKey_DownArrow)) panel->selected = (panel->selected + 1) % panel->match_count;
        if (wpe__key(ImGuiKey_UpArrow))
            panel->selected = (panel->selected + panel->match_count - 1) % panel->match_count;
        if ((enter && enter_accepts) || wpe__key(ImGuiKey_Tab)) wpe__accept(panel, doc);
        if (igIsKeyPressed_Bool(ImGuiKey_Escape, false) || (enter && !enter_accepts))
            panel->completing = false;
    }
    static const struct {
        ImGuiKey key;
        WPE_CodeMove plain, with_ctrl;
    } moves[] = {{ImGuiKey_LeftArrow, WPE_CODE_LEFT, WPE_CODE_WORD_LEFT},
                 {ImGuiKey_RightArrow, WPE_CODE_RIGHT, WPE_CODE_WORD_RIGHT},
                 {ImGuiKey_UpArrow, WPE_CODE_UP, WPE_CODE_UP},
                 {ImGuiKey_DownArrow, WPE_CODE_DOWN, WPE_CODE_DOWN},
                 {ImGuiKey_Home, WPE_CODE_HOME, WPE_CODE_START},
                 {ImGuiKey_End, WPE_CODE_END, WPE_CODE_FINISH},
                 {ImGuiKey_PageUp, WPE_CODE_PAGE_UP, WPE_CODE_PAGE_UP},
                 {ImGuiKey_PageDown, WPE_CODE_PAGE_DOWN, WPE_CODE_PAGE_DOWN}};
    for (size_t i = 0; i < sizeof moves / sizeof moves[0]; ++i) {
        const bool vertical = moves[i].key == ImGuiKey_UpArrow || moves[i].key == ImGuiKey_DownArrow;
        if (wpe__key(moves[i].key) && !(listing && vertical))
            wpe_code_text_move(t, ctrl ? moves[i].with_ctrl : moves[i].plain, shift, page);
    }
    if (enter && !enter_accepts) wpe_code_text_newline(t);
    if (wpe__key(ImGuiKey_Backspace)) wpe_code_text_erase(t, false);
    if (wpe__key(ImGuiKey_Delete)) wpe_code_text_erase(t, true);
    if (!listing && wpe__key(ImGuiKey_Tab)) wpe_code_text_indent(t, shift);
    const bool ask = ctrl && igIsKeyPressed_Bool(ImGuiKey_Space, false); /* Ctrl+Space: suggest now. */
    if (ctrl) {
        if (igIsKeyPressed_Bool(ImGuiKey_A, false)) wpe_code_text_select_all(t);
        if (igIsKeyPressed_Bool(ImGuiKey_C, false)) wpe__copy_selection(t);
        if (igIsKeyPressed_Bool(ImGuiKey_X, false)) {
            wpe__copy_selection(t);
            size_t start, end;
            wpe_code_text_selection(t, &start, &end);
            if (start != end) wpe_code_text_erase(t, false);
        }
        if (igIsKeyPressed_Bool(ImGuiKey_V, false)) {
            const char* clip = igGetClipboardText();
            if (clip && clip[0]) wpe_code_text_insert(t, clip, strlen(clip));
        }
        if (wpe__key(ImGuiKey_Z)) shift ? wpe_code_text_redo(t) : wpe_code_text_undo(t);
        if (wpe__key(ImGuiKey_Y)) wpe_code_text_redo(t);
        if (igIsKeyPressed_Bool(ImGuiKey_S, false)) wpe__save(panel, doc);
        if (igIsKeyPressed_Bool(ImGuiKey_F, false)) {
            panel->find_open = true;
            panel->find_focus = true;
            size_t start, end;
            wpe_code_text_selection(t, &start, &end);
            if (end > start && end - start < sizeof panel->find &&
                !memchr(t->text + start, '\n', end - start))
                snprintf(panel->find, sizeof panel->find, "%.*s", (int)(end - start), t->text + start);
        }
    }
    bool typed = false;
    if (!ctrl) {
        /* Typed characters (UTF-16 or UTF-32 code units from the backend), as UTF-8. */
        for (int i = 0; i < io->InputQueueCharacters.Size; ++i) {
            const uint32_t c = (uint32_t)io->InputQueueCharacters.Data[i];
            if (c < 32 || c == 127 || (c >= 0xD800 && c <= 0xDFFF)) continue;
            char utf8[4];
            size_t n;
            if (c < 0x80) {
                utf8[0] = (char)c;
                n = 1;
            } else if (c < 0x800) {
                utf8[0] = (char)(0xC0 | (c >> 6));
                utf8[1] = (char)(0x80 | (c & 0x3F));
                n = 2;
            } else if (c < 0x10000) {
                utf8[0] = (char)(0xE0 | (c >> 12));
                utf8[1] = (char)(0x80 | ((c >> 6) & 0x3F));
                utf8[2] = (char)(0x80 | (c & 0x3F));
                n = 3;
            } else {
                utf8[0] = (char)(0xF0 | (c >> 18));
                utf8[1] = (char)(0x80 | ((c >> 12) & 0x3F));
                utf8[2] = (char)(0x80 | ((c >> 6) & 0x3F));
                utf8[3] = (char)(0x80 | (c & 0x3F));
                n = 4;
            }
            wpe_code_text_insert(t, utf8, n);
            typed = true;
        }
    }
    io->InputQueueCharacters.Size = 0; /* Typed into the code: nothing else gets them. */
    if (igIsKeyPressed_Bool(ImGuiKey_F3, true)) wpe__find_next(panel, doc, !shift);
    if (t->version != before) doc->check_wait = WPE__CHECK_DELAY;
    if (t->version != before || t->cursor != cursor_before) doc->follow_cursor = true;
    /* Suggestions open as a member after "name." or a plain name of two letters is typed (or on Ctrl+Space),
     * follow the typing, and close when the cursor leaves the name. */
    const WPE_CodeCompletion context = wpe_code_completion_at(t->text, t->length, t->cursor);
    const size_t typed_length = t->cursor - context.start;
    const bool possible =
        context.kind == WPE_CODE_COMPLETE_MEMBER ||
        (context.kind == WPE_CODE_COMPLETE_NAME && (ask || typed_length >= (panel->completing ? 1u : 2u)));
    if (!possible) {
        panel->completing = false;
    } else if (typed || ask || (panel->completing && (t->version != before || t->cursor != cursor_before))) {
        if (!panel->completing) panel->selected = 0;
        panel->completing = true;
        panel->context = context;
        wpe__update_matches(panel, doc);
    }
}

/* ---- The code view --------------------------------------------------------------------------------------
 */

static void wpe__code_view(WPE_ScriptPanel* panel, wpe__Doc* doc) {
    WPE_CodeText* t = &doc->text;
    wpe__update_states(doc);
    const float char_w = igCalcTextSize("M", NULL, false, -1.0f).x;
    const float line_h = igGetTextLineHeight() + 2.0f;
    char digits[16];
    const int digit_count = snprintf(digits, sizeof digits, "%u", t->line_count);
    const float gutter = (float)(digit_count + 2) * char_w + 8.0f;

    if (panel->focus_code) {
        igSetNextWindowFocus();
        panel->focus_code = false;
    }
    igPushStyleColor_U32(ImGuiCol_ChildBg, WPE__COL(18, 19, 23, 255));
    const bool open = igBeginChild_Str("##code", (ImVec2_c){0, 0}, ImGuiChildFlags_Borders,
                                       ImGuiWindowFlags_HorizontalScrollbar | ImGuiWindowFlags_NoNavInputs);
    igPopStyleColor(1);
    if (!open) {
        igEndChild();
        return;
    }
    panel->focused = igIsWindowFocused(0);
    const float view_h = igGetWindowHeight();
    const float view_w = igGetWindowWidth();
    const uint32_t page = view_h / line_h > 2.0f ? (uint32_t)(view_h / line_h) - 1 : 1;
    if (panel->focused) wpe__keyboard(panel, doc, page);
    wpe__update_states(doc); /* Keys may have changed the text. */

    ImDrawList* draw = igGetWindowDrawList();
    const ImVec2_c origin = igGetCursorScreenPos(); /* The text's top-left, scrolled. */
    const float text_x = origin.x + gutter;

    /* Mouse: click to place the cursor, shift-click or drag to select, double-click for a word. */
    const ImVec2_c mouse = igGetMousePos();
    const bool hovered = igIsWindowHovered(0);
    if (hovered && mouse.x >= text_x - 4.0f) igSetMouseCursor(ImGuiMouseCursor_TextInput);
    const int64_t mouse_line = (int64_t)floorf((mouse.y - origin.y) / line_h);
    const uint32_t clamped_line =
        mouse_line < 0 ? 0
                       : (mouse_line >= (int64_t)t->line_count ? t->line_count - 1 : (uint32_t)mouse_line);
    size_t mouse_offset;
    {
        size_t length = 0;
        const char* line = wpe_code_text_line(t, clamped_line, &length);
        mouse_offset =
            (size_t)(line - t->text) + wpe__offset_at_column(line, length, (mouse.x - text_x) / char_w);
        if (mouse_line >= (int64_t)t->line_count) mouse_offset = t->length;
    }
    if (hovered && igIsMouseClicked_Bool(ImGuiMouseButton_Left, false)) {
        if (igIsMouseDoubleClicked_Nil(ImGuiMouseButton_Left)) {
            wpe_code_text_select_word(t, mouse_offset);
        } else {
            wpe_code_text_set_cursor(t, mouse_offset, igGetIO_Nil()->KeyShift);
            panel->dragging = true;
        }
    }
    if (panel->dragging) {
        if (igIsMouseDown_Nil(ImGuiMouseButton_Left)) {
            wpe_code_text_set_cursor(t, mouse_offset, true);
            /* Dragging past the top or bottom scrolls. */
            const ImVec2_c window = igGetWindowPos();
            if (mouse.y < window.y) igSetScrollY_Float(igGetScrollY() - line_h);
            if (mouse.y > window.y + view_h) igSetScrollY_Float(igGetScrollY() + line_h);
        } else {
            panel->dragging = false;
        }
    }

    /* Keep the cursor (or a requested line) in view. */
    const uint32_t cursor_line = wpe_code_text_line_of(t, t->cursor);
    const float scroll_y = igGetScrollY(), scroll_x = igGetScrollX();
    if (doc->scroll_to_line >= 0) {
        igSetScrollY_Float(fmaxf((float)doc->scroll_to_line * line_h - view_h * 0.35f, 0.0f));
        doc->scroll_to_line = -1;
    } else if (doc->follow_cursor) {
        const float top = (float)cursor_line * line_h;
        if (top < scroll_y) igSetScrollY_Float(top);
        if (top + line_h * 2.0f > scroll_y + view_h) igSetScrollY_Float(top + line_h * 2.0f - view_h);
        size_t cursor_length = 0;
        const char* cursor_text = wpe_code_text_line(t, cursor_line, &cursor_length);
        const float x =
            gutter +
            (float)wpe__visual_column(cursor_text, t->cursor - (size_t)(cursor_text - t->text)) * char_w;
        if (x - gutter < scroll_x) igSetScrollX_Float(fmaxf(x - gutter - char_w * 4.0f, 0.0f));
        if (x + char_w * 4.0f > scroll_x + view_w) igSetScrollX_Float(x + char_w * 4.0f - view_w);
    }
    doc->follow_cursor = false;

    size_t sel_start, sel_end;
    wpe_code_text_selection(t, &sel_start, &sel_end);
    const uint32_t first = (uint32_t)fmaxf(floorf(scroll_y / line_h), 0.0f);
    const uint32_t last = first + (uint32_t)(view_h / line_h) + 2;
    const bool blink = fmod(igGetTime(), 1.0) < 0.6;
    WPE_CodeToken tokens[128];
    for (uint32_t i = first; i < t->line_count && i <= last; ++i) {
        size_t length = 0;
        const char* line = wpe_code_text_line(t, i, &length);
        const size_t line_start = (size_t)(line - t->text);
        const float y = origin.y + (float)i * line_h;
        const bool error = (doc->error_line && doc->error_line - 1 == i) ||
                           (doc->runtime_line && doc->runtime_line - 1 == i);
        /* Background: the error line, the cursor's line, the selection. */
        if (error) {
            ImDrawList_AddRectFilled(draw, (ImVec2_c){origin.x, y},
                                     (ImVec2_c){origin.x + view_w + scroll_x, y + line_h},
                                     WPE__COL(120, 30, 30, 110), 0.0f, 0);
        } else if (i == cursor_line && sel_start == sel_end && panel->focused) {
            ImDrawList_AddRectFilled(draw, (ImVec2_c){origin.x, y},
                                     (ImVec2_c){origin.x + view_w + scroll_x, y + line_h},
                                     WPE__COL(255, 255, 255, 12), 0.0f, 0);
        }
        if (sel_end > line_start && sel_start <= line_start + length && sel_start != sel_end) {
            const size_t a = sel_start > line_start ? sel_start - line_start : 0;
            const size_t b = sel_end < line_start + length ? sel_end - line_start : length;
            const float x0 = text_x + (float)wpe__visual_column(line, a) * char_w;
            float x1 = text_x + (float)wpe__visual_column(line, b) * char_w;
            if (sel_end > line_start + length) x1 += char_w * 0.5f; /* The newline is selected too. */
            ImDrawList_AddRectFilled(draw, (ImVec2_c){x0, y}, (ImVec2_c){x1, y + line_h},
                                     WPE__COL(238, 153, 56, 80), 0.0f, 0);
        }
        /* The line number, and a marker on error lines. */
        char number[16];
        const int n = snprintf(number, sizeof number, "%u", i + 1);
        ImDrawList_AddText_Vec2(draw, (ImVec2_c){origin.x + gutter - 8.0f - (float)n * char_w, y + 1.0f},
                                i == cursor_line ? WPE__COL(200, 204, 214, 255) : WPE__COL(90, 96, 108, 255),
                                number, number + n);
        if (error) {
            ImDrawList_AddRectFilled(draw, (ImVec2_c){origin.x + 2.0f, y + 2.0f},
                                     (ImVec2_c){origin.x + 6.0f, y + line_h - 2.0f},
                                     WPE__COL(240, 90, 80, 255), 1.0f, 0);
            if (hovered && mouse_line == (int64_t)i && mouse.x < text_x) {
                igSetTooltip("%s", doc->error_line - 1 == i ? doc->error : doc->runtime_error);
            }
        }
        /* The text, token by token. */
        uint32_t count = 0;
        (void)wpe_code_tokenize(line, length, doc->states ? doc->states[i] : 0, tokens,
                                (uint32_t)(sizeof tokens / sizeof tokens[0]), &count);
        uint32_t column = 0;
        for (uint32_t k = 0; k < count; ++k)
            column =
                wpe__draw_run(draw, text_x, y + 1.0f, char_w, line + tokens[k].start, tokens[k].length,
                              wpe__visual_column(line, tokens[k].start), wpe__token_colors[tokens[k].kind]);
        (void)column;
        /* The cursor (where the suggestion and signature popups go). */
        if (i == cursor_line) {
            const float x = text_x + (float)wpe__visual_column(line, t->cursor - line_start) * char_w;
            panel->cursor_x = x;
            panel->cursor_y = y;
            panel->line_h = line_h;
            if (panel->focused && blink)
                ImDrawList_AddRectFilled(draw, (ImVec2_c){x, y}, (ImVec2_c){x + 2.0f, y + line_h},
                                         WPE__COL(238, 153, 56, 255), 0.0f, 0);
        }
    }
    /* Hovering a `wpe.` function shows its help. */
    if (hovered && !panel->dragging && mouse.x >= text_x && mouse_line >= 0 &&
        mouse_line < (int64_t)t->line_count && !panel->completing) {
        const WPE_ApiDoc* help = wpe__doc_at(panel, t, mouse_offset);
        if (help && igBeginTooltip()) {
            wpe__doc_text(help);
            igEndTooltip();
        }
    }
    /* The scrollable size: every line, as wide as the widest. */
    igDummy((ImVec2_c){gutter + (float)(doc->widest + 8) * char_w,
                       (float)t->line_count * line_h + view_h * 0.5f});
    igEndChild();
    wpe__completion_popup(panel, doc);
    wpe__signature_popup(panel, doc);
}

/* ---- Prompts ------------------------------------------------------------------------------------------- */

static void wpe__close_prompt(WPE_ScriptPanel* panel) {
    if (panel->closing >= 0 && !igIsPopupOpen_Str("Unsaved script", 0)) igOpenPopup_Str("Unsaved script", 0);
    if (!igBeginPopupModal("Unsaved script", NULL, ImGuiWindowFlags_AlwaysAutoResize)) return;
    if (panel->closing < 0 || panel->closing >= (int)panel->count) {
        igCloseCurrentPopup();
        igEndPopup();
        return;
    }
    wpe__Doc* doc = panel->docs[panel->closing];
    igText("Save the changes to %s?", doc->path);
    bool close = false;
    if (igButton("Save", (ImVec2_c){120, 0})) close = wpe__save(panel, doc);
    igSameLine(0, -1);
    if (igButton("Don't save", (ImVec2_c){120, 0})) close = true;
    igSameLine(0, -1);
    if (igButton("Cancel", (ImVec2_c){120, 0}) || igIsKeyPressed_Bool(ImGuiKey_Escape, false)) {
        panel->closing = -1;
        igCloseCurrentPopup();
    }
    if (close) {
        wpe__close_doc(panel, panel->closing);
        panel->closing = -1;
        igCloseCurrentPopup();
    }
    igEndPopup();
}

static void wpe__close_all_prompt(WPE_ScriptPanel* panel) {
    if (panel->close_all_popup) {
        igOpenPopup_Str("Unsaved scripts", 0);
        panel->close_all_popup = false;
    }
    if (!igBeginPopupModal("Unsaved scripts", NULL, ImGuiWindowFlags_AlwaysAutoResize)) return;
    igText("These scripts have unsaved changes:");
    for (uint32_t i = 0; i < panel->count; ++i)
        if (wpe_code_text_dirty(&panel->docs[i]->text)) igBulletText("%s", panel->docs[i]->path);
    if (igButton("Save all", (ImVec2_c){120, 0})) {
        bool saved = true;
        for (uint32_t i = 0; i < panel->count; ++i)
            if (wpe_code_text_dirty(&panel->docs[i]->text)) saved = wpe__save(panel, panel->docs[i]) && saved;
        panel->close_all_result = saved ? 1 : -1; /* A failed save cancels (the console says why). */
        panel->close_all_asked = false;
        igCloseCurrentPopup();
    }
    igSameLine(0, -1);
    if (igButton("Don't save", (ImVec2_c){120, 0})) {
        panel->close_all_result = 1;
        panel->close_all_asked = false;
        igCloseCurrentPopup();
    }
    igSameLine(0, -1);
    if (igButton("Cancel", (ImVec2_c){120, 0}) || igIsKeyPressed_Bool(ImGuiKey_Escape, false)) {
        panel->close_all_result = -1;
        panel->close_all_asked = false;
        igCloseCurrentPopup();
    }
    igEndPopup();
}

/* ---- The panel ----------------------------------------------------------------------------------------- */

void wpe_script_panel_draw(WPE_ScriptPanel* panel) {
    if (!panel) return;
    WPE_Editor* editor = panel->editor;
    /* A script the open_script command asked for. */
    if (editor->open_script[0]) {
        wpe_script_panel_open(panel, editor->open_script, editor->open_script_line,
                              editor->open_script_column);
        editor->open_script[0] = '\0';
    }
    panel->log_seen =
        wpe__editor_log_since(editor, panel->log_seen, WPE__EDITOR_LOG_LINES, wpe__scan_line, panel);
    /* Compile checks, a moment after typing stops. */
    const float dt = igGetIO_Nil()->DeltaTime;
    for (uint32_t i = 0; i < panel->count; ++i) {
        wpe__Doc* doc = panel->docs[i];
        if (doc->checked_version == doc->text.version) continue;
        doc->check_wait -= dt;
        if (doc->check_wait <= 0.0f) wpe__check(panel, doc);
    }

    if (panel->show_window) {
        igSetWindowFocus_Str("Script");
        panel->show_window = false;
    }
    panel->focused = false;
    if (!igBegin("Script", NULL, 0)) {
        igEnd();
        wpe__close_all_prompt(panel);
        return;
    }
    if (panel->count == 0) {
        igTextDisabled(
            "No script open. Double-click a .luau file in Assets, use the inspector's Edit button,");
        igTextDisabled("or double-click an error naming a script in the Console.");
        if (igButton("New script", (ImVec2_c){0, 0})) panel->new_requested = true;
    }
    if (panel->count > 0 &&
        igBeginTabBar("##scripts", ImGuiTabBarFlags_Reorderable | ImGuiTabBarFlags_AutoSelectNewTabs)) {
        for (uint32_t i = 0; i < panel->count; ++i) {
            wpe__Doc* doc = panel->docs[i];
            bool open = true;
            ImGuiTabItemFlags flags = wpe_code_text_dirty(&doc->text) ? ImGuiTabItemFlags_UnsavedDocument : 0;
            if (panel->select_tab == (int)i) flags |= ImGuiTabItemFlags_SetSelected;
            const char* name = strrchr(doc->path, '/');
            char label[WPE_VFS_PATH_MAX * 2 + 8]; /* "name###path": the id stays the path. */
            snprintf(label, sizeof label, "%s###%s", name ? name + 1 : doc->path, doc->path);
            if (igBeginTabItem(label, &open, flags)) {
                panel->active = (int)i;
                if (igIsItemHovered(0)) igSetTooltip("%s", doc->path);
                /* Toolbar: save, where the cursor is, what the compiler says. */
                if (igButton("Save", (ImVec2_c){0, 0})) wpe__save(panel, doc);
                igSameLine(0, -1);
                if (igButton("Find", (ImVec2_c){0, 0})) {
                    panel->find_open = !panel->find_open;
                    panel->find_focus = panel->find_open;
                }
                igSameLine(0, -1);
                if (igButton("New", (ImVec2_c){0, 0})) panel->new_requested = true;
                if (igIsItemHovered(0)) igSetTooltip("A new script from a template");
                igSameLine(0, -1);
                igTextDisabled("Ln %u, Col %u", wpe_code_text_line_of(&doc->text, doc->text.cursor) + 1,
                               wpe_code_text_column(&doc->text, doc->text.cursor) + 1);
                igSameLine(0, -1);
                if (doc->error_line) {
                    igTextColored((ImVec4_c){1.0f, 0.45f, 0.4f, 1.0f}, "Line %u: %s", doc->error_line,
                                  doc->error);
                } else if (doc->runtime_line) {
                    igTextColored((ImVec4_c){1.0f, 0.65f, 0.4f, 1.0f}, "Ran into an error on line %u: %s",
                                  doc->runtime_line, doc->runtime_error);
                } else {
                    igTextColored((ImVec4_c){0.55f, 0.8f, 0.5f, 1.0f}, "Compiles");
                }
                if (panel->find_open) {
                    if (panel->find_focus) {
                        igSetKeyboardFocusHere(0);
                        panel->find_focus = false;
                    }
                    igSetNextItemWidth(240.0f);
                    if (igInputText("##find", panel->find, sizeof panel->find,
                                    ImGuiInputTextFlags_EnterReturnsTrue, NULL, NULL)) {
                        wpe__find_next(panel, doc, !igGetIO_Nil()->KeyShift);
                        panel->find_focus = true; /* Enter again finds the next one. */
                    }
                    igSameLine(0, -1);
                    if (igButton("Next", (ImVec2_c){0, 0})) wpe__find_next(panel, doc, true);
                    igSameLine(0, -1);
                    if (igButton("Previous", (ImVec2_c){0, 0})) wpe__find_next(panel, doc, false);
                    igSameLine(0, -1);
                    igTextDisabled("F3 / Shift+F3");
                    if (igIsKeyPressed_Bool(ImGuiKey_Escape, false)) {
                        panel->find_open = false;
                        panel->focus_code = true;
                    }
                }
                wpe__code_view(panel, doc);
                igEndTabItem();
            }
            if (!open) {
                if (wpe_code_text_dirty(&doc->text)) {
                    panel->closing = (int)i;
                } else {
                    wpe__close_doc(panel, (int)i);
                    break;
                }
            }
        }
        panel->select_tab = -1;
        igEndTabBar();
    }
    igEnd();
    wpe__close_prompt(panel);
    wpe__close_all_prompt(panel);
}
