# Changelog

All notable changes to WillPowerEngine are recorded here. Backward-incompatible changes are marked **BREAKING**.

## [Unreleased]

### Added (Milestone 10: the editor)
- `wpe_editor` (`editor/`, docs in `docs/editor.md`): Dear ImGui (cimgui) interface drawn by the
  engine's software rasterizer, with docked panels: viewport (fly camera, picking, move/rotate/scale
  gizmos, model drag and drop), hierarchy (reparenting by drag and drop), inspector (every component
  and script property, add/remove), environment, assets, and a console that runs commands. Play,
  pause and stop run the game in place and restore the scene afterwards; undo/redo covers every
  edit (continuous drags are one step); scenes are created, opened and saved; games are exported.
- Agent control: one JSON command set (entities, components, environment, scenes, undo, play,
  frames, screenshots of the game or the whole editor, Luau, assets, export) used by the UI itself,
  headless batches (`--run`), one-shot calls (`--command`) and a live local channel to the running
  editor (127.0.0.1:47800, `--send` client). Replies carry the log messages raised by the command.
- Engine: embedded mode (the host owns the window and forwards events), a simulation switch (edit
  mode), scenes loaded from text, entity- and component-level scene functions
  (`wpe_scene_save_entity`, `wpe_scene_create_entity`, `wpe_scene_set_component`,
  `wpe_scene_remove_component`, environment read/write) and a save filter.
- Renderer: per-draw scissor rectangles; `wpe_texture_update`. Platform: local TCP sockets
  (`platform/net.h`), window size and text input. Core: compact JSON writing.
- Dependency: Dear ImGui v1.92.9b (docking) with cimgui 1.92.9bdock (both approved, MIT).
- Tests: scissor, texture updates, the UI backend (rendered headless), sockets, scene editing
  functions, edit mode, editor commands (undo/redo, coalescing, scenes, play mode, screenshots),
  and a command-line test that drives the editor like an agent.

### Added (2D scenes and UI)
- UI module (`ui/ui.h`, docs in `docs/scenes.md#ui`): retained UI elements as entities.
  - `ui`: a rectangle anchored to its parent element or the screen (anchor fractions plus pixel
    offsets; the default fills the parent).
  - `sprite`: an image (PNG/JPEG, a region of it, optionally nine-sliced) or a plain color.
  - `text`: bitmap-font text, aligned horizontally and vertically, optionally word-wrapped.
  - `button`: focusable and clickable; its state tints it and its children.
- Navigation: the mouse, arrow keys, D-pad and left stick (nearest button in that direction, with
  repeat), Return/Space/pad south to click. Project actions `ui_up`... `ui_submit` rebind it.
- 2D scenes: a scene without a camera shows its clear color and UI. The UI draws after
  post-processing and debug lines, under the scripts' immediate 2D.
- Scripts: `on_click`, and `wpe.get_text`/`set_text`, `set_text_color`, `set_sprite_color`,
  `set_ui_visible`/`is_ui_visible`, `get_ui_offsets`/`set_ui_offsets`, `ui_rect`,
  `set_interactable`, `ui_focus`/`ui_focused`. C: `wpe_script_host_notify`.
- Assets: images (`WPE_ASSET_IMAGE`). Engine: `wpe_engine_ui`, `wpe_engine_set_draw_ui`.
- Demo: `scenes/title.scene`, a 2D title menu (now the start scene) with buttons for the demo, the
  master test scene and quitting; `ui/panel.png`. The master test scene has a HUD over 3D.
- Tests: layout, picking, drawing (pixel-exact sprites, nine-slice, tints, text alignment and
  wrapping against plain 2D drawing), keyboard and mouse navigation with clicks, action rebinding;
  UI scene round trips and rejected mistakes; engine clicks reaching scripts in a 2D scene.

