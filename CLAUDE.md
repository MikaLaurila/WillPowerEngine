# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project

WillPowerEngine is a C23 multithreaded software-rasterizer 3D game engine (Windows + Linux). Read these before working on a module:

- `WillPowerEngine Spec (Refined).md`: the product spec, which is the source of truth for *what* to build.
- `WillPowerEngine AGENTS.md`: the authoritative coding standards, testing rules, architecture and planned layout. Update it when an architectural decision changes.

Development runs in milestones; the user reviews each one before the next starts. Commit and push to `origin main` (github.com/MikaLaurila/WillPowerEngine) at each working milestone, and record notable changes in `CHANGELOG.md`. End every commit message with the trailer `Co-Authored-By: Claude <noreply@anthropic.com>` (the user asked for it starting with Milestone 6).

1. Build system, core and tests (done)
2. ECS, plus math and the scene transform hierarchy (done)
3. Rasterizer and window (SDL3) (done)
4. glTF, materials, textures, fonts and skinned animation (done)
5. Lighting, shadows, fog, subdivision and post-processing (done)
6. Luau scripting (done)
7. Box3D physics (done)
8. Audio, `.pak` archives, VFS and the asset manager (done)
9. Runtime/export, scene files and input (done)
10. Editor (done)

Do not look outside the working directory. Third-party libraries come in through CMake FetchContent, pinned to release tags or commits. Only use the approved list in AGENTS.md §7 (SDL3, cglm, cgltf, stb_image, miniaudio, Luau, cimgui (with Dear ImGui), Box3D, zstd); anything else needs the user's approval. Add each one in the milestone that needs it, wrapped so its API is not exposed in public headers, and compiled *without* `wpe_configure_target` so its warnings don't break the build.

## Commands

The local toolchain is MSYS2 UCRT64 GCC 16 + Ninja + CMake 4, plus clang-format and clang-tidy (MSYS2 clang-tools-extra). There's no MSVC.

```bash
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Debug   # Release: use a separate build-release/ dir
cmake --build build
cmake --build build --target test_all                   # build all tests and run ctest
ctest --test-dir build -R "^core_" --output-on-failure  # one module (tests are named <module>_<area>)
./build/tests/unit/test_core_log --filter <substring>   # individual cases; --list lists them
ctest --test-dir build -L stress                        # stress suite (randomized model check, 10k perf budget)
cmake --build build --target format                     # clang-format; run before committing
cmake --build build --target lint                       # clang-tidy over src/ (warnings are errors)
WPE_UPDATE_GOLDEN=1 ctest --test-dir build -R golden    # regenerate golden PNGs; open and review them before committing
./build/samples/spinning_cubes --frames 30 --screenshot out.png   # render headless-ish and inspect the PNG
./build/samples/model_viewer assets/meshes/RiggedFigure.glb --frames 2 --yaw 30 --time 0.6 --screenshot out.png
./build/samples/lighting_demo --frames 2 --time 3 --screenshot out.png --shadow-map shadow.png   # + --ssao --fog --ps1 ...
./build/samples/script_demo --watch                               # edit assets/scripts/*.luau while it runs
./build/samples/script_demo --frames 150 --debug-physics --screenshot out.png   # collider wireframes (headless: no audio device)
./build/bin/wpe_pak create out.pak assets && ./build/samples/script_demo --pak out.pak     # run from an archive (also list/verify/extract)
./build/bin/wpe_runtime --project assets                          # play the demo game (assets/ is a project; --watch hot-reloads)
./build/bin/wpe_runtime --project assets --scene scenes/test_full.scene --headless --frames 300 --fixed-dt 0.0333 --screenshot out.png
./build/bin/wpe_export assets out/demo.exe                        # standalone game: the runtime with the project appended as a .pak
ctest --test-dir build -L integration                             # the master test scene for 60 simulated seconds (every scripting API)
./build/bin/wpe_editor assets                                     # the editor (window + command server on 127.0.0.1:47800)
./build/bin/wpe_editor assets --command '{"cmd": "help"}'         # editor commands headless (--run file.jsonl for batches)
./build/bin/wpe_editor assets --command '{"cmd": "screenshot", "target": "editor", "path": "out.png"}'   # see the whole editor
./build/bin/wpe_editor --send '{"cmd": "status"}'                 # a command to a running editor
```

