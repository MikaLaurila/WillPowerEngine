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

return Spinner
```

`self.entity` holds the entity handle. All callbacks are optional.

## Lifecycle and rules

- **Callback order:**
  - `on_start` runs during the first update after the script is attached.
  - `on_fixed_update` only begins after `on_start` has run.
  - `on_destroy` runs only for instances that started.
  - Collision and trigger callbacks run after each physics step, for both entities involved (each gets the other as `other`). A trigger's own script and the visiting entity's script both get `on_trigger_enter`/`on_trigger_exit`.
- **Changing the world:** scripts may spawn, modify and destroy entities freely during `on_start`, `on_update` and `on_fixed_update`. `wpe.destroy` is deferred: the entity and its children are destroyed at the end of the current update.
- **Inside `on_destroy`:** you may read and modify components and call `wpe.destroy`. Spawning, attaching scripts, adding/removing lights or physics components and reparenting raise errors there. When the entity is being despawned, its physics body is already gone.
- **Errors:** an error in a callback is logged with a stack trace, and that instance is disabled until its script is reloaded. Other scripts keep running.
- **Budgets:** each callback has a time budget (infinite loops are interrupted), and the VM has a memory limit. Hitting either counts as an error.
- **Hot reload:** keeps every instance's state and swaps in the new code. A file that fails to compile leaves the previous version running.

## Sandbox

- **Available:** the Luau standard libraries `math`, `string`, `table`, `coroutine`, `bit32`, `utf8`, `buffer`, `vector`, and `os.time/clock/date/difftime`.
- **Not available:** `io`, `loadstring`/`load`, `require`, `dofile`, `getfenv`/`setfenv`, and any other `os` functions.
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
| `wpe.get_parent(entity) -> entity?` | Parent, or nil. |
| `wpe.set_parent(entity, parent?, keep_world = true)` | Reparent (nil detaches). With `keep_world`, the world placement is preserved. |
| `wpe.children(entity) -> {entity}` | Direct children in order. |
| `wpe.instantiate(prefab, [position]) -> entity` | Instantiate a registered prefab, or a glTF asset path (loaded once and cached). Returns the root entity. An animated model starts playing its first animation. |
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
| `wpe.set_visible(entity, bool)` | Show or hide every mesh in the entity's subtree. |
| `wpe.play_animation(entity, [index], [loop = true])` | Play an instantiated model's animation (1-based index). nil keeps the current one. |
| `wpe.stop_animation(entity)` | Pause playback. |
| `wpe.animation_time(entity, [seconds]) -> seconds` | Get or set the playback position. |
| `wpe.time() -> seconds` | Accumulated game time from updates. |
| `wpe.frame() -> number` | Number of completed updates. |

### Physics

Available when the host was created with a physics world (otherwise these raise errors). Units are meters, kilograms and seconds; the default density of 1 makes a 1 m cube weigh 1 kg.

| Function | Description |
|---|---|
| `wpe.add_rigidbody(entity, [{type, linear_damping, angular_damping, gravity_scale, lock_rotation, continuous}])` | Add or update a rigid body (default: dynamic). The body is created immediately from the entity's current world transform. `lock_rotation` keeps it upright (characters); `continuous` prevents fast bodies tunneling through other moving bodies. |
| `wpe.BODY_STATIC`, `wpe.BODY_KINEMATIC`, `wpe.BODY_DYNAMIC` | Body types. Dynamic bodies are simulated and write their pose back into the Transform. Kinematic bodies follow their Transform (moving it pushes dynamic bodies) or a velocity set with `wpe.set_velocity`. |
| `wpe.add_collider(entity, [{shape, size, radius, height, offset, density, friction, restitution, trigger, model, mesh}])` | Add or update the entity's collider (one per entity; default: a 1×1×1 box). The entity's world scale is applied when the shape is built. Without a rigid body it is static level geometry. `trigger = true` reports overlaps instead of colliding. |
| `wpe.COLLIDER_BOX` (`size`), `wpe.COLLIDER_SPHERE` (`radius`), `wpe.COLLIDER_CAPSULE` (`radius`, `height` along local Y) | Primitive shapes. |
| `wpe.COLLIDER_CONVEX`, `wpe.COLLIDER_MESH` (`model`, `mesh = 1`) | The convex hull or exact triangles of a prefab's mesh (`model` is a prefab name or glTF path). Triangle meshes can't be dynamic. |
| `wpe.remove_rigidbody(entity)` / `wpe.remove_collider(entity)` | Remove the component (a collider left without a body becomes static). |
| `wpe.apply_force(entity, v)` | Apply a force (newtons) for the next step. Dynamic bodies only. |
| `wpe.apply_impulse(entity, v)` | Change the momentum immediately (velocity change = impulse / mass). Dynamic bodies only. |
| `wpe.get_velocity(entity)` / `wpe.set_velocity(entity, v)` | Linear velocity (m/s). Setting it works for dynamic and kinematic bodies. |
| `wpe.raycast(origin, direction, [max_distance = 1000]) -> {entity, point, normal, distance}?` | Closest hit with a non-trigger collider, or nil. |
| `wpe.get_gravity()` / `wpe.set_gravity(v)` | World gravity (m/s²). |

Moving a dynamic body's entity with `wpe.set_position`/`wpe.set_rotation` teleports it. Changing a collider or rigid body after creation means calling `wpe.add_collider`/`wpe.add_rigidbody` again; changing the entity's scale needs the same.

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

`assets/scripts/api_tour.luau` exercises every function above. The integration test runs it for 60 simulated seconds.

## Hosting from C

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
wpe_script_host_create(&(WPE_ScriptHostDesc){.ecs = ecs, .assets = assets, .physics = physics, .audio = audio}, &host);
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
