/**
 * @file project.h
 * @brief Project settings: the "project.json" file at the root of a game's assets, read by the
 *        runtime at startup (display, simulation rate, audio levels, input actions, start scene).
 *
 * Format (documented in docs/scenes.md):
 *
 * @code
 * {
 *   "format": "wpe.project", "version": 1,
 *   "name": "My Game", "start_scene": "scenes/level1.scene", "font": "fonts/small.png",
 *   "display": {"width": 320, "height": 240, "scale": 3, "vsync": true, "resizable": true, "fullscreen":
 * false}, "simulation": {"fixed_rate": 60, "max_steps": 8}, "assets": {"max_texture_size": 256}, "audio":
 * {"master": 1, "effects": 1, "music": 0.8}, "scripting": {"memory_limit_mb": 64, "time_limit_ms": 250},
 *   "prefabs": {"crate": "meshes/crate.prim", "figure": "meshes/RiggedFigure.glb"},
 *   "input": {
 *     "keyboard_player": 1, "stick_deadzone": 0.2, "stick_outer": 0.95, "trigger_deadzone": 0.1,
 *     "press_threshold": 0.5,
 *     "actions": {
 *       "move_x": [{"key": "d"}, {"key": "a", "scale": -1}, {"pad_axis": "left_x"}],
 *       "jump": [{"key": "space"}, {"pad_button": "south"}],
 *       "menu_up": [{"pad_axis": "left_y", "range": "negative"}, {"key": "up"}]
 *     }
 *   }
 * }
 * @endcode
 *
 * Every key is optional except "format" and "version"; reading is strict (unknown keys are errors).
 *
 * - Dependencies: core (JSON), vfs, input, audio
 * - Thread affinity: any thread
 *
 * @author WillPowerEngine Team
 */
#ifndef WPE_ENGINE_PROJECT_H
#define WPE_ENGINE_PROJECT_H

#include <stdint.h>

#include "audio/audio.h"
#include "core/json.h"
#include "core/result.h"
#include "input/input.h"
#include "vfs/vfs.h"

#define WPE_PROJECT_FORMAT_VERSION 1u
/** Asset path of the project file inside a game's VFS. */
#define WPE_PROJECT_FILE "project.json"
#define WPE_PROJECT_NAME_MAX 64u
#define WPE_PROJECT_MAX_PREFABS 64u

/** A named prefab: scripts call wpe.instantiate(name) for the model at `path`. */
typedef struct WPE_ProjectPrefab {
    char name[WPE_PROJECT_NAME_MAX];
    char path[WPE_VFS_PATH_MAX];
} WPE_ProjectPrefab;

typedef struct WPE_ProjectSettings {
    char name[WPE_PROJECT_NAME_MAX];    /* Window title. */
    char start_scene[WPE_VFS_PATH_MAX]; /* Loaded at startup ("" = start empty). */
    char font[WPE_VFS_PATH_MAX];        /* Font for script text drawing ("" = none). */
    uint32_t width, height;             /* Framebuffer resolution (16..4096). */
    uint32_t scale;                     /* Initial window size multiplier (1..16). */
    bool vsync, resizable, fullscreen;
    double fixed_rate;                  /* Simulation steps per second (1..1000). */
    uint32_t max_steps;                 /* Fixed steps per frame before the clock drops time (1..64). */
    uint32_t max_texture_size;          /* Model texture cap (0 = original size). */
    float volumes[WPE_AUDIO_BUS_COUNT]; /* Bus gains (master, effects, music). */
    uint32_t script_memory_mb;          /* Luau heap limit (1..4096). */
    double script_time_limit_ms;        /* Per-callback budget (1..60000). */
    WPE_InputSettings input;            /* keyboard_player is 0-based here, 1-based in the file. */
    WPE_InputActionDesc actions[WPE_INPUT_MAX_ACTIONS];
    uint32_t action_count;
    WPE_ProjectPrefab prefabs[WPE_PROJECT_MAX_PREFABS];
    uint32_t prefab_count;
} WPE_ProjectSettings;

/** Fills `out` with the defaults: 320x240 at scale 3, 60 Hz, unit volumes, no actions. @thread_safety Any
 * thread */
void wpe_project_settings_default(WPE_ProjectSettings* out);

/**
 * Parses a project document. `out` is reset to the defaults first; on failure it holds defaults.
 *
 * @param name Used in error messages.
 * @return WPE_RESULT_OK; WPE_RESULT_ERROR_NULL_ARG; WPE_RESULT_ERROR_PARSE (logged with the line);
 *         WPE_RESULT_ERROR_UNSUPPORTED (newer format version); WPE_RESULT_ERROR_OUT_OF_MEMORY.
 *
 * @thread_safety Any thread
 */
WPE_Result wpe_project_parse(const char* text, size_t size, const char* name, WPE_ProjectSettings* out);

/** Reads and parses WPE_PROJECT_FILE from a VFS; also returns WPE_RESULT_ERROR_NOT_FOUND (logged).
 * @thread_safety Any thread */
WPE_Result wpe_project_load(const WPE_Vfs* vfs, WPE_ProjectSettings* out);

/**
 * Writes a project document (every setting, so the file documents itself).
 *
 * @return WPE_RESULT_OK, WPE_RESULT_ERROR_NULL_ARG or WPE_RESULT_ERROR_INVALID_ARG (a binding that
 *         has no name, i.e. a key code outside the named set; logged).
 *
 * @thread_safety Any thread
 */
WPE_Result wpe_project_write(const WPE_ProjectSettings* settings, WPE_JsonWriter* writer);

#endif /* WPE_ENGINE_PROJECT_H */
