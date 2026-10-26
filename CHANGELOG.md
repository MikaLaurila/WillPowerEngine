# Changelog

All notable changes to WillPowerEngine are recorded here. Backward-incompatible changes are marked **BREAKING**.

## [Unreleased]

### Added
- Assets (`src/assets`): glTF 2.0 / GLB loader (meshes, materials, textures, nodes, skins,
  animations; strips/fans, flat normals, data URIs, KHR_texture_transform, emissive_strength,
  specular-glossiness, unlit, transmission approximation, mesh quantization; parallel texture
  decoding with optional downscaling), JPEG/PNG decoding, bitmap font sheets.
- Renderer: mipmapped textures (sRGB-correct mip generation, nearest/bilinear/trilinear, wrap
  modes) and exact per-pixel UV derivatives from the rasterizer for LOD selection.
- Graphics (`src/graphics`): MeshRenderer/Camera/Animator components, model instantiation into
  entities, keyframe animation (linear/step/cubic spline), CPU skinning in the vertex shader,
  material shader (diffuse/specular/gloss, per-pixel metallic-roughness conversion, alpha
  mask/blend/dither, emissive, occlusion), scene rendering with frustum culling and depth sorting,
  batched 2D drawing with pixel-exact bitmap text.
- Platform: UTF-8 safe whole-file reading.
- `samples/model_viewer`; golden images for a textured model with text and a skinned figure.
- Dependencies: cgltf v1.15, stb_image (pinned commit), both approved.
- Renderer (`src/renderer`): multithreaded tile-based software rasterizer with C vertex/fragment
  shaders, homogeneous clipping with guard band, top-left fill rule, perspective-correct varyings,
  depth test/write, culling, sort keys, opaque/alpha/additive/Bayer-dithered-alpha blending,
  deterministic output for any thread count, allocation-free steady-state frames.
- Built-in vertex-color shader program.
- Jobs (`src/jobs`): worker pool with deterministic `parallel_for`.
- Platform (`src/platform`): SDL3 window with letterboxed nearest-neighbor presentation, timing,
  PNG load/save.
- Core: frame arena allocator (`core/arena.h`).
- Math: perspective/orthographic/look-at matrices, matrix × vec4.
- `samples/spinning_cubes` demo (`--frames`, `--screenshot` for automated runs).
- Golden-image test infrastructure (`tests/golden`, `WPE_UPDATE_GOLDEN=1` to regenerate).
- Dependency: SDL3 3.4.8 (static, FetchContent, hash-pinned).
- ECS (`src/ecs`): generational 32-bit entities with slot retirement, sparse-set component pools,
  add/remove hooks, queries with include/exclude masks, deferred despawn, pool views.
- Math (`src/math`): vector/quaternion/matrix types and operations wrapping cglm v0.9.7.
- Scene (`src/scene`): Transform/Hierarchy/Name components, parenting with cycle checks and
  keep-world reparenting, recursive destroy, non-recursive world-matrix propagation.
- Stress tests: randomized ECS operations checked against a reference model; 10k-entity transform
  update benchmark (Release budget 4 ms).
- clang-format / clang-tidy configuration.
- CMake + Ninja build with C23, warnings-as-errors, optional sanitizers, `format`/`lint`/`test_all` targets.
- Core module: `WPE_Result` codes, leveled module-tagged logging with sinks, `wpe_assert`/`wpe_check`,
  tracked allocation (`wpe_alloc`, `wpe_free`, …) with leak statistics.
- Unit test framework with per-test leak detection and engine-assert capture.
- GitHub Actions CI for Linux (GCC 14) and Windows (MSYS2 UCRT64 GCC).

### Changed
- **BREAKING**: `wpe_assert_fail` is now `[[noreturn]]`; an assert handler that returns aborts the process.
