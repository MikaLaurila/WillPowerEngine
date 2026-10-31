#include "wpe_test.h"

#include <string.h>

#include "editor/api_docs.h"
#include "editor/code_symbols.h"
#include "editor/code_text.h"

static void set(WPE_CodeText* t, const char* text) {
    WPE_TEST_ASSERT(wpe_code_text_set(t, text, strlen(text)));
}

static void type(WPE_CodeText* t, const char* text) {
    for (const char* c = text; *c; ++c)
        WPE_TEST_ASSERT(wpe_code_text_insert(t, c, 1));
}

static void test_code_text_lines_and_positions(void) {
    WPE_CodeText t;
    wpe_code_text_init(&t);
    set(&t, "local a = 1\r\n\tif a then\r\nend");
    WPE_TEST_ASSERT_EQ_STR("local a = 1\n\tif a then\nend", t.text); /* Windows line ends become newlines. */
    WPE_TEST_ASSERT_EQ_UINT(3, t.line_count);
    size_t length = 0;
    const char* line = wpe_code_text_line(&t, 1, &length);
    WPE_TEST_ASSERT(length == 10 && strncmp(line, "\tif a then", 10) == 0);
    WPE_TEST_ASSERT_EQ_UINT(1, wpe_code_text_line_of(&t, 12));
    WPE_TEST_ASSERT_EQ_UINT(2, wpe_code_text_line_of(&t, t.length));
    WPE_TEST_ASSERT_EQ_UINT(4, wpe_code_text_column(&t, 16));
    WPE_TEST_ASSERT_EQ_UINT(16, wpe_code_text_offset(&t, 1, 4));
    WPE_TEST_ASSERT_EQ_UINT(t.length, wpe_code_text_offset(&t, 2, 99)); /* Clamped to the line. */
    WPE_TEST_ASSERT(!wpe_code_text_dirty(&t));
    wpe_code_text_free(&t);
}

static void test_code_text_typing_and_undo(void) {
    WPE_CodeText t;
    wpe_code_text_init(&t);
    set(&t, "x");
    wpe_code_text_move(&t, WPE_CODE_FINISH, false, 0);
    type(&t, " = 12");
    WPE_TEST_ASSERT_EQ_STR("x = 12", t.text);
    WPE_TEST_ASSERT(wpe_code_text_dirty(&t));
    /* A run of typing is one undo step; so is the backspacing after it. */
    WPE_TEST_ASSERT(wpe_code_text_erase(&t, false) && wpe_code_text_erase(&t, false));
    WPE_TEST_ASSERT_EQ_STR("x = ", t.text);
    WPE_TEST_ASSERT(wpe_code_text_undo(&t));
    WPE_TEST_ASSERT_EQ_STR("x = 12", t.text);
    WPE_TEST_ASSERT(wpe_code_text_undo(&t));
    WPE_TEST_ASSERT_EQ_STR("x", t.text);
    WPE_TEST_ASSERT(!wpe_code_text_dirty(&t)); /* Back to what was saved. */
    WPE_TEST_ASSERT(!wpe_code_text_undo(&t));
    WPE_TEST_ASSERT(wpe_code_text_redo(&t));
    WPE_TEST_ASSERT_EQ_STR("x = 12", t.text);
    WPE_TEST_ASSERT_EQ_UINT(6, t.cursor);
    /* Moving the cursor ends a run: the next typing is its own step. */
    wpe_code_text_move(&t, WPE_CODE_HOME, false, 0);
    type(&t, "local ");
    WPE_TEST_ASSERT_EQ_STR("local x = 12", t.text);
    WPE_TEST_ASSERT(wpe_code_text_undo(&t));
    WPE_TEST_ASSERT_EQ_STR("x = 12", t.text);
    /* A new edit drops what could be redone. */
    type(&t, "-");
    WPE_TEST_ASSERT(!wpe_code_text_redo(&t));
    wpe_code_text_mark_saved(&t);
    WPE_TEST_ASSERT(!wpe_code_text_dirty(&t));
    /* Typing over a selection replaces it; a paste loses carriage returns. */
    wpe_code_text_select_all(&t);
    WPE_TEST_ASSERT(wpe_code_text_insert(&t, "a\r\nb", 4));
    WPE_TEST_ASSERT_EQ_STR("a\nb", t.text);
    wpe_code_text_free(&t);
}

