# AGENTS.md — WillPowerEngine Development Guidelines

This document provides concrete guidelines for AI agents contributing to the WillPowerEngine project. Follow these rules consistently to ensure production-grade, maintainable code.

---

## 1. Core Principles

### 1.1 Quality First
- **Production-ready code only.** No prototypes, no "this will work for now" patches.
- **No hacks, no shortcuts.** Every line must be justified and defensible.
- **Stability over speed.** Prefer correct solutions over clever ones.

### 1.2 Autonomy
- Ask questions only when you lack critical information to proceed.
- Make reasonable assumptions when blocked, document them in code comments.
- Use the test scene to validate your understanding of engine behavior.

### 1.3 Scope Discipline
- **Do not look outside the working directory.**
- All dependencies come from the tech stack specified in `WillPowerEngine Spec (Refined).md`.
- New dependencies require justification and approval.

---

## 2. Coding Standards

### 2.1 C23 Conventions

#### Naming
| Element | Convention | Example |
|---------|------------|---------|
| Files | snake_case | `render_pipeline.c`, `render_pipeline.h` |
| Types/structs | PascalCase with prefix | `WPE_Entity`, `WPE_Mesh` |
| Functions | module_snake_case | `wpe_renderer_init()`, `wpe_ecs_add_component()` |
| Constants/macros | UPPER_SNAKE_CASE | `WPE_MAX_LIGHTS`, `WPE_VERSION_STRING` |
| Private symbols | `wpe__` prefix (double underscore) | `wpe__renderer_internal_state`, `wpe__ecs_alloc_entity()` |

#### Structure
- One logical unit per `.c` file (e.g., one system, one subsystem).
- Public API in `*.h` files with clear includes guards (`#ifndef WPE_MODULE_H`).
- Forward declarations over full includes when possible.
- Use `_Static_assert` for compile-time invariants.

#### Memory
- All allocations must have corresponding frees; prefer RAII-style wrappers.
- Document ownership semantics for every pointer parameter.
- Use `wpe_alloc(), wpe_free()` wrappers (not raw `malloc/free`) for tracking.
- Stack allocation preferred for small, short-lived objects.

