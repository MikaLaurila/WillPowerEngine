# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project

WillPowerEngine is a C23 multithreaded software-rasterizer 3D game engine (Windows + Linux). Read these before working on a module:

- `WillPowerEngine Spec (Refined).md`: the product spec, which is the source of truth for *what* to build.
- `WillPowerEngine AGENTS.md`: the authoritative coding standards, testing rules, architecture and planned layout. Update it when an architectural decision changes.

Development runs in milestones; the user reviews each one before the next starts. Commit and push to `origin main` (github.com/MikaLaurila/WillPowerEngine) at each working milestone, and record notable changes in `CHANGELOG.md`.

1. Build system, core and tests (done)
2. ECS
3. Rasterizer and window (SDL3)
4. glTF and materials
5. Lighting, shadows and post-processing
6. Luau scripting
7. Box3D physics
8. Audio and `.pak` archives
9. Runtime/export
10. Editor

Do not look outside the working directory. Third-party libraries come in through CMake FetchContent, pinned to release tags or commits. Only use the approved list in AGENTS.md §7 (SDL3, cglm, miniaudio, Luau, cimgui, Box3D, zstd). Add each one in the milestone that needs it, wrapped so its API is not exposed in public headers, and compiled *without* `wpe_configure_target` so its warnings don't break the build.

## Commands

The local toolchain is MSYS2 UCRT64 GCC 16 + Ninja + CMake 4 (no MSVC or Clang installed; no clang-format or clang-tidy, so the `format` and `lint` targets fail with a message).

```bash
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Debug   # Release: use a separate build-release/ dir
cmake --build build
cmake --build build --target test_all                   # build all tests and run ctest
ctest --test-dir build -R "^core_" --output-on-failure  # one module (tests are named <module>_<area>)
./build/tests/unit/test_core_log --filter <substring>   # individual cases; --list lists them
```

CI (`.github/workflows/ci.yml`) builds Debug and Release on Linux GCC 14 (with ASan/UBSan in Debug) and on Windows MSYS2. Keep code portable to both platforms.

## Architecture

- Each `src/<module>/` is a static library `wpe_<module>` (alias `wpe::<module>`) with an umbrella header `<module>/<module>.h`. The include root is `src/`, so includes look like `"core/log.h"`; never add a module directory itself to the include path. Every first-party target must call `wpe_configure_target()` (from `cmake/WpeCompilerOptions.cmake`), which applies `-Wall -Wextra -Werror -Wpedantic` and related flags, plus `WPE_DEBUG` in Debug/RelWithDebInfo builds.
- **core** (`src/core/`) is the dependency root and uses only the C standard library:
  - `WPE_Result` codes.
  - Logging: each `.c` file does `#define WPE_LOG_MODULE "name"` before including `core/log.h`, then calls `wpe_log_error(...)` etc. Logging can add sinks (`wpe_log_add_sink`) and is thread-safe.
  - `wpe_assert`: debug-only; a failure means a bug.
  - `wpe_check`: always evaluated; returns a bool for runtime validation.
  - `wpe_alloc`/`wpe_free`: tracked allocation that counts live allocations.
- **Tests:** `tests/framework/wpe_test.h` is a custom runner. Every case automatically fails if it changes the live `wpe_alloc` count (a leak) or triggers an engine `wpe_assert`; use `WPE_TEST_EXPECT_ASSERT` when an assert is expected. Register test executables with `wpe_add_test(...)` in `tests/<category>/CMakeLists.txt`.
- **Planned (see AGENTS.md §4–5 and §9):**
  - ECS: POD components, sparse set, generational 32-bit IDs.
  - Renderer pipeline: cull → sort → tile-parallel raster → post-process → SDL texture. The main thread owns window, input and scene; worker threads render.
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
