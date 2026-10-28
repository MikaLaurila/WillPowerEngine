# Scripting with Luau

WillPowerEngine runs entity behavior in [Luau](https://luau.org) scripts. Each script file returns a table of callbacks (its "class"). Every entity that has the script attached gets its own instance table, `self`.

```lua
local Spinner = {}

function Spinner:on_start()             -- once, before the first on_update
	self.speed = 1.5                    -- instance state lives on self
end

function Spinner:on_update(dt)          -- every frame (variable dt)
	wpe.rotate(self.entity, vector.create(0, 1, 0), self.speed * dt)
end

function Spinner:on_fixed_update(dt)    -- every fixed simulation step (physics-aligned)
end

function Spinner:on_destroy()           -- component removed, entity destroyed, or host shut down
end

function Spinner:on_reload()            -- after hot reload; self keeps all its fields
	self.speed = 1.5                    -- re-apply constants that on_start set
end

function Spinner:on_collision_enter(other)  -- physics: started touching `other` (also on_collision_exit)
end

function Spinner:on_trigger_enter(other)    -- physics: a trigger overlap began (also on_trigger_exit)
end

function Spinner:on_click()                 -- UI: this entity's button was clicked
end

return Spinner
```

`self.entity` holds the entity handle. All callbacks are optional.

## Script properties

A scene file can configure each script instance through `properties` (see [scenes.md](scenes.md)):

```json
"script": {"path": "scripts/spinner.luau", "properties": {"speed": 0.8, "label": "slow", "axis": [0, 1, 0]}}
```

Properties are plain fields of `self`, set before `on_start`. Numbers, booleans, strings and `[x, y, z]` vectors are allowed.

A script declares the properties it takes, with their defaults, in a `properties` table on its class:

```lua
local Spinner = {}
Spinner.properties = { speed = 1.2, label = "", axis = vector.create(0, 1, 0) }

function Spinner:on_start()
	wpe.log(self.speed) -- 1.2 unless the scene sets another value
end
```

Every instance starts with the declared defaults, and the scene's values are then set over them. The editor's inspector lists the declared properties, with a Reset button on those that differ from their default. Entries that aren't numbers, booleans, strings or vectors are ignored, and so are names that can't be properties (such as `entity`). When hot reload declares a new property, the running instances receive its default, and properties they already hold keep their values.

When a scene is saved, each property is written with its current value, so a script that changes `self.speed` saves the new value. A property equal to its declared default is not written, and neither is one that now holds something else (a table, a function or nil). Scripts that declare nothing still accept any property from the scene; read those with a fallback (`self.speed or 1`). From C, `wpe_script_set_property()` (NIL removes a property, or resets a declared one), `wpe_script_visit_properties()` and `wpe_script_visit_declared()` do the same.

## Lifecycle and rules

- **Callback order:**
  - `on_start` runs during the first update after the script is attached.
  - `on_fixed_update` only begins after `on_start` has run.
  - `on_destroy` runs only for instances that started.
  - Collision and trigger callbacks run after each physics step, for both entities involved (each gets the other as `other`). A trigger's own script and the visiting entity's script both get `on_trigger_enter`/`on_trigger_exit`.
  - `on_click` runs at the start of the frame, before the fixed steps and `on_update`, for each button clicked by that frame's input (on the button entity's own script).
- **Changing the world:** scripts may spawn, modify and destroy entities freely during `on_start`, `on_update` and `on_fixed_update`. `wpe.destroy` is deferred: the entity and its children are destroyed at the end of the current update.
- **Inside `on_destroy`:** you may read and modify components and call `wpe.destroy`. Spawning, attaching scripts, adding/removing lights or physics components and reparenting raise errors there. When the entity is being despawned, its physics body is already gone.
- **Errors:** an error in a callback is logged with a stack trace, and that instance is disabled until its script is reloaded. Other scripts keep running.
- **Budgets:** each callback has a time budget (infinite loops are interrupted), and the VM has a memory limit. Hitting either counts as an error.
- **Hot reload:** keeps every instance's state and swaps in the new code. A file that fails to compile leaves the previous version running. When a module changes, every script reloads, so each one gets the new module.

## Modules

`require(path)` loads a module: a Luau file that returns a value, usually a table of functions. The path is an asset path, and `.luau` may be left out:

