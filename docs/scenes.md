# Scene files

A scene is a JSON document (`.scene`) holding entities, their components and the scene's environment (render settings, post-processing, gravity, music). Scenes are assets: they live in the game's files and are loaded by asset path (`scenes/level1.scene`). The format is written for people and tools alike, including AI agents:

- assets are referenced by **path**;
- angles are in **degrees**;
- entities refer to each other by **file-local ids**;
- everything optional has a default.

Loading is **strict**. Unknown keys, wrong types, out-of-range values, dangling references and cycles are errors. The log names the file and line (`scene 'scenes/a.scene', line 12: collider: unknown key "sise"`). A failed load creates nothing.

```json
{
  "format": "wpe.scene",
  "version": 1,
  "environment": {
    "ambient": [0.2, 0.21, 0.25],
    "fog": {"mode": "linear", "color": [0.36, 0.45, 0.58], "start": 22, "end": 55},
    "postfx": [{"effect": "vignette", "strength": 0.35}],
    "music": {"path": "music/ambient.wav", "fade": 2}
  },
  "camera": 1,
  "entities": [
    {"id": 1, "name": "camera", "transform": {"position": [0, 4, 8], "euler": [-20, 0, 0]}, "camera": {"fov": 65}},
    {"id": 2, "name": "sun", "transform": {"euler": [-52, 35, 0]},
     "light": {"type": "directional", "intensity": 0.85, "cast_shadows": true}},
    {"id": 3, "name": "ground", "mesh": {"model": "meshes/ground.prim"},
     "collider": {"shape": "box", "size": [60, 1, 60], "offset": [0, -0.5, 0]}},
    {"id": 4, "name": "crate", "transform": {"position": [0, 3, 0]}, "mesh": {"model": "meshes/crate.prim"},
     "rigidbody": {"type": "dynamic"}, "collider": {"shape": "box"}},
    {"id": 5, "name": "helmet", "parent": 4, "transform": {"position": [0, 1, 0]},
     "instance": {"model": "meshes/damaged_helmet.glb"},
     "script": {"path": "scripts/spinner.luau", "properties": {"speed": 0.8}}}
  ]
}
```