static void test_code_text_auto_indent(void) {
    WPE_CodeText t;
    wpe_code_text_init(&t);
    const struct {
        const char* line;
        const char* expected;
    } cases[] = {
        {"\tif ready then", "\tif ready then\n\t\t"},
        {"\tlocal x = 1", "\tlocal x = 1\n\t"},
        {"function Hero:on_update(dt)", "function Hero:on_update(dt)\n\t"},
        {"local t = {", "local t = {\n\t"},
        {"\twpe.after(1, function()", "\twpe.after(1, function()\n\t\t"},
        {"\tprint(x)", "\tprint(x)\n\t"}, /* A call's ")" is not a function header. */
        {"for i = 1, 3 do", "for i = 1, 3 do\n\t"},
        {"\tredo()", "\tredo()\n\t"}, /* Ends in "do" only inside a word. */
    };
    for (size_t i = 0; i < sizeof cases / sizeof cases[0]; ++i) {
        set(&t, cases[i].line);
        wpe_code_text_move(&t, WPE_CODE_FINISH, false, 0);
        WPE_TEST_ASSERT(wpe_code_text_newline(&t));
        WPE_TEST_ASSERT_EQ_STR(cases[i].expected, t.text);
        WPE_TEST_ASSERT_EQ_UINT(t.length, t.cursor);
    }
    wpe_code_text_free(&t);
}

static void test_code_text_erase_and_indent(void) {
    WPE_CodeText t;
    wpe_code_text_init(&t);
    /* UTF-8: one character at a time, never half of one. */
    set(&t, "\"Säde\"");
    wpe_code_text_set_cursor(&t, 4, false); /* After "ä" (2 bytes). */
    WPE_TEST_ASSERT(wpe_code_text_erase(&t, false));
    WPE_TEST_ASSERT_EQ_STR("\"Sde\"", t.text);
    WPE_TEST_ASSERT(wpe_code_text_erase(&t, true));
    WPE_TEST_ASSERT_EQ_STR("\"Se\"", t.text);
    wpe_code_text_set_cursor(&t, 0, false);
    WPE_TEST_ASSERT(!wpe_code_text_erase(&t, false));

    /* Tab over a selection of lines indents them all; shift-tab takes it back off. */
    set(&t, "a\nb\nc");
    wpe_code_text_set_cursor(&t, 0, false);
    wpe_code_text_set_cursor(&t, 3, true); /* Into "b". */
    WPE_TEST_ASSERT(wpe_code_text_indent(&t, false));
    WPE_TEST_ASSERT_EQ_STR("\ta\n\tb\nc", t.text);
    size_t start, end;
    wpe_code_text_selection(&t, &start, &end);
    WPE_TEST_ASSERT(start == 0 && end == 5); /* Both lines, whole. */
    WPE_TEST_ASSERT(wpe_code_text_indent(&t, true));
    WPE_TEST_ASSERT_EQ_STR("a\nb\nc", t.text);
    /* Without a selection a tab is typed; shift-tab outdents the line (spaces too). */
    set(&t, "    x");
    wpe_code_text_move(&t, WPE_CODE_FINISH, false, 0);
    WPE_TEST_ASSERT(wpe_code_text_indent(&t, true));
    WPE_TEST_ASSERT_EQ_STR("x", t.text);
    WPE_TEST_ASSERT(wpe_code_text_indent(&t, false));
    WPE_TEST_ASSERT_EQ_STR("x\t", t.text);
    wpe_code_text_free(&t);
}

