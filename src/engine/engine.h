/**
 * @file engine.h
 * @brief The game runtime: owns every subsystem of a running game, loads its project and scenes
 *        and runs the frame loop. The standalone runtime (and later the editor's play mode) is a
 *        thin shell around it.
 *
 * A game is a set of files reached through one VFS: a development directory or a `.pak` archive
 * (possibly embedded in the game executable, see export.h). Its root holds `project.json`
 * (project.h), which names the start scene (scene_file.h).
 *
 * Frame (wpe_engine_frame()):
 *  1. Input: OS events are pumped into the input module and actions evaluated. A multiplayer
 *     session (wpe_engine_net_start()) takes its events: a player follows the host's scene.
 *  2. Simulation: the UI's focus and clicks (scripts' on_click), fixed steps (scripts'
 *     on_fixed_update, physics, collision callbacks), then the scripts' on_update, animation,
 *     transforms and audio.
 *  3. Rendering from the scene camera (a scene without one is 2D: only the clear color), the
 *     scene's post-processing, then the UI and the scripts' 2D drawing on top (not
 *     post-processed), and presenting to the window.
 *  4. Requests made by scripts during the frame (wpe.load_scene, wpe.quit) take effect, and the
 *     session's queued messages are sent.
 *
 * Headless engines (no window) still render, so frames can be captured with
 * wpe_engine_screenshot(); audio mixes without a device, in step with the frame time.
 *
 * - Dependencies: every engine module
 * - Thread affinity: main thread only
 *
 * @author WillPowerEngine Team
 */
#ifndef WPE_ENGINE_ENGINE_H
#define WPE_ENGINE_ENGINE_H

#include <stdint.h>

#include "assets/asset_manager.h"
#include "audio/audio.h"
#include "core/result.h"
#include "ecs/ecs.h"
#include "engine/export.h"
#include "engine/prefab.h"
#include "engine/project.h"
#include "engine/replication.h"
#include "engine/scene_file.h"
#include "input/input.h"
#include "network/network.h"
#include "physics/physics.h"
#include "renderer/renderer.h"
#include "script/script.h"
#include "ui/ui.h"
#include "vfs/vfs.h"

typedef struct WPE_Engine WPE_Engine;

typedef struct WPE_EngineDesc {
    /* The game's files: exactly one of these. */
    const char* project_dir; /* An OS directory (development; scripts can be hot-reloaded). */
    const char* pak_path;    /* An archive, or an executable carrying one. */

    const char* scene; /* Scene to start with instead of the project's start scene (NULL = project's). */
    bool headless;     /* No window: nothing is shown and no audio device is opened. */
    /* No window of its own, for hosts such as the editor: the host shows the framebuffer
     * (wpe_engine_renderer()) and forwards OS events with wpe_engine_handle_event(). Audio still
     * plays through a device. */
    bool embedded;
    bool no_audio_device; /* Mix audio without an output device even with a window (silent). */
    uint32_t workers;     /* Render worker threads (0 = one per core). */
    /* Where wpe.save_data keeps its files. NULL: the user's directory for the game,
     * wpe_platform_save_directory("WillPowerEngine", <project name>). Either is made, if missing, when a
     * script first saves or loads. */
    const char* save_dir;
} WPE_EngineDesc;

/**
 * Starts a game: mounts its files, reads project.json, creates every subsystem and loads the start
 * scene.
 *
 * @return WPE_RESULT_OK; WPE_RESULT_ERROR_NULL_ARG; WPE_RESULT_ERROR_INVALID_ARG (not exactly one
 *         of project_dir and pak_path); project, mount, subsystem or scene errors (all logged).
 *
 * @thread_safety Main thread only
 */
[[nodiscard]] WPE_Result wpe_engine_create(const WPE_EngineDesc* desc, WPE_Engine** out_engine);

/** Shuts everything down in dependency order (scripts get on_destroy); NULL is a no-op. @thread_safety Main
 * thread only */
void wpe_engine_destroy(WPE_Engine* engine);

/**
 * Replaces the world's contents with a scene now (between frames): every entity is destroyed, the
 * scene's entities are created and its environment (render settings, post-processing, gravity,
 * music) applied. On failure the world is left empty.
 *
 * @return WPE_RESULT_OK, WPE_RESULT_ERROR_NULL_ARG or a wpe_scene_load_asset() error.
 *
 * @thread_safety Main thread only; not from inside a frame (scripts use wpe.load_scene)
 */
WPE_Result wpe_engine_load_scene(WPE_Engine* engine, const char* path);

/** wpe_engine_load_scene() from a scene document in memory (`name` labels messages). @thread_safety Main
 * thread only */
WPE_Result wpe_engine_load_scene_text(WPE_Engine* engine, const char* text, size_t size, const char* name);

/** @return The VFS the game's files are read from (borrowed). @thread_safety Main thread only */
const WPE_Vfs* wpe_engine_vfs(const WPE_Engine* engine);