`assets/scenes/title.scene` (the demo's 2D title menu), `assets/scenes/demo.scene` (the playable demo) and `assets/scenes/test_full.scene` (the master test scene, which uses every component) are complete examples.

## Document

| Key | Description |
|---|---|
| `format` | Always `"wpe.scene"`. Required. |
| `version` | Format version, currently `1`. Required. Files from a newer engine are rejected. |
| `environment` | Scene-wide settings (below). Optional. |
| `camera` | Id of the entity rendered from (it must have a camera). Optional: without it, the first camera found is used. Scripts can change it with `wpe.set_camera`. |
| `entities` | Array of entities (below). |

## Entities

| Key | Description |
|---|---|
| `id` | A whole number from 1 to 2147483647, unique in the file. Required. Other entities refer to this one by it. Ids only matter within the file: loading creates new entities. |
| `name` | Display name (also what `wpe.find` searches). |
| `parent` | Id of the parent entity. Children are attached in file order, which is their order among siblings. Without a parent the entity is a root. |
| `transform` | `position` `[x, y, z]`; the rotation as `rotation` `[x, y, z, w]` (a quaternion, written by tools) or `euler` `[x, y, z]` (degrees, applied X, then Y, then Z; handy by hand); `scale` `[x, y, z]`. All optional (identity). Relative to the parent. |
| `instance` | Authoring shorthand that places a whole model under this entity: `model` (asset path), `animation` (index to attach, optional), `play` (start playing). Creates the model's node entities, meshes, skins and animator. Saving writes those created entities out explicitly. |
| `mesh` | `model` (asset path, or null), `mesh` (index in the model, default 0), `skin` and `joints` (skinned meshes: the skin index and one entity id or null per joint), `visible` (default true), `unlit` (false: the material's own colors without lighting, so the mesh stands out; the sun's shadow still darkens it toward the ambient light unless `receive_shadows` is off), `cast_shadows` (true), `receive_shadows` (true), `double_sided` (false: both faces drawn and lit even when the material is single-sided, which also hides gaps in low-poly models). |
| `camera` | `fov` (vertical, degrees, default 60), `near` (0.1), `far` (500), `orthographic` (false), `ortho_height` (visible height of orthographic cameras, 10). Cameras look down their local −Z axis. |
| `light` | `type` (`"directional"`, `"point"`, `"spot"`), `color` `[r, g, b]`, `intensity`, `range`, `inner_cone` and `outer_cone` (spot half-angles in degrees), `cast_shadows` (the main directional light). Lights shine along local −Z. |
| `billboard` | A 3D sprite: a textured quad at the entity (see [Billboards](#billboards)). `image` (asset path, or null for a plain quad), `size` `[width, height]` (world units, default `[1, 1]`, times the entity's scale), `pivot` `[x, y]` (the point at the entity: `[0.5, 0.5]` centered, `[0.5, 0]` standing on it), `rotation` (degrees, in the quad's plane), `color` `[r, g, b, a]`, `source` `[x, y, width, height]` (a region of the image in texels), `columns`, `rows`, `frame_count` (0 = the whole grid), `frame` (from 0), `fps` (0 = still), `loop` (default true), `mode` (`"fixed"`, `"axis_aligned"`, `"axis_aligned_y"`, `"screen_aligned"`, the default), `blend` (`"cutout"`, the default, `"alpha"`, `"additive"`), `lit` (default false), `visible`. |
| `particles` | A particle emitter (see [Particles](#particles)). |
| `animator` | `model`, `animation` (index), `time`, `speed`, `playing`, `loop`, `nodes` (one entity id or null per model node). Usually created by `instance`. |
| `rigidbody` | `type` (`"static"`, `"kinematic"`, `"dynamic"`), `linear_damping`, `angular_damping`, `gravity_scale`, `lock_rotation`, `continuous`, `initial_velocity` `[x, y, z]`. |
| `collider` | `shape` (`"box"`, `"sphere"`, `"capsule"`, `"convex"`, `"mesh"`), `size` (box: full extents; sphere: `[radius, …]`; capsule: `[radius, height, …]` with height ≥ 2 × radius), `offset`, `density`, `friction`, `restitution`, `trigger`; convex and mesh colliders also take `model` and `mesh` (the mesh's index in the model, from 0); without `model` they use the entity's own mesh (its `mesh` component). Without a rigid body a collider is static level geometry. |
| `character` | A character controller ([scripting.md](scripting.md#character-controllers)): `radius` (0.4), `height` (1.8), `offset` (`[0, 0.9, 0]`, the capsule's center from the entity's origin), `max_slope` (45, degrees), `step_height` (0.3), `snap_distance` (0.3). Scripts move it with `wpe.move_character`. |
| `audio_source` | `sound` (asset path, required), `volume`, `pitch`, `min_distance`, `max_distance`, `bus` (`"effects"` or `"music"`), `loop`, `spatial`, `playing`. |
| `ui` | Makes the entity a UI element (see [UI](#ui) below): `anchor_min`, `anchor_max`, `offset_min`, `offset_max` (each `[x, y]`), `visible` (default true). |
| `sprite` | `image` (asset path of a PNG or JPEG, or null for a plain rectangle), `color` `[r, g, b, a]` (0..1, default white), `source` `[x, y, width, height]` (a region of the image in texels; zero size = all of it), `slice` `[left, top, right, bottom]` (nine-slice borders in texels; 0 = stretch the whole image). |
| `text` | `text` (the string; `\n` breaks lines), `font` (asset path of a font sheet, or null for the project's font, which is the built-in one when the project names none), `color` `[r, g, b, a]`, `scale` (0.1..64, default 1), `align` (`"left"`, `"center"`, `"right"`), `valign` (`"top"`, `"middle"`, `"bottom"`), `wrap` (break lines at spaces to fit the width). |
| `button` | `interactable` (default true), and the tints `normal`, `focused`, `pressed`, `disabled` (`[r, g, b, a]`, 0..2, multiplying everything the button and its descendants draw; defaults 1, 1.25, 0.75 and 0.6 with alpha 0.6). |
| `toggle` | With a `button`: `on` (default false), `mark_color` `[r, g, b, a]` (the mark drawn while on; alpha 0 for none), `mark_inset` (pixels from the edge, default 3). See [Toggles and sliders](#ui). |
| `slider` | With a `button`: `value`, `min` (0), `max` (1), `step` (0 = any value; the value must be in range and on a step), `track_color`, `fill_color`, `handle_color` `[r, g, b, a]`, `handle_width` (pixels, default 6; 0 for none). |
| `script` | `path` (asset path of a Luau script) and `properties` (an object of numbers, booleans, strings and `[x, y, z]` vectors, set on the instance before `on_start`; see [scripting.md](scripting.md#script-properties)). |
| `components` | Game-defined components by name, read and written by the game's `WPE_ComponentSerializer`s. |

An entity may have no components at all (`{"id": 7}`).

## Billboards

A `billboard` is a 3D sprite: an image on a quad that lives in the world like any object (depth tested, fogged), placed by the entity's transform.

- **Turning** (`mode`):
  - `screen_aligned` lies parallel to the screen, turning and rolling with the camera: particles, icons, pick-ups.
  - `axis_aligned_y` stays upright and turns only about the world's Y axis toward the camera: trees, characters, signs that shouldn't tip back when seen from above.
  - `axis_aligned` faces the camera from every side, above included, and stays upright: it tips back to face a camera above it, but never tilts sideways or rolls with the camera (clouds, smoke, distant trees).
  - `fixed` is a double-sided quad in the entity's XY plane, turned only by its transform: a decal, a sign on a wall. It is the only mode that follows the entity's rotation; the others use its position and scale (and `rotation` turns any of them within its plane).
- **Blending** (`blend`): `cutout` draws texels with alpha ≥ 0.5 as solid and skips the rest, so it needs no sorting. `alpha` blends soft edges and is drawn back to front after opaque geometry. `additive` adds light, for glows, fire and sparks (fog fades it out rather than tinting it).
- **Lighting:** unlit by default: the image's own colors. With `lit`, the scene's ambient light, point and spot lights and the sun (with its shadow) light it from the side facing the camera, softly, so a quad turned away from a light is dimmer rather than black.
- **Sprite sheets:** `columns` × `rows` cells of the image (or of `source`), read left to right and top to bottom. `fps` plays them; `loop` false stops on the last frame. Scripts can set `frame` directly.

NOTE(limitation): billboards don't cast shadows yet, and their images are sampled without mipmaps (pixel-art style).

```json
{"id": 9, "name": "tree", "transform": {"position": [-6, 0, -5]},
 "billboard": {"image": "sprites/tree.png", "size": [2, 3], "pivot": [0.5, 0], "mode": "axis_aligned_y", "lit": true}},
{"id": 10, "name": "coin", "transform": {"position": [3, 1.5, -3]},
 "billboard": {"image": "sprites/coin.png", "size": [0.8, 0.8], "columns": 4, "fps": 8, "blend": "alpha"}}
```

## Particles

A `particles` component emits particles from its entity and simulates them: screen-facing quads, many per draw, shaded like billboards (depth tested, fogged, the same blending and optional lighting). `prefabs/fire.prefab`, `smoke.prefab` and `sparks.prefab` are ready-made effects to place and tweak.

| Key | Meaning (default) |
|---|---|
| `image` | Asset path, or null for plain squares. With `columns`, `rows` and `frame_count` (0 = the whole grid) it is a sprite sheet played over each particle's life. |
| `blend`, `lit` | As for billboards (`"alpha"`, false). Alpha-blended particles are sorted back to front within the emitter. |
| `color_start`, `color_end` | RGBA (0..1) from birth to death (white to transparent white). |
| `size` | `[start, end]` in world units over each particle's life (`[0.2, 0.05]`). |
| `playing` | Emitting now (true). Particles already out live on when it stops. |
| `rate` | Particles per second while playing (20). |
| `burst` | Particles emitted at once when a cycle starts (0). |
| `duration`, `loop` | Seconds of emission per cycle (0 = endless); `loop` starts the next cycle (and burst), otherwise the emitter stops (true). |
| `max_particles` | At most this many alive at once, 1..100000 (256). |
| `world_space` | Particles stay where they were emitted (true); false: they move and turn with the emitter. |
| `seed` | Random seed (0 = from the entity): a scene plays the same every time. |
| `shape`, `shape_size` | Where particles appear: `"point"`, `"sphere"` (radius `shape_size[0]`) or `"box"` (full extents), in the emitter's space. |
| `lifetime`, `speed` | `[low, high]` ranges in seconds and world units per second (`[1, 1]`, `[1, 2]`). |
| `direction`, `spread` | Emission direction in the emitter's space (`[0, 1, 0]`) and the half-angle of the cone around it in degrees (15). |
| `gravity`, `drag` | World-space acceleration (`[0, 0, 0]`) and the fraction of velocity lost per second (0). |
| `spin`, `random_rotation` | Spin range in degrees per second (`[0, 0]`) and a random starting angle (false). |

The simulation runs while the game plays (after transforms each frame); large emitters are spread over the worker threads with the same result. In the editor, the selected emitter plays in edit mode so it can be tuned without pressing Play.

```json
{"id": 12, "name": "campfire", "transform": {"position": [0, 0, -4]}, "prefab": {"path": "prefabs/fire.prefab"}},
{"id": 13, "name": "dust", "transform": {"position": [2, 0, 0]},
 "particles": {"rate": 0, "burst": 20, "duration": 1, "loop": false, "speed": [0.5, 1.5], "spread": 80,
               "size": [0.3, 0.8], "color_start": [0.6, 0.5, 0.4, 0.6], "color_end": [0.6, 0.5, 0.4, 0]}}
```

## UI

Menus, HUDs and other 2D interfaces are entities with a `ui` component, drawn over the scene after post-processing. A **2D scene** is simply a scene without a camera: it shows the clear color and its UI. A scene with a camera can have UI too (a HUD, or a menu over a 3D backdrop).

**Layout.** Each corner of an element is placed relative to its parent element's rectangle (or to the screen, the game's framebuffer, when the parent is not a UI element):

- `anchor_min` / `anchor_max` pick a point of the parent for the top-left and bottom-right corners, as fractions: `[0, 0]` is the parent's top-left, `[1, 1]` its bottom-right, `[0.5, 0.5]` its center.
- `offset_min` / `offset_max` move each corner from its anchor, in pixels (y grows downwards).

With both anchors at the same point the element keeps its size (`offset_max - offset_min`); with the anchors apart it stretches with its parent. The defaults (anchors at the parent's corners, no offsets) fill the parent, so `"ui": {}` makes a full-screen background or a label covering its button. Some common layouts:

| Element | `ui` |
|---|---|
| 200 × 40, centered | `{"anchor_min": [0.5, 0.5], "anchor_max": [0.5, 0.5], "offset_min": [-100, -20], "offset_max": [100, 20]}` |
| 64 × 16 in the top-right corner, 4 pixels in | `{"anchor_min": [1, 0], "anchor_max": [1, 0], "offset_min": [-68, 4], "offset_max": [-4, 20]}` |
| A bar across the bottom, 12 pixels tall | `{"anchor_min": [0, 1], "offset_min": [0, -12]}` |
| Its parent, minus a 6-pixel margin | `{"offset_min": [6, 6], "offset_max": [-6, -6]}` |

**Drawing order.** Parents draw before their children, children in hierarchy order, and top-level elements in the order they were created, so later ones are on top. An element's `Transform` is ignored, and children are not clipped to their parent.

**Buttons.** A `button` makes its element focusable and clickable. One button has the focus at a time, and its state tints the button and everything under it:

- **Mouse:** moving over a button focuses it; pressing and releasing the left button on it clicks it.
- **Keyboard and gamepad:** the arrow keys, the D-pad and the left stick move the focus to the nearest button in that direction (holding one repeats). Return, Space or the pad's south button (A / Cross) click the focused button. When nothing has the focus, the first of these inputs focuses the first button.
- **Rebinding:** input actions named `ui_up`, `ui_down`, `ui_left`, `ui_right` or `ui_submit` in `project.json` replace that input's defaults.

A click calls `on_click` on the button entity's script; scripts can also move the focus (`wpe.ui_focus`), see [scripting.md](scripting.md#ui). The UI reacts only while the game runs (not in the editor's edit mode).

**Toggles and sliders.** Add a `toggle` or a `slider` to a button for an on/off switch or a value. Each change calls `on_change(value)` on the entity's script.

- **Toggle:** every click flips it. While on, it draws a mark in its rectangle (a check box over the button's sprite).
- **Slider:** draws a track across the middle of its rectangle, filled up to the value, with a handle there. Pressing on it jumps the value there, and dragging follows the mouse until the button is released. While it has the focus, left and right step it (by `step`, or by 1/20 of the range without one), and up and down still move the focus.

```json
{"id": 7, "name": "volume", "parent": 3, "ui": {"anchor_max": [1, 0], "offset_min": [58, 100], "offset_max": [-12, 110]},
 "button": {}, "slider": {"value": 0.7, "step": 0.05}, "script": {"path": "scripts/volume_slider.luau"}}
```

```json
{"id": 1, "name": "menu", "ui": {"anchor_min": [0.5, 0.5], "anchor_max": [0.5, 0.5], "offset_min": [-70, -30], "offset_max": [70, 30]},
 "sprite": {"image": "ui/panel.png", "color": [0.17, 0.19, 0.27, 1], "slice": [4, 4, 4, 4]}},
{"id": 2, "name": "play", "parent": 1, "ui": {"anchor_max": [1, 0], "offset_min": [10, 10], "offset_max": [-10, 32]},
 "sprite": {"image": "ui/panel.png", "color": [0.93, 0.6, 0.22, 1], "slice": [4, 4, 4, 4]},
 "button": {}, "script": {"path": "scripts/menu_button.luau", "properties": {"scene": "scenes/demo.scene"}}},
{"id": 3, "parent": 2, "ui": {}, "text": {"text": "Play", "align": "center", "valign": "middle"}}
```

## Prefabs

A prefab is a reusable entity tree: an enemy with its collider, script and lights, a lamp post, a crate. It is stored in a `.prefab` file and placed in scenes as **linked instances**. Edit the prefab, and every instance changes too, except where an instance overrides it.

A prefab file holds entities in the scene schema above, exactly one of them without a parent (the root):

```json
{
  "format": "wpe.prefab",
  "version": 1,
  "entities": [
    {"id": 1, "name": "lamp", "transform": {}, "light": {"type": "point", "intensity": 2}},
    {"id": 2, "name": "bulb", "parent": 1, "transform": {"position": [0, 1, 0]}, "mesh": {"model": "meshes/bulb.prim"}}
  ]
}
```

A scene places an instance with a `prefab` entry:

```json
{"id": 4, "name": "porch lamp", "transform": {"position": [2, 0, 0]},
 "prefab": {"path": "prefabs/lamp.prefab", "ids": {"2": 5},
            "overrides": {"1": {"light": {"type": "spot"}}, "2": {"mesh": null}}}}
```

| Key | Description |
|---|---|
| `id`, `name`, `parent`, `transform` | The instance's own. They replace the prefab root's name and transform. An instance takes no other components: they come from the prefab, changed by `overrides`. |
| `prefab.path` | The prefab file. Required. |
| `prefab.ids` | Scene ids for the prefab's other entities, keyed by their ids in the prefab file. With them, other scene entities can refer to them, for example to be attached to the bulb (`"parent": 5`) or to use a camera inside the prefab. Optional; tools write it. |
| `prefab.overrides` | How this instance differs from the prefab, keyed by prefab entity id. Each entry maps a component key (or `"name"`) to its value for this instance, or to `null` to remove it. An entity mapped to `null` is removed from this instance. Values use the scene schema, and their entity references are prefab ids. |

When a scene is saved, each instance is compared with its prefab as the prefab instantiates it. Only the differences are written, so changes made by the editor, by a script or by code all count, and nothing is duplicated. Entities attached beneath an instance are saved as ordinary scene entities.

Rules and limits:
- An instance keeps its prefab's structure. Its entities can be removed (a `null` override), and others attached beneath them, but they can't be moved. To restructure an instance, unpack it.
- A prefab file can't contain prefab instances (no nesting yet).
- Override values are whole components: a changed light writes the whole light.

From C (`engine/prefab.h`):
- `wpe_prefab_instantiate` creates an instance.
- `wpe_prefab_save_file` saves an entity tree as a prefab file, or writes an instance back into its prefab ("apply").
- `wpe_prefab_revert`, `wpe_prefab_unpack` and `wpe_prefab_is_overridden` revert, unpack and inspect instances.

Scene contexts that load instances need `WPE_COMP_PREFAB` registered (`wpe_prefab_register`), and should pass a `WPE_PrefabCache`, which keeps parsed prefab files until they change on disk. The engine does both.

## Environment

| Key | Description |
|---|---|
| `clear_color` | Background `[r, g, b]` (0..1) where nothing is drawn. |
| `ambient` | Ambient light `[r, g, b]`. |
| `fog` | `mode` (`"none"`, `"linear"`, `"exponential"`), `color`, `start` and `end` (linear), `density` (exponential). |
| `sky` | What shows where no geometry is. `mode`: `"color"` (the `clear_color`; the default), `"gradient"` (`zenith`, `horizon` and `ground` colors by view direction; `curve`, default 0.5, is how fast the zenith and ground colors take over: 1 linear, lower keeps the horizon color thinner), or `"panorama"` (`image`: an equirectangular image, its middle straight ahead along −Z, turned by `rotation` degrees). Both draw a disc where the scene's sun is: `sun_size` (its radius in degrees, 0 for none), `sun_color`, `sun_glow` (0..1). With fog, give the horizon the fog's color so distant geometry fades into the sky. Panorama images are scaled to the project's `max_texture_size` like other images. |
| `shadows` | `enabled`, `map_size` (16..4096), `distance` (shadows cover the view up to this distance), `caster_extension`, `bias`. |
| `dither_transparency` | Render alpha-blended materials with ordered dithering (order-independent). |
| `frustum_culling` | Skip objects outside the view (default true). |
| `subdivision` | Adaptive subdivision of near triangles: `levels` (0..4, 0 = off) and `edge_pixels`. |
| `default_sun` | `enabled`, `direction`, `color`: a light used when the scene has no light entities at all. |
| `gravity` | Physics gravity `[x, y, z]` in m/s² (default `[0, -9.81, 0]`). |
| `music` | `path` of a music track started when the scene loads (looping) and the crossfade `fade` in seconds. The same track keeps playing across scenes. Without `music`, the previous scene's music fades out. |
| `postfx` | Up to 8 post-processing effects, applied in order. Each object has `effect` and `enabled` (default true), plus the effect's parameters: `ssao` (`radius_pixels`, `strength`, `depth_range`, `bias`), `chromatic_aberration` (`strength_pixels`), `ps1` (`levels`, `dither_strength`), `vignette` (`strength`, `radius`), `color_adjust` (`brightness`, `contrast`, `saturation`), `grayscale` (`strength` 0..1: how much of the image turns gray; `tint_r`, `tint_g`, `tint_b` 0..2 multiply the gray, e.g. 1, 0.9, 0.7 for sepia), `depth_of_field` (`focus_distance`: the sharpest distance from the camera; `focus_range`: how far either side of it stays sharp; `falloff`: over how much further the blur grows to full; `radius_pixels`: the full blur, up to 16). Depth of field keeps sharp edges in front of a blurred background, while a blurred foreground softens over what's behind it. Four effects are mostly for scripts to animate ([scripting.md](scripting.md#post-processing)): `shockwave` (`center_x`, `center_y`: fractions of the screen; `radius`, `width`: fractions of its height; `strength_pixels`), `radial_blur` (`center_x`, `center_y`, `strength` 0..1), `cracks` (straight cuts across the screen: `count` 0..32, fractional while the newest grows; `seed`; `center_x`, `center_y` where they cross; `spread`; `slide_pixels` how far each side of every cut slides along it, opposite ways (depth); like broken glass, only pixels near a cut change otherwise: `offset_pixels` how far the image is pulled across a cut (a seam), `bevel_pixels` how far from it the image is sheared along it (streaks), `glow_pixels` the width of a rim on one side, tinted by `glow_r`/`g`/`b` (darkening bright areas toward the tint, lighting dark ones with it); `width_pixels` an optional line along the cut in `core_r`/`g`/`b`; `strength` 0..1) and `shatter` (the screen breaking along the same lines as a cracks effect with the same `count` (up to 16), `seed`, `center_x`, `center_y` and `spread`; `time` since the break; `speed`, `gravity` in screen heights per second; `spin`; `background` 0..1 darkens what's behind the pieces; `capture` 1..8 shows a frame from `wpe.capture_screen`, 0 the live one; `fade` seconds over which the pieces fade into the scene behind, 0 for never). |

## Saving

`wpe_scene_save()` (or `wpe_engine_save_scene()`) writes every entity of the world. Roots come in creation order, each followed by its descendants in child order. Every value is written, defaults included, so a saved file documents itself.

Loading a saved file and saving it again produces the same text, which keeps diffs clean. Saved state is each component's authored state: physics bodies, playing voices and similar runtime internals are recreated on load. For scripts, the path and the current values of their properties are saved; other fields of `self` are not.

## From C

```c
const WPE_SceneContext context = {.ecs = ecs, .assets = assets, .scripts = scripts};
WPE_SceneEnvironment environment;
WPE_Entity camera;
wpe_scene_load_asset(&context, "scenes/level1.scene", &environment, &camera); // adds to the world
wpe_scene_save_file(&context, &environment, camera, "out/level1.scene");     // OS path
wpe_scene_clear(ecs);                                                       // despawns everything
```

Game components join through `WPE_ComponentSerializer` (a name, a save and a load function). Entity references inside them map through `wpe_scene_ids_to_file()` and `wpe_scene_ids_to_entity()`, so they survive saving and loading.