### Added (editor: 2D view)
- A 2D view (toolbar **2D**, View menu, `set_view {"mode": "2d"}`): edit mode shows what the game
  shows, the scene's camera (or only the clear color without one) with the UI on top. UI elements
  have outlines; click to select, drag to move, drag the eight handles to resize; anchors are marked.
  Drags change offsets in whole pixels, or in steps of the new `snap_ui` (8 px) when snapping. The
  3D view hides the UI.
- **+ Entity** adds a UI panel, text or button (with its label), inside the selected UI element;
  images in Assets become sprites at their size (double-click or drag). The inspector adds `ui`,
  `sprite`, `text` and `button` components, with choices for text alignment.
- Engine: `wpe_engine_set_draw_scene`. UI: `wpe_ui_pick` can count every element (editors).
- Tests: the 2D and 3D views (camera, UI, a scene without a camera, play mode), view settings.

### Changed
- The editor's ImGui backend functions are renamed `wpe_ui_backend_*` (the `wpe_ui_` prefix now
  belongs to the UI module).

### Added (editor: light icons)
- An icon at every light in the viewport in edit mode, in the light's color: a bulb for point lights,
  a cone showing a spot light's direction and angle, and a sun with an arrow for directional lights.
  Clicking an icon selects its light. View > Light icons (L) and `set_view` `lights` switch them.

### Fixed (editor)
- The fly camera no longer snaps to another direction when the right mouse button is pressed again
  after turning more than 90 degrees from where it started: the yaw and pitch it resumes from now come
  from the view direction instead of Euler angles, whose yaw only reads back within ±90 degrees.
- Moving along an axis that points nearly at the camera no longer flings the entity thousands of
  units away. Move drags follow the point on the axis nearest the mouse ray (and pause where that
  point is undefined), and handles shorter than 12 pixels on screen can't be grabbed.

### Added (prefabs)
- Prefab files (`.prefab`): reusable entity trees in the scene schema, placed in scenes as linked
  instances (`"prefab": {path, ids, overrides}`, docs in `docs/scenes.md#prefabs`). Editing a prefab
  updates every instance except where an instance overrides it.
  - Overrides are found by comparing each instance with its prefab as instantiated, so any change
    counts (editor, agent, script or code).
  - Members can be removed per instance, and entities attached beneath them.
- Engine: `engine/prefab.h`:
  - instantiate, save as prefab or apply to it, revert (a component or the whole instance),
    unpack, and override queries;
  - `WPE_COMP_PREFAB`;
  - a cache of parsed prefab files, re-read when they change.
- Scripts: `wpe.instantiate` takes prefab files, by path or by a project prefab name.
- Editor:
  - Commands `save_prefab`, `apply_prefab`, `revert_prefab` and `unpack_prefab`. `create_entity`
    places prefabs, `list_entities` shows membership, and members can't be reparented.
  - Hierarchy: instances in blue, right-click Save as prefab / Apply / Revert / Unpack.
  - Inspector: a prefab bar with Apply, Revert all and Unpack; overridden components are marked
    and revert from their header's right-click menu.
  - Assets place prefabs by double-click or drag.
- Core JSON: `wpe_json_write_value` (shortest exact numbers) and `wpe_json_equal`.
- Demo: `prefabs/crate.prefab`. The demo's crates are instances of it, and the master test scene
  has an instance with an override.
- Tests: instances and overrides, exact scene round trips with instances, prefab edits reaching
  instances, save/apply/revert/unpack, scripts, rejected mistakes; the editor's prefab commands;
  JSON value writing and equality.