/**
 * Runs one frame (see the file comment).
 *
 * @param dt Seconds since the previous frame (clamped to 0.25 to survive stalls).
 * @return WPE_RESULT_OK or the first error of a subsystem (rendering, a requested scene load...).
 *
 * @thread_safety Main thread only
 */
WPE_Result wpe_engine_frame(WPE_Engine* engine, double dt);

/** Forwards an OS event to the game's input (embedded engines; others pump their own). @thread_safety Main
 * thread only */
void wpe_engine_handle_event(WPE_Engine* engine, const WPE_PlatformEvent* event);

/**
 * Turns the simulation on or off (on by default). While off, frames still read input, update
 * transforms and render, but scripts, physics, animation and audio do not run: the editor's edit
 * mode. Turning it on starts the scene's music; turning it off stops it.
 *
 * @thread_safety Main thread only
 */
void wpe_engine_set_simulating(WPE_Engine* engine, bool simulating);
bool wpe_engine_simulating(const WPE_Engine* engine);

/** @return false once the window was closed or a quit was requested. @thread_safety Main thread only */
bool wpe_engine_running(const WPE_Engine* engine);

/** Asks the game to stop (wpe_engine_running() turns false). @thread_safety Main thread only */
void wpe_engine_request_quit(WPE_Engine* engine);

/** Withdraws a quit request (hosts such as the editor end play mode instead). @thread_safety Main thread only
 */
void wpe_engine_cancel_quit(WPE_Engine* engine);

typedef struct WPE_RunOptions {
    uint32_t frames;        /* Stop after this many frames (0 = until quit). */
    double fixed_dt;        /* Frame time to simulate instead of the real clock (0 = real time). */
    const char* screenshot; /* Save the last frame as PNG at this OS path (NULL = none). */
    bool watch;             /* Hot-reload changed scripts twice a second (directory projects). */
} WPE_RunOptions;

/**
 * Runs frames until the game quits (or options->frames have run).
 *
 * @param options Optional (NULL = until quit, real time).
 * @return WPE_RESULT_OK or the error that stopped the game.
 *
 * @thread_safety Main thread only
 */
WPE_Result wpe_engine_run(WPE_Engine* engine, const WPE_RunOptions* options);

/** Saves the current framebuffer as PNG. @return WPE_RESULT_OK, WPE_RESULT_ERROR_NULL_ARG or
 * WPE_RESULT_ERROR_IO. @thread_safety Main thread only */
WPE_Result wpe_engine_screenshot(const WPE_Engine* engine, const char* os_path);

/** The asset path of the scene file last loaded ("" after loading from text, or a failed load).
 * @thread_safety Main thread only */
const char* wpe_engine_scene_path(const WPE_Engine* engine);

/** Saves the world and environment as a scene file. @thread_safety Main thread only */
WPE_Result wpe_engine_save_scene(const WPE_Engine* engine, const char* os_path);

/** The entity rendered from (and listened from); WPE_ENTITY_NULL picks the first camera found.
 * @thread_safety Main thread only */
void wpe_engine_set_camera(WPE_Engine* engine, WPE_Entity camera);
WPE_Entity wpe_engine_camera(const WPE_Engine* engine);

/* Subsystems (borrowed; valid until wpe_engine_destroy()). @thread_safety Main thread only */
WPE_Ecs* wpe_engine_ecs(const WPE_Engine* engine);
WPE_AssetManager* wpe_engine_assets(const WPE_Engine* engine);
WPE_Input* wpe_engine_input(const WPE_Engine* engine);
WPE_Physics* wpe_engine_physics(const WPE_Engine* engine);
/** Parsed prefab files (for scene contexts that load or instantiate prefabs). */
WPE_PrefabCache* wpe_engine_prefabs(const WPE_Engine* engine);
WPE_Audio* wpe_engine_audio(const WPE_Engine* engine);
WPE_Ui* wpe_engine_ui(const WPE_Engine* engine);
WPE_ScriptHost* wpe_engine_scripts(const WPE_Engine* engine);
WPE_Renderer* wpe_engine_renderer(const WPE_Engine* engine);
const WPE_ProjectSettings* wpe_engine_project(const WPE_Engine* engine);
/** The current scene's environment; changes apply from the next frame. */
WPE_SceneEnvironment* wpe_engine_environment(WPE_Engine* engine);
/** The game clock's speed (slow motion, hit-stops) and shakes; reset by every scene load. */
WPE_GameFeel* wpe_engine_game_feel(WPE_Engine* engine);

/** Debug overlays drawn over the scene (depth tested, not post-processed, under the scripts' 2D). */
typedef struct WPE_EngineDebugDraw {
    bool physics;       /* Collider wireframes, colored by body type (see wpe_physics_debug_draw()). */
    bool grid;          /* A grid on the ground plane (y = 0) around the camera, X axis red, Z axis blue. */
    float grid_spacing; /* World units between grid lines (0 = 1); every 10th line is brighter. */
} WPE_EngineDebugDraw;