miniaudio is compiled from its `miniaudio.c` in `src/audio` (`-O2 -w`) with `MA_NO_RESOURCE_MANAGER MA_NO_ENCODING MA_NO_GENERATION` as *interface* definitions on `wpe_dep_miniaudio`: they change struct layouts, so every includer must see them.

Luau is the only C++ dependency. It's compiled without the engine's warning flags (with `-Wno-attributes` on MinGW), and its API gets C linkage through *private* compile definitions on `Luau.VM` and `Luau.Compiler`.

The test models live in `assets/meshes/`:
- `damaged_helmet.glb`: JPEG textures
- `car_concept.glb`: 213k triangles, many material extensions
- `commercial_refrigerator.glb`: requires `KHR_materials_transmission`
- `RiggedFigure.glb`: skinned and animated

The font is `assets/fonts/small.png`; it is also compiled into the engine (`wpe_font_create_builtin`, a CMake-generated byte array) as the font of projects that name none or whose font file is missing.

The first configure downloads and builds SDL3 (about 70 s). To check rendering output yourself, use `--screenshot` or golden test images and open the PNG; when a golden test fails, its output is saved as `build/tests/golden/<name>_actual.png`.

When mutation-testing (deliberately breaking code to prove a test catches it), make sure the mutated code still compiles: with `-Werror`, a failed build silently leaves the old test binary in place.

Run the Release build in a separate tree (`build-release/`). Some checks only happen there; for example, the 10k-transform perf budget is asserted only when `WPE_DEBUG` is off.

CI (`.github/workflows/ci.yml`) builds Debug and Release on Linux GCC 14 (with ASan/UBSan in Debug) and on Windows MSYS2. Keep code portable to both platforms.

## Architecture

- Each `src/<module>/` is a static library `wpe_<module>` (alias `wpe::<module>`) with an umbrella header `<module>/<module>.h`. The include root is `src/`, so includes look like `"core/log.h"`; never add a module directory itself to the include path. Every first-party target must call `wpe_configure_target()` (from `cmake/WpeCompilerOptions.cmake`), which applies `-Wall -Wextra -Werror -Wpedantic` and related flags, plus `WPE_DEBUG` in Debug/RelWithDebInfo builds.
- **core** (`src/core/`) is the dependency root and uses only the C standard library:
  - `WPE_Result` codes.
  - Logging: each `.c` file does `#define WPE_LOG_MODULE "name"` before including `core/log.h`, then calls `wpe_log_error(...)` etc. Logging can add sinks (`wpe_log_add_sink`) and is thread-safe.
  - `wpe_assert`: debug-only; a failure means a bug.
  - `wpe_check`: always evaluated; returns a bool for runtime validation.
  - `wpe_alloc`/`wpe_free`: tracked allocation that counts live allocations.
- **math** (`math/wpe_math.h`): `WPE_Vec3`/`WPE_Quat`/`WPE_Mat4` unions. Matrices are column-major (`m[col][row]`), and `wpe_mat4_mul(a, b)` applies `b` first. Euler angles apply X, then Y, then Z. cglm is used only inside `wpe_math.c`, and anything handed to cglm must be non-const.
- **ecs** (`ecs/ecs.h`, depends only on core): sparse-set pools with generational entities (20-bit index, 12-bit generation).
  - Component IDs are a plain `uint32_t`; built-ins are `WPE_COMP_*` with stable values, and games use IDs from `WPE_COMP_USER_FIRST`.
  - Every component type must be registered before use.
  - Never remove components or despawn during a query; use the deferred despawn functions. Adding components and spawning are allowed.
  - Re-fetch component pointers after any structural change.
  - `on_remove` hooks are where resource cleanup goes; hooks must not make structural changes.
- **scene** (`scene/scene.h`): Transform (local TRS plus a cached `world`), Hierarchy (parent/child links) and Name.
  - Only change parenting through `wpe_scene_set_parent`.
  - `wpe_scene_update_transforms` recomputes all world matrices; it uses an explicit stack, not recursion.
  - Despawning a parent turns its children into roots (a hook does this); `wpe_scene_destroy_recursive` deletes a whole subtree.
