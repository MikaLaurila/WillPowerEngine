#define WPE_LOG_MODULE "engine"
#include "engine/profiler.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

#include "platform/platform.h"

static const char* const wpe__stage_names[WPE__STAGE_COUNT] = {"input",
                                                               "network",
                                                               "ui",
                                                               "scripts (fixed)",
                                                               "navigation",
                                                               "physics",
                                                               "scripts (update)",
                                                               "animation",
                                                               "effects",
                                                               "transforms",
                                                               "audio",
                                                               "render",
                                                               "post effects",
                                                               "overlay",
                                                               "present",
                                                               "scene loads",
                                                               "other"};
static_assert(sizeof wpe__stage_names / sizeof wpe__stage_names[0] == WPE__STAGE_COUNT,
              "every stage is named");

#define WPE__AVERAGE_SECONDS 0.5 /* The averages' time constant. */
#define WPE__PEAK_WINDOW 1.0     /* Peaks are the highest frame of this window and the one before. */
#define WPE__SCRIPTS_TAKEN 128u  /* Script entries read from the host per frame. */

void wpe__profiler_begin_frame(wpe__Profiler* profiler, double now) {
    profiler->mark = now;
    memset(profiler->stage_ms, 0, sizeof profiler->stage_ms);
}

void wpe__profiler_lap(wpe__Profiler* profiler, wpe__Stage stage) {
    const double now = wpe_platform_time_seconds();
    profiler->stage_ms[stage] += (now - profiler->mark) * 1000.0;
    profiler->mark = now;
}

static void wpe__reset_entries(wpe__Profiler* profiler) {
    memset(profiler->entries, 0, sizeof profiler->entries);
    memset(profiler->window_peak, 0, sizeof profiler->window_peak);
    memset(profiler->last_peak, 0, sizeof profiler->last_peak);
    memset(profiler->fresh, 1, sizeof profiler->fresh);
    for (uint32_t s = 0; s < WPE__STAGE_COUNT; ++s) {
        snprintf(profiler->entries[s].name, sizeof profiler->entries[s].name, "%s", wpe__stage_names[s]);
        profiler->entries[s].kind = WPE_PROFILE_SYSTEM;
    }
    profiler->count = WPE__STAGE_COUNT;
    profiler->window_time = 0.0;
    profiler->frame_average_ms = 0.0;
}

void wpe__profiler_set_on(wpe__Profiler* profiler, bool on, WPE_ScriptHost* scripts) {
    if (on && !profiler->on) wpe__reset_entries(profiler);
    profiler->on = on;
    if (!on) profiler->count = 0;
    wpe_script_host_set_profiling(scripts, on);
}

/* The entry of a script or section, made when there is room; NULL when the profiler is full. */
static uint32_t wpe__find_entry(wpe__Profiler* profiler, const char* name, WPE_ProfileKind kind) {
    for (uint32_t i = WPE__STAGE_COUNT; i < profiler->count; ++i) {
        if (profiler->entries[i].kind == kind && strcmp(profiler->entries[i].name, name) == 0) return i;
    }
    if (profiler->count == WPE_PROFILE_MAX_ENTRIES) return UINT32_MAX;
    const uint32_t i = profiler->count++;
    profiler->entries[i] = (WPE_ProfileEntry){.kind = kind};
    snprintf(profiler->entries[i].name, sizeof profiler->entries[i].name, "%s", name);
    profiler->window_peak[i] = profiler->last_peak[i] = 0.0;
    profiler->fresh[i] = true;
    return i;
}