static void test_code_text_moves_and_search(void) {
    WPE_CodeText t;
    wpe_code_text_init(&t);
    set(&t, "\tlocal speed = 10\nx\n\tspeed += 1");
    /* Home: the first non-blank, then the very start. */
    wpe_code_text_set_cursor(&t, 8, false);
    wpe_code_text_move(&t, WPE_CODE_HOME, false, 0);
    WPE_TEST_ASSERT_EQ_UINT(1, t.cursor);
    wpe_code_text_move(&t, WPE_CODE_HOME, false, 0);
    WPE_TEST_ASSERT_EQ_UINT(0, t.cursor);
    /* Words. */
    wpe_code_text_move(&t, WPE_CODE_WORD_RIGHT, false, 0);
    WPE_TEST_ASSERT_EQ_UINT(1, t.cursor);
    wpe_code_text_move(&t, WPE_CODE_WORD_RIGHT, false, 0);
    WPE_TEST_ASSERT_EQ_UINT(7, t.cursor); /* Past "local" and its space. */
    wpe_code_text_move(&t, WPE_CODE_WORD_RIGHT, true, 0);
    size_t start, end;
    wpe_code_text_selection(&t, &start, &end);
    WPE_TEST_ASSERT(start == 7 && end == 13);
    wpe_code_text_move(&t, WPE_CODE_WORD_LEFT, false, 0);
    WPE_TEST_ASSERT_EQ_UINT(7, t.cursor);
    /* Up and down keep the column across a short line. */
    wpe_code_text_set_cursor(&t, 9, false); /* Column 9 of line 0. */
    wpe_code_text_move(&t, WPE_CODE_DOWN, false, 0);
    WPE_TEST_ASSERT_EQ_UINT(19, t.cursor); /* The end of "x". */
    wpe_code_text_move(&t, WPE_CODE_DOWN, false, 0);
    WPE_TEST_ASSERT_EQ_UINT(9, wpe_code_text_column(&t, t.cursor));
    /* A word under a double-click. */
    wpe_code_text_select_word(&t, 9);
    wpe_code_text_selection(&t, &start, &end);
    WPE_TEST_ASSERT(start == 7 && end == 12);
    /* Find: case-insensitive, wrapping, either way. */
    const size_t second = wpe_code_text_find(&t, "SPEED", 8, true);
    WPE_TEST_ASSERT_EQ_UINT(21, second);
    WPE_TEST_ASSERT_EQ_UINT(7, wpe_code_text_find(&t, "speed", second + 1, true)); /* Wraps around. */
    WPE_TEST_ASSERT_EQ_UINT(7, wpe_code_text_find(&t, "speed", second, false));
    WPE_TEST_ASSERT_EQ_UINT(21, wpe_code_text_find(&t, "speed", 7, false)); /* Wraps backwards. */
    WPE_TEST_ASSERT(wpe_code_text_find(&t, "missing", 0, true) == SIZE_MAX);
    wpe_code_text_free(&t);
}

/* The token kinds of a line, as a string (one letter per token): t text, k keyword, c constant, n number,
 * s string, m comment, l library. */
static uint8_t kinds(const char* line, uint8_t state, char* out) {
    WPE_CodeToken tokens[32];
    uint32_t count = 0;
    const uint8_t next = wpe_code_tokenize(line, strlen(line), state, tokens, 32, &count);
    static const char letters[] = "tkcnsml";
    for (uint32_t i = 0; i < count; ++i)
        out[i] = letters[tokens[i].kind];
    out[count] = '\0';
    return next;
}

static void test_code_text_tokenizer(void) {
    char k[64];
    WPE_TEST_ASSERT_EQ_UINT(0, kinds("local x = 0x1F + 2.5e-3", 0, k));
    WPE_TEST_ASSERT_EQ_STR("ktntn", k); /* "local", " x = ", "0x1F", " + ", "2.5e-3" */
    kinds("if self.type == nil then wpe.log(\"a \\\" b\") end -- done", 0, k);
    WPE_TEST_ASSERT_EQ_STR("ktctctktlts"
                           "tktm",
                           k);
    /* A field named like a keyword is a field. */
    kinds("t.end", 0, k);
    WPE_TEST_ASSERT_EQ_STR("t", k);
    /* Long comments and strings carry over lines; the level must match to close. */
    uint8_t state = kinds("x = 1 --[==[ starts", 0, k);
    WPE_TEST_ASSERT(state != 0);
    WPE_TEST_ASSERT_EQ_STR("tntm", k);
    state = kinds("still ]] comment", state, k);
    WPE_TEST_ASSERT(state != 0);
    WPE_TEST_ASSERT_EQ_STR("m", k);
    state = kinds("ends ]==] return", state, k);
    WPE_TEST_ASSERT_EQ_UINT(0, state);
    WPE_TEST_ASSERT_EQ_STR("mtk", k);
    state = kinds("local s = [[two", 0, k);
    WPE_TEST_ASSERT_EQ_STR("kts", k);
    state = kinds("lines]] .. `x{1}`", state, k);
    WPE_TEST_ASSERT_EQ_UINT(0, state);
    WPE_TEST_ASSERT_EQ_STR("sts", k);
    /* More tokens than room: the rest joins the last one. */
    WPE_CodeToken few[2];
    uint32_t count = 0;
    wpe_code_tokenize("a = 1 + 2", 9, 0, few, 2, &count);
    WPE_TEST_ASSERT(count == 2 && few[1].start + few[1].length == 9);
}

