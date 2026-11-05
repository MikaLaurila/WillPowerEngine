# Projects, the runtime and export

A game is a directory of assets with a `project.json` at its root. During development the runtime plays that directory directly. For release, `wpe_export` packs it into a single executable. This repository's `assets/` directory is such a project: the demo game.

```
assets/
├── project.json      # name, display, start scene, input actions, prefabs
├── scenes/           # title.scene (the 2D start menu), demo.scene, test_full.scene
├── scripts/          # Luau behavior
├── meshes/           # glTF models and .prim primitives
├── sounds/  music/  fonts/  ui/
```

## Starting a new game

```bash
./build/bin/wpe_editor my_game --init                     # create my_game/ and open it in the editor
./build/bin/wpe_editor my_game --init --name "My Game"    # ... with a window title other than the folder's
./build/bin/wpe_editor my_fps --init --template first_person   # start from a game template
```

`--init` creates the project in a new or empty directory (it never overwrites files) from a template. Each template is a complete game to build on:

| Template | What you get |
|---|---|
| `empty` (the default) | A camera, a sun with shadows, a ground with a collider and a cube spun by `scripts/spin.luau`; `move_x` / `move_y` / `jump` input actions (WASD, arrows, left stick; Space, A). |
| `first_person` | `scripts/player.luau` on a character controller with the camera at eye height (the child `head`): mouse look (click to capture, Escape to release) and right-stick look, walking (WASD, left stick), sprinting (Shift, left trigger), jumping (Space, A), a crosshair, and "use" (E, left click, right trigger) that pushes the crate in sight. The level has stairs, a ramp, a jumping route onto a platform and crates to push. Speeds, jump height, gravity, look sensitivity and reach are the script's properties, in the editor's inspector. |
| `shooter` | A first-person shooter to build on: the first-person player plus a gun held in front of the camera (`scripts/gun.luau`, a child of the head: the model is its child boxes, so drop in your own and keep the `muzzle` child at the barrel's tip). Shots are rays from the screen's center (left mouse, right trigger; `automatic` to hold): sparks fly where they hit, crates get pushed, and scripts that have `on_shot(damage, point, direction)` take damage. A muzzle flash with a light, recoil that kicks the view, bob while walking, a magazine with reloading (R, X; empty reloads by itself), a crosshair with a hit marker, and the gun pulls back near walls so it never pokes through them. Three training dummies (`scripts/dummy.luau`) wander with the navigation system, topple when shot and get back up. Sounds are in `sounds/` to replace. |
| `third_person` | An animated figure (`meshes/RiggedFigure.glb`) on a character controller, seen by `scripts/orbit_camera.luau`: mouse look (click to capture, Escape to release), right-stick look, mouse-wheel zoom, and a camera that moves in front of walls rather than through them. `scripts/player.luau` walks relative to the camera (WASD, left stick), sprints (Shift, left trigger), jumps (Space, A) and turns to face its way. Its animations are script properties chosen by name (`idle_animation`, `move_animation`, `jump_animation`; empty: the first clip, held still when idle), so your own character drops in: replace the `figure` model and type its clip names in the inspector. The level adds an archway to the first-person one, and a villager that wanders around on its own (`scripts/wanderer.luau`: a nav agent walking to random reachable spots, [navigation.md](navigation.md)). |
| `platformer` | A 2.5D side-scroller: `scripts/player.luau` runs along X on a character controller (A/D, arrows, left stick, d-pad) and jumps (Space, W, Up, A) with a platformer's feel: hold jump for higher jumps, a jump pressed just before landing or just after leaving a ledge still counts, and the hero squashes and stretches. Its movement is a state machine (`scripts/lib/states.luau`): `idle` and `run` share a `grounded` state, `jump` and `fall` an `airborne` one, plus `respawn` and `finished`, each with its own rules; a new move (a dash, a wall slide) is one more state, and `show_state` in the inspector shows the current one over the hero. `scripts/side_camera.luau` follows, looking ahead. The level has coins (`prefabs/coin.prefab`, a prefab instanced fifteen times: spinning triggers that burst into sparkles) with a counter, spikes (`scripts/hazard.luau`), a moving platform the hero rides (`scripts/moving_platform.luau`), a checkpoint, and a goal that finishes the level with the time; R restarts. The scripts talk through messages (`wpe.send(player, "collect_coin")`, `"hurt"`, `"set_checkpoint"`, `"finish"`), so new pickups and hazards are a few lines each. |
| `racing` | An arcade racer on an eight-sided circuit with walls. `scripts/car.luau` drives a sphere rigid body (walls and bumps are the physics'), setting its velocity from the controls: throttle (W, Up, right trigger), brake and reverse (S, Down, left trigger), steering (A/D, arrows, left stick) and a handbrake for drifting (Space, B); grip, speeds and turning are its properties. `scripts/chase_camera.luau` follows behind and pulls back at speed. `scripts/race.luau` runs a 3-2-1 countdown, three laps counted through the checkpoints in order (`scripts/checkpoint.luau`, so cutting across doesn't count), lap times, and a best lap kept with `wpe.save_data`. The track is generated, but it's ordinary scene entities: move the walls in the editor to change it. |
| `stealth` | A stealth game: get past three guards to the green exit. The third-person player (`scripts/player.luau`) makes noise when sprinting or landing. Each guard (`scripts/guard.luau`) walks with the navigation system: it patrols a route (an entity whose children are the points), sees in a cone (`view_distance`, `view_angle`, a line-of-sight ray), grows suspicious (a `?` with a bar over its head) and then chases (`!`), calling nearby guards; out of sight it searches around where it saw you, then goes back to its route. `scripts/game.luau` shows the goal and how alert the guards are, and starts the level over when you are caught or escape. Senses and speeds are the guard script's properties; routes and cover are ordinary entities to move in the editor. [navigation.md](navigation.md#a-complete-game-the-stealth-template) walks through it. |
| `dialogue` | A village square with people to talk to, on the third-person player and camera. Walk up to someone and they turn to you and show what to press ("E Talk to Maren", or the pad's button); the dialogue box types their lines out with voice blips (the music ducks under them, see [scripting.md](scripting.md#audio)) and offers choices (Up/Down or the mouse, Return, Space or E). Conversations are data: `dialogues/*.luau` tables of nodes with pages of text, choices, conditions on story flags (`when`, `unless`) and effects (`set`, `clear`, and `event`, a method every script hears), run by `scripts/lib/conversation.luau` (the format is described at its top) with the flags in `scripts/lib/story.luau`. A small quest ties it together: Maren lost a ring, Pip has it, and Tobin's lines follow the story; `scripts/quest.luau` shows the goal and hears the events. The villagers (`scripts/talker.luau`) run on a reusable state machine, `scripts/lib/states.luau` (states as tables of `enter`/`update`/`exit` and event functions; shared states, push/pop), which suits players and enemies just as well. |
| `racing_online` | The racer for one to four players over the network. A menu (`scripts/menu.luau`) to practice alone, host a game, or join one on this computer, at a typed address or found on the local network; a lobby to warm up in until the host starts the race (Return); four networked cars, one per player, handed out by the host (`scripts/race.luau`), each driven only on its owner's machine (`scripts/car.luau`); laps counted by each racer's own machine, positions and results ranked by the host, names over the other cars. `wpe_runtime --host` and `--connect` skip the menu. [networking.md](networking.md#a-complete-game-racing-online) walks through how it works. |

Running `wpe_editor` without arguments lists the templates. They live in the engine's source tree under `templates/<name>/`, as ordinary projects (`wpe_runtime --project templates/first_person` plays one), and are compiled into the engine, so a new file there is in the next build. Text uses the engine's built-in font until the project names its own. `--init` works with the editor's other options, for example `--init --command '...'` for an agent setting up a project headless. From C: `wpe_project_create(dir, name, template)`.

### Claude in a game project

A new project also gets the `willpowerengine` Claude skill, in `.claude/skills/willpowerengine/`: the skill's guide (`SKILL.md`: how games are made with the engine, and how the generic game-development skills map onto its API) and, in `references/`, the documentation it points to (scripting, scenes, runtime, editor, navigation, networking, assets). So Claude can help with the game from the game's own folder, outside the engine's repository, and it works from the documentation of the engine the project was made with.

The copy belongs to an engine build: its front matter records the engine version and a `skill_id`, a hash of the skill and its documents. After updating the engine, refresh it:

```bash
./build/bin/wpe_editor my_game --update-skill     # installs or replaces the project's copy, then exits
```

The editor's File > Update Claude skill and the `update_skill` command do the same, and the editor warns when it opens a project whose copy is from another build (and mentions it when a project has none, like one made before projects had it). Refreshing replaces the skill's files and removes ones the new copy no longer has; it touches nothing else, so don't keep your own notes in that folder. From C: `wpe_project_install_skill(dir)`, `wpe_project_skill_status(dir)`.

## Running a game

```bash
./build/bin/wpe_runtime --project assets                 # play a project directory
./build/bin/wpe_runtime --project assets --watch         # ... hot-reloading changed scripts
./build/bin/wpe_runtime --pak game.pak                   # play an archive (wpe_pak create)
./build/bin/wpe_runtime --project assets --scene scenes/test_full.scene --headless --frames 300 \
    --fixed-dt 0.0333 --screenshot out.png                # automated run: no window, no audio device
```

| Option | Effect |
|---|---|
| `--project <dir>` / `--pak <archive>` | The game to play. Without either, the runtime plays the game embedded in its own executable (an exported game). |
| `--scene <path>` | Start with this scene instead of the project's `start_scene`. |
| `--watch` | Reload changed scripts twice a second (project directories). |
| `--mute` | Silence all audio. |
| `--headless` | No window and no audio device. Rendering still runs, so `--screenshot` works. |
| `--frames <n>` / `--fixed-dt <s>` | Stop after n frames / simulate a fixed frame time (reproducible runs). |
| `--screenshot <png>` | Save the last frame. |
| `--workers <n>` | Render threads (default: one per core). |
| `--save-dir <dir>` | Keep saved data (`wpe.save_data`) in this directory (made if missing) instead of the player's own (tests, portable installs). |
| `--max-fps <n>` | Sleep so frames don't come faster than this (headless hosts default to 60). |
| `--debug-physics` | Draw every collider as a wireframe, colored by body type. |
| `--debug-nav` | Draw the navigation grid: where a character fits and can walk, built from the static colliders with the scene's `navigation` settings ([scenes.md](scenes.md#environment)). The outline marks walls, drops and ledges; the lines across it are about a meter apart; each color is one connected area (an island). |
| `--profile` | Show the profiler in the top-right corner: the engine's stages (input, physics, scripts, render, post effects, ...) and the costliest scripts and script sections, as bars of their average over the last half second with a tick at the recent peak. See [scripting.md](scripting.md#profiling). |
| `--host [port]`, `--connect <address>`, `--name <player>`, `--local`, `--net-sim <settings>`, `--net-stats` | Multiplayer: host or join a game, with a simulated bad network and a stats overlay if wanted. See [Trying it out](networking.md#trying-it-out). A `--connect` run that never gets into the game exits with an error. |

On Windows the runtime is a windowed (GUI) program: an exported game started by double-click opens no console. Started from a terminal it still logs there. A startup error shows a message box unless `--headless` is given. PowerShell does not wait for GUI programs unless their output is piped, so in scripts pipe it (`wpe_runtime ... | Out-Host`) or use `Start-Process -Wait`. The editor (`wpe_editor`) is a console program and needs neither.

Saved data (`wpe.save_data`) goes to the player's own directory for the game, `%APPDATA%\WillPowerEngine\<project name>\` on Windows or `~/.local/share/WillPowerEngine/<project name>/` on Linux, named after the letters, digits and spaces of the project's `name`. The directory is made the first time a script saves or loads, and the editor's play mode uses the same one. Hosts can choose another with `WPE_EngineDesc.save_dir`.

The runtime is a thin shell around `WPE_Engine` (`src/engine/engine.h`), which the editor will reuse. Each frame the engine does the following:

1. Reads input, and takes multiplayer events when a game is hosted or joined.
2. Runs fixed steps (scripts' `on_fixed_update`, physics, collision callbacks), then `on_update`, animation, transforms and audio.
3. Renders from the scene camera, applies the scene's post-processing, then draws the scripts' 2D drawing on top.
4. Performs scene switches requested with `wpe.load_scene`.

## The project file

```json
{
  "format": "wpe.project",
  "version": 1,
  "name": "WillPowerEngine Demo",
  "start_scene": "scenes/title.scene",
  "font": "fonts/small.png",
  "display": {"width": 320, "height": 240, "scale": 3, "vsync": true, "resizable": true, "fullscreen": false, "scaling": "integer", "alt_enter": true},
  "simulation": {"fixed_rate": 60, "max_steps": 8},
  "assets": {"max_texture_size": 256},
  "audio": {"master": 1, "effects": 1, "music": 0.7},
  "scripting": {"memory_limit_mb": 64, "time_limit_ms": 250},
  "prefabs": {"crate": "meshes/crate.prim", "figure": "meshes/RiggedFigure.glb"},
  "network": {"port": 47810, "discovery_port": 47809, "max_players": 4, "timeout_ms": 5000, "tick_rate": 30, "interpolation_ms": 100},
  "input": {"keyboard_player": 1, "stick_deadzone": 0.2, "actions": {"jump": [{"key": "space"}, {"pad_button": "south"}]}}
}
```

| Key | Description |
|---|---|
| `name` | Window title. |
| `start_scene` | Scene loaded at startup (asset path; empty starts with an empty world). |
| `font` | Bitmap font sheet for text: UI texts without a font of their own and `wpe.draw_text` (optional). Without it, or when the file is missing (a warning), the engine uses its built-in font, `assets/fonts/small.png` compiled in. |
| `display` | `width` × `height`: the framebuffer resolution (16..4096). `scale`: the initial window size multiplier. Also `vsync`, `resizable`, and `fullscreen` (borderless, on the desktop's own mode: no mode switch, instant alt-tab). `scaling` says how the image (nearest-filtered) fills the window or screen: `"integer"` (the default) by whole multiples only (2×, 3×, …), keeping the aspect ratio, with a border around the rest, so every game pixel is the same size on screen; `"fit"` as large as fits, keeping the aspect ratio (on a wider screen it fills the height, with bars at the sides), but at a fractional scale (a maximized or dragged window) some pixels are a screen pixel wider than others, which shows on thin diagonals and text; `"stretch"` fills the whole window, distorting the image. With `"integer"`, a window smaller than the resolution shrinks the image like `"fit"`. The editor's viewport scales by whole multiples too for `"integer"`, and fits otherwise. `alt_enter` (default true): Alt+Enter switches fullscreen. These are a new player's defaults: an options screen changes them for the player (see [Display options](#display-options)). |
| `simulation` | `fixed_rate`: fixed steps per second. `max_steps`: fixed steps per frame before time is dropped, which avoids a spiral of death. |
| `assets` | `max_texture_size` caps model textures (0 = original size). Larger ones are halved until they fit, averaging the texels, except pixel art: textures whose glTF sampler minifies with `NEAREST` (e.g. `NEAREST_MIPMAP_NEAREST`) take the texel in the middle of each block instead (for their mip levels too), so they keep their painted colors and hard edges. For textures that should be crisp up close but calm far away (water, grass), use `NEAREST` magnification with `LINEAR_MIPMAP_LINEAR` minification: their mips are averaged, so distant surfaces don't sparkle. |
| `audio` | Bus volumes: `master`, `effects`, `music` and `voice` (dialogue). Ducking, how the music makes room for voice lines and other ducking sounds: `duck_volume` (its volume while ducked, 0..1, default 0.35), `duck_attack` (seconds to go down, 0.15) and `duck_release` (seconds to come back after the last one ends, 0.6). Scripts can change them (`wpe.set_ducking`). |
| `scripting` | Luau heap limit and per-callback time budget. |
| `prefabs` | Names for `wpe.instantiate(name)`, each mapped to a prefab file (`.prefab`) or a model asset path. |
| `network` | Multiplayer (see [networking.md](networking.md#playing-together)): the UDP `port` a host listens on, the `discovery_port` of LAN discovery (the two must differ), `max_players` (2..8, the host included), `timeout_ms`, after which a silent player is dropped (1000..60000), `tick_rate`, how many times a second networked entities' states are sent (1..120, default 30), and `interpolation_ms`, how far in the past remote entities are shown (0..1000, default 100). |
| `input` | Dead zones, the press threshold, the keyboard's player and the actions (below). |

Like scene files, the project file is read strictly: unknown keys and bad values are errors that report the line.

## Input actions

Scripts should ask for actions ("jump", "move_x"), not keys or buttons. Each action lists up to 8 bindings across devices:

```json
"actions": {
  "move_x": [{"key": "d"}, {"key": "a", "scale": -1}, {"pad_axis": "left_x"}],
  "move_y": [{"key": "w"}, {"key": "s", "scale": -1}, {"pad_axis": "left_y", "scale": -1}],
  "jump":   [{"key": "space"}, {"pad_button": "south"}],
  "fire":   [{"mouse_button": "left"}, {"pad_axis": "right_trigger"}],
  "menu_up": [{"pad_axis": "left_y", "range": "negative"}, {"key": "up"}]
}
```

- **A binding** names one source: `key`, `mouse_button`, `pad_button` or `pad_axis` (the names are listed in [scripting.md](scripting.md#input)).
- **`scale`** multiplies the binding's value (−1 inverts it).
- **`range`** (`"positive"` or `"negative"`) makes an axis binding read only one direction, e.g. pushing a stick up as a button.
- **The value** of an action is the sum of its bindings, clamped to [−1, 1]. A held key or button contributes its scale; an axis contributes its dead-zoned position. Opposite keys therefore cancel.
- **Down** means the absolute value reaches `press_threshold` (default 0.5).

| Input setting | Description |
|---|---|
| `keyboard_player` | Which player (1–4) the keyboard and mouse belong to (default 1). |
| `stick_deadzone` / `stick_outer` | Radial stick dead zone (0.2) and the deflection treated as full (0.95). Values are rescaled in between, so motion starts smoothly. |
| `trigger_deadzone` | Trigger dead zone (0.1). |
| `press_threshold` | When an analog action counts as down (0.5). |

**Gamepads** use the standard layout. They are hot-pluggable:

- A connecting pad takes the lowest free player slot, and its player LED lights up where the pad has one.
- A disconnected pad frees its slot, so reconnecting usually gives the same slot back.
- Up to 4 pads are used.

Stick Y axes are positive downwards, as on the hardware; bind them with `"scale": -1` for "up is positive". Rumble is available with `wpe.rumble`.

**The player's own bindings.** The project's bindings are the defaults. A controls screen changes them with `wpe.set_bindings` ([scripting.md](scripting.md#rebinding-and-prompts)), and each change is saved at once to `wpe.bindings.json` in the save directory, next to `wpe.save_data`'s files:

```json
{"format": "wpe.bindings", "version": 1, "actions": {"jump": [{"key": "k"}, {"pad_button": "south"}]}}
```

Only actions that differ from the project's are listed. The runtime reads the file at startup, and the editor each time play starts (stopping puts the project's bindings back). Reading never creates the save directory. An action the project no longer has is skipped with a warning; a damaged file is logged and the project's bindings apply. Delete the file to give a player the defaults again, or call `wpe.reset_bindings()`.

**Prompts.** `wpe.action_prompt("jump")` gives the label to show for an action on the device the player used last ("Space" after typing, "A" or "Cross" after touching the pad), so hints follow the player when they switch.

## Display options

The project's `display` is what a new player starts with. An options screen changes it with `wpe.set_display` ([scripting.md](scripting.md#display)), which applies the change at once and saves it to `wpe.display.json` in the save directory, next to the bindings:

```json
{"format": "wpe.display", "version": 1, "fullscreen": true, "scaling": "integer", "scale": 3, "vsync": true}
```

The runtime reads the file *before* it opens the window, so a player who chose fullscreen never sees a window first. A damaged file is logged and the project's display applies. Delete the file to give a player the defaults again.

- **Fullscreen** is borderless, on the desktop's own resolution (the monitor never switches modes), so switching is instant and alt-tab is safe.
- **Alt+Enter** switches between fullscreen and windowed (turn it off with `"alt_enter": false` in `display`). The game never sees that key press, so it doesn't click a focused menu button. The switch is saved like a `wpe.set_display` call, and `wpe.get_display().fullscreen` follows it, so an options screen that reads it when it opens shows the right value.
- **Window size**: `scale` is the window's size in multiples of the framebuffer (2 means 640×480 for a 320×240 game), centered on the desktop. `wpe.get_display().max_scale` is the largest one that fits the desktop (with the window's frame), for a list of choices. A scale changed while fullscreen becomes the window's size when it is windowed again.
- **Scaling** is `"integer"` (pixel perfect), `"fit"` or `"stretch"`, as in `display`.
- The editor has no game window, so in play mode these settings are kept and saved but not applied (`wpe.get_display().window` is false). Headless runs are the same.

## Exporting a game

```bash
./build/bin/wpe_export assets dist/demo_game.exe          # Linux: dist/demo_game
```

Export validates the project first: `project.json` must parse, and the start scene and font must exist. It then packs every file of the project directory into a `.pak` archive (zstd-compressed where it pays off) and writes a copy of `wpe_runtime` with the archive appended. The result is one self-contained executable:

- It needs no other files: no DLLs or shared libraries beyond the operating system's own, since the runtime links its compiler runtimes statically.
- Started without arguments, it finds the archive inside itself and plays the game.

| Option | Effect |
|---|---|
| `--runtime <path>` | The runtime to package (default: `wpe_runtime` next to `wpe_export`). |
| `--level <1-19>` | zstd compression level. |

The exported executable runs on the platform the runtime was built for, so export on Windows for Windows and on Linux for Linux. The output must lie outside the project directory, which would otherwise pack itself. The exported game still accepts the runtime's options, which is how the export test plays one headless.

From C, `wpe_export_game()` (`engine/export.h`) does the same. The archive layout is described in [assets.md](assets.md#embedded-archives).
