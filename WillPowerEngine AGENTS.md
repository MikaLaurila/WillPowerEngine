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
├── integration/    # Multi-component tests
├── scene/          # Full scene playback tests
└── stress/         # Long-running, memory leak detection
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
- Use the master test scene from `scenes/test_full.scene`.
- Run through scripted sequences that exercise all systems.
- Validate state at checkpoints with assertions.

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

The master test scene (`scenes/test_full.scene`) must:
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
- Post-processing (Milestone 5) will run full-screen in parallel.

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

**Script host responsibilities:**
- Expose ECS API to Luau (entity manipulation, component access).
- Hot reload: detect script changes, restart scripts preserving state.
- Sandbox: prevent arbitrary file/system access.
- Error recovery: script crashes don't crash the engine.

### 4.4 Asset Management

```c
// Asset handle system
typedef uint32_t WPE_AssetHandle;

#define WPE_HANDLE_INVALID ((WPE_AssetHandle)~0u)

WPE_AssetHandle wpe_asset_load(const char* path);
WPE_Mesh* wpe_asset_get_mesh(WPE_AssetHandle handle);
void wpe_asset_unload(WPE_AssetHandle handle);

// Internal asset cache (private)
typedef struct {
    WPE_AssetHandle* handles;
    char** paths;
    uint32_t count;
} wpe__AssetCache;

static wpe__AssetCache wpe__g_asset_cache;
```

**Rules:**
- Assets loaded from `.pak` archives (zstd/LZ4 compressed).
- Reference counting for shared assets.
- Lazy loading for large scenes.
- Texture streaming not required (target resolutions low).

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
│   ├── platform/       # SDL3 wrapper: window, present, time, PNG I/O
│   ├── physics/        # Box3D wrapper
│   ├── audio/          # miniaudio wrapper
│   ├── script/         # Luau VM and bindings
│   ├── assets/         # Asset loading and management
│   └── input/          # SDL3 input handling
├── editor/             # Standalone editor application
│   ├── ui/             # cimgui-based UI
│   ├── tools/          # Editor-specific tools
│   └── main.c
├── runtime/            # Standalone game runtime
│   └── main.c
├── samples/            # Small demo programs (e.g. spinning_cubes) with --frames/--screenshot for automated checks
├── tests/              # Test suites
├── scenes/             # Test and demo scenes
├── assets/             # Test assets (glTF, fonts, etc.)
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
- Only `jobs` and `platform` may include SDL headers; the renderer stays pure CPU so it can be tested headless.

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

**Version:** 1.1  
**Last Updated:** 26 Oct 2026  
**Maintainer:** WillPowerEngine Development Team  
**Changes from v1.0:** Updated private symbol naming convention to `wpe__` prefix
