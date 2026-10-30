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
- The inspector edits empty asset fields (a text's font, a sprite's image, a mesh's model...): type a
  path to set one; clearing a path removes the asset.
- Engine: `wpe_engine_set_draw_scene`. UI: `wpe_ui_pick` can count every element (editors).
- Tests: the 2D and 3D views (camera, UI, a scene without a camera, play mode), view settings.

### Added (mesh options)
- Mesh renderers (`mesh` in scenes) take `unlit`, `cast_shadows`, `receive_shadows` and
  `double_sided`. Unlit meshes show their material's colors without lighting but still receive the
  sun's shadow (darkened toward the ambient light) unless told not to; double-sided draws and lights
  both faces whatever the material says. `wpe_mesh_renderer_default` gives the defaults.
- Tests: shadows cast and received or not, unlit color with and without shadow, double-sided from
  behind.

### Added (editor: drag assets onto fields)
- Assets dragged from the Assets panel drop onto the inspector's asset fields (models, images, sounds,
  scripts, fonts, the sky's image) and onto the "Add component" path box. A field lights up only for an
  asset that fits it; the drop is the same undoable edit as typing the path. Sounds and scripts can now be
  dragged too, and the viewport accepts only what it can place (models, prefabs, images).
### Added (console clear, grayscale)
- Editor console: typing `clear`, `cls` or `clr` empties it (command `clear_log`, which also empties the
  `log` command's lines).
- Post effect `grayscale`: the image's brightness alone, mixed in by `strength` (fade into a flashback)
  and multiplied by a tint (`tint_r`, `tint_g`, `tint_b`; warm for sepia). In scene files, scripts
  (`wpe.add_postfx({effect = "grayscale"})`) and the editor's effect list.
- Tests: clearing the log; grayscale's luma, strength and tint.
### Added (project templates: racing)
- `racing` template: an arcade car (a sphere rigid body driven by a script: throttle, brake and reverse,
  speed-scaled steering, adjustable grip and a handbrake for drifts) on a walled eight-sided circuit, a
  chase camera, a 3-2-1 countdown, three laps counted through ordered checkpoints, lap times and a best
  lap saved with `wpe.save_data`.
- Tests: the countdown holds the car, it drives and steers and the walls hold it, laps need every
  checkpoint in order, and the race finishes with the best lap saved.

### Changed
- A save directory given by the host (`WPE_EngineDesc.save_dir`, `wpe_runtime --save-dir`) is made if
  it's missing, as the player's own directory always was.
### Added (project templates: platformer)
- `platformer` template: a 2.5D side-scroller. The hero runs along one plane on a character controller
  with jump buffering, coyote time, variable jump height and squash and stretch; a side camera looks
  ahead. Coins (a prefab instanced across the level) with a counter and sparkle bursts, spikes, a moving
  platform the hero rides, a checkpoint, and a goal that shows the time and restarts the level. Scripts
  talk through `wpe.send` messages.
- Tests: coins and their count, tapped versus held jumps, spikes and pits sending the hero back to the
  last checkpoint, riding the platform, finishing and restarting.

### Fixed (character controllers moved by game code)
- A character moved by game code (a respawn, a teleport) was carried to its new place with velocity, and
  Box3D caps body speed, so over long distances it took several steps and entered triggers on the way
  (a respawned platformer hero was hurt by spikes it never touched). Its body now jumps to positions that
  `wpe_character_move()` didn't produce, and walks keep moving it with velocity (pushing crates).
### Added (project templates: third person)
- `third_person` template: an animated figure on a character controller with an orbiting camera (mouse
  and right-stick look, wheel zoom, a sphere cast that keeps the camera in front of walls), walking
  relative to the camera, sprinting, jumping and turning to face its way. Idle, move and jump
  animations are script properties chosen by name, so another character drops in; the move animation
  plays faster with speed. The level is the first-person one plus an archway. `RiggedFigure.glb`
  (CC BY 4.0) comes with its attribution.
- Tests: the figure walks away from the camera, turns, animates only while moving, jumps, and the camera
  stays under the archway's roof.
### Fixed (editor: post effects, mesh colliders)
- The Environment panel's `postfx` list can be edited: an "Add effect" menu (the effect starts with its
  default parameters), and Move up and Remove under each effect. An effect's kind is no longer an
  editable dropdown, which produced effects with the wrong parameters.
- Convex and mesh colliders without a `model` take the entity's own mesh, in scene files, the editor
  and `wpe.add_collider`. Before, switching a collider's shape to `mesh` in the editor failed.

### Fixed (editor: games that capture the mouse)
- A play test whose game captured the mouse (mouse look) couldn't be stopped with the mouse. Shift+F1
  now frees the mouse whatever the game does (command `release_mouse`), the viewport shows how while
  the game holds it, and pausing frees it too.

### Added (project templates: first person)
- Project templates: each directory of `templates/` is a complete, runnable project, embedded into
  the engine at build time (`cmake/WpeEmbedTemplates.cmake`). `wpe_editor <dir> --init --template
  <name>` creates a game from one (the usage text lists them); the project gets its name in
  `project.json`. `empty` (the former starter project) stays the default.
- **BREAKING** (C API): `wpe_project_create(dir, name, template_name)` takes the template (NULL: the
  default); `wpe_project_template_count` and `wpe_project_template_name` list them.
- `first_person` template: a character controller with an eye-height camera, mouse look (click to
  capture, Escape to release) and right-stick look, walking, sprinting and jumping, a crosshair, and
  a "use" ray that pushes the crate in sight; a walled test level with stairs, a ramp, a jumping
  route onto a platform, pillars and crates.
- Script API: `wpe.get_rigidbody(entity)` returns a body's settings (its type tells pushable crates
  from walls).
