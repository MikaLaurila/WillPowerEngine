---
name: willpowerengine
description: >
  Build games with WillPowerEngine: Luau scripts on ECS entities, JSON scene and prefab files, the
  editor's JSON command interface, and the templates (first_person, third_person, platformer, racing,
  racing_online, stealth, shooter, dialogue). Translates the generic game-dev skills (game-ai, camera-systems, game-feel,
  input-systems, level-design, save-systems, game-ui-ux, audio-design, physics-tuning, genres) into this
  engine's API. Use when making or changing a game in a WillPowerEngine project: a directory with
  project.json ("format": "wpe.project"), scenes/*.scene, scripts/*.luau, or when the user mentions wpe_,
  wpe_editor, wpe_runtime, or a template by name.
license: Apache-2.0
compatibility: WillPowerEngine (this repository), Luau scripting; read docs/scripting.md for the exact API
metadata:
  engine: willpowerengine
  category: engines
  difficulty: intermediate
---

# WillPowerEngine

A C23 engine with a software rasterizer (low resolutions, a retro look), Box3D physics, Luau scripting,
an editor, multiplayer and navigation. Games are data plus Luau: you rarely touch C to make one.

## When to use

- Making a game or a feature in a WillPowerEngine project: scripts, scenes, prefabs, input, UI.
- When a generic game-dev skill applies (enemy AI, a camera, juice, saving...): use the mapping below to
  do it the engine's way instead of inventing a system the engine already has.
- Not for changing the engine itself (src/): follow CLAUDE.md and "WillPowerEngine AGENTS.md" there.

## The source of truth

Read before writing code; these are complete and tested against the engine:

| Topic | Document |
|---|---|
| Every `wpe.*` function, callbacks, rules | `docs/scripting.md` |
| Scene and prefab files, every component key | `docs/scenes.md` |
| Projects, templates, the runtime, export, input actions | `docs/runtime.md` |
| Editor and its JSON commands | `docs/editor.md` |
| Navigation: grid, agents, obstacles, the stealth template | `docs/navigation.md` |
| Multiplayer | `docs/networking.md` |
| Assets, `.prim` primitives, `.pak` | `docs/assets.md` |

The editor's Script panel and `test_editor_every_api_function_is_documented` keep scripting.md complete:
if a function isn't there, it doesn't exist.

## Project anatomy

```
project.json        name, start scene, display (e.g. 320x240, scale 3), fixed rate, input actions
scenes/*.scene      JSON: environment (sky, fog, shadows, gravity, navigation, postfx) + entities
prefabs/*.prefab    reusable entity trees, instanced in scenes or with wpe.instantiate
scripts/*.luau      one class per file (return a table), attached with "script": {"path": ...}
meshes/ (.glb, .prim)  sounds/  music/  fonts/  sprites/  ui/
```

Start from a template, never from nothing: `wpe_editor my_game --init --template stealth`. Each template
is a small complete game in `templates/<name>/`, written to be read.

## Scripts in one screen

```lua
local Enemy = {}
Enemy.properties = { speed = 3, target = "player" } -- defaults; scenes and the inspector override

function Enemy:on_start() self.player = wpe.find(self.target) end       -- once, first update
function Enemy:on_update(dt) end                     -- every frame (game time); drawing, timers, input edges
function Enemy:on_fixed_update(dt) end               -- fixed steps: movement with wpe.move_character
function Enemy:on_collision_enter(other) end         -- also _exit, on_trigger_enter/_exit
function Enemy:on_nav_arrived() end                  -- also on_nav_failed
function Enemy:take_damage(n) end                    -- any method: wpe.send(e, "take_damage", 5)

return Enemy
```

- Script properties are instance fields, so they hide methods of the same name: a property `impact` makes `self:impact()` call a number. Name them differently.
- Find entities once (`wpe.find(name)`) and keep the handle; `wpe.get_script(e)` is another script's
  `self`; `wpe.broadcast(method, ...)` calls everyone who has the method.
- Angles: scripts use radians (`wpe.set_rotation(e, vector.create(0, yaw, 0))`), scene files degrees
  (`"euler"`). Entities look down **−Z**: `yaw = math.atan2(-dir.x, -dir.z)`.
- `wpe.destroy` is deferred to the end of the update. Structural changes are refused in `on_destroy`.
- Input: name actions in project.json (`"jump": [{"key": "space"}, {"pad_button": "south"}]`), read them
  with `wpe.action_value/down/pressed`. Keep `pressed` edges in `on_update` and act on them in
  `on_fixed_update` (see the templates' players).
- Gamepad players are numbered from 1 in Luau.

## Mapping the generic skills

| Generic skill | In WillPowerEngine |
|---|---|
| game-ai (pathfinding, steering, state machines, perception) | **Never hand-roll A\* or steering.** Give the entity `nav_agent` (and a `character` to walk with physics), then `wpe.nav_move_to` / `nav_follow` / `nav_stop`; react in `on_nav_arrived` / `on_nav_failed`. Perception: a distance + angle test with `wpe.forward`, then `wpe.raycast(eye, dir, dist, {ignore = self.entity})` for line of sight. Noise: `wpe.broadcast("on_noise", pos, radius)`. Wandering: `wpe.nav_random_point(home, r)`. Moving blockers: `nav_obstacle`. The full pattern is `templates/stealth/scripts/guard.luau`; a minimal one `templates/third_person/scripts/wanderer.luau`. Animation-driven AI: `wpe.set_anim_states`. **State machines** (players, enemies, NPCs): copy `templates/dialogue/scripts/lib/states.luau` (states as tables of `enter`/`exit`/`update`/`fixed_update` and event functions; returning a state's name moves there; `machine:send(event)`, `States.extend` for shared behaviour, `push`/`pop` for interruptions like hurt or stun; `machine.state`/`.time` to show while debugging); `templates/dialogue/scripts/talker.luau` uses it. |
| camera-systems | Cameras are entities with `"camera"`; `wpe.set_camera(e)`. Orbit with collision: `templates/third_person/scripts/orbit_camera.luau`; chase camera: `templates/racing/scripts/chase_camera.luau`; side-scroller: `templates/platformer/scripts/side_camera.luau`; first person: a `head` child (first_person). Shake: `wpe.shake`. |
| game-feel (juice) | Built in: `wpe.hit_stop`, `wpe.slow_motion`, `wpe.set_time_scale`, `wpe.shake`, `wpe.tween` / `tween_value` with easing, particles (`wpe.set_particles`, `emit_particles`), trails, slashes, post effects (`wpe.set_postfx`: shockwave, radial_blur, cracks, shatter, color_adjust...), scene transitions (`wpe.load_scene(path, {transition = "fade", duration = 0.4})`: fade, wipe, iris, dissolve). Squash and stretch: the platformer's player. |
| input-systems | Actions in project.json bound to keys, mouse, pad buttons and axes at once (docs/runtime.md#input-actions); dead zones in `input`; `wpe.rumble`; mouse look with `wpe.set_mouse_captured` and `wpe.mouse_delta`. Rebinding: `wpe.get_bindings` / `set_bindings` / `reset_bindings` (saved for the player automatically, loaded at every start), `wpe.listen_binding(device)` + `wpe.heard_binding()` to catch the key or button to use (the press is swallowed). Device prompts: `wpe.action_prompt(action, player, direction)` gives "Space" / "A" / "Cross" for the device used last, `wpe.input_device()` the device and pad style. A complete controls screen with swapping: `templates/platformer/scripts/controls.luau` (its pause menu). |
| physics-tuning | `rigidbody` (static/kinematic/dynamic, damping, gravity_scale), `collider` (box, sphere, capsule, convex, mesh; friction, restitution, trigger), `character` for player-like movement (`wpe.move_character` returns the clipped velocity: keep it). Queries: `wpe.raycast`, `shape_cast`, `overlap`. Gravity is the script's to add for characters. |
| level-design | Scenes are JSON entities; build with `.prim` boxes or glTF models with mesh colliders. Prefabs for repeated pieces. Check walkability with the editor's View > Navigation (N) and Test a path; check collisions with View > Physics shapes (P). |
| save-systems | `wpe.save_data(name, value)` / `load_data` / `delete_data` (numbers, strings, booleans, vectors and tables of them, written atomically to the player's save directory). |
| game-ui-ux | Retained UI entities: `ui` (anchors + offsets), `sprite` (nine-slice), `text`, `button` (`on_click`), `toggle` / `slider` (`on_change`), `text_input` text fields (`on_change(text)`, `on_submit(text)`; keys stop driving actions while typing; ASCII only; `racing_online`'s menu has two), focus navigation with keys and pads built in. Immediate HUD: `wpe.draw_text`, `draw_rect`, `world_to_screen` (marks over heads). The editor's 2D view edits UI. |
| audio-design | `wpe.play_sound(path, {volume, pitch, position, bus, duck})`, `audio_source` components (spatial, looping), `wpe.play_music` with crossfades, buses with `wpe.set_volume` (master, effects, music, voice). Dialogue on `wpe.BUS_VOICE` ducks the music automatically (`wpe.set_ducking`, project `duck_*`; `duck = true` for other sounds). Beat sync and resuming: `wpe.music_position`, `music_length`, `seek_music`. |
| save/load scenes, menus | `wpe.load_scene`, a title scene without a camera is a 2D screen (UI only). |
| multiplayer | `net` component + `wpe.net_*`; host-authoritative ownership; read docs/networking.md, start from `racing_online`. |
| performance-optimization | The target is a CPU rasterizer at low resolution: keep triangle counts modest, use frustum culling (on), `max_texture_size`, fewer lights with shadows. Measure first: `wpe_runtime --profile` (or `wpe.show_profiler(true)`) shows the engine's stages and the costliest scripts as bars; wrap suspect code in `wpe.profile_begin(name)` / `wpe.profile_end()` to see it as its own bar; headless, read `wpe.get_profile()` or take a `--screenshot` with `--profile`. The editor's Performance panel and the `stats` command show the same. |
| shader-programming | No user shaders (software renderer). Use materials (glTF PBR, emissive, vertex colors), `texture_filter`, water, UV scrolling, and the post-effect chain. |
| dialogue-systems, visual-novel | The `dialogue` template: conversations as data (`dialogues/*.luau`: nodes with pages, choices, `when`/`unless` flag conditions, `set`/`clear`/`event` effects; format at the top of `scripts/lib/conversation.luau`), story flags (`scripts/lib/story.luau`, optionally saved), a typing dialogue box with voice blips on `wpe.BUS_VOICE` (the music ducks) and choices, and villagers that notice you and show the device's interact prompt. New characters: a `talker.luau` entity with `conversation = "dialogues/<name>"`. |
| card-game | No built-in system: `text`/`button` UI entities plus a script holding the data (Luau tables in a `require`d module). |
| genres | platformer → `platformer` (with a pause menu and controls screen); fps-shooter → `shooter` (gun, hitscan, ammo, targets; `first_person` without the gun); action/adventure → `third_person`; RPG / adventure with NPCs and quests → `dialogue`; racing → `racing` / `racing_online`; stealth / enemy AI → `stealth`. Copy the closest template. |

## Working as an agent

- **The editor is scriptable.** Every UI action is a JSON command, headless or live:
  `wpe_editor my_game --command '{"cmd": "help"}'`, batches with `--run file.jsonl`, a running editor with
  `--send`. Useful: `list_entities`, `get_entity`, `create_entity`, `set_component`, `set_script_property`,
  `set_environment`, `save_scene`, `play` / `frames` / `stop`, `screenshot` (`"target": "editor"` for the
  whole window), `nav_info` / `nav_path`, `check_script`, `run_lua` (edit mode). Ids change after undo,
  stop and loads: list again.
- **Look at the result.** `wpe_runtime --project my_game --headless --frames 120 --fixed-dt 0.0333
  --screenshot out.png` (add `--debug-nav` or `--debug-physics`), then open the PNG.
- **Errors are loud and precise.** Scene loads reject unknown keys with the line; script errors name the
  file and line in the log. Fix what the message says.
- **Unsaved scenes are protected:** pass `"discard": true` to `load_scene` / `new_scene` on purpose.

## Pitfalls

- `wpe` is read-only and the sandbox has no `io`/`os`: files go through `wpe.save_data` and assets.
- Don't move a character entity with `wpe.set_position` every frame: that teleports. Use
  `wpe.move_character`, or give it a `nav_agent`.
- A `nav_agent` with a `character` falls with physics gravity; without one it sticks to the nav grid's
  floor. The grid is built from **static** colliders only; crates and doors are obstacles.
- Agents need a reachable destination: a point on a crate's top is usually its own island (they walk as
  near as they can and report `on_nav_failed`).
- Doorways narrower than about 2 x `agent_radius` + `cell_size` (1.05 m by default) close in the nav grid.
- The fixed step runs at the project's `fixed_rate`; movement belongs in `on_fixed_update`.