- **jobs** (`jobs/jobs.h`): `wpe_jobs_parallel_for(jobs, count, grain, fn, ctx)` blocks until done, and the calling thread takes part as thread index 0. `jobs == NULL` runs inline. Keep outputs per-chunk (fixed `grain`) so results don't depend on the thread count. Don't call it from inside a callback.
- **renderer** (`renderer/renderer.h`): pure CPU with no SDL; see AGENTS.md §4.2 for the pipeline.
  - A frame is `begin_frame(clear)` → `submit(draw)…` → `end_frame()`.
  - Uniforms are copied at submit; vertex/index buffers and the program are borrowed until `end_frame`.
  - Clip space follows the OpenGL convention (z in [−w, w]); front faces are counter-clockwise; the framebuffer origin is top-left; colors are `0xAARRGGBB`.
  - Shaders are C functions called concurrently from worker threads; varyings are capped at 32 (`WPE_MAX_VARYINGS`).
  - Output is deterministic across thread counts, and steady-state frames don't allocate; tests enforce both.
  - The rasterizer files are always compiled with `-O2`, even in Debug.
- **platform** (`platform/platform.h`): the SDL3 window, presenting (nearest-neighbour, letterboxed), event pumping, time, PNG load/save, file streams and UTF-8-safe `wpe_file_read_all`. Only `jobs`, `platform`, `input` and the editor may include SDL.
- **renderer textures** (`renderer/texture.h`):
  - Textures are mipmapped, with sRGB-correct mip generation, and have an optional `max_dimension` downscale.
  - For LOD selection, a program sets `uv_derivatives`/`uv_varying`, and the rasterizer then provides exact per-pixel `uv_dx`/`uv_dy`.
  - Shading happens in gamma (sRGB) space.
- **assets** (`assets/`): `wpe_model_load` (cgltf) produces an immutable `WPE_Model` whose arrays live in an internal arena.
  - Color factors are stored sRGB-encoded.
  - Metallic-roughness inputs are kept and converted per pixel.
  - A texture slot can be NULL if decoding failed; treat it as absent.
  - Unsupported *required* glTF extensions are rejected.
  - `wpe_image_decode` uses stb_image for JPEG and PNG.
  - Fonts: `wpe_font_load` reads magenta-separated glyph sheets and checks for exactly 95 glyphs.
  - cgltf and stb are compiled in `src/assets/third_party/` without engine warnings and allocate through `wpe_alloc`.
- **graphics** (`graphics/`):
  - Components: `WPE_MeshRenderer`, `WPE_Camera` (looks down local −Z) and `WPE_Animator`. Mesh renderers and animators hold a `model_asset` handle: their add hook retains it and resolves the cached `model` pointer, and their remove hook releases it and frees the joint/node arrays. `wpe_graphics_register_components(ecs, assets)` therefore needs the asset manager, which must outlive the world.
  - `wpe_model_instantiate(ecs, assets, handle, ...)` builds the entity tree.
  - Frame order: `wpe_animation_update` → `wpe_scene_update_transforms` → `wpe_scene_render` (inside a renderer frame) → `wpe_draw2d_*`.
  - Skinning happens in the material vertex shader using a world-space joint palette. Skinned meshes skip the node transform and frustum culling.
  - Lighting is hybrid; see AGENTS.md §4.2.1.
    - Lights are `WPE_Light` entities. The sun is the first directional light with `cast_shadows`, otherwise the first directional light. If the world has no lights at all, `settings.default_sun` is used.
    - Material varyings are laid out at the top of `material_shader.c` (26 of 32 are used). Keep that layout and the `static_assert` next to it in sync.
  - Post-processing: `wpe_postfx_apply(chain, renderer, effects, n, camera)` runs after `end_frame`. Draw HUD/2D in a second, non-clearing frame if it shouldn't be post-processed.
  - Procedural models: `assets/procedural.h` provides a plane, a box and `wpe_model_create_mesh`.