```lua
-- scripts/lib/util.luau
local util = {}
function util.approach(value, target, step) ... end
return util

-- scripts/hero.luau
local util = require("scripts/lib/util")
```

A module runs once, and every `require` of it returns the same value, so modules can also hold state shared between scripts. Modules can require other modules; a module that ends up requiring itself raises an error. A module has its own globals, like a script file. When a module changes, hot reload drops every module and reloads every script, so each one requires the new version. If the module is broken, the scripts keep running as they were until it's fixed. `scripts/lib/util.luau` holds the demo scripts' shared helpers.

## Sandbox

- **Available:** the Luau standard libraries `math`, `string`, `table`, `coroutine`, `bit32`, `utf8`, `buffer`, `vector`, and `os.time/clock/date/difftime`. `require` loads modules from the game's own files.
- **Not available:** `io`, `loadstring`/`load`, `dofile`, `getfenv`/`setfenv`, and any other `os` functions.
- **Read-only:** globals and libraries can't be modified.
- **Separate globals per file:** each script file has its own global environment, so globals never leak between files.
- **Paths:** script, prefab, sound and music paths are asset paths in the game's virtual file system (a directory during development, `.pak` archives in a shipped game), with forward slashes. They can't be absolute or climb above the root with `..`, so scripts can only read what the game ships.

## API reference (`wpe`)

Entity handles are numbers. Passing a handle that isn't alive raises an error. Vectors use Luau's built-in `vector` type (`vector.create(x, y, z)`; arithmetic and `vector.magnitude`, `vector.normalize`, `vector.dot` and `vector.cross` work as usual).