static void test_code_text_api_docs(void) {
    const char* markdown =
        "# Scripting\n"
        "### Tweens\n"
        "| Function | Description |\n"
        "|---|---|\n"
        "| `wpe.tween(entity, {...}) -> id` | **Eases** a [transform](scenes.md#x): `a \\| b`<br>next |\n"
        "| `wpe.get_ui_offsets(e) -> x` / `wpe.set_ui_offsets(e, x)` | The offsets. |\n"
        "### UI\n"
        "| `wpe.tween(other)` | A second row for a name keeps the first. |\n"
        "| `not wpe.code` | Ignored. |\n";
    WPE_ApiDocs docs;
    WPE_TEST_ASSERT(wpe_api_docs_parse(markdown, strlen(markdown), &docs));
    WPE_TEST_ASSERT_EQ_UINT(3, docs.count);
    WPE_TEST_ASSERT_EQ_STR("get_ui_offsets", docs.docs[0].name); /* Sorted by name. */
    const WPE_ApiDoc* tween = wpe_api_docs_find(&docs, "tween", 5);
    WPE_TEST_ASSERT(tween != NULL);
    WPE_TEST_ASSERT_EQ_STR("wpe.tween(entity, {...}) -> id", tween->signature);
    WPE_TEST_ASSERT_EQ_STR("Eases a transform: a | b next", tween->summary); /* Plain text. */
    WPE_TEST_ASSERT_EQ_STR("Tweens", tween->section);
    const WPE_ApiDoc* set = wpe_api_docs_find(&docs, "set_ui_offsets", 14);
    WPE_TEST_ASSERT(set && strcmp(set->signature, "wpe.set_ui_offsets(e, x)") == 0 &&
                    strcmp(set->summary, "The offsets.") == 0);
    WPE_TEST_ASSERT(wpe_api_docs_find(&docs, "tweens", 6) == NULL &&
                    wpe_api_docs_find(&docs, "twee", 4) == NULL);
    wpe_api_docs_free(&docs);
}

static bool has(const WPE_CodeSymbols* symbols, const char* name, WPE_CodeSymbolKind kind) {
    const WPE_CodeSymbol* s = wpe_code_symbols_find(symbols, name, strlen(name));
    return s && s->kind == kind;
}

/* Where completion applies: names, simple members, not comments, strings, numbers or new names. */
static void test_code_text_completion_context(void) {
    const char* text = "local speed = 1 -- spe\nprint(\"wpe.lo\", self.spe, a.b.c, 1.5, x..y)\nlocal ne";
    const size_t length = strlen(text);
    const char* at = strstr(text, "= 1");
    WPE_CodeCompletion c = wpe_code_completion_at(text, length, (size_t)(at - text)); /* After "speed ". */
    WPE_TEST_ASSERT_EQ_INT(WPE_CODE_COMPLETE_NAME, c.kind);
    at = strstr(text, "-- spe") + 6;
    WPE_TEST_ASSERT_EQ_INT(WPE_CODE_COMPLETE_NONE,
                           wpe_code_completion_at(text, length, (size_t)(at - text)).kind);
    at = strstr(text, "wpe.lo") + 6;
    WPE_TEST_ASSERT_EQ_INT(WPE_CODE_COMPLETE_NONE,
                           wpe_code_completion_at(text, length, (size_t)(at - text)).kind);
    at = strstr(text, "self.spe") + 8;
    c = wpe_code_completion_at(text, length, (size_t)(at - text));
    WPE_TEST_ASSERT_EQ_INT(WPE_CODE_COMPLETE_MEMBER, c.kind);
    WPE_TEST_ASSERT(c.object_length == 4 && strncmp(text + c.object_start, "self", 4) == 0 &&
                    c.separator == '.');
    WPE_TEST_ASSERT_EQ_UINT((size_t)(at - text) - 3, c.start);
    at = strstr(text, "a.b.c") + 5;
    WPE_TEST_ASSERT_EQ_INT(WPE_CODE_COMPLETE_NONE,
                           wpe_code_completion_at(text, length, (size_t)(at - text)).kind);
    at = strstr(text, "1.5") + 3;
    WPE_TEST_ASSERT_EQ_INT(WPE_CODE_COMPLETE_NONE,
                           wpe_code_completion_at(text, length, (size_t)(at - text)).kind);
    at = strstr(text, "x..y") + 4;
    WPE_TEST_ASSERT_EQ_INT(WPE_CODE_COMPLETE_NONE,
                           wpe_code_completion_at(text, length, (size_t)(at - text)).kind);
    WPE_TEST_ASSERT_EQ_INT(WPE_CODE_COMPLETE_NONE,
                           wpe_code_completion_at(text, length, length).kind); /* local ne */
    c = wpe_code_completion_at("obj:me", 6, 6);
    WPE_TEST_ASSERT(c.kind == WPE_CODE_COMPLETE_MEMBER && c.separator == ':' && c.start == 4);
}