- **script** (`script/script.h`, Luau; API reference in `docs/scripting.md`):
  - There is one `WPE_ScriptHost` per world. It registers `WPE_COMP_SCRIPT` and must be destroyed *before* the world. Scripts, prefabs and sounds are asset paths read through `desc.assets`' VFS; the host keeps prefab models and used sounds referenced until it is destroyed.
  - The game loop is `wpe_fixed_step_advance` → for each step: `wpe_script_host_fixed_update` → `wpe_physics_step` → `wpe_script_host_dispatch_physics` → then `wpe_script_host_update` → animation → transforms → `wpe_audio_update` → render. Destroy order: script host, audio, physics, world, asset manager, VFS.
  - Luau is built with `LUA_USE_LONGJMP=1`, so script errors unwind with `longjmp` rather than C++ exceptions. API bindings in `script_api.c` must therefore validate all arguments (which may raise) *before* touching engine state, and must never hold engine allocations across calls that can raise.
  - Host-side Luau operations run inside `lua_cpcall`.
  - Never call `lua_settop` in host code that may run nested inside a binding.
  - The `wpe` table is read-only once sandboxed. To add an API function, add it to `wpe__api[]`, `docs/scripting.md` and `assets/scripts/api_tour.luau`, which runs inside the master test scene (`assets/scenes/test_full.scene`, 60-second `integration_full_scene` test). Functions that end the scene (`wpe.load_scene`, `wpe.quit`) are covered by the engine tests instead.
