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

`assets/scenes/demo.scene` (the playable demo) and `assets/scenes/test_full.scene` (the master test scene, which uses every component) are complete examples.

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
| `mesh` | `model` (asset path, or null), `mesh` (index in the model, default 0), `skin` and `joints` (skinned meshes: the skin index and one entity id or null per joint), `visible` (default true). |
| `camera` | `fov` (vertical, degrees, default 60), `near` (0.1), `far` (500), `orthographic` (false), `ortho_height` (visible height of orthographic cameras, 10). Cameras look down their local −Z axis. |
| `light` | `type` (`"directional"`, `"point"`, `"spot"`), `color` `[r, g, b]`, `intensity`, `range`, `inner_cone` and `outer_cone` (spot half-angles in degrees), `cast_shadows` (the main directional light). Lights shine along local −Z. |
| `animator` | `model`, `animation` (index), `time`, `speed`, `playing`, `loop`, `nodes` (one entity id or null per model node). Usually created by `instance`. |
| `rigidbody` | `type` (`"static"`, `"kinematic"`, `"dynamic"`), `linear_damping`, `angular_damping`, `gravity_scale`, `lock_rotation`, `continuous`, `initial_velocity` `[x, y, z]`. |
| `collider` | `shape` (`"box"`, `"sphere"`, `"capsule"`, `"convex"`, `"mesh"`), `size` (box: full extents; sphere: `[radius, …]`; capsule: `[radius, height, …]` with height ≥ 2 × radius), `offset`, `density`, `friction`, `restitution`, `trigger`; convex and mesh colliders also take `model` and `mesh`. Without a rigid body a collider is static level geometry. |
| `audio_source` | `sound` (asset path, required), `volume`, `pitch`, `min_distance`, `max_distance`, `bus` (`"effects"` or `"music"`), `loop`, `spatial`, `playing`. |
| `script` | `path` (asset path of a Luau script) and `properties` (an object of numbers, booleans, strings and `[x, y, z]` vectors, set on the instance before `on_start`; see [scripting.md](scripting.md#script-properties)). |
| `components` | Game-defined components by name, read and written by the game's `WPE_ComponentSerializer`s. |

An entity may have no components at all (`{"id": 7}`).

## Environment

| Key | Description |
|---|---|
| `clear_color` | Background `[r, g, b]` (0..1) where nothing is drawn. |
| `ambient` | Ambient light `[r, g, b]`. |
| `fog` | `mode` (`"none"`, `"linear"`, `"exponential"`), `color`, `start` and `end` (linear), `density` (exponential). |
| `shadows` | `enabled`, `map_size` (16..4096), `distance` (shadows cover the view up to this distance), `caster_extension`, `bias`. |
| `dither_transparency` | Render alpha-blended materials with ordered dithering (order-independent). |
| `frustum_culling` | Skip objects outside the view (default true). |
| `subdivision` | Adaptive subdivision of near triangles: `levels` (0..4, 0 = off) and `edge_pixels`. |
| `default_sun` | `enabled`, `direction`, `color`: a light used when the scene has no light entities at all. |
| `gravity` | Physics gravity `[x, y, z]` in m/s² (default `[0, -9.81, 0]`). |
| `music` | `path` of a music track started when the scene loads (looping) and the crossfade `fade` in seconds. The same track keeps playing across scenes. Without `music`, the previous scene's music fades out. |
| `postfx` | Up to 8 post-processing effects, applied in order. Each object has `effect` and `enabled` (default true), plus the effect's parameters: `ssao` (`radius_pixels`, `strength`, `depth_range`, `bias`), `chromatic_aberration` (`strength_pixels`), `ps1` (`levels`, `dither_strength`), `vignette` (`strength`, `radius`), `color_adjust` (`brightness`, `contrast`, `saturation`). |

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