/* Names in scope at a point: closed blocks drop theirs, parameters and loop variables, methods' self. */
static void test_code_text_symbols_in_scope(void) {
    const char* text = "local util = require(\"scripts/lib/util\")\n"
                       "local Hero = {}\n"
                       "local function helper(a: number, b)\n"
                       "\tlocal inner = a\n"
                       "end\n"
                       "function Hero:on_update(dt: number)\n"
                       "\tfor i, v in ipairs({}) do\n"
                       "\t\tlocal in_loop = i\n"
                       "\tend\n"
                       "\tif dt > 0 then\n"
                       "\t\tlocal step: {number}, other = {}, nil\n"
                       "\telse\n"
                       "\t\tlocal fallback = 0\n"
                       "\t\t|\n"
                       "\tend\n"
                       "end\n"
                       "local later = 1\n";
    const size_t cursor = (size_t)(strchr(text, '|') - text);
    WPE_CodeSymbols s = {0};
    WPE_TEST_ASSERT(wpe_code_symbols_in_scope(text, strlen(text), cursor, &s));
    WPE_TEST_ASSERT_EQ_STR("Hero", s.self_class);
    WPE_TEST_ASSERT(has(&s, "util", WPE_CODE_SYMBOL_LOCAL) && has(&s, "Hero", WPE_CODE_SYMBOL_LOCAL));
    WPE_TEST_ASSERT_EQ_STR("scripts/lib/util", wpe_code_symbols_find(&s, "util", 4)->module);
    WPE_TEST_ASSERT(has(&s, "helper", WPE_CODE_SYMBOL_FUNCTION) && has(&s, "dt", WPE_CODE_SYMBOL_PARAMETER));
    WPE_TEST_ASSERT(has(&s, "fallback", WPE_CODE_SYMBOL_LOCAL));
    WPE_TEST_ASSERT_EQ_UINT(13, wpe_code_symbols_find(&s, "fallback", 8)->line);
    /* Out of scope here: the helper's parameters and locals, the loop's, the then-branch's; not yet: later.
     */
    static const char* const gone[] = {"a",       "b",    "inner", "i",     "v",
                                       "in_loop", "step", "other", "later", "number"};
    for (size_t i = 0; i < sizeof gone / sizeof gone[0]; ++i)
        WPE_TEST_ASSERT(wpe_code_symbols_find(&s, gone[i], strlen(gone[i])) == NULL);
    WPE_TEST_ASSERT_EQ_STR("fallback", s.items[0].name); /* Innermost first. */
    wpe_code_symbols_free(&s);

    /* Inside the loop and the then-branch. */
    const char* loop = strstr(text, "local in_loop");
    WPE_TEST_ASSERT(wpe_code_symbols_in_scope(text, strlen(text), (size_t)(loop - text), &s));
    WPE_TEST_ASSERT(has(&s, "i", WPE_CODE_SYMBOL_LOCAL) && has(&s, "v", WPE_CODE_SYMBOL_LOCAL));
    wpe_code_symbols_free(&s);
    const char* branch = strstr(text, "\telse");
    WPE_TEST_ASSERT(wpe_code_symbols_in_scope(text, strlen(text), (size_t)(branch - text), &s));
    WPE_TEST_ASSERT(has(&s, "step", WPE_CODE_SYMBOL_LOCAL) && has(&s, "other", WPE_CODE_SYMBOL_LOCAL));
    wpe_code_symbols_free(&s);
    /* After the method: self is gone. */
    WPE_TEST_ASSERT(wpe_code_symbols_in_scope(text, strlen(text), strlen(text), &s));
    WPE_TEST_ASSERT_EQ_STR("", s.self_class);
    WPE_TEST_ASSERT(has(&s, "later", WPE_CODE_SYMBOL_LOCAL) && !wpe_code_symbols_find(&s, "dt", 2));
    wpe_code_symbols_free(&s);
}