- **physics** (`physics/physics.h`, Box3D private): one `WPE_Physics` per world registers `WPE_COMP_PHYSICS_RIGIDBODY`/`COLLIDER`; destroy it before the world.
  - Bodies and shapes are created lazily in `wpe_physics_step` (or immediately by `wpe_physics_refresh`). Component settings and world scale are read only at creation, so call refresh after changing them.
  - A body is matched to its entity through `body_entities[b3BodyId.index1]`, not Box3D `userData` (clang-tidy rejects integer-to-pointer casts, and the codebase uses no NOLINT).
  - Teleport detection compares the entity's world pose with the body using a distance-scaled tolerance, so the float error of the parent-relative write-back never registers as a move.
  - Box3D's allocator and assert hooks are process-global and are installed once. Its triangle meshes are referenced, not copied, so each mesh collider owns its `b3MeshData` (hulls are copied by Box3D).
  - The solver is single-threaded (Box3D's task callbacks need asynchronous tasks, which `wpe_jobs` lacks).
  - `wpe_physics_sync` brings bodies in line with an edited world without stepping: it creates pending bodies, places them at their entities' poses and rebuilds shapes whose world scale changed (`WPE_Collider.wpe__scale`). The editor calls it every edit-mode frame. Never call it between steps of a running simulation.
- **vfs** (`vfs/vfs.h`, `vfs/pak.h`; format and tool in `docs/assets.md`): asset paths over mounted directories and `.pak` archives; later mounts win. Reads are thread-safe (each read opens its own stream), mounting is not. Validate or normalize user paths with `wpe_vfs_path_is_valid`/`wpe_vfs_path_normalize`. zstd allocates through `wpe_alloc` via `ZSTD_customMem`.
- **asset manager** (`assets/asset_manager.h`): generational, ref-counted `WPE_AssetHandle`s cached by (type, normalized path). Every component that references an asset holds its own reference from its add hook to its remove hook (colliders take it in `on_add` and swap it only if `model` changes before a rebuild). Code-made assets go in with `wpe_asset_add`. A reference left at shutdown is reported as a leak warning.
- **audio** (`audio/audio.h`, miniaudio): sounds are `WPE_ASSET_SOUND` assets decoded to native-rate float PCM; voices (max 64) read them through `ma_audio_buffer` and resample on playback. Music streams through a custom `ma_vfs` over the engine VFS on the mixing thread. Voices hold sound references; finished voices are recycled (and release them) in `wpe_audio_update`. Without a device, `wpe_audio_render` drives the mixer, which is how tests and headless runs work. The bus count is a `#define` because a `_COUNT` enumerator next to a vector union trips clang-tidy's tagged-union check.
- **input** (`input/input.h`, SDL private): keys are physical (HID codes), mouse coordinates are framebuffer pixels, gamepads take player slots 0..3 (hot-plug, dead zones, rumble), and named actions bind across devices. Per frame: `wpe_input_begin_frame` → `wpe_input_handle_event` (fed with opaque `WPE_PlatformEvent`s by `wpe_window_set_event_handler` or `wpe_platform_poll_events`) → `wpe_input_update`. Tests use real SDL events and SDL virtual joysticks (link `wpe_dep_sdl3`). Luau players are 1-based, C slots 0-based.
- **engine** (`engine/engine.h`; docs in `docs/scenes.md`, `docs/runtime.md`): `WPE_Engine` owns every subsystem and runs the frame. Its script services (`WPE_ScriptApp`: load_scene, quit, camera, screen size) and the 2D drawing it gives scripts are optional in `WPE_ScriptHostDesc`, so plain hosts raise on those functions.
  - Scene and project files are strict JSON (`core/json.h`), read through the private helpers in `engine/json_read.h`. Unknown keys are errors.
  - Scene loads are atomic. Saving is lossless and stable: roots are ordered by `wpe_ecs_spawn_serial`, rotations are kept as written, and degrees are written rounded to 6 digits. The scene round-trip test asserts that a load-save cycle reproduces the text exactly.
  - `assets/` is the sample project (`project.json`, `scenes/`).
  - Export embeds a `.pak` in a copy of `wpe_runtime` (trailer magic `WPE_EMBD`). `wpe_runtime` is a GUI program on Windows that links statically.
  - Executables of `runtime/` and `tools/` build into `build/bin`.
  - **Prefabs** (`engine/prefab.{h,c}`, format in `docs/scenes.md#prefabs`):
    - A `.prefab` file holds entities in the scene schema, exactly one of them the root.
    - Instances are linked: every member has `WPE_COMP_PREFAB` (13; the root's link holds the path and a baseline).
    - The baseline is the members as instantiated, in prefab-local ids. It is reference-counted and shared through the engine's `WPE_PrefabCache`, which re-reads a file when its modification time changes.
    - Saving compares members with the baseline and writes only the differences (`prefab.overrides`), so changes from any source count. Scene saves skip members and write `"prefab": {path, ids, overrides}`.
    - `scene_file_internal.h` shares the loader and saver with `prefab.c`. `core/json` has `wpe_json_write_value`/`wpe_json_equal` for the comparison.
    - Prefab files can't nest instances yet. Worlds built by hand must call `wpe_prefab_register` before loading scenes with instances.
  - `wpe_engine_set_debug_draw` turns on the ground grid and physics wireframes. They're drawn in the overlay pass after post-processing, depth tested, under the scripts' 2D drawing.
- **ui** (`ui/ui.h`, docs in `docs/scenes.md#ui`, AGENTS.md §4.6.1): retained UI entities: `WPE_UiRect` (`WPE_COMP_UI` 14: anchors + pixel offsets against the parent element or the framebuffer; the default fills the parent), `WPE_Sprite` (15, `WPE_ASSET_IMAGE` images, nine-slice), `WPE_Text` (16; the add hook copies the string, `wpe_ui_set_text` replaces it), `WPE_Button` (17).
  - The engine owns one `WPE_Ui`: `wpe_ui_update` (only while simulating) runs focus, navigation and clicks before the fixed steps and the engine calls `on_click` through `wpe_script_host_notify`; `wpe_ui_draw` runs in the overlay frame after debug lines and before scripts' immediate 2D. A scene without a camera is a 2D scene.
  - Draw order: parents before children, top-level elements by spawn serial. `wpe_ui_layout` stores `rect`/`shown` in the component (picking, navigation and the editor read them).
  - The editor's ImGui backend functions are `wpe_ui_backend_*` (the `wpe_ui_` prefix belongs to this module).
  - Tests building SDL events must zero them whole (`SDL_zero`): `= {0}` or designated initializers only set the union's first member, and garbage in `key.repeat` makes key presses vanish at random.
- **ECS hook order:** on despawn, `on_remove` hooks run in descending type-id order: Button (17) to UiRect (14) and PrefabLink (13) first, then AudioSource (11), Collider (10) and RigidBody (9) before Script (8), which runs before Hierarchy (2). So the physics body is already gone when `on_destroy` runs.
- **renderer extras:**
  - `WPE_RenderState.depth_only` writes depth only; the fragment shader may be NULL in that mode. This is used for shadow passes.
  - `WPE_DrawDesc.subdivision_levels/edge_pixels` require `program->vertex_lerp`.
  - `wpe_renderer_framebuffer_for_write` returns NULL while a frame is recording.
- **Tests:** `tests/framework/wpe_test.h` is a custom runner. Every case automatically fails if it changes the live `wpe_alloc` count (a leak) or triggers an engine `wpe_assert`; use `WPE_TEST_EXPECT_ASSERT` when an assert is expected. Register test executables with `wpe_add_test(...)` in `tests/<category>/CMakeLists.txt`. `tests/framework/wpe_test_assets.h` creates a VFS plus asset manager, adds/loads models with assertions and synthesizes WAV files; tests that write scratch files set `WORKING_DIRECTORY` to the build tree. Categories are `unit/`, `scene/` (the master scene, label `integration`), `stress/` (label `stress`), `tools/` (CMake scripts that drive the executables) and `golden/` (reference PNGs in `tests/golden/references/`, compared with a small tolerance).
- **graphics debug lines** (`graphics/debug_lines.h`): world-space segments become screen-width quads, depth-tested with a small bias, drawn with sort key `UINT64_MAX - 1` (after the scene, before draw2d). Feed it from `wpe_physics_debug_draw`.
- **editor** (`editor/`, docs in `docs/editor.md`, AGENTS.md §4.8):
  - The model (`editor.c`) holds an embedded `WPE_Engine` with the simulation off in edit mode, plus a never-saved editor camera.
  - Undo/redo and play/stop use whole-scene snapshots. Failed commands drop their undo step; a `coalesce` key merges drags.
  - Every change, the UI's included, is a JSON command (`editor_commands.c`, scene-file schema with live ids), so a new UI feature needs a command.
  - The 2D view (`view_2d`, `set_view {"mode": "2d"}`) renders edit mode from the game's camera with the UI, and the viewport picks, moves and resizes UI elements; the 3D view uses the editor camera and hides the UI (`wpe__editor_apply_view` sets the engine's camera, `draw_ui` and `draw_scene` each edit-mode frame).
  - View settings (`WPE_EditorView`: grid, physics shapes, light icons, 2D view, gizmo mode and world/local space, hot reload) live in the model behind `get_view`/`set_view`. They are not part of the scene, so they are never saved or undone; the UI reads them and changes them by command.
  - Each edit-mode frame of `wpe_editor_frame` syncs physics and applies the debug drawing. With hot reload on, it also checks scripts for changes.
  - `new_scene`, `load_scene` and `quit` refuse while the scene has unsaved changes, unless they carry `"discard": true` (tests and agent scripts must pass it). The UI routes these actions through `wpe__ui_guard` and its prompt, and the window's close button calls `wpe_editor_ui_request_quit`.
  - The UI is cimgui with two renderers in `ui_backend.c`:
    - The window draws it on the GPU through `wpe_window_renderer` (SDL_Renderer, in C after `imgui_impl_sdlrenderer3`), and the live `screenshot` command reads back the shown frame.
    - Headless runs rasterize it in software (scissor), so `screenshot` with target `"editor"` works without a window.
    - The `editor_ui_backend` test compares the two paths, running the GPU path on SDL's software renderer.
    - Keep GPU vertices interleaved: SDL's software renderer steps through UVs with the color stride.
  - `wpe_editor` is a console program, so PowerShell waits for it and captures its output.
  - The `tools_editor_cli` test drives it like an agent would.
  - cimgui/Dear ImGui come from two pinned archives copied into `build/cimgui/` (`wpe_dep_cimgui`, which needs `imgui_demo.cpp`).

## Conventions that are easy to get wrong

- **Naming:**
  - Types: `WPE_PascalCase`
  - Public functions: `wpe_module_verb()`
  - Macros: `WPE_UPPER_SNAKE`
  - Private or static symbols, including file-local types: `wpe__` prefix (double underscore)
- **Public headers** need:
  - An include guard `WPE_<MODULE>_<FILE>_H`
  - A `@file` doc block listing dependencies and thread affinity
  - A doc comment on every public function, with ownership and `@thread_safety`
- **Errors:** log before returning an error code; never ignore failures. Mark functions whose result must be used with `[[nodiscard]]`.
- **printf-style functions:** use `WPE_PRINTF_LIKE` (it selects `gnu_printf` on MinGW so `%zu` and similar are accepted).
- **`setjmp`:** may only appear as a controlling expression (see `wpe_test.c`).
- **No undefined behaviour, even in tests:** for example, don't test double-free detection by reading freed memory.
- **Comments:** `// TODO(agent): reason` and `// NOTE(limitation): explanation`.
