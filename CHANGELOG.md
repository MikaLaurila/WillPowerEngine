# Changelog

All notable changes to WillPowerEngine are recorded here. Backward-incompatible changes are marked **BREAKING**.

## [Unreleased]

### Added
- Physics (`src/physics`): Box3D v0.1.0 rigid bodies driven by the `WPE_RigidBody` (static,
  kinematic, dynamic; damping, gravity scale, rotation lock, continuous collision) and
  `WPE_Collider` (box, sphere, capsule, convex hull and triangle mesh from model meshes; offset,
  density, friction, restitution, triggers) components. World scale is baked into shapes; a
  collider without a rigid body becomes static level geometry. Each fixed step creates missing
  bodies, follows game-code moves (kinematic bodies move with velocity so they push; others
  teleport), simulates, writes poses back into local transforms (parent-relative) and reports
  contact/trigger begin/end events. Raycasts, forces, impulses, velocities, gravity, refresh after
  settings changes, statistics and wireframe debug drawing. Box3D allocates through `wpe_alloc`
  and reports assertions through the engine.
- Scripting: `wpe.add_rigidbody`, `wpe.add_collider`, `wpe.remove_rigidbody`,
  `wpe.remove_collider`, `wpe.apply_force`, `wpe.apply_impulse`, `wpe.get/set_velocity`,
  `wpe.raycast`, `wpe.get/set_gravity`, body/collider constants, and the callbacks
  `on_collision_enter/exit(other)` and `on_trigger_enter/exit(other)` dispatched by
  `wpe_script_host_dispatch_physics()`. `WPE_ScriptHostDesc.physics` enables them.
- Graphics: `graphics/debug_lines.h`, depth-tested screen-space-width 3D lines drawn after the
  scene and before 2D.
- `script_demo` uses physics: boxes fall, tumble and get batted away by the spinning (kinematic)
  helmets, then remove themselves after landing; `--debug-physics` draws collider wireframes.
- Tests: physics unit tests (resting, restitution, contacts, triggers, raycasts, teleports,
  component removal, determinism, rotation lock, parenting, scaling, capsules, mesh/convex
  colliders, kinematic carrying, forces, refresh, debug drawing), script physics tests, a
  physics stress test, and physics in the 60-second scripting integration test.
- Dependency: Box3D v0.1.0 (MIT).
- Scripting (`src/script`): Luau 0.742 VM host with per-entity scripts (`on_start`,
  `on_update`, `on_fixed_update`, `on_destroy`, `on_reload`), sandbox (safe libraries only,
  read-only globals, per-file environments, asset-root path checks), per-callback time budget,
  VM memory limit, error isolation with tracebacks, hot reload preserving instance state, named
  and path-based prefabs, and the `wpe` API (entities, hierarchy, transforms, lights,
  visibility, animation, time). API reference in `docs/scripting.md`.
- Core: fixed-timestep clock (`core/fixed_step.h`) with spiral-of-death protection.
- ECS: `wpe_ecs_unregister_component`.
- Math: `wpe_quat_to_euler_xyz`. Platform: `wpe_file_modified_time`.
- Example scripts (`assets/scripts`), `samples/script_demo` (fixed-step game loop, `--watch`
  hot reload), and an integration test running the full scripting API for 60 simulated seconds.
- Lighting: `WPE_Light` component (directional/point/spot). Hybrid shading: per-vertex lighting
  for ambient, up to 4 point/spot lights per object and the sun; per-pixel sun shadows and
  normal-mapped sun lighting. Ambient is also reflected specularly.
- Renderer: crack-free adaptive subdivision of near-camera triangles before vertex shading
  (`subdivision_levels`, `vertex_lerp`), depth-only draws, 32 varyings, writable framebuffer
  access between frames.
- Shadows: texel-snapped orthographic shadow map for the main directional light with 3x3 PCF.
- Fog (linear/exponential) in the material shader.
- Post-processing chain (`graphics/postfx.h`): multi-pass, row-parallel effects with SSAO,
  chromatic aberration, PS1-style dither/quantization, vignette and color adjustment.
- Assets: tangent generation for normal-mapped meshes without tangents; procedural plane/box
  models and `wpe_model_create_mesh`.
- `samples/lighting_demo`; golden images for lights/shadows and post-processing.
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
- ECS `on_remove` hooks now run in descending component type id order (higher-level components
  such as scripts see transform/hierarchy intact during teardown).
- **BREAKING**: `WPE_SceneRenderSettings` replaced its single `light` with ambient, fog, shadow,
  subdivision and default-sun settings; lights now come from `WPE_Light` entities.
- **BREAKING**: `WPE_MAX_VARYINGS` raised to 32; `WPE_ShaderProgram` gained `vertex_lerp`.
- **BREAKING**: `wpe_assert_fail` is now `[[noreturn]]`; an assert handler that returns aborts the process.