void wpe__profiler_end_frame(wpe__Profiler* profiler, double frame_ms, double dt, WPE_ScriptHost* scripts) {
    double claimed = 0.0;
    for (uint32_t s = 0; s + 1 < WPE__STAGE_COUNT; ++s)
        claimed += profiler->stage_ms[s];
    profiler->stage_ms[WPE__STAGE_OTHER] = frame_ms > claimed ? frame_ms - claimed : 0.0;
    if (!profiler->on) return;

    /* This frame's numbers: the stages, then what the scripts report (others read 0). */
    for (uint32_t i = 0; i < profiler->count; ++i) {
        profiler->entries[i].ms = i < WPE__STAGE_COUNT ? profiler->stage_ms[i] : 0.0;
        profiler->entries[i].calls = 0;
    }
    WPE_ScriptProfileEntry taken[WPE__SCRIPTS_TAKEN];
    uint32_t count = wpe_script_host_take_profile(scripts, taken, WPE__SCRIPTS_TAKEN);
    if (count > WPE__SCRIPTS_TAKEN) count = WPE__SCRIPTS_TAKEN;
    for (uint32_t t = 0; t < count; ++t) {
        const uint32_t i = wpe__find_entry(profiler, taken[t].name,
                                           taken[t].section ? WPE_PROFILE_SECTION : WPE_PROFILE_SCRIPT);
        if (i == UINT32_MAX) continue; /* Full: the 64 kept are the ones that showed up first. */
        profiler->entries[i].ms += taken[t].ms;
        profiler->entries[i].calls += taken[t].calls;
    }

    /* Averages follow over about half a second whatever the frame rate; peaks over a window of a second. */
    const double alpha = dt > 0.0 ? 1.0 - exp(-dt / WPE__AVERAGE_SECONDS) : 1.0;
    profiler->frame_average_ms +=
        (frame_ms - profiler->frame_average_ms) * (profiler->fresh[0] ? 1.0 : alpha);
    profiler->window_time += dt;
    const bool new_window = profiler->window_time >= WPE__PEAK_WINDOW;
    if (new_window) profiler->window_time = 0.0;
    uint32_t kept = 0;
    for (uint32_t i = 0; i < profiler->count; ++i) {
        WPE_ProfileEntry e = profiler->entries[i];
        e.average_ms += (e.ms - e.average_ms) * (profiler->fresh[i] ? 1.0 : alpha); /* No ramp from 0. */
        double window = fmax(profiler->window_peak[i], e.ms);
        double last = profiler->last_peak[i];
        e.peak_ms = fmax(window, last);
        if (new_window) {
            last = window;
            window = 0.0;
        }
        /* A script or section gone quiet leaves once its average has died down. */
        if (i >= WPE__STAGE_COUNT && e.average_ms < 1e-4 && e.peak_ms == 0.0) continue;
        profiler->entries[kept] = e;
        profiler->window_peak[kept] = window;
        profiler->last_peak[kept] = last;
        profiler->fresh[kept] = false;
        ++kept;
    }
    profiler->count = kept;
}

/* ---- The overlay ----------------------------------------------------------------------------------- */

#define WPE__SHOWN_SCRIPTS 6u
#define WPE__SHOWN_SECTIONS 4u
#define WPE__LABEL_CHARS 16u

typedef struct wpe__Row {
    const WPE_ProfileEntry* entry;
    char label[WPE__LABEL_CHARS + 1];
} wpe__Row;

static float wpe__width(const WPE_BitmapFont* font, const char* text) {
    float width = 0.0f;
    for (const unsigned char* c = (const unsigned char*)text; *c; ++c)
        if (*c >= WPE_FONT_FIRST_CHAR && *c <= WPE_FONT_LAST_CHAR)
            width += (float)(font->glyphs[*c - WPE_FONT_FIRST_CHAR].width + font->glyph_spacing);
    return width;
}

/* A short label: a script's file name without "scripts/" and ".luau", cut to fit. */
static void wpe__label(const WPE_ProfileEntry* e, char* out) {
    const char* name = e->name;
    if (e->kind == WPE_PROFILE_SCRIPT) {
        const char* slash = strrchr(name, '/');
        if (slash) name = slash + 1;
    }
    size_t length = strlen(name);
    if (e->kind == WPE_PROFILE_SCRIPT && length > 5 && strcmp(name + length - 5, ".luau") == 0) length -= 5;
    if (length > WPE__LABEL_CHARS) length = WPE__LABEL_CHARS;
    memcpy(out, name, length);
    out[length] = '\0';
}

/* Picks up to `limit` entries of a kind, costliest first. */
static uint32_t wpe__pick(const wpe__Profiler* p, WPE_ProfileKind kind, uint32_t limit, wpe__Row* rows) {
    uint32_t count = 0;
    for (uint32_t i = WPE__STAGE_COUNT; i < p->count; ++i) {
        const WPE_ProfileEntry* e = &p->entries[i];
        if (e->kind != kind) continue;
        uint32_t at = count < limit ? count : limit;
        while (at > 0 && rows[at - 1].entry->average_ms < e->average_ms)
            --at;
        if (at >= limit) continue;
        const uint32_t last = count < limit ? count : limit - 1;
        for (uint32_t j = last; j > at; --j)
            rows[j] = rows[j - 1];
        rows[at].entry = e;
        wpe__label(e, rows[at].label);
        if (count < limit) ++count;
    }
    return count;
}