- Tests: every template creates a project equal to its source files that plays for 20 seconds
  without a script error; the first-person template walks up the stairs, jumps and pushes a crate.

### Added (post effects from scripts; shockwave and radial blur)
- Scripts change the scene's post effects while the game runs: `wpe.get_postfx`, `wpe.set_postfx`
  (by index or effect name), `wpe.add_postfx`, `wpe.remove_postfx` (`WPE_ScriptApp.postfx`).
- New effects to animate: `shockwave` (a ring warping the image outward from a screen point) and
  `radial_blur` (smearing toward a point). `wpe.world_to_screen` turns a world position into
  pixels through the current camera (fresh, even after scripts moved it this frame).
- Effect kinds, configs and a table of each kind's parameters (names, ranges, whole numbers) moved
  to `graphics/postfx.h` (`WPE_PostFxConfig`, `wpe_postfx_params`, `wpe_postfx_from_config`); scene
  files read and write post effects through it, with unchanged text.
- The character demo's fire button sends a shockwave out from the hero.
- Tests: the shockwave moves only its ring, the radial blur leaves its center; configs (names,
  parameters, clamping); the script functions in the master scene.

### Added (depth of field)
- `depth_of_field` post effect (`wpe_postfx_depth_of_field`): sharp around `focus_distance` (within
  `focus_range`), blurring over `falloff` to `radius_pixels`, in front and behind. Each pixel's blur
  radius comes from the depth buffer, then a separable blur over it, where samples count only when
  they're behind the pixel and within its blur, or in front and their own blur reaches it. So sharp
  objects in front of a blurred background keep their edges (no halos), and a blurred foreground
  softens over what's behind it.
- Scene files and the editor's effect list; the character demo uses it.
- Tests: the sharp half unchanged, the far half blurred without the near half's color, a blurred
  foreground spilling over, no radius no change.

### Added (sky)
- `sky` in the scene environment (`WPE_SkySettings` in the render settings): `color` (the clear
  color, as before), `gradient` (zenith, horizon and ground colors, with a curve) or `panorama` (an
  equirectangular image, turned by `rotation`), plus a sun disc with a glow where the scene's sun
  is. One full-screen triangle at the far plane, depth-tested after the opaque geometry (so covered
  pixels aren't shaded), on the same multithreaded pipeline. The engine loads the panorama image
  named by the environment (`sky_image`).
- The character demo has a gradient sky with a sun; the master test scene a panorama
  (`assets/skies/dusk.png`, a generated 256 x 128 dusk). The editor's Environment panel offers the
  sky's modes, and its image is an editable asset path.
- Tests: gradient colors up, level and down; the sun's disc; geometry covering the sky; no holes at
  any angle at 320 x 240 (a sky exactly on the far plane left some); panorama directions and
  rotation; color mode and a missing image draw nothing.

### Added (UI toggles and sliders)
- `toggle` (component 21) and `slider` (22) turn a UI button into an on/off switch (clicks flip
  it; a mark is drawn while on) or a value: a track, the part up to the value filled and a handle.
  Pressing jumps the value there and dragging follows the mouse; focused, left/right step it
  (`step`, or 1/20 of the range) while up/down still move the focus. Values clamp and snap
  (`wpe_slider_constrain`).
- Changes are collected like clicks (`wpe_ui_changes`); the engine calls the entity's script's
  `on_change(value)` before `on_click` (`wpe_script_host_notify_value`). Scripts read and set values
  with `wpe.ui_value`.
- Scene files, editor ("+ Entity" toggle and slider, Add component), docs.
- The title screen has a Music volume slider and a Sound on toggle, remembered with saved data
  (`scripts/volume_slider.luau`, `scripts/sound_toggle.luau`).
- Tests: mouse clicks and drags, keyboard stepping and repeats, steps and clamping, no button no
  reaction, exact pixels drawn; `on_change` reaching a script with the value; scene round trip.

### Added (animation crossfades)
- `wpe_animator_play(animator, animation, fade_seconds, loop)`: with a fade, the previous animation
  keeps playing underneath while the new one's weight rises to 1 (rotations slerp, translations and
  scales lerp). `wpe_model_find_animation` finds an animation by name.