### Added (editor: unsaved changes, focus, snapping)
- An unsaved-changes prompt (Save / Don't save / Cancel) before a new scene, opening a scene,
  quitting or closing the window. Saving a scene that was never saved asks for a name, then
  continues. Platform: `wpe_window_cancel_close`.
- **BREAKING (commands):** `new_scene`, `load_scene` and `quit` refuse while the scene has unsaved
  changes, unless they carry `"discard": true`.
- Focus on the selection: F, Edit menu, double-click in the hierarchy, and the `focus` command.
  The camera moves, keeping its direction, until the entity and its children fill the view.
- Gizmo snapping: the Snap toggle (toolbar and View menu), adjustable steps, Ctrl to invert while
  dragging. World-axis moves land on the step grid; rotations and local moves go in whole steps;
  scales land on multiples. New view settings `snap`, `snap_move`, `snap_rotate` and `snap_scale`.
- The fly camera starts from where commands left the camera: before, it snapped back to where the
  user last flew.

### Added (editor: the rest of the spec's tool list)
- Ground grid in the viewport: fades with distance, X axis red and Z axis blue, adjustable spacing.
- Physics debug drawing: collider wireframes colored by body type, in edit and play mode.
- Gizmo axes in world or local space (toolbar, X key). Moves and rotations are converted into the
  parent's space, and scaling uses the entity's own axes.
- Script hot reload in the editor, in edit and play mode (on by default).
- View settings for all of the above through the `get_view`/`set_view` commands; they are never
  saved or undone. View menu, plus shortcuts G (grid) and P (physics).
- Engine: `wpe_engine_set_debug_draw` (grid, physics wireframes), drawn after post-processing and
  depth tested.
- Physics: `wpe_physics_sync` updates bodies for an edited world without stepping.
- Tests: view settings and overlays, edit-mode physics, hot reload in the editor, `wpe_physics_sync`.

### Fixed
- A collider rescaled in the editor kept its old size in play mode, because shapes are built for
  the world scale they were created with. Edit mode now rebuilds them.

### Changed (editor performance)
- The editor window draws its interface on the GPU through SDL's renderer (a C backend after Dear
  ImGui's `imgui_impl_sdlrenderer3`) instead of the software rasterizer. Measured at 1600x900:
  - before: about 13 ms per frame for the interface alone, so frame rates were capped well below the
    display's refresh rate;
  - after: the editor reaches the display's refresh rate (about 236 fps on a 240 Hz display).
  - The game view is still rendered by the engine.
- Headless runs keep the software path. In a running editor, `screenshot` with target `"editor"`
  reads back the frame the window showed.
- Platform: `wpe_window_renderer` gives hosts that draw the window themselves its SDL renderer
  (opaque `WPE_PlatformRenderer`).
- UI backend API: `wpe_ui_create` takes the renderer (or NULL for software); `wpe_ui_present`,
  `wpe_ui_capture` and `wpe_ui_output_size` are new. `wpe_ui_set_image` replaces
  `wpe_ui_texture_id`.
- Test: the GPU path, run on SDL's software `SDL_Renderer`, must draw the same frame as the
  software path.
- Vertices for SDL are interleaved. SDL's software renderer reads UVs with the color stride in its
  rectangle fast path, so separate arrays crashed it (found by the new test, about 70% of runs).

### Fixed (editor, after hands-on testing)
- The rotate gizmo draws one colored ring per axis instead of handles that looked like the move
  gizmo's; dragging follows the ring's direction where it was grabbed, and the angle is shown.
- Dragging a field flickered every field below it: the inspector and environment panels stopped
  drawing after the changed field. They now draw everything and apply the change afterwards.
- Field IDs could collide with their component header (the `mesh` field of the `mesh` component);
  every field has its own ID scope.

### Changed
- Editor look: rounded widgets, more spacing, a dark palette with an amber accent, and a fixed
  column for field names in the inspector and environment.
- Spec: the viewport keeps its fly camera rather than Blender-style orbit navigation (user's
  decision after testing).
- `format` and `lint` also cover `editor/`, `runtime/` and `tools/` (two unchecked `atoi` calls in
  the tools were replaced).
- Enumeration errors in scene and project files name the rejected value.
- Command-line programs (`wpe_pak`, the new `wpe_export` and `wpe_runtime`) build into `build/bin/`.
- `WPE_PakEntryInfo.offset` is the byte offset in the file, which includes the embedding offset for
  archives appended to another file.
- The scripting integration test is replaced by the master scene test (`integration_full_scene`):
  the API tour now runs inside `scenes/test_full.scene` under the engine runtime.
- Example scripts read their settings from script properties (`spinner.luau`: speed;
  `orbit_light.luau`: radius, speed, height).
- **BREAKING** Assets are shared through a reference-counted asset manager instead of borrowed
  pointers: `WPE_MeshRenderer`/`WPE_Animator` hold a `model_asset` handle (the `model` pointer is
  resolved by the component's add hook), `WPE_Collider.model` is a handle,
  `wpe_graphics_register_components(ecs, assets)` and `wpe_model_instantiate(ecs, assets, handle,
  ...)` take the manager, and `wpe_script_host_register_prefab()` takes a handle.
- **BREAKING** `WPE_ScriptHostDesc` takes `assets` (scripts and prefabs are asset paths in its
  VFS) instead of `asset_root`, `jobs` and `max_texture_size`; a missing script file is reported
  as `WPE_RESULT_ERROR_NOT_FOUND`. `WPE_PhysicsDesc` gains `assets` (for convex/mesh colliders).

### Added
- Engine runtime (`src/engine`): `WPE_Engine` owns every subsystem of a running game, reads
  `project.json`, loads scenes, runs the frame loop (input, fixed-step simulation, scripts,
  animation, audio, rendering with the scene's post-processing and the scripts' 2D drawing on top)
  and performs scene switches and quits requested by scripts. Headless mode renders without a
  window and mixes audio without a device.
- Scene files (`engine/scene_file.h`, format in `docs/scenes.md`): JSON scenes with every built-in
  component, file-local entity references, model instances (`"instance"`), script properties, the
  environment (render settings, fog, shadows, up to 8 post effects, gravity, music) and game
  components through serializers. Loading is strict (errors name the line) and atomic; saving is
  lossless and stable (load-save cycles reproduce the text).
- Project files (`engine/project.h`, `docs/runtime.md`): name, display, start scene, fixed rate,
  audio levels, script limits, named prefabs and input actions.
- Input (`src/input`): keyboard (physical keys), mouse (framebuffer coordinates, deltas, wheel,
  capture) and gamepads (standard layout, up to 4 player slots, hot-plug, radial stick and trigger
  dead zones, rumble) with named actions bound across devices, per player or for any player.
- `wpe_runtime`: plays a project directory, an archive, or the game embedded in its own executable;
  `--headless`, `--frames`, `--fixed-dt` and `--screenshot` for automated runs, `--watch` for hot
  reload. A windowed program on Windows that still logs to a parent terminal; statically linked
  compiler runtimes.
- Game export: `wpe_export` (and `wpe_export_game()`) validates a project, packs it and appends the
  archive to the runtime, producing one self-contained game executable.
- `.pak` embedding: archives appended to another file are found through a trailer
  (`wpe_pak_probe`, `wpe_pak_embed`); `wpe_pak_create_from_directory` packs a directory (used by the
  tool and export).
- Scripting: input (`wpe.action_*`, `wpe.key_*`, `wpe.mouse_*`, `wpe.set_mouse_captured`,
  `wpe.gamepad_*`, `wpe.rumble`), game services (`wpe.load_scene`, `wpe.quit`, `wpe.set_camera`,
  `wpe.get_camera`, `wpe.screen_size`), 2D drawing (`wpe.draw_text`, `wpe.draw_rect`,
  `wpe.text_size`, `wpe.color`), `wpe.find`, `velocity` in `wpe.add_rigidbody`, and script
  properties (`wpe_script_set_property`, `wpe_script_visit_properties`, `wpe_script_path`).
- Primitive model files (`.prim`): planes, boxes and spheres with a material, described in JSON and
  loaded as models; `wpe_model_create_sphere`.
- Core: strict JSON reader and pretty-printing writer (`core/json.h`). ECS:
  `wpe_ecs_component_size`, `wpe_ecs_spawn_serial`. Physics: `WPE_RigidBody.initial_velocity`, and
  `wpe_physics_create_bodies` (bodies exist right after a scene loads, before the first step).
  Platform: window event handlers, a window-less event pump, relative mouse mode, window titles,
  fullscreen windows, the executable's path, file renaming and executable permissions, console
  attachment and error message boxes.
- The demo game (`assets/`): `project.json`, a playable third-person scene (`scenes/demo.scene`:
  a rolling ball with camera-relative movement, jumping, throwing crates, rumble on hard landings,
  a trigger zone, and a HUD that shows every connected gamepad's live state) and the master test
  scene (`scenes/test_full.scene`) with every component type, edge cases (an entity without
  components, extreme transforms, a 2000-entity spawn, prefab cycles) and four post effects.
- Tests: JSON, input (real SDL events and SDL virtual gamepads: edges, actions, dead zones,
  triggers, slots, hot-plug, rumble), embedded archives, primitive models, script properties and
  services, project and scene files (round trips, 40+ rejected mistakes with rollback, game
  components), the engine (scene switching, quitting, saving, headless rendering) and export, the
  60-second master scene test, and an export round trip that plays the exported executable.
- Virtual file system (`src/vfs`): asset paths over mounted directories and `.pak` archives
  (later mounts override earlier ones), whole-file reads, streaming, listing, change times and path
  validation/normalization. glTF models load through it, including `..` references.
- `.pak` archives: format version 1 (documented in `docs/assets.md`) with per-file zstd
  compression where it pays off, CRC-32 integrity checks, a sorted index and strict validation;
  writer and reader APIs and the `wpe_pak` tool (create/list/verify/extract; reproducible output).
- Asset manager (`assets/asset_manager.h`): generational, reference-counted handles cached by type
  and path, built-in model and font loaders, code-created assets (`wpe_asset_add`), user asset
  types, and leak reports at shutdown.
- Audio (`src/audio`, miniaudio 0.11.25): WAV/FLAC/MP3 sound assets decoded once and shared, 3D
  `WPE_AudioSource` components (distance attenuation, panning, looping, pitch), one-shot voices
  with stealing, streamed music with crossfades (also straight from archives), master/effects/music
  buses, a listener that follows an entity, and a device-less mode rendered on demand (tests,
  headless runs, machines without audio hardware).
- Scripting: `wpe.play_sound`, `wpe.stop_sound`, `wpe.add_audio_source`,
  `wpe.remove_audio_source`, `wpe.set_audio_playing`, `wpe.is_audio_playing`, `wpe.play_music`,
  `wpe.stop_music`, `wpe.set_volume`, `wpe.get_volume` and the `wpe.BUS_*` constants.
- Platform: file streams (`wpe_file_open/read/write/seek/...`), recursive directory walks and
  directory creation/removal. Core: CRC-32.
- Samples: `script_demo` plays impact thuds (louder for harder hits) and an ambient music loop
  (`--mute`), and runs entirely from an archive with `--pak`; all samples load through the VFS.
  Synthesized demo sounds in `assets/sounds` and `assets/music`.
- Tests: VFS and archive tests (round trips, corruption, priority, streaming, listing), asset
  manager tests (caching, references, stale handles, custom types, glTF from archives), audio
  tests (rendered without a device: playback, pitch, buses, panning, attenuation, sources, voice
  stealing, streamed and crossfaded music), script audio API tests, a `wpe_pak` round trip, and
  audio in the 60-second integration test. Golden images are unchanged.
- Dependencies: zstd v1.5.7 and miniaudio 0.11.25 (both on the approved list).
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