void wpe__profiler_draw(const wpe__Profiler* p, WPE_Draw2D* draw2d, const WPE_BitmapFont* font,
                        uint32_t canvas_width) {
    if (!p->on || !font || p->count == 0) return;
    enum { MAX_ROWS = WPE__STAGE_COUNT + WPE__SHOWN_SCRIPTS + WPE__SHOWN_SECTIONS };
    wpe__Row rows[MAX_ROWS];
    uint32_t count = 0;
    /* The stages that cost anything, in frame order; then the costliest scripts and sections. */
    for (uint32_t s = 0; s < WPE__STAGE_COUNT; ++s) {
        if (p->entries[s].average_ms < 0.01 && p->entries[s].peak_ms < 0.05) continue;
        rows[count].entry = &p->entries[s];
        wpe__label(&p->entries[s], rows[count].label);
        ++count;
    }
    const uint32_t systems = count;
    count += wpe__pick(p, WPE_PROFILE_SCRIPT, WPE__SHOWN_SCRIPTS, &rows[count]);
    const uint32_t scripts_end = count;
    count += wpe__pick(p, WPE_PROFILE_SECTION, WPE__SHOWN_SECTIONS, &rows[count]);

    /* Bars share one scale: the costliest average shown (at least 1 ms), so small stages still show. */
    double scale_ms = 1.0;
    for (uint32_t r = 0; r < count; ++r)
        scale_ms = fmax(scale_ms, rows[r].entry->average_ms);
    const float line = (float)font->line_height, pad = 3.0f;
    const float label_width = wpe__width(font, "scripts (update)") + 4.0f;
    const float bar_width = 48.0f, value_width = wpe__width(font, "00.00") + 2.0f;
    const float width = pad * 2.0f + label_width + bar_width + 4.0f + value_width;
    /* Whole pixels everywhere: bitmap glyphs drawn at half a pixel come out cut. */
    const float gap = floorf(line * 0.5f);
    const bool gap_before_scripts = systems > 0 && systems < count;
    const bool gap_before_sections = scripts_end > systems && scripts_end < count;
    const float gaps = (gap_before_scripts ? gap : 0.0f) + (gap_before_sections ? gap : 0.0f);
    const float height = pad * 2.0f + line * (float)(count + 1) + gaps;
    const float x0 = floorf((float)canvas_width - width - 2.0f), y0 = 2.0f;
    wpe_draw2d_rect(draw2d, (WPE_Rect){x0, y0, width, height}, wpe_color_rgba8(0, 0, 0, 170));

    char text[64];
    const double fps = p->frame_average_ms > 0.0 ? 1000.0 / p->frame_average_ms : 0.0;
    snprintf(text, sizeof text, "frame %.2f ms  %.0f fps", p->frame_average_ms, fps);
    wpe_draw2d_text(draw2d, font, x0 + pad, y0 + pad, text, wpe_color_rgba8(255, 220, 120, 255), 1.0f);
    float y = y0 + pad + line;
    for (uint32_t r = 0; r < count; ++r) {
        if ((r == systems && gap_before_scripts) || (r == scripts_end && gap_before_sections)) y += gap;
        const WPE_ProfileEntry* e = rows[r].entry;
        const WPE_Color color = e->kind == WPE_PROFILE_SYSTEM   ? wpe_color_rgba8(110, 170, 255, 255)
                                : e->kind == WPE_PROFILE_SCRIPT ? wpe_color_rgba8(120, 220, 120, 255)
                                                                : wpe_color_rgba8(255, 170, 90, 255);
        wpe_draw2d_text(draw2d, font, x0 + pad, y, rows[r].label, wpe_color_rgba8(230, 230, 230, 255), 1.0f);
        const float bx = x0 + pad + label_width, by = y + 1.0f, bh = line - 3.0f;
        wpe_draw2d_rect(draw2d, (WPE_Rect){bx, by, bar_width, bh}, wpe_color_rgba8(255, 255, 255, 30));
        const float filled = (float)fmin(e->average_ms / scale_ms, 1.0) * bar_width;
        if (filled > 0.0f) wpe_draw2d_rect(draw2d, (WPE_Rect){bx, by, fmaxf(filled, 1.0f), bh}, color);
        const float peak = (float)fmin(e->peak_ms / scale_ms, 1.0) * bar_width;
        wpe_draw2d_rect(draw2d, (WPE_Rect){bx + fmaxf(peak - 1.0f, 0.0f), by, 1.0f, bh},
                        wpe_color_rgba8(255, 255, 255, 220));
        snprintf(text, sizeof text, "%.2f", fmin(e->average_ms, 99.99));
        wpe_draw2d_text(draw2d, font, bx + bar_width + 4.0f, y, text, wpe_color_rgba8(230, 230, 230, 255),
                        1.0f);
        y += line;
    }
}