- Scripts: `wpe.play_animation(entity, index or name, loop, fade)`, `wpe.get_animation` (what's
  playing, whether it ended, the model's animation names) and `wpe.animation_speed`.
- Tests: a crossfade's pose halfway and after it ends, cuts, fading out of a paused animation,
  arguments and names. With `assets/meshes/Beowulf.glb` (the project owner's character, 24 clips):
  Run fading into the single-pose Idle, and a one-shot Slash0 ending.

### Added (modules, timers, messages, saved data)
- `require(path)` loads Luau modules from the game's files (`.luau` optional): each runs once in
  its own sandbox and every require returns its value. Circular requires raise. A changed module
  reloads every script; a broken one leaves them running until it's fixed (it stays watched).
- Timers: `wpe.after`, `wpe.every`, `wpe.cancel`, on game time, run after `on_update`. A timer
  goes with the script that made it; one that errors is cancelled.
- Messages: `wpe.get_script` (another script's `self`), `wpe.send` (call a method if it exists,
  errors go to the caller), `wpe.broadcast` (every script with the method; an error disables only
  that receiver).
- Saved data: `wpe.save_data`, `wpe.load_data`, `wpe.delete_data` keep JSON files in the player's
  directory for the game (`wpe_platform_save_directory`, made on first use;
  `WPE_EngineDesc.save_dir` overrides it). Files are replaced whole.
- `WPE_ScriptApp.save_directory`; `scripts/lib/util.luau` holds the demo scripts' shared helpers.
- Tests: modules (caching, circular, hot reload, a broken module fixed later), timers (order,
  cancellation, errors, long frames, owners), messages, saved data (round trip, refusals, damaged
  files, no save directory), and all of it in the master test scene.

### Added (character controller and shape queries)
- Character controllers (`WPE_CharacterController`, component 20, `"character"` in scenes and the
  editor): an upright capsule moved by `wpe_character_move` (Box3D's mover). It slides along walls,
  walks up slopes up to `max_slope` and steps up to `step_height`, follows the ground down
  (`snap_distance`) without sliding downhill, stops at ceilings, pushes dynamic bodies and enters
  triggers through its kinematic body. Its capsule shows in the physics view (cyan).
- Shape casts (`wpe_physics_shape_cast`: box, sphere, capsule) and overlaps (`wpe_physics_overlap`).
- **BREAKING:** `wpe_physics_raycast` takes a `WPE_QueryFilter` (NULL as before): ignore an entity,
  or include triggers. All queries share it.
- Scripts: `wpe.shape_cast`, `wpe.overlap`, options for `wpe.raycast` (`ignore`, `triggers`),
  `wpe.add_character`, `wpe.move_character`, `wpe.get_character`, `wpe.remove_character`.
- Demo: `scenes/character.scene` with `scripts/hero.luau` (ramps, stairs, a ledge, a low ceiling,
  crates and a trigger). `game.luau` takes a `help` property for the scene's control lines.
- Fixed: the master test scene's ball-at-rest check never ran (two branches tested the same frame).
- Tests: queries and filters; characters landing, walking, sliding along walls, on walkable and
  steep slopes, on stairs and against a taller ledge, under a ceiling, pushing crates (and stopped by
  a pinned one), entering triggers, and the component's lifecycle.

### Added (declared script properties)
- Scripts declare their properties with defaults (`Spinner.properties = {speed = 1.2}`): every
  instance starts with them, hot reload adds newly declared ones, and a value equal to its default is
  not saved. Setting a declared property to nil resets it. `wpe_script_visit_declared` lists them,
  sorted by name, with current values and defaults.
- Editor: the inspector shows a script's declared properties ready to edit, with Reset for changed
  ones, × for undeclared ones, and a row to add a property (name and type). Commands
  `get_script_properties` and `set_script_property` (undoable; null removes or resets).
- The demo scripts and the `--init` template declare their properties instead of `self.x = self.x or`.
- Tests: defaults, values equal to defaults, resets, hot reload, sorting; the editor commands with undo.

### Added (editor: particle icons)
- Particle emitters have viewport icons (a sparkle in their starting color) next to the light icons;
  clicking one selects the emitter. The view setting `lights` is now `icons` (View > Icons, L).

### Fixed (editor play tests)
- A key held when a play test stopped stayed held in the next one: key releases now always reach
  the game, and play and stop release every key and mouse button (`wpe_input_release_all`).
- While the game holds the mouse (mouse look), the editor's panels no longer react to the hidden
  cursor, so it can't drift onto them and take the focus from the game view.

### Added (particles)
- `WPE_ParticleEmitter` (`particles` in scenes, docs in `docs/scenes.md#particles`): emits and
  simulates particles drawn through the billboard shader, one draw per emitter.
  - Emission: rate, bursts, cycles with a duration (looping or once), a particle limit, point/sphere/
    box shapes, a direction cone; world or local space; a seed for repeatable effects.
  - Motion: lifetime and speed ranges, gravity, drag, spin and random rotation.
  - Look: size and color over life, sprite sheets over life, cutout/alpha/additive (alpha sorted back
    to front), optional lighting.
  - Large emitters integrate on the job system with results identical to a single thread.
- Scripts: `wpe.set_particles`, `get_particles`, `emit_particles`, `play_particles`,
  `stop_particles`, `particle_count`, `remove_particles`.
- Editor: the selected emitter plays in edit mode without changing the scene; **+ Entity** adds an
  emitter; the inspector has choices for blend and shape.
- Graphics: `wpe_particles_update`, `wpe_particles_emit`, `wpe_particles_restart`,
  `wpe_particles_clear`, `wpe_particles_preview`; render stats count particles.
- Assets: `prefabs/fire.prefab`, `smoke.prefab`, `sparks.prefab`; the master test scene has them and a
  lit local-space emitter.
- Tests: emission, bursts, cycles, limits, clearing, restarting and preview; motion, emission
  direction and spaces; identical results with and without worker threads; drawing and culling; scene
  round trips and rejected mistakes; the script API in the master test scene.

### Added (billboards)
- `WPE_Billboard` (`billboard` in scenes, docs in `docs/scenes.md#billboards`): 3D sprites drawn by
  the scene renderer, depth tested and fogged.
  - Modes: fixed, axis-aligned (faces the camera from any side, upright), axis-aligned to world Y, and screen-aligned; a pivot
    and an in-plane rotation.
  - Blending: cutout (no sorting), alpha (sorted with transparent geometry) or additive.
  - Unlit by default; `lit` uses the ambient, point/spot lights and the sun with its shadow, wrapped
    softly.
  - Sprite sheets: a grid of frames played at `fps` (looping or once) by `wpe_billboard_update`.
- Scripts: `wpe.set_billboard`, `wpe.get_billboard`, `wpe.remove_billboard`.
- Editor: billboards are picked in the viewport, added from **+ Entity** or by dropping an image
  into the 3D view, and have choices for mode and blend in the inspector.
- Graphics: `wpe_billboard_corners`, `wpe_fog_factor`; render stats count billboards.
- Assets: `sprites/tree.png`, `glow.png` and `coin.png`; the master test scene has a billboard of
  every mode and blend.
- Tests: corner geometry for every mode, pivot, scale and rotation; sprite-sheet timing; drawing
  (frames, cutout, additive, lit, culling, image lifetime); scene round trips and rejected mistakes;
  the script API in the master test scene.

### Added (new projects)
- `wpe_editor <dir> --init [--name <title>]` (and `wpe_project_create`): creates a project in a new or
  empty directory: project.json with move/jump actions, and a starter scene (camera, sun, ground with a
  collider, a cube spun by a script). It never overwrites files.

### Added (editor: fit colliders to meshes)
- `fit_collider` command and inspector buttons (**Fit to meshes** on a collider, **Add box collider fitted
  to meshes** without one): sizes and centers a box, sphere or capsule collider around the meshes of
  the entity and its children, in the entity's own space; other collider settings are kept.

### Added (built-in font)
- `assets/fonts/small.png` is compiled into the engine (`wpe_font_create_builtin`): projects that name no
  font, or whose font file is missing (with a warning), get text with it.

### Changed
- Scene, prefab and other engine JSON files write floats in their shortest exact form: a value typed
  as `0.06` is saved as `0.06` rather than `0.0599999987`, and whole numbers as integers.
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
