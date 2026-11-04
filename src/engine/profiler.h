/**
 * @file profiler.h
 * @brief Private to the engine: the profiler behind wpe_engine_set_profiling / wpe_engine_profile and its
 * overlay (WPE_EngineDebugDraw.profiler).
 *
 * Each frame the engine laps the stages in order (wpe__profiler_lap charges the time since the last lap to a
 * stage), then folds the frame into smoothed averages and recent peaks, with the scripts' times taken from
 * the script host.
 *
 * - Dependencies: core, platform (time), graphics (draw2d), assets (font), script
 * - Thread affinity: main thread
 *
 * @author WillPowerEngine Team
 */
#ifndef WPE_ENGINE_PROFILER_H
#define WPE_ENGINE_PROFILER_H

#include "assets/font.h"
#include "core/profile.h"
#include "graphics/draw2d.h"
#include "script/script.h"

typedef enum wpe__Stage {
    WPE__STAGE_INPUT = 0,
    WPE__STAGE_NETWORK,
    WPE__STAGE_UI,
    WPE__STAGE_SCRIPTS_FIXED,
    WPE__STAGE_NAVIGATION,
    WPE__STAGE_PHYSICS,
    WPE__STAGE_SCRIPTS_UPDATE,
    WPE__STAGE_ANIMATION,
    WPE__STAGE_EFFECTS,
    WPE__STAGE_TRANSFORMS,
    WPE__STAGE_AUDIO,
    WPE__STAGE_RENDER,
    WPE__STAGE_POSTFX,
    WPE__STAGE_OVERLAY,
    WPE__STAGE_PRESENT,
    WPE__STAGE_SCENE_LOAD,
    WPE__STAGE_OTHER, /* The frame's time no lap claimed. */
} wpe__Stage;
#define WPE__STAGE_COUNT 17u

typedef struct wpe__Profiler {
    bool on;
    double mark;                       /* Time of the last lap (seconds). */
    double stage_ms[WPE__STAGE_COUNT]; /* This frame so far. */
    WPE_ProfileEntry entries[WPE_PROFILE_MAX_ENTRIES];
    double window_peak[WPE_PROFILE_MAX_ENTRIES]; /* The highest frame of the running window... */
    double last_peak[WPE_PROFILE_MAX_ENTRIES];   /* ...and of the one before. */
    bool fresh[WPE_PROFILE_MAX_ENTRIES];         /* No frame yet: the first one sets the average. */
    uint32_t count;
    double window_time; /* Seconds into the running peak window. */
    double frame_average_ms;
} wpe__Profiler;

/* Starts a frame's laps at `now` (seconds). */
void wpe__profiler_begin_frame(wpe__Profiler* profiler, double now);

/* Charges the time since the last lap to `stage`. */
void wpe__profiler_lap(wpe__Profiler* profiler, wpe__Stage stage);

/* Ends a frame of `frame_ms` (real `dt` seconds): the rest goes to "other"; while on, the stages and the
 * scripts' times (taken from `scripts`) go into the averages and peaks. */
void wpe__profiler_end_frame(wpe__Profiler* profiler, double frame_ms, double dt, WPE_ScriptHost* scripts);

/* Turns profiling on (starting over) or off, the script host's timing with it. */
void wpe__profiler_set_on(wpe__Profiler* profiler, bool on, WPE_ScriptHost* scripts);

/* The overlay panel at the canvas's top-right corner (into draw2d; nothing without a font). */
void wpe__profiler_draw(const wpe__Profiler* profiler, WPE_Draw2D* draw2d, const WPE_BitmapFont* font,
                        uint32_t canvas_width);

#endif /* WPE_ENGINE_PROFILER_H */
