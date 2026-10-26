# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project

WillPowerEngine is a C23 multithreaded software-rasterizer 3D game engine (Windows + Linux). Read these before working on a module:

- `WillPowerEngine Spec (Refined).md`: the product spec, which is the source of truth for *what* to build.
- `WillPowerEngine AGENTS.md`: the authoritative coding standards, testing rules, architecture and planned layout. Update it when an architectural decision changes.

Development runs in milestones; the user reviews each one before the next starts. Commit and push to `origin main` (github.com/MikaLaurila/WillPowerEngine) at each working milestone, and record notable changes in `CHANGELOG.md`.

1. Build system, core and tests (done)
2. ECS, plus math and the scene transform hierarchy (done)
3. Rasterizer and window (SDL3) (done)
4. glTF, materials, textures, fonts and skinned animation (done)
5. Lighting, shadows and post-processing
6. Luau scripting
7. Box3D physics
8. Audio and `.pak` archives
9. Runtime/export
10. Editor

Do not look outside the working directory. Third-party libraries come in through CMake FetchContent, pinned to release tags or commits. Only use the approved list in AGENTS.md §7 (SDL3, cglm, cgltf, stb_image, miniaudio, Luau, cimgui, Box3D, zstd); anything else needs the user's approval. Add each one in the milestone that needs it, wrapped so its API is not exposed in public headers, and compiled *without* `wpe_configure_target` so its warnings don't break the build.

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
```

The test models live in `assets/meshes/`:
- `damaged_helmet.glb`: JPEG textures
- `car_concept.glb`: 213k triangles, many material extensions
- `commercial_refrigerator.glb`: requires `KHR_materials_transmission`
- `RiggedFigure.glb`: skinned and animated

The font is `assets/fonts/small.png`.

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
  - Shaders are C functions called concurrently from worker threads; varyings are capped at 16.
  - Output is deterministic across thread counts, and steady-state frames don't allocate; tests enforce both.
  - The rasterizer files are always compiled with `-O2`, even in Debug.
- **platform** (`platform/platform.h`): the SDL3 window, presenting (nearest-neighbour, letterboxed), time, PNG load/save, and UTF-8-safe `wpe_file_read_all`. Only `jobs` and `platform` may include SDL.
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
  - Components: `WPE_MeshRenderer`, `WPE_Camera` (looks down local −Z) and `WPE_Animator`. Their remove hooks free the joint and node arrays, and they hold borrowed `WPE_Model*` pointers, so the model must outlive its entities.
  - `wpe_model_instantiate` builds the entity tree.
  - Frame order: `wpe_animation_update` → `wpe_scene_update_transforms` → `wpe_scene_render` (inside a renderer frame) → `wpe_draw2d_*`.
  - Skinning happens in the material vertex shader using a world-space joint palette. Skinned meshes skip the node transform and frustum culling.
  - Lighting is a per-vertex placeholder (one directional light plus ambient, with ambient also reflected specularly) until Milestone 5. Normal maps are loaded but not used yet.
- **Tests:** `tests/framework/wpe_test.h` is a custom runner. Every case automatically fails if it changes the live `wpe_alloc` count (a leak) or triggers an engine `wpe_assert`; use `WPE_TEST_EXPECT_ASSERT` when an assert is expected. Register test executables with `wpe_add_test(...)` in `tests/<category>/CMakeLists.txt`. Categories are `unit/`, `stress/` (label `stress`) and `golden/` (reference PNGs in `tests/golden/references/`, compared with a small tolerance).
- **Planned (see AGENTS.md §4–5 and §9):**
  - Fixed-timestep physics and Luau `on_fixed_update`.
  - Separate `editor/` and `runtime/` executables. The editor must be fully drivable by AI agents.

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
