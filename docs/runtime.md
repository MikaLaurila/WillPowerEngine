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
```

`--init` creates the project in a new or empty directory (it never overwrites files):

- `project.json`: the default settings, `move_x` / `move_y` / `jump` input actions (WASD, arrows, left stick; Space, A), starting in `scenes/main.scene`;
- `scenes/main.scene`: a camera, a sun with shadows, a ground with a collider, and a cube;
- `meshes/ground.prim`, `meshes/cube.prim`, and `scripts/spin.luau`, which spins the cube.

Text uses the engine's built-in font until the project names its own. It works with the editor's other options, for example `--init --command '...'` for an agent setting up a project headless. From C: `wpe_project_create()`.

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

On Windows the runtime is a windowed (GUI) program: an exported game started by double-click opens no console. Started from a terminal it still logs there. A startup error shows a message box unless `--headless` is given. PowerShell does not wait for GUI programs unless their output is piped, so in scripts pipe it (`wpe_runtime ... | Out-Host`) or use `Start-Process -Wait`. The editor (`wpe_editor`) is a console program and needs neither.

The runtime is a thin shell around `WPE_Engine` (`src/engine/engine.h`), which the editor will reuse. Each frame the engine does the following:

1. Reads input.
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
  "display": {"width": 320, "height": 240, "scale": 3, "vsync": true, "resizable": true, "fullscreen": false},
  "simulation": {"fixed_rate": 60, "max_steps": 8},
  "assets": {"max_texture_size": 256},
  "audio": {"master": 1, "effects": 1, "music": 0.7},
  "scripting": {"memory_limit_mb": 64, "time_limit_ms": 250},
  "prefabs": {"crate": "meshes/crate.prim", "figure": "meshes/RiggedFigure.glb"},
  "input": {"keyboard_player": 1, "stick_deadzone": 0.2, "actions": {"jump": [{"key": "space"}, {"pad_button": "south"}]}}
}
```

| Key | Description |
|---|---|
| `name` | Window title. |
| `start_scene` | Scene loaded at startup (asset path; empty starts with an empty world). |
| `font` | Bitmap font sheet for text: UI texts without a font of their own and `wpe.draw_text` (optional). Without it, or when the file is missing (a warning), the engine uses its built-in font, `assets/fonts/small.png` compiled in. |
| `display` | `width` × `height`: the framebuffer resolution (16..4096). `scale`: the initial window size multiplier. Also `vsync`, `resizable`, `fullscreen`. The image is scaled with nearest filtering and letterboxed. |
| `simulation` | `fixed_rate`: fixed steps per second. `max_steps`: fixed steps per frame before time is dropped, which avoids a spiral of death. |
| `assets` | `max_texture_size` caps model textures (0 = original size). |
| `audio` | Bus volumes. |
| `scripting` | Luau heap limit and per-callback time budget. |
| `prefabs` | Names for `wpe.instantiate(name)`, each mapped to a prefab file (`.prefab`) or a model asset path. |
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