/* A table's members, a class's properties, a module's returned table, the libraries. */
static void test_code_text_symbols_members(void) {
    const char* text = "local Hero = {}\n"
                       "Hero.properties = { speed = 1, label = \"x\", nested = { deep = 2 } }\n"
                       "local SPEEDS = { walk = 2, run = function() end }\n"
                       "function Hero:on_start() self.health = 3; self:heal(1) end\n"
                       "function Hero.helper() end\n"
                       "Hero.count = 0\n"
                       "print(other.Hero.fake)\n"
                       "-- Hero.commented\n"
                       "return Hero\n";
    const size_t length = strlen(text);
    WPE_CodeSymbols s = {0};
    WPE_TEST_ASSERT(wpe_code_symbols_members(text, length, "Hero", 4, &s));
    WPE_TEST_ASSERT(has(&s, "on_start", WPE_CODE_SYMBOL_METHOD) &&
                    has(&s, "helper", WPE_CODE_SYMBOL_FUNCTION));
    WPE_TEST_ASSERT(has(&s, "count", WPE_CODE_SYMBOL_FIELD) && has(&s, "properties", WPE_CODE_SYMBOL_FIELD));
    WPE_TEST_ASSERT(!wpe_code_symbols_find(&s, "fake", 4) && !wpe_code_symbols_find(&s, "commented", 9));
    wpe_code_symbols_free(&s);
    WPE_TEST_ASSERT(wpe_code_symbols_members(text, length, "self", 4, &s));
    WPE_TEST_ASSERT(has(&s, "health", WPE_CODE_SYMBOL_FIELD) && has(&s, "heal", WPE_CODE_SYMBOL_METHOD));
    wpe_code_symbols_free(&s);
    WPE_TEST_ASSERT(wpe_code_symbols_members(text, length, "SPEEDS", 6, &s));
    WPE_TEST_ASSERT(has(&s, "walk", WPE_CODE_SYMBOL_FIELD) && has(&s, "run", WPE_CODE_SYMBOL_FUNCTION));
    wpe_code_symbols_free(&s);
    WPE_TEST_ASSERT(wpe_code_symbols_properties(text, length, "Hero", 4, &s));
    WPE_TEST_ASSERT_EQ_UINT(3, s.count); /* speed, label, nested; not deep. */
    WPE_TEST_ASSERT(has(&s, "speed", WPE_CODE_SYMBOL_FIELD) && has(&s, "nested", WPE_CODE_SYMBOL_FIELD));
    wpe_code_symbols_free(&s);
    char name[WPE_CODE_SYMBOL_NAME_MAX];
    WPE_TEST_ASSERT(wpe_code_symbols_returned(text, length, name, sizeof name));
    WPE_TEST_ASSERT_EQ_STR("Hero", name);
    WPE_TEST_ASSERT(!wpe_code_symbols_returned("return {}", 9, name, sizeof name));
    WPE_TEST_ASSERT(wpe_code_symbols_library("math", 4, &s));
    WPE_TEST_ASSERT(has(&s, "floor", WPE_CODE_SYMBOL_FUNCTION) && has(&s, "pi", WPE_CODE_SYMBOL_CONSTANT));
    WPE_TEST_ASSERT(!wpe_code_symbols_library("mat", 3, &s));
    WPE_TEST_ASSERT(wpe_code_symbols_globals(&s));
    WPE_TEST_ASSERT(has(&s, "wpe", WPE_CODE_SYMBOL_LIBRARY) && has(&s, "ipairs", WPE_CODE_SYMBOL_FUNCTION) &&
                    has(&s, "end", WPE_CODE_SYMBOL_KEYWORD));
    wpe_code_symbols_free(&s);
}

WPE_TEST_MAIN(WPE_TEST_CASE(test_code_text_completion_context),
              WPE_TEST_CASE(test_code_text_symbols_in_scope), WPE_TEST_CASE(test_code_text_symbols_members),
              WPE_TEST_CASE(test_code_text_api_docs), WPE_TEST_CASE(test_code_text_lines_and_positions),
              WPE_TEST_CASE(test_code_text_typing_and_undo), WPE_TEST_CASE(test_code_text_auto_indent),
              WPE_TEST_CASE(test_code_text_erase_and_indent), WPE_TEST_CASE(test_code_text_moves_and_search),
              WPE_TEST_CASE(test_code_text_tokenizer))