/** Sets the debug overlays (all off by default). @thread_safety Main thread only */
void wpe_engine_set_debug_draw(WPE_Engine* engine, const WPE_EngineDebugDraw* debug_draw);

/** @return The debug overlays. @thread_safety Main thread only */
WPE_EngineDebugDraw wpe_engine_debug_draw(const WPE_Engine* engine);

/** Whether the UI (ui/ui.h) is drawn over the scene (default true; the editor's 3D view turns it off).
 * @thread_safety Main thread only */
void wpe_engine_set_draw_ui(WPE_Engine* engine, bool draw_ui);
bool wpe_engine_draws_ui(const WPE_Engine* engine);

/** Whether the 3D scene is rendered from the camera (default true). Off, frames show the clear color with the
 * UI and 2D drawing on top, as a scene without a camera does (the editor's 2D view of such a scene).
 * @thread_safety Main thread only */
void wpe_engine_set_draw_scene(WPE_Engine* engine, bool draw_scene);
bool wpe_engine_draws_scene(const WPE_Engine* engine);

/* ---- Multiplayer (docs/networking.md) ------------------------------------------------------------ */

/** How to start a multiplayer session. Ports, player limits and timeouts come from the project. */
typedef struct WPE_EngineNetDesc {
    const char* join; /* NULL: host a game; otherwise the host's address ("a.b.c.d[:port]", or a name). */
    const char* player_name;      /* NULL: "Host" or "Player". */
    bool hidden;                  /* Host: do not answer LAN discovery. */
    bool local_only;              /* This machine only (127.0.0.1): tests, and no firewall prompt. */
    uint16_t port;                /* Host: 0 means the project's port. */
    WPE_NetConditions conditions; /* A simulated bad network (see transport.h). */
    WPE_NetHub* hub;              /* Tests: play on this in-memory network instead of UDP. Borrowed. */
} WPE_EngineNetDesc;

/**
 * Hosts or joins a game (ending any session already running).
 *
 * A host shares its scene: every scene it loads from a file is loaded by every player too, and a
 * player joining loads the host's current one. Only the same game (wpe_engine_game_id()) can join.
 *
 * @return WPE_RESULT_OK; WPE_RESULT_ERROR_NULL_ARG; address, transport and discovery errors (logged).
 *
 * @thread_safety Main thread only
 */
[[nodiscard]] WPE_Result wpe_engine_net_start(WPE_Engine* engine, const WPE_EngineNetDesc* desc);

/** Leaves or closes the session (the others see this side leave); no-op without one. @thread_safety Main
 * thread only */
void wpe_engine_net_stop(WPE_Engine* engine);

/** The running session (also after it ENDED, until stopped), or NULL. @thread_safety Main thread only */
WPE_Session* wpe_engine_session(const WPE_Engine* engine);

/**
 * Host: gives a networked entity (one with a WPE_NetIdentity, see replication.h) to a player, who
 * simulates it from then on; everyone else shows it as that player sends it.
 *
 * @return As wpe_replication_set_owner().
 *
 * @thread_safety Main thread only
 */
WPE_Result wpe_engine_net_set_owner(WPE_Engine* engine, WPE_Entity entity, WPE_PlayerId player);

/** Whether this side simulates the entity (always true outside a multiplayer game). @thread_safety Main
 * thread only */
bool wpe_engine_net_is_local(const WPE_Engine* engine, WPE_Entity entity);

/**
 * The game's network identity: a hash of its project file, scenes, prefabs and scripts (line endings
 * ignored), so only the same game and build can play together. Computed once, at first use.
 *
 * @thread_safety Main thread only
 */
uint64_t wpe_engine_game_id(WPE_Engine* engine);

/** Per-frame statistics of the last frame. Times are wall-clock milliseconds. */
typedef struct WPE_EngineStats {
    uint64_t frames;
    uint32_t fixed_steps;  /* Fixed steps run by the last frame. */
    double frame_ms;       /* Wall time spent in the last wpe_engine_frame(). */
    double simulate_ms;    /* Scripts, physics, animation, transforms and particles (0 in edit mode). */
    double physics_ms;     /* The physics steps, part of simulate_ms. */
    double render_ms;      /* The scene pass: culling, shadows, the sky and rasterizing. */
    double postfx_ms;      /* Post-processing. */
    double overlay_ms;     /* Debug lines, the UI and scripts' 2D drawing, and presenting. */
    WPE_RenderStats scene; /* The scene pass's draws, triangles and shaded pixels. */
    uint32_t entities;     /* Live entities after the frame. */
} WPE_EngineStats;

/** @return Statistics of the last frame. @thread_safety Main thread only */
WPE_EngineStats wpe_engine_stats(const WPE_Engine* engine);

#endif /* WPE_ENGINE_ENGINE_H */