| Function | Description |
|---|---|
| `wpe.log(...)` / `print(...)` | Log the arguments (separated by spaces) at INFO level, tagged with the script location. |
| `wpe.spawn([name]) -> entity` | Create an entity with an identity Transform and an optional name. |
| `wpe.destroy(entity)` | Destroy an entity and its children at the end of the current update. Dead handles are ignored. |
| `wpe.is_alive(entity) -> bool` | Whether the handle refers to a live entity. |
| `wpe.get_name(entity) -> string?` / `wpe.set_name(entity, name)` | Display name. |
| `wpe.find(name) -> entity?` | An entity with this name, or nil. It scans every name, so look entities up once (in `on_start`) and keep the handle. |
| `wpe.get_parent(entity) -> entity?` | Parent, or nil. |
| `wpe.set_parent(entity, parent?, keep_world = true)` | Reparent (nil detaches). With `keep_world`, the world placement is preserved. |
| `wpe.children(entity) -> {entity}` | Direct children in order. |
| `wpe.instantiate(prefab, [position]) -> entity` | Instantiate a prefab file (`.prefab`, see [scenes.md](scenes.md#prefabs)), a model asset path (glTF or `.prim`), or a name from the project's `prefabs` (or registered in C) that maps to either. Prefab files are parsed once and read again when they change; models are loaded once. Returns the root entity. An animated model starts playing its first animation. |
| `wpe.attach_script(entity, path) -> bool` | Attach (or replace) a script. |
| `wpe.get_position(entity)` / `wpe.set_position(entity, v)` | Local position. |
| `wpe.get_rotation(entity)` / `wpe.set_rotation(entity, euler)` | Local rotation as XYZ Euler angles in radians (applied X, then Y, then Z). |
| `wpe.rotate(entity, axis, radians)` | Rotate around an axis given in parent space. |
| `wpe.get_scale(entity)` / `wpe.set_scale(entity, v or number)` | Local scale. |
| `wpe.get_world_position(entity)` | World-space position, computed from the current transforms. |
| `wpe.forward(entity)` | World-space forward direction (local −Z). |
| `wpe.look_at(entity, target, [up])` | Turn so that forward points at a world-space target. |
| `wpe.set_light(entity, {type, color, intensity, range, inner_cone, outer_cone, shadows})` | Add or update a light. Missing fields keep their current values (or the defaults). |
| `wpe.get_light(entity) -> table?` / `wpe.remove_light(entity)` | Read or remove a light. |
| `wpe.LIGHT_DIRECTIONAL`, `wpe.LIGHT_POINT`, `wpe.LIGHT_SPOT` | Light type constants. |
| `wpe.set_visible(entity, bool)` | Show or hide every mesh in the entity's subtree. An instantiated model has an entity per glTF node, named as in the file, so its parts (a sword drawn or sheathed) can be shown and hidden one by one: find them under the instance with `util.find_child(root, name)` from `scripts/lib/util.luau`, which unlike `wpe.find` stays within that copy of the model. |
| `wpe.play_animation(entity, [animation], [loop = true], [fade = 0])` | Play an instantiated model's animation from its start: a 1-based index or the animation's name. nil resumes the current one where it is. With `fade` seconds it crossfades: the previous animation carries on underneath while the new one blends in, so a walk turns into a run without a jump. |
| `wpe.stop_animation(entity)` | Pause playback. |
| `wpe.animation_time(entity, [seconds]) -> seconds` | Get or set the playback position. |
| `wpe.animation_speed(entity, [speed]) -> speed` | Get or set the playback rate (1 normal, 2 twice as fast, negative backwards). |
| `wpe.get_animation(entity) -> {index, name, time, duration, speed, playing, loop, fading, names}` | What's playing. `playing` turns false when a non-looping animation ends; `names` lists the model's animations. |
| `wpe.time() -> seconds` | Accumulated game time from updates. |
| `wpe.frame() -> number` | Number of completed updates. |

### Timers

| Function | Description |
|---|---|
| `wpe.after(seconds, fn) -> id` | Calls `fn()` once, `seconds` of game time from now (0: at the end of this update). |
| `wpe.every(seconds, fn) -> id` | Calls `fn()` every `seconds` (more than 0) until cancelled. After a long frame it fires once and keeps its rhythm from then, rather than catching up. |
| `wpe.cancel(id) -> bool` | Stops a timer; false when it had already finished or was cancelled. |

Timers run at the end of each update, after `on_update`, on game time, so they stop while the game is paused. A timer belongs to the script that made it and goes away with it. An error in a timer is logged and cancels that timer; the script keeps running.

```lua
function Door:open()
	wpe.play_sound("sounds/creak.wav")
	wpe.after(2, function()
		self:close()
	end)
end
```

### Messages between scripts

| Function | Description |
|---|---|
| `wpe.get_script(entity) -> self?` | Another entity's script instance, its `self`: read its fields and call its methods directly (`wpe.get_script(e):take_damage(5)`). nil for entities that are gone, have no script, or whose script failed. |
| `wpe.send(entity, method, ...) -> ...` | Calls `method` on the entity's script with the arguments and returns what it returns. A script without the method, or a missing entity, ignores it (returns nothing). An error inside is the caller's error. |
| `wpe.broadcast(method, ...) -> count` | Calls `method` on every started script that has it; returns how many did. An error in one receiver disables that receiver only. |

### Saved data

Games keep progress, settings and high scores with these. Data is stored per user, outside the game's files: `%APPDATA%\WillPowerEngine\<project name>\` on Windows and `~/.local/share/WillPowerEngine/<project name>/` on Linux, one `<name>.json` file per save.

| Function | Description |
|---|---|
| `wpe.save_data(name, value)` | Saves a number, string, boolean, vector, or a table of those (tables nest up to 32 deep). A table is either a list (keys 1..n) or has string keys. The file is replaced whole, so a crash mid-save never leaves half a file. |
| `wpe.load_data(name) -> value?` | What was saved under the name, or nil if nothing was. A damaged file raises an error. |
| `wpe.delete_data(name) -> bool` | Removes a save; false if there was none. |

Names are 1 to 64 letters, digits, `_` and `-`. Functions, cyclic tables and non-finite numbers can't be saved (that raises an error). Vectors are stored as `{"$vector": [x, y, z]}`, so don't use a table with a single `"$vector"` key for anything else.

```lua
local best = wpe.load_data("best") or { score = 0 }
if score > best.score then
	wpe.save_data("best", { score = score, when = os.time() })
end
```

### Physics

Available when the host was created with a physics world (otherwise these raise errors). Units are meters, kilograms and seconds; the default density of 1 makes a 1 m cube weigh 1 kg.

| Function | Description |
|---|---|
| `wpe.add_rigidbody(entity, [{type, linear_damping, angular_damping, gravity_scale, lock_rotation, continuous, velocity}])` | Add or update a rigid body (default: dynamic). The body is created immediately from the entity's current world transform, moving at `velocity` (e.g. a projectile). `lock_rotation` keeps it upright (characters); `continuous` prevents fast bodies tunneling through other moving bodies. |
| `wpe.BODY_STATIC`, `wpe.BODY_KINEMATIC`, `wpe.BODY_DYNAMIC` | Body types. Dynamic bodies are simulated and write their pose back into the Transform. Kinematic bodies follow their Transform (moving it pushes dynamic bodies) or a velocity set with `wpe.set_velocity`. |
| `wpe.add_collider(entity, [{shape, size, radius, height, offset, density, friction, restitution, trigger, model, mesh}])` | Add or update the entity's collider (one per entity; default: a 1×1×1 box). The entity's world scale is applied when the shape is built. Without a rigid body it is static level geometry. `trigger = true` reports overlaps instead of colliding. |
| `wpe.COLLIDER_BOX` (`size`), `wpe.COLLIDER_SPHERE` (`radius`), `wpe.COLLIDER_CAPSULE` (`radius`, `height` along local Y) | Primitive shapes. |
| `wpe.COLLIDER_CONVEX`, `wpe.COLLIDER_MESH` (`model`, `mesh = 1`) | The convex hull or exact triangles of a prefab's mesh (`model` is a prefab name or glTF path). Triangle meshes can't be dynamic. |
| `wpe.remove_rigidbody(entity)` / `wpe.remove_collider(entity)` | Remove the component (a collider left without a body becomes static). |
| `wpe.apply_force(entity, v)` | Apply a force (newtons) for the next step. Dynamic bodies only. |
| `wpe.apply_impulse(entity, v)` | Change the momentum immediately (velocity change = impulse / mass). Dynamic bodies only. |
| `wpe.get_velocity(entity)` / `wpe.set_velocity(entity, v)` | Linear velocity (m/s). Setting it works for dynamic and kinematic bodies. |
| `wpe.raycast(origin, direction, [max_distance = 1000], [options]) -> {entity, point, normal, distance}?` | Closest hit, or nil. Options: `{ignore = entity, triggers = false}`: skip an entity's collider and character capsule (such as your own), or see trigger colliders too. |
| `wpe.shape_cast(shape, origin, direction, [max_distance = 1000], [options]) -> hit?` | Sweeps a shape and reports the first collider it touches, like `raycast`. `distance` is how far the shape's center travelled; a shape that starts inside something hits it at distance 0. |
| `wpe.overlap(shape, center, [options]) -> {entity, ...}` | The entities whose colliders (or character capsules) overlap the shape, each once (at most 64). |
| `wpe.get_gravity()` / `wpe.set_gravity(v)` | World gravity (m/s²). |

Query shapes are tables: `{shape = wpe.COLLIDER_SPHERE, radius}`, `{shape = wpe.COLLIDER_CAPSULE, radius, height}` (standing along local Y) or `{shape = wpe.COLLIDER_BOX, size}`, with an optional `rotation` (Euler angles in radians).

Moving a dynamic body's entity with `wpe.set_position`/`wpe.set_rotation` teleports it. Changing a collider or rigid body after creation means calling `wpe.add_collider`/`wpe.add_rigidbody` again; changing the entity's scale needs the same.

#### Character controllers

A character controller is an upright capsule moved by your script rather than by forces: it slides along walls, walks up slopes and stairs, stays on the ground walking down them, stops at ceilings and pushes crates. Its kinematic body lets triggers and dynamic bodies see it. The capsule stands along world Y whatever the entity's rotation, so turning the entity to face its way is free. The entity shouldn't also have a rigid body or collider.

| Function | Description |
|---|---|
| `wpe.add_character(entity, [{radius = 0.4, height = 1.8, offset = (0, 0.9, 0), max_slope = 45, step_height = 0.3, snap_distance = 0.3}])` | Add or update the controller. `offset` is the capsule's center from the entity's origin (the default puts the origin at the feet); `max_slope` is the steepest walkable ground in degrees; `step_height` the tallest step walked up onto; `snap_distance` how far it follows the ground down slopes and steps. |
| `wpe.move_character(entity, velocity, dt) -> velocity, grounded` | Moves for `dt` seconds at `velocity` and returns what is left of it: the parts that ran into something are gone (landing zeroes the fall, a wall the speed into it, a ceiling the rise). Keep the returned velocity for the next frame. |
| `wpe.get_character(entity) -> {grounded, ground_normal, ground?, radius, height, offset, max_slope, step_height, snap_distance}` | The settings, and what the latest move found under it (`ground` is the entity it stands on). |
| `wpe.remove_character(entity)` | Remove the controller. |

Gravity is the script's to apply: add it to the velocity each frame. A velocity pointing up (a jump) leaves the ground; one pointing down on walkable ground doesn't slide it downhill. Call `move_character` from `on_fixed_update` for frame-rate-independent movement:

```lua
function Hero:on_fixed_update(dt)
	local v = vector.create(wish.x * self.speed, self.velocity.y - 18 * dt, wish.z * self.speed)
	if jump and wpe.get_character(self.entity).grounded then
		v = vector.create(v.x, 6.5, v.z)
	end
	self.velocity = wpe.move_character(self.entity, v, dt)
end
```

`scripts/hero.luau` with `scenes/character.scene` is a complete example.

### Audio

Available when the host was created with audio (otherwise these raise errors). Sounds are WAV, FLAC or MP3 asset paths; the host keeps each sound it has used loaded, so replaying is cheap. Positions are world space, and the listener is set by the host (usually the camera).

| Function | Description |
|---|---|
| `wpe.play_sound(path, [{volume, pitch, position, min_distance, max_distance, bus, loop}]) -> voice` | Play a sound once (or looping). With `position` it is 3D: attenuated with distance (full volume within `min_distance`) and panned. Returns a voice id, or 0 if all 64 voices are busy (the oldest one-shot is reused first). |
| `wpe.stop_sound(voice)` | Stop a voice. Ids of finished voices are ignored. |
| `wpe.add_audio_source(entity, {sound, volume, pitch, loop, spatial, min_distance, max_distance, bus, play = true})` | Give the entity a sound that plays at its position (`spatial`, the default) or unpositioned. Replaces an existing source. |
| `wpe.remove_audio_source(entity)` | Remove the entity's sound source. |
| `wpe.set_audio_playing(entity, bool)` / `wpe.is_audio_playing(entity)` | Start or stop the source. A non-looping source reports false once its sound has ended. |
| `wpe.play_music(path, [fade_seconds = 0], [loop = true])` | Stream a music track, crossfading from the current one. |
| `wpe.stop_music([fade_seconds = 0])` | Fade out and stop the music. |
| `wpe.set_volume(bus, volume)` / `wpe.get_volume(bus)` | Linear volume (1 = unchanged) of `wpe.BUS_MASTER`, `wpe.BUS_EFFECTS` (sounds and sources) or `wpe.BUS_MUSIC`. |

### Input

Available when the host has input (the engine runtime always has). Players are numbered 1 to 4. In functions with an optional `player`, nil means any device. The keyboard and mouse belong to the project's `keyboard_player` (player 1 by default); each gamepad belongs to the player slot it was assigned when it connected.

Prefer **actions** to raw devices. They are named in the project file and bound to keys, mouse buttons and gamepad buttons or axes at once, so one script supports every device (see [runtime.md](runtime.md#input-actions)).

| Function | Description |
|---|---|
| `wpe.action_value(name, [player]) -> number` | The action's value in [-1, 1]: the sum of its bindings (a held key gives its `scale`, a stick its dead-zoned deflection). |
| `wpe.action_down(name, [player]) -> bool` | Whether \|value\| reaches the project's press threshold. |
| `wpe.action_pressed(name, [player])` / `wpe.action_released(name, [player])` | Whether the action went down / up during this frame. A tap shorter than a frame reports both. |
| `wpe.key_down(key)`, `wpe.key_pressed(key)`, `wpe.key_released(key)` | A physical key by name: `"a"`–`"z"`, `"0"`–`"9"`, `"f1"`–`"f12"`, `"space"`, `"return"`, `"escape"`, `"tab"`, `"backspace"`, `"up"`, `"down"`, `"left"`, `"right"`, `"lshift"`, `"rshift"`, `"lctrl"`, `"rctrl"`, `"lalt"`, `"ralt"`, `"kp_0"`–`"kp_9"`, ... (positions on a US layout). |
| `wpe.mouse_down(button)`, `wpe.mouse_pressed(button)`, `wpe.mouse_released(button)` | `"left"`, `"middle"`, `"right"`, `"x1"`, `"x2"`. |
| `wpe.mouse_position() -> x, y` | Cursor position in screen (framebuffer) pixels. |
| `wpe.mouse_delta() -> dx, dy` | Mouse movement during this frame, in screen pixels. |
| `wpe.mouse_wheel() -> number` | Vertical wheel movement during this frame (up is positive). |
| `wpe.set_mouse_captured(bool)` / `wpe.is_mouse_captured()` | Capture hides the cursor and keeps reporting `mouse_delta` at the window edge (mouse look). |
| `wpe.gamepad_count() -> number` | Connected gamepads (up to 4). |
| `wpe.gamepad_connected(player) -> bool` / `wpe.gamepad_name(player) -> string?` | Whether the player has a pad, and its product name. |
| `wpe.gamepad_down(button, [player])`, `wpe.gamepad_pressed(...)`, `wpe.gamepad_released(...)` | Buttons by position: `"south"`, `"east"`, `"west"`, `"north"` (aliases `"a"`, `"b"`, `"x"`, `"y"`: A/B/X/Y on Xbox, Cross/Circle/Square/Triangle on PlayStation), `"back"`, `"guide"`, `"start"`, `"left_stick"`, `"right_stick"`, `"left_shoulder"`, `"right_shoulder"`, `"dpad_up"`, `"dpad_down"`, `"dpad_left"`, `"dpad_right"`, `"misc1"`, `"touchpad"`, paddles. |
| `wpe.gamepad_axis(axis, [player]) -> number` | `"left_x"`, `"left_y"`, `"right_x"`, `"right_y"` (-1..1, Y positive **down**, radial dead zone applied) and `"left_trigger"`, `"right_trigger"` (0..1). With no player: the most deflected pad. |
| `wpe.rumble(low, high, seconds, [player]) -> bool` | Rumble the low- and high-frequency motors (0..1) for up to 10 s (0 stops). Returns whether any pad accepted it. |

Edges (`pressed`, `released`) belong to frames, so read them in `on_update`. A fixed step may run zero or several times per frame. To act on a press in `on_fixed_update`, remember it in `on_update` (see `scripts/player.luau`).

### Game: scenes, camera, screen

Provided by the application running the scripts (the engine runtime). Elsewhere they raise errors.

| Function | Description |
|---|---|
| `wpe.load_scene(path)` | Switch to another scene after the current frame: every entity is destroyed (scripts get `on_destroy`) and the new scene's entities and environment are loaded. Raises if the file doesn't exist. |
| `wpe.quit()` | Exit the game after the current frame. |
| `wpe.set_camera(entity)` / `wpe.get_camera() -> entity?` | The camera the scene is rendered from and heard from. Without one, the scene file's `camera`, then the first camera found, is used. |
| `wpe.screen_size() -> width, height` | The framebuffer (canvas) size in pixels. |
| `wpe.world_to_screen(position) -> x, y, in_front` | Where a world point shows on screen, in pixels from the top-left, through the current camera (as it is now, even after a script moved it this frame). `in_front` is false for points behind the camera. |

### Post-processing

The scene's post effects (its file's `postfx` list, see [scenes.md](scenes.md)) can be changed while the game runs, and new ones added for a moment: flash a shockwave on a hit, blur the screen on a dash, refocus the depth of field on the player. Changes last until the scene is loaded again. An effect is a table `{effect = "shockwave", enabled = true, ...}` with its parameters by their scene-file names; missing ones keep their values (or the defaults, when adding), and values are clamped to each parameter's range.

| Function | Description |
|---|---|
| `wpe.get_postfx() -> {effect, ...}` | The scene's effects, in the order they apply. |
| `wpe.set_postfx(which, {...})` | Changes an effect's `enabled` and parameters. `which` is a 1-based index or an effect name (the first effect of that kind). |
| `wpe.add_postfx({effect = name, ...}) -> index` | Appends an effect (at most 8 in all). |
| `wpe.remove_postfx(which)` | Removes an effect; later ones move up. |

Two effects are made for scripts to animate: `shockwave` (a ring warping the image outward from `center_x`, `center_y` (fractions of the screen), at `radius` (fractions of its height), `width` thick, pushing `strength_pixels`) and `radial_blur` (smearing toward `center_x`, `center_y` by `strength`, 0..1). A hit that ripples out from the player:

```lua
function Hero:on_start()
	wpe.add_postfx({ effect = "shockwave", enabled = false })
end

function Hero:hit_wave(t) -- t from 0 to 1 over a few tenths of a second
	local x, y = wpe.world_to_screen(wpe.get_world_position(self.entity))
	local width, height = wpe.screen_size()
	wpe.set_postfx("shockwave", { enabled = t < 1, center_x = x / width, center_y = y / height,
		radius = 0.05 + 0.5 * t, strength_pixels = 8 * (1 - t) })
end
```

`scripts/hero.luau` in `scenes/character.scene` does this on the fire button, with a radial blur. Effects run in C over every pixel; scripts choose and drive them, but can't run their own per-pixel code (Luau would be far too slow for that).

### 2D drawing

Drawn on top of the 3D scene after post-processing (HUDs, menus), in call order, during the frame in which it is called. Coordinates are screen pixels with the origin at the top left. Colors are numbers `0xAARRGGBB`, conveniently built with `wpe.color`.

| Function | Description |
|---|---|
| `wpe.color(r, g, b, [a = 1]) -> color` | Pack components in 0..1. |
| `wpe.draw_rect(x, y, width, height, [color = white])` | A filled rectangle (alpha-blended). |
| `wpe.draw_text(x, y, text, [color = white], [scale = 1])` | Bitmap text in the project's font (`"\n"` starts a line). |
| `wpe.text_size(text, [scale = 1]) -> width, height` | The size `draw_text` would cover. |

### Billboards

3D sprites ([scenes.md](scenes.md#billboards)). A billboard table has the scene-file fields: `image` (asset path), `size` and `pivot` (vectors, x and y used), `rotation` (radians), `color` (`wpe.color`), `columns`, `rows`, `frame_count`, `frame` (**1-based** here, like animations), `fps`, `loop`, `mode` and `blend` (the scene-file names as strings), `lit`, `visible`.

| Function | Description |
|---|---|
| `wpe.set_billboard(entity, table)` | Changes the fields given (others keep their values), adding a billboard if the entity has none. A new `image` is loaded and replaces the old one. |
| `wpe.get_billboard(entity) -> table?` | Every field (`image` is nil for a plain quad), or nil without a billboard. |
| `wpe.remove_billboard(entity)` | Removes it (a no-op without one). |

### Particles

Particle emitters ([scenes.md](scenes.md#particles)). A settings table uses the scene-file names: `playing`, `loop`, `world_space`, `random_rotation`, `rate`, `duration`, `drag`, `spread` (**radians** here), `burst`, `max_particles`, the pairs `lifetime`, `speed`, `spin` (radians per second) and `size` as `{a, b}`, the vectors `direction` and `gravity`, and the colors `color_start`, `color_end` (`wpe.color`). The image, sheet, blending, lighting and shape are set in the scene or the editor.

| Function | Description |
|---|---|
| `wpe.set_particles(entity, table)` | Changes the fields given, adding an emitter (with the defaults) if the entity has none. Live particles stay. |
| `wpe.get_particles(entity) -> table?` | The settings above plus `count` (live particles), or nil. |
| `wpe.emit_particles(entity, count) -> number` | Emits now, playing or not; returns how many fit under `max_particles`. |
| `wpe.play_particles(entity)` | Starts over: no particles, a new cycle with its burst, playing. |
| `wpe.stop_particles(entity, [clear = false])` | Stops emitting; `clear` also removes the live particles. |
| `wpe.particle_count(entity) -> number` | Live particles. |
| `wpe.remove_particles(entity)` | Removes the emitter. |

### UI

The scene's UI elements (`ui`, `sprite`, `text` and `button` components, see [scenes.md](scenes.md#ui)) are retained: change them and they stay changed. Functions raise when the entity lacks the component they need.

| Function | Description |
|---|---|
| `wpe.get_text(entity) -> string` / `wpe.set_text(entity, text)` | A text's string. |
| `wpe.set_text_color(entity, color)` / `wpe.set_sprite_color(entity, color)` | A text's or sprite's color (`wpe.color`). |
| `wpe.set_ui_visible(entity, visible)` / `wpe.is_ui_visible(entity) -> boolean` | Hides or shows an element and everything under it. |
| `wpe.get_ui_offsets(entity) -> min_x, min_y, max_x, max_y` / `wpe.set_ui_offsets(entity, min_x, min_y, max_x, max_y)` | The corner offsets in pixels (moving or resizing an element; anchors stay). |
| `wpe.ui_rect(entity) -> x, y, width, height` | Where the element is on screen, from the last layout (this frame's, which runs before scripts; changes made since show next frame). |
| `wpe.set_interactable(entity, interactable)` | Enables or disables a button. |
| `wpe.ui_focus(entity or nil) -> boolean` / `wpe.ui_focused() -> entity?` | Moves the focus to a shown, interactable button (false if it can't take it), or clears it. |
| `wpe.ui_value(entity, [value]) -> value` | A toggle's state (boolean) or a slider's value (number, clamped to its range and snapped to its step). Setting it from a script doesn't call `on_change`. |

Toggles and sliders report the player's changes to their entity's script as `on_change(value)`, before `on_click`; a toggle's value is a boolean, a slider's a number. `scripts/volume_slider.luau` and `scripts/sound_toggle.luau` (on the title screen) set the music volume and mute the sound, and remember the settings with saved data.

`assets/scripts/api_tour.luau` exercises every function above, as part of the master test scene that the scene test runs for 60 simulated seconds. `wpe.load_scene` and `wpe.quit` end a scene, so the engine tests cover those two instead.

## Hosting from C

Games normally run inside the engine runtime (`engine/engine.h`, see [runtime.md](runtime.md)), which creates every subsystem, loads scenes and drives the loop. Hosting the script module directly is for tools and tests:

```c
// Assets come through a VFS (directories and/or .pak archives) and a shared asset manager.
wpe_vfs_mount_directory(vfs, "assets");                     // or wpe_vfs_mount_pak(vfs, "game.pak")
wpe_asset_manager_create(&(WPE_AssetManagerDesc){.vfs = vfs}, &assets);
wpe_graphics_register_components(ecs, assets);

WPE_Physics* physics;                                       // optional
wpe_physics_create(&(WPE_PhysicsDesc){.ecs = ecs, .assets = assets, .gravity = {{0, -9.81f, 0}}}, &physics);
WPE_Audio* audio;                                           // optional
wpe_audio_create(&(WPE_AudioDesc){.ecs = ecs, .assets = assets}, &audio);
wpe_audio_set_listener(audio, camera);
WPE_ScriptHost* host;
wpe_script_host_create(&(WPE_ScriptHostDesc){.ecs = ecs, .assets = assets, .physics = physics, .audio = audio,
                                             .input = input,            // optional: input API
                                             .draw2d = draw2d, .font = font, // optional: 2D drawing
                                             .ui = ui,                  // optional: UI focus (ui/ui.h)
                                             .app = &app},              // optional: load_scene, quit, camera
                       &host);
wpe_script_host_register_prefab(host, "box", box_handle);  // optional named prefabs (asset handles)
wpe_script_attach(host, entity, "scripts/spinner.luau");

// Per frame (see samples/script_demo):
uint32_t steps = wpe_fixed_step_advance(&clock, frame_dt);
for (uint32_t i = 0; i < steps; ++i) {
    wpe_script_host_fixed_update(host, clock.step);
    wpe_physics_step(physics, clock.step);
    wpe_script_host_dispatch_physics(host);                 // collision and trigger callbacks
}
wpe_script_host_update(host, frame_dt);
// ... animation, wpe_scene_update_transforms(ecs) ...
wpe_audio_update(audio);                                    // after transforms: listener and 3D sounds
wpe_script_host_reload_changed(host);                       // e.g. a few times per second while editing

wpe_script_host_destroy(host);                              // then audio, physics, the world, the assets
wpe_audio_destroy(audio);
wpe_physics_destroy(physics);
wpe_ecs_destroy(ecs);
wpe_asset_manager_destroy(assets);
```