#### Error Handling
- Return error codes, not exceptions (C23 doesn't have exceptions).
- Define `WPE_Result` enum: `WPE_RESULT_OK`, then specific `WPE_RESULT_ERROR_*` codes (e.g. `WPE_RESULT_ERROR_NULL_ARG`, `WPE_RESULT_ERROR_NOT_FOUND`).
- Log errors through `wpe_log_error()` before returning.
- Never silently ignore failures.

```c
// Good
typedef enum {
    WPE_RESULT_OK,
    WPE_RESULT_ERROR_NULL_ARG,
    WPE_RESULT_ERROR_OUT_OF_MEMORY,
    WPE_RESULT_ERROR_INVALID_STATE
} WPE_Result;

WPE_Result wpe_mesh_load(WPE_Mesh* mesh, const char* path) {
    if (!mesh || !path) return WPE_RESULT_ERROR_NULL_ARG;
    
    // ...
}

// Internal/private function (not exposed in header)
static WPE_Result wpe__mesh_parse_header(FILE* f) {
    // Internal implementation
}
```

### 2.2 Multithreading Rules

- Shared state requires explicit synchronization documentation.
- Prefer lock-free structures where performance-critical.
- Use SDL mutexes (`SDL_LockMutex`) only when unavoidable.
- Render job graph should be data-driven, not command-driven.
- Main thread owns window/input; worker threads own rendering jobs.

```c
// Document thread affinity clearly
// Thread Affinity: Main thread only
void wpe_input_poll(SDL_Window* window);

// Thread Affinity: Worker pool threads
void wpe_render_submit_job(WPE_RenderJob* job);
```

### 2.3 Assertions and Logging

- Use `wpe_assert()` for programmer errors (should never trigger in release).
- Use `wpe_check()` for runtime validation (may fail in production).
- Log levels: `TRACE`, `DEBUG`, `INFO`, `WARN`, `ERROR`, `CRITICAL`.
- Always include module name in log output.
- Internal logging helpers use `wpe__` prefix: `wpe__log_trace()`, `wpe__log_error_internal()`.

---

## 3. Testing Requirements

### 3.1 Test Coverage Rules

Every public function must have tests. Minimum coverage targets:
- **Core ECS:** 95%+ branch coverage
- **Renderer:** 80%+ path coverage (hardware-independent tests)
- **Physics:** 90%+ boundary condition coverage
- **Scripting API:** 85%+ end-to-end coverage

### 3.2 Test Organization

```
tests/
├── unit/           # Isolated component tests
├── scene/          # Full scene playback tests (label `integration`)
├── golden/         # Reference-image comparisons
├── stress/         # Long-running, memory leak detection
└── tools/          # CMake scripts driving the command-line tools (pak and export round trips)
```

### 3.3 Test Patterns

#### Unit Tests
Tests use the in-repo framework `tests/framework/wpe_test.h`. Test macros are `WPE_TEST_ASSERT*` (distinct from the engine's `wpe_assert()`). The runner fails a test if it leaks `wpe_alloc` memory or triggers an engine `wpe_assert()`.

```c
static void test_ecs_entity_creation(void) {
    WPE_Ecs* ecs = wpe_ecs_create();
    WPE_TEST_ASSERT(ecs != NULL);

    WPE_Entity e = wpe_ecs_spawn(ecs);
    WPE_TEST_ASSERT(e != WPE_ENTITY_NULL);

    wpe_ecs_destroy(ecs);
}

WPE_TEST_MAIN(WPE_TEST_CASE(test_ecs_entity_creation))
```

Register each test file in `tests/<category>/CMakeLists.txt` with `wpe_add_test(<module>_<area> SOURCES ... LIBS ... LABELS ...)`.

#### Integration Tests
- Use the master test scene `assets/scenes/test_full.scene` (run by `tests/scene/test_full_scene.c` through the engine, headless).
- Run through scripted sequences that exercise all systems.
- Validate state at checkpoints with assertions (scripts report progress through entity names; any script error fails the test).

#### Golden Tests
- Save renderer output frames as reference images.
- Compare new builds pixel-by-pixel (allow minor floating-point drift).
- Update golden files only after manual verification.

### 3.4 Automated Testing

```bash
# Run all tests
cmake --build build --target test_all

# Run specific test category / module
ctest --test-dir build -R "^ecs_" --output-on-failure

# Run a single test case inside one test executable
./build/tests/unit/test_core_log --filter test_log_filters_below_min_level
```

Leak checking runs in every test through the `wpe_alloc` tracker. Use `-DSANITIZERS=ON` for ASan/UBSan, which works on Linux but not on MinGW.

### 3.5 Test Scene Requirements

The master test scene (`assets/scenes/test_full.scene`; scenes live with the other assets of the sample project) must:
- Include at least one entity per component type.
- Exercise all scripting APIs within 60 seconds of simulated time.
- Trigger edge cases: zero components, max entities, extreme transforms.
- Validate post-processing pipeline (at least 3 effects active).
- Test prefab instantiation and destruction cycles.

---

## 4. Architecture Guidelines

### 4.1 ECS Design

Implemented in `src/ecs/ecs.h` (see its file comment for the full contract):

```c
// Component type ids are integers; built-ins have stable values (they are serialized),
// games use ids from WPE_COMP_USER_FIRST upward.
typedef uint32_t WPE_ComponentType;
enum WPE_BuiltinComponent { WPE_COMP_NONE = 0, WPE_COMP_TRANSFORM = 1, /* ... */ WPE_COMP_USER_FIRST = 32 };

// Each module registers its own components (with optional on_add/on_remove hooks) at init.
wpe_ecs_register_component(ecs, WPE_COMP_MESH, &(WPE_ComponentDesc){.name = "Mesh", .size = sizeof(WPE_Mesh), .alignment = alignof(WPE_Mesh)});

// Iterate: the smallest required pool drives the loop.
WPE_EcsIter it = wpe_ecs_query(ecs, (WPE_EcsQuery){.all = WPE_ECS_MASK(WPE_COMP_TRANSFORM)});
while (wpe_ecs_iter_next(&it)) { WPE_Transform* t = wpe_ecs_iter_get(&it, WPE_COMP_TRANSFORM); }
```

**Rules:**
- Components are POD (Plain Old Data). No constructors, no virtual methods. Resource cleanup goes in `on_remove` hooks.
- One sparse-set pool per component type: a dense contiguous array with swap-remove, plus a sparse index for O(1) lookup. Batch systems can use `wpe_ecs_pool_view()`.
- Entity IDs are 32-bit: 20-bit slot index and 12-bit generation. Generation 0 is never issued, so `WPE_ENTITY_NULL == 0`. Slots whose generation is exhausted are retired, never wrapped.
- Never remove components or despawn while a query is iterating. Use `wpe_ecs_despawn_deferred()` + `wpe_ecs_flush_deferred()`. Adding components and spawning entities during iteration is allowed.
- Any structural change can invalidate component pointers, so re-fetch them afterwards.
- Hooks may edit component data but must not make structural changes.
- Transforms and the parent/child hierarchy live in `src/scene/` (`wpe_scene_*`). Always change parenting through `wpe_scene_set_parent()`.

### 4.2 Renderer Pipeline

```
Input (scene graph) 
    ↓
Cull (frustum, occlusion)
    ↓
Sort (painters algorithm, depth buckets)
    ↓
Rasterize (tile-based, per-thread)
    ↓
Composite (post-processing chain)
    ↓
Output (SDL texture)
```

**Implementation (`src/renderer/`):** the stages run inside `wpe_renderer_end_frame()`, in this order:

1. Stable sort of draws by `sort_key`.
2. Vertex shading, in parallel chunks of 1024 vertices.
3. Clipping, culling and triangle setup, in parallel chunks of 512 triangles. Clipping is homogeneous against the near and far planes and a 4× guard band. Setup snaps vertices to 28.4 fixed point.
4. Serial binning into 32×32-pixel tiles, keeping draw order.
5. Per-tile rasterization in parallel: int64 edge functions with the top-left fill rule, depth test, perspective-correct varyings, fragment shader, then blend.

**Parallelism strategy and rules:**
- Chunk boundaries are fixed and never depend on the thread count, so output is bit-identical for any number of workers; a test enforces this. Never make results depend on which thread runs a chunk.
- Sorting and binning are single-threaded (fast enough for the target resolutions).
- Each tile is owned by exactly one thread, so tiles need no locking.
- Per-frame memory comes from a frame arena, and per-job buffers persist between frames, so steady-state frames make no heap allocations (also enforced by a test).
- Shaders are plain C function pointers called concurrently. They must be free of side effects apart from writing their outputs.
- Blend modes: opaque, alpha, additive, and `WPE_BLEND_ALPHA_DITHER` (4×4 Bayer screen-door transparency, order-independent).
- Adaptive subdivision runs inside the parallel triangle stage. It splits edges, not triangles: whether an edge is split depends only on its own two endpoints, and midpoints are computed in a canonical order, so meshes stay crack-free. New vertices come from the program's `vertex_lerp` and are run through the vertex shader. Per-thread scratch for this is allocated in the serial planning stage.
- Post-processing (`graphics/postfx.h`) runs between frames. Effects are multi-pass, process rows in parallel, and swap between two color buffers.

### 4.2.1 Lighting model (hybrid, decided in Milestone 5)

- **Per vertex:** ambient, up to `WPE_MAX_OBJECT_LIGHTS` (4) point/spot lights per object (the most influential are chosen per object's bounding sphere), and the sun.
- **Subdivision:** adaptive subdivision near the camera keeps vertex lighting smooth up close.
- **Per pixel:** the sun's shadow (orthographic shadow map, texel-snapped, 3×3 PCF), plus normal-mapped sun lighting using tangent-space sun and half vectors.
- **Materials:** traditional diffuse/specular/gloss. Metallic-roughness inputs are converted per pixel, and ambient is also reflected specularly as a uniform environment.
- **Fog:** applied in the material shader.
- **Light falloff:** windowed and inverse-square-like, `(1 - (d/r)^4)^2 / (1 + (4d/r)^2)`. It is exactly zero at the light's range.
- **Shadow limits:** only the main directional light casts shadows. Point and spot lights don't (NOTE: limitation).

### 4.3 Scripting Integration

```luau
-- Entity script template
local MyEntity = {}

function MyEntity:on_start()
    -- Called once when entity is spawned
end

function MyEntity:on_update(dt)
    -- Variable-rate update (frame timing)
end

function MyEntity:on_fixed_update(dt)
    -- Fixed-rate update (physics alignment)
end

function MyEntity:on_destroy()
    -- Cleanup resources
end

return MyEntity
```

**Implementation:** `src/script/` (public API in `script/script.h`; Luau API reference in `docs/scripting.md`).
- The module is a C host around the Luau VM. Luau is built with `LUA_USE_LONGJMP=1` so its errors can unwind through C bindings.
- The `on_reload(self)` callback runs after a hot reload.
- On a despawn, the script's `on_destroy` runs before hierarchy and transform teardown, because ECS remove hooks run in descending component id order. Physics components (ids 9 and 10) are torn down before it.
- Physics callbacks `on_collision_enter/exit(other)` and `on_trigger_enter/exit(other)` are dispatched by `wpe_script_host_dispatch_physics()` after each `wpe_physics_step()`, to both entities of every event.

### 4.3.1 Physics Integration

**Implementation:** `src/physics/` (public API in `physics/physics.h`), wrapping Box3D privately.
- `WPE_RigidBody` and `WPE_Collider` are ECS components (one collider per entity). A collider without a rigid body is static level geometry.
- Physics steps at the fixed rate. Each step creates missing bodies, follows game-code moves of entities, simulates, writes dynamic and kinematic poses back into the parent-relative Transform, and collects contact and trigger events.
- Settings and world scale are read when a body or shape is created; `wpe_physics_refresh()` rebuilds them.
- Box3D allocates through `wpe_alloc`, and its assertions go through the engine's assert handler.
- Queries: rays, shape casts (box, sphere, capsule) and overlaps, all through a `WPE_QueryFilter` (ignore an entity, include triggers). Box3D's queries report triggers too, so every callback filters.
- `WPE_CharacterController` (component 20) is a kinematic capsule moved by `wpe_character_move()`, built on Box3D's mover tools (`b3World_CollideMover` → `b3SolvePlanes` → `b3World_CastMover`). Engine rules on top:
  - Steep planes get a vertical companion plane, and velocity is clipped against those first, so a character can't climb or skid up slopes steeper than `max_slope`.
  - On walkable ground a downward velocity is dropped and the character snaps down instead, so it doesn't slide downhill.
  - Dynamic bodies are soft (it may sink in `WPE__PUSH_DEPTH`, and sweeps ignore them), so its kinematic body pushes them.
  - Steps: a held-back grounded move is retried lifted by `step_height` and set down again; an edge under the capsule counts as ground when the surface inside it is walkable.
  - Walks and teleports: the body follows a walk (to where `wpe_character_move()` left the entity) with velocity over the step, so it pushes bodies; any other change of the entity's position is game code moving it, and the body is placed there directly (at the next move or step). Sending it across with velocity would take several steps for long distances (Box3D caps body speed) and enter every trigger on the way.
- NOTE: limitation. The solver is single-threaded, because Box3D's task API needs asynchronous tasks that the job system doesn't provide yet. Colliders don't combine across the hierarchy into compound bodies.

**Script host responsibilities:**
- Expose ECS API to Luau (entity manipulation, component access).
- Hot reload: detect script changes, restart scripts preserving state.
- Sandbox: prevent arbitrary file/system access.
- Error recovery: script crashes don't crash the engine.

### 4.4 Asset Management

Implemented in `src/vfs/` (virtual file system and `.pak` archives) and `assets/asset_manager.h`. The format, the tool and the usage rules are in `docs/assets.md`.

```c
// Assets are shared, reference-counted and addressed by generational handles (0 = none).
WPE_AssetHandle model = WPE_ASSET_NONE;
wpe_asset_load(assets, WPE_ASSET_MODEL, "meshes/helmet.glb", &model);  // cached by (type, path)
const WPE_Model* data = wpe_asset_model(assets, model);              // borrowed while referenced
wpe_asset_release(assets, model);                                   // the last reference destroys it
```

- **Asset paths:** relative and `/`-separated, resolved through a `WPE_Vfs` of mounted directories and `.pak` archives. Later mounts win, which supports patch archives and dev overrides.
- **Ownership:** every component that references an asset holds one reference from its add hook to its remove hook:
  - mesh renderer and animator (model)
  - collider (convex/mesh model)
  - audio source (sound)

  The script host keeps prefab models and the sounds it has used referenced.
- **Asset types:** models and fonts are built in, and the audio module registers sounds. Game-defined types start at `WPE_ASSET_USER_FIRST`. Assets created in code enter through `wpe_asset_add`.
- **Archives:** `.pak` v1 stores each file as-is or as one zstd frame (when that saves at least 5%), with per-file CRC-32 and a validated, sorted index. Archives are built reproducibly by `tools/wpe_pak`. Compression uses **zstd** (the documented choice over LZ4: better ratios at comparable decode speed).
- **Model conversion (glTF):** metallic-roughness inputs are kept and converted per pixel to diffuse/specular/gloss. Color factors are stored sRGB-encoded. Required extensions outside the supported list are rejected.

- **Primitive models:** `.prim` JSON files describe planes, boxes and spheres with a material; the model loader builds them, so scenes need no code for simple geometry.
- **Embedded archives:** an archive appended to another file (the exported game executable) is found through a 24-byte trailer; offsets inside stay archive-relative.

**Rules:**
- Assets are loaded through the VFS (`.pak` archives in shipped games).
- Shared assets are reference-counted.
- NOTE(limitation): scenes load their assets synchronously when the scene loads; streaming in large scenes is not implemented (scenes at the target scale load quickly).
- Texture streaming is not required, because target resolutions are low.

### 4.5 Audio

Implemented in `src/audio/` (miniaudio, private).
- Sounds (WAV/FLAC/MP3) are assets decoded once to float PCM and shared by voices; voices resample to the mixer rate on playback.
- `WPE_AudioSource` components play at their entity's position, with inverse-distance attenuation and panning; the listener follows one entity (usually the camera).
- One-shots use a pool of 64 voices (the oldest one-shot is stolen when full). Music streams from the VFS with crossfades.
- There are master, effects and music buses.
- Without an output device, mixing runs on demand (`wpe_audio_render`), which is what tests and headless runs use.

### 4.6 Input

Implemented in `src/input/` (public API in `input/input.h`; SDL3 private).
- Devices: physical keys (USB HID codes, layout-independent), the mouse (framebuffer coordinates, because the window converts event coordinates; per-frame deltas and wheel; capture for mouse look), and gamepads with the SDL standard layout.
- Gamepads occupy player slots 0..3: a new pad takes the lowest free slot and a disconnected one frees it (hot-plug). Sticks get a rescaled radial dead zone, triggers a linear one, and rumble is supported.
- Actions are named and bound to keys, mouse buttons, pad buttons and pad axes at once. Their value is the clamped sum of the bindings, and an action is "down" past a press threshold. Queries name a player slot or `WPE_INPUT_ANY_PLAYER`; the keyboard and mouse belong to one configurable player.
- The protocol each frame is `begin_frame` → `handle_event` for every OS event → `update` → queries. Edges belong to frames, and taps shorter than a frame still report both edges.
- Events reach the module as opaque `WPE_PlatformEvent`s from the platform's pump. Tests push real SDL events and use SDL virtual joysticks, so no hardware is needed.

### 4.5.1 Billboards

Implemented in `src/graphics/billboard.c` (component in `graphics/graphics.h`). Docs: `docs/scenes.md#billboards`.
- A billboard is a quad built on the CPU each frame for the camera (`wpe_billboard_corners`) and drawn by `wpe_scene_render` with its own shader: depth tested and fogged like meshes, unlit by default, optionally lit (ambient, point/spot lights, the sun with its shadow, half-Lambert so quads facing away stay readable).
- Blending decides sorting: cutouts are opaque (no sort), alpha and additive go back to front with transparent meshes.
- Sprite-sheet frames advance in the simulation (`wpe_billboard_update`); scripts and scenes can set the frame directly.
- Particles (`graphics/particles.c`) build on them: an emitter component owns its particles, simulates them in the engine's simulation (after transforms; integration chunked on the job system, spawning and compaction serial, so the result is the same with any thread count) and draws them as one batch of quads per emitter through the billboard shader. Particles are not entities.
- NOTE(limitation): no shadow casting, no mipmaps (nearest sampling).

### 4.6.1 UI

Implemented in `src/ui/` (public API in `ui/ui.h`). Docs: `docs/scenes.md#ui`.
- **Retained, entity-based.** A UI element is an entity with a `WPE_UiRect` (`WPE_COMP_UI`); `WPE_Sprite`, `WPE_Text` and `WPE_Button` add what it shows and does. Elements are laid out against their parent element's rectangle with anchors (fractions) plus offsets (pixels), or against the framebuffer when the parent is not a UI element. The default fills the parent.
- **2D is "no camera", not a separate scene type.** The engine draws the UI in the overlay pass (after post-processing, after debug lines, before the scripts' immediate 2D), so a scene without a camera is a 2D screen and a scene with one gets a HUD.
- **Order and picking** follow the hierarchy: parents before children, top-level elements by `wpe_ecs_spawn_serial`. `wpe_ui_layout` stores each element's rectangle and `shown` flag in the component; picking and navigation use that layout.
- **One focus.** The mouse, the arrow keys / D-pad / left stick (spatial navigation with repeat) and Return / Space / south button drive it. Project actions named `ui_up`... `ui_submit` replace an input's defaults. Clicks are collected per `wpe_ui_update` and the engine passes them to scripts (`wpe_script_host_notify(..., "on_click")`) before the fixed steps. The UI reacts only while simulating.
- **Asset and string lifetime:** sprites and texts hold their image/font reference from their add hook to their remove hook; a text's add hook copies the string (the component owns the copy, `wpe_ui_set_text` replaces it).
- NOTE(limitation): no rotation/scale for elements and no clipping of children; text is the bitmap font (no proportional TTF yet).

### 4.7 Engine runtime, scenes and export

Implemented in `src/engine/`. Docs: `docs/scenes.md` and `docs/runtime.md`.
- `WPE_Engine` is the integration layer, used by the runtime and later the editor. It owns every subsystem, reads `project.json`, loads scenes and runs the frame:
  1. Input, then the UI's focus and clicks (scripts' `on_click`).
  2. Fixed steps (scripts, physics, collision callbacks).
  3. Scripts' update, animation, transforms and audio.
  4. Scene rendering, then the scene's post-processing, then debug lines, the UI and the scripts' 2D drawing on top.
  5. Present.
  6. Scene switches and quits requested by scripts are applied.
- Scenes are JSON documents:
  - Entities are referenced by file-local ids; assets by path; angles are in degrees.
  - Loading is strict (unknown keys are errors with line numbers) and atomic (a failed load removes everything it created).
  - Saving writes roots in creation order (`wpe_ecs_spawn_serial`), so load-save cycles reproduce the text.
  - Game components plug in through `WPE_ComponentSerializer`.
  - The `"instance"` shorthand places a whole model; saving writes it out expanded. TODO(agent): keep prefab instances as references once the editor needs it.
- Script components save their path and properties: fields set through `wpe_script_set_property()`, saved with their current values.
- Export appends a `.pak` of the project to `wpe_runtime`, producing one self-contained executable. The runtime links its compiler runtimes statically and, on Windows, is a GUI program that attaches to a parent console.

### 4.8 Editor

Implemented in `editor/` (docs: `docs/editor.md`).
- **One command path.** The editor model (`editor.c`) holds the project in an embedded `WPE_Engine` and exposes JSON commands (`editor_commands.c`) in the scene-file schema, with live entity ids. The UI (`editor_ui.c`) makes every change through those commands, so agents and people share validation, undo and behavior.
  - Agents reach the commands headless (`--run`, `--command`) or live over a local TCP channel (`--send`).
  - Replies include the warnings and errors logged while a command ran.
- **Undo** keeps whole-scene snapshots, so any command is undoable with one mechanism. Failed commands leave no step, and a `coalesce` key merges continuous edits.
- **Play mode** snapshots the scene and restores it on stop. Edit mode renders from an editor camera that is never saved (the scene context's include filter).
- **Edit mode keeps physics current without simulating** (`wpe_physics_sync` each frame): moved, rescaled and new colliders are right in the debug drawing, in raycasts, and when play starts from the edited world.
- **2D view** (`view_2d`): edit mode renders from the game's camera with the UI (`wpe_engine_set_draw_ui`; `wpe_engine_set_draw_scene(false)` for a scene without a camera), and the viewport edits UI rectangles (pick with `wpe_ui_pick(..., true)`, drag offsets through `set_component "ui"`). The 3D view uses the editor camera and hides the UI. The editor applies this every edit-mode frame (`wpe__editor_apply_view`); play mode always shows the game.
- **View settings** (`WPE_EditorView`: grid, physics shapes, light icons, gizmo mode and space, script hot reload, snapping, 2D view) belong to the editor, not the scene. Commands (`get_view`/`set_view`) reach them like everything else, but they are never saved or undone.
- **The UI is Dear ImGui (cimgui) with two renderers** in `ui_backend.c`:
  - **The window draws it on the GPU** through its SDL renderer (`wpe_window_renderer`), in C after Dear ImGui's `imgui_impl_sdlrenderer3`. Rasterizing the UI in software took about 13 ms per frame at 1600x900; only the game view stays software.
  - **Headless runs draw it with the software rasterizer** (scissor rectangles), so agents can capture the whole editor without a window.
  - **The live `screenshot` command reads back the frame the window showed**, so it captures exactly what the user sees.
  - **`editor_ui_backend` tests that both paths draw the same frame**, running the GPU path on SDL's software `SDL_Renderer` with no GPU needed.
  - **Keep GPU vertices interleaved**: SDL's software renderer mis-strides separate UV arrays.
- **No hidden state.** Every editor feature must be reachable through a command; when adding UI, add or reuse a command first.
- **Prefabs are linked, and overrides are computed.** Instances keep a baseline (their prefab as instantiated). Saving writes only the differences from it, so no editing path needs to record what it changed. Applying an instance rewrites the prefab file, and the editor then rebuilds the other instances by reloading the scene. Instances keep their prefab's structure; restructuring means unpacking.
- **Unsaved work is lost only on purpose.** Commands that replace the scene (`new_scene`, `load_scene`, `quit`) refuse while it has unsaved changes, unless they say `"discard": true`. The UI asks first, Save / Don't save / Cancel, and the window's close button goes through the same prompt. A new command that drops the scene must follow the same rule.

---
## 5. File Organization

```
willpower_engine/
├── src/
│   ├── core/           # Base utilities, logging, alloc
│   ├── math/           # Vectors/quaternions/matrices (cglm wrapper)
│   ├── ecs/            # Entity Component System
│   ├── scene/          # Transform, hierarchy, name components and systems
│   ├── jobs/           # Worker thread pool (parallel_for)
│   ├── renderer/       # Software rasterizer (no SDL dependency)
│   ├── platform/       # SDL3 wrapper: window, present, time, file and PNG I/O
│   ├── assets/         # glTF models (cgltf), image decoding (stb_image), bitmap fonts
│   ├── graphics/       # Mesh/camera/animator components, material shader, scene render, 2D
│   ├── physics/        # Box3D wrapper
│   ├── audio/          # miniaudio wrapper: sounds, sources, music, buses
│   ├── vfs/            # Virtual file system and .pak archives (zstd)
│   ├── script/         # Luau VM and bindings
│   ├── input/          # SDL3 keyboard, mouse and gamepad input with actions
│   ├── ui/             # Retained UI: anchored rectangles, sprites, text, buttons, focus navigation
│   └── engine/         # Game runtime: project, scene and prefab files, frame loop, export
├── editor/             # wpe_editor: model and commands (editor.c, editor_commands.c), UI (editor_ui.c,
│   │                   # ui_backend.c: ImGui on the GPU or the software rasterizer), main.c (window, server, CLI)
│   └── main.c
├── runtime/            # Standalone game runtime (wpe_runtime; exported games are this executable)
│   └── main.c
├── samples/            # Small demo programs (e.g. spinning_cubes) with --frames/--screenshot for automated checks
├── tests/              # Test suites
├── tools/              # Command-line tools (wpe_pak: .pak archives; wpe_export: game export)
├── docs/               # Guides and specifications (scripting, assets and .pak, scenes, runtime and export)
├── assets/             # The sample game project: project.json, scenes/ (2D title menu, demo and master test
│                       # scene), scripts/, meshes/ (glTF and .prim), sounds/, music/, fonts/, ui/ (images)
├── CMakeLists.txt
├── AGENTS.md           # This file
└── README.md
```

**Module boundaries:**
- Each `src/*` module is a static library dependency.
- No circular dependencies allowed.
- Module interfaces declared in `<module>/<module>.h` files. Exception: `math/wpe_math.h`, named so it can't be confused with `<math.h>`.
- Current dependencies:
  - `math`, `jobs` and `ecs` each depend only on `core`; `jobs` also uses SDL3 privately.
  - `scene` depends on `core`, `math` and `ecs`.
  - `renderer` depends on `core`, `math` and `jobs`.
  - `platform` depends on `core` and `renderer`, and uses SDL3 privately.
  - `vfs` depends on `core` and `platform`, and uses zstd privately.
  - `assets` depends on `core`, `math`, `renderer`, `jobs`, `platform` and `vfs`, and uses cgltf and stb_image privately.
  - `graphics` depends on `ecs`, `scene`, `renderer` and `assets`.
  - `physics` depends on `ecs`, `scene` and `assets`, and uses Box3D privately.
  - `audio` depends on `ecs`, `scene` and `assets`, and uses miniaudio privately.
  - `input` depends on `core` and `platform`, and uses SDL3 privately.
  - `script` depends on `graphics`, `assets`, `physics`, `audio`, `input` and `platform`, and uses Luau privately.
  - `engine` depends on all of the above.
  - The editor (`editor/`, an application, not an engine module) depends on `engine`, cimgui and SDL3.
- Only `jobs`, `platform`, `input` and the editor may include SDL headers; the renderer stays pure CPU so it can be tested headless.

---

## 6. Development Workflow

### 6.1 Before Starting Work

1. Review relevant sections of `WillPowerEngine Spec (Refined).md`.
2. Check existing tests for behavior expectations.
3. Identify affected modules and their interfaces.

### 6.2 During Implementation

1. **Write tests first** (TDD preferred for new features).
2. Implement minimum viable solution.
3. Refactor for clarity and performance.
4. Update documentation/comments.

### 6.3 Before Submitting Changes

- [ ] All tests pass locally
- [ ] No compiler warnings (treating `-Wall -Wextra -Werror` as mandatory)
- [ ] Code formatted consistently
- [ ] Public API documented
- [ ] Test scene updated if new feature added
- [ ] No debug logging left behind

### 6.4 Change Communication

For non-trivial changes:
- Add a comment block at the top of modified files explaining the change.
- Update this document if architectural decisions change.
- Note any backward-incompatible changes in a `CHANGELOG.md`.

---

## 7. Dependency Guidelines

### 7.1 Approved Dependencies

| Library | Purpose | Version Constraint |
|---------|---------|-------------------|
| SDL3 | Windowing, Input | Latest stable |
| cglm | Math | Pinned release (the spec's "cgml" refers to this library) |
| miniaudio | Audio | Latest stable |
| Luau | Scripting | Latest stable |
| cimgui | Editor UI | Matching ImGui version |
| Box3D | Physics | Latest stable |
| cgltf | glTF 2.0 / GLB parsing (approved for the glTF milestone) | Pinned release (v1.15) |
| stb_image | JPEG/PNG decoding (approved for the glTF milestone) | Pinned commit |
| zstd or LZ4 | Compression | Either, documented choice |

### 7.2 Adding New Dependencies

1. Justify necessity in `README.md` or module docs.
2. Ensure BSD/MIT/Apache license compatibility.
3. Pin version (avoid HEAD/main branches).
4. Add wrapper layer for abstraction (don't expose raw dependency APIs publicly).

---

## 8. Anti-Patterns to Avoid

### 8.1 Code Smells

| Problem | Fix |
|---------|-----|
| Global mutable state | Singleton pattern with explicit initialization |
| Deep nesting (>3 levels) | Early returns, extract functions |
| Large switch statements | Use function pointers or ECS component dispatch |
| Magic numbers | Named constants with documentation |
| Repeated code (>3 occurrences) | Extract common function |

### 8.2 Architectural Mistakes

- ❌ Direct access to another module's internal data.
- ✅ Use public API or friend declarations where justified.

- ❌ Blocking the render thread on I/O.
- ✅ Async loading with completion callbacks.

- ❌ Dynamic casts or RTTI (not available in C23).
- ✅ Component type flags and explicit checks.

- ❌ String-based entity/component lookups at runtime.
- ✅ Precomputed hashes or integer IDs.

---

## 9. Performance Expectations

### 9.1 Target Metrics

| Metric | Target | Notes |
|--------|--------|-------|
| Frame time | < 16ms @ 640×480 | 60 FPS minimum |
| Entity count | 10,000+ active | With simple components |
| Draw calls | Batched automatically | Manual override if needed |
| Memory footprint | < 128MB baseline | Including editor |

### 9.2 Profiling Guidelines

- Profile before optimizing.
- Use SDL profiler or platform-native tools.
- Optimize hot paths identified by profiling data.
- Measure impact of optimization (don't guess).

### 9.3 Parallelism Budget

- Main thread: Input, scene graph, final composite
- Worker pool (N-1 cores): Culling, rasterization, audio processing
- Async loader thread: Asset decompression and parsing

---

## 10. Documentation Standards

### 10.1 Header Comments

Every public function requires:
```c
/**
 * Brief description of function purpose.
 *
 * Detailed description if needed.
 *
 * @param param1 Description of parameter
 * @return Status code (WPE_RESULT_OK on success)
 *
 * @thread_safety Main thread only
 */
WPE_Result wpe_function_name(ParameterType param1);
```

### 10.2 Module Documentation

Each `*.h` file starts with:
```c
/**
 * @file module_name.h
 * @brief Brief module description
 * 
 * Detailed module overview, including:
 * - Primary responsibilities
 * - Dependencies
 * - Thread affinity
 * - Usage examples
 * 
 * @author WillPowerEngine Team
 */
```

### 10.3 Inline Comments

- Explain **why**, not what (the code shows what).
- Flag TODO items with `// TODO(agent): reason` format.
- Mark known limitations with `// NOTE(limitation): explanation`.
- Private/internal helper functions use `wpe__` prefix in comments.

---

## 11. Agent Skills Matrix

Agents should develop proficiency in these areas:

| Skill Area | Priority | Relevant Modules |
|------------|----------|------------------|
| C23 syntax and idioms | Critical | All |
| ECS architecture | Critical | ecs/ |
| Software rasterization | Critical | renderer/ |
| Linear algebra | Critical | All graphics |
| Multithreading patterns | High | renderer/, core/ |
| Luau scripting | High | script/ |
| Box3D physics | Medium | physics/ |
| cimgui UI | Medium | editor/ui/ |
| glTF parsing | Medium | assets/ |
| Build systems (CMake) | High | Root CMakeLists.txt |

---

## 12. Emergency Procedures

### 12.1 Breaking Builds

If you introduce a breaking change:
1. Immediately fix or revert.
2. Notify through the development channel.
3. Document the issue and resolution.

### 12.2 Failing Tests

Never commit with failing tests unless:
- You're actively debugging and documenting the failure.
- The test itself is broken (fix test alongside feature).

### 12.3 Unclear Requirements

When requirements are ambiguous:
1. Check `WillPowerEngine Spec (Refined).md` for original specification.
2. Make the most conservative assumption.
3. Document the assumption in code comments.
4. Raise a question if the ambiguity blocks progress.

---

## Appendix A: Quick Reference Commands

```bash
# Configure (Debug enables wpe_assert via WPE_DEBUG)
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Debug

# Build
cmake --build build

# Run tests
cmake --build build --target test_all

# Build with sanitizers (GCC/Clang on Linux)
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Debug -DSANITIZERS=ON

# Format code / run linter (require clang-format / clang-tidy on PATH)
cmake --build build --target format
cmake --build build --target lint
```

---

## Appendix B: Contact Points

- General questions: Check `WillPowerEngine Spec (Refined).md` first
- Engine architecture: Review section 4 of this document
- Bug reports: Include test case reproduction
- Feature requests: Document in CHANGELOG.md draft

---

**Version:** 1.2  
**Last Updated:** 27 Oct 2026  
**Maintainer:** WillPowerEngine Development Team  
**Changes from v1.1:** Input, engine runtime, scene files and export (§4.6, §4.7); the editor (§4.8); scenes live in `assets/scenes`; test layout.
