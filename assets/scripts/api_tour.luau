-- Exercises every function of the `wpe` scripting API. Part of the master test scene
-- (scenes/test_full.scene), which the scene test runs for 60 simulated seconds, and a living
-- reference. Fails loudly via assert. wpe.load_scene and wpe.quit end the scene, so the engine tests
-- cover them instead.
local JudgementCut = require("scripts/lib/judgement_cut")
local Tour = {}

local function near(a, b, eps)
	return math.abs(a - b) <= (eps or 1e-4)
end

local function vnear(a, b, eps)
	return near(a.x, b.x, eps) and near(a.y, b.y, eps) and near(a.z, b.z, eps)
end

function Tour:on_start()
	local e = self.entity
	wpe.log("api tour starting on", e)

	-- Names and liveness.
	wpe.set_name(e, "tour")
	assert(wpe.get_name(e) == "tour")
	assert(wpe.is_alive(e))
	assert(wpe.find("tour") == e) -- look entities up by name once, then keep the handle
	assert(wpe.find("no such entity") == nil)

	-- Transforms.
	wpe.set_position(e, vector.create(1, 2, 3))
	assert(wpe.get_position(e) == vector.create(1, 2, 3))
	wpe.set_rotation(e, vector.create(0.1, 0.2, 0.3))
	assert(vnear(wpe.get_rotation(e), vector.create(0.1, 0.2, 0.3)))
	wpe.set_scale(e, vector.create(1, 2, 1))
	assert(wpe.get_scale(e) == vector.create(1, 2, 1))
	wpe.set_scale(e, 1)

	-- Hierarchy and world-space queries.
	self.child = wpe.spawn("tour_child")
	wpe.set_parent(self.child, e, false)
	wpe.set_position(self.child, vector.create(0, 1, 0))
	assert(wpe.get_parent(self.child) == e)
	assert(#wpe.children(e) == 1)
	assert(wpe.get_world_position(self.child).y > 2)
	wpe.set_parent(self.child, nil) -- keeps its world placement by default
	assert(wpe.get_parent(self.child) == nil)

	-- Orientation helpers.
	self.pointer = wpe.spawn("tour_pointer")
	wpe.look_at(self.pointer, vector.create(0, 0, -5))
	assert(vnear(wpe.forward(self.pointer), vector.create(0, 0, -1)))
	wpe.rotate(self.pointer, vector.create(0, 1, 0), math.pi / 2)

	-- Lights.
	wpe.set_light(self.pointer, { type = wpe.LIGHT_SPOT, intensity = 2, range = 5, inner_cone = 0.2, outer_cone = 0.4 })
	local light = wpe.get_light(self.pointer)
	assert(light.type == wpe.LIGHT_SPOT and light.range == 5)
	wpe.remove_light(self.pointer)
	assert(wpe.get_light(self.pointer) == nil)

	-- Prefabs, visibility, animation.
	self.figure = wpe.instantiate("figure", vector.create(-2, 0, 0))
	-- Prefab files instantiate by asset path (or by a project prefab name mapped to one).
	local crate = wpe.instantiate("prefabs/crate.prefab", vector.create(6, 3, 6))
	assert(wpe.get_position(crate).y == 3, "prefab instance placed")
	wpe.destroy(crate)
	wpe.set_visible(self.figure, false)
	wpe.set_visible(self.figure, true)
	wpe.play_animation(self.figure, 1, true)
	wpe.animation_time(self.figure, 0.25)
	assert(near(wpe.animation_time(self.figure), 0.25))
	-- By name, crossfading over 0.3 s; speed; what's playing.
	local clip = wpe.get_animation(self.figure)
	assert(clip.index == 1 and clip.playing and clip.loop and clip.duration > 0 and #clip.names >= 1)
	wpe.play_animation(self.figure, clip.names[1], true, 0.3)
	assert(wpe.get_animation(self.figure).fading and near(wpe.animation_time(self.figure), 0))
	assert(wpe.animation_speed(self.figure, 1.5) == 1.5 and wpe.animation_speed(self.figure) == 1.5)
	wpe.animation_speed(self.figure, 1)
	assert(not pcall(wpe.play_animation, self.figure, "no such clip"))
	assert(not pcall(wpe.play_animation, self.figure, 1, true, -1))

	-- Scripts attaching scripts, and deferred destruction.
	self.temp = wpe.spawn("tour_temp")
	assert(wpe.attach_script(self.temp, "scripts/spinner.luau"))
	assert(not pcall(wpe.get_position, 4294967295)) -- invalid handles raise errors

	-- Physics (the host provides a ground collider with its top at y = 0).
	self.ball = wpe.spawn("tour_ball")
	wpe.set_position(self.ball, vector.create(8, 3, 8))
	wpe.add_rigidbody(self.ball, {
		type = wpe.BODY_DYNAMIC,
		linear_damping = 0.1,
		angular_damping = 0.1,
		gravity_scale = 1,
		lock_rotation = false,
		continuous = true,
		velocity = vector.create(0, 0.5, 0), -- initial velocity
	})
	wpe.add_collider(self.ball, { shape = wpe.COLLIDER_SPHERE, radius = 0.5, density = 2, friction = 0.5, restitution = 0.1 })
	wpe.set_velocity(self.ball, vector.create(0, 1, 0))
	assert(vnear(wpe.get_velocity(self.ball), vector.create(0, 1, 0)))
	wpe.apply_impulse(self.ball, vector.create(0.1, 0, 0))
	wpe.apply_force(self.ball, vector.create(0, 0, 1))
	local body = wpe.get_rigidbody(self.ball)
	assert(body.type == wpe.BODY_DYNAMIC and body.continuous and not body.lock_rotation and near(body.linear_damping, 0.1))

	self.pillar = wpe.spawn("tour_pillar")
	wpe.set_position(self.pillar, vector.create(-8, 1, -8))
	wpe.add_rigidbody(self.pillar, { type = wpe.BODY_KINEMATIC })
	wpe.add_collider(self.pillar, { shape = wpe.COLLIDER_CAPSULE, radius = 0.3, height = 2, offset = vector.create(0, 0, 0) })
	wpe.remove_collider(self.pillar)
	wpe.remove_rigidbody(self.pillar)
	assert(wpe.get_rigidbody(self.pillar) == nil)
	-- Without a rigid body a collider is static level geometry; this one is a trigger hull from a prefab.
	wpe.add_collider(self.pillar, { shape = wpe.COLLIDER_CONVEX, model = "box", mesh = 1, trigger = true })
	-- Without a model, a convex (or mesh) collider takes the shape of the entity's own mesh.
	-- (An instantiated model's meshes are on the entities of its nodes, under the returned root.)
	local block = wpe.instantiate("box", vector.create(-9, 3, 9))
	wpe.add_collider(wpe.children(block)[1], { shape = wpe.COLLIDER_CONVEX, trigger = true })
	wpe.destroy(block)

	local gravity = wpe.get_gravity()
	assert(gravity.y < 0)
	wpe.set_gravity(gravity)
	local hit = wpe.raycast(vector.create(20, 5, 20), vector.create(0, -1, 0), 10)
	assert(hit and near(hit.distance, 5, 1e-3) and hit.normal.y > 0.99 and wpe.get_name(hit.entity) == "ground")
	assert(wpe.raycast(vector.create(20, 5, 20), vector.create(0, 1, 0)) == nil)
	local ground = hit.entity
	assert(wpe.raycast(vector.create(20, 5, 20), vector.create(0, -1, 0), 10, { ignore = ground }) == nil)
	assert(wpe.raycast(vector.create(20, 5, 20), vector.create(0, -1, 0), 10, { triggers = true }).entity == ground)

	-- Shape casts and overlaps.
	local ball = { shape = wpe.COLLIDER_SPHERE, radius = 0.5 }
	hit = wpe.shape_cast(ball, vector.create(20, 5, 20), vector.create(0, -1, 0), 10)
	assert(hit and hit.entity == ground and near(hit.distance, 4.5, 1e-2) and hit.normal.y > 0.99)
	local slab = { shape = wpe.COLLIDER_BOX, size = vector.create(2, 0.2, 2), rotation = vector.create(0, 0.5, 0) }
	assert(near(wpe.shape_cast(slab, vector.create(20, 5, 20), vector.create(0, -1, 0)).distance, 4.9, 1e-2))
	local rod = { shape = wpe.COLLIDER_CAPSULE, radius = 0.2, height = 1 }
	assert(wpe.shape_cast(rod, vector.create(20, 5, 20), vector.create(0, 1, 0), 10, { ignore = ground }) == nil)
	local touching = wpe.overlap(ball, vector.create(20, 0.2, 20))
	assert(#touching == 1 and touching[1] == ground)
	assert(#wpe.overlap(ball, vector.create(20, 3, 20), { triggers = true }) == 0)
	assert(not pcall(wpe.overlap, { shape = wpe.COLLIDER_MESH }, vector.create(0, 0, 0)))

	-- UI toggles and sliders: values set by scripts (clamped and snapped) don't call on_change.
	local toggle, slider = wpe.find("tour toggle"), wpe.find("tour slider")
	assert(wpe.ui_value(toggle) == true and wpe.ui_value(toggle, false) == false)
	assert(near(wpe.ui_value(slider), 0.25) and near(wpe.ui_value(slider, 0.62), 0.6))
	assert(wpe.ui_value(slider, 7) == 1 and wpe.ui_value(slider, -1) == 0)
	assert(not pcall(wpe.ui_value, self.entity))

	-- Modules: loaded once, by path with or without ".luau".
	local util = require("scripts/lib/util")
	assert(require("scripts/lib/util.luau") == util, "a module loads once")
	assert(util.approach(0, 10, 3) == 3 and near(util.angle_delta(0, 1.5 * math.pi), -0.5 * math.pi))
	assert(not pcall(require, "scripts/lib/missing"))
	local parent, inner = wpe.spawn("tour_find_parent"), wpe.spawn("tour_find_inner")
	local leaf = wpe.spawn("tour_find_leaf")
	wpe.set_parent(inner, parent)
	wpe.set_parent(leaf, inner)
	assert(util.find_child(parent, "tour_find_leaf") == leaf and util.find_child(parent, "nothing") == nil)
	wpe.destroy(parent)
	assert(not pcall(require, "../outside"))

	-- Timers run on game time; a cancelled one never runs.
	self.timer_hits = 0
	self.ticks = 0
	wpe.after(0.5, function()
		self.timer_hits += 1
	end)
	local cancelled = wpe.after(0.5, function()
		error("a cancelled timer ran")
	end)
	assert(wpe.cancel(cancelled) and not wpe.cancel(cancelled))
	self.ticker = wpe.every(0.25, function()
		self.ticks += 1
	end)
	assert(not pcall(wpe.every, 0, function() end))

	-- Messages: another script's self, method calls that skip scripts without the method, broadcasts.
	assert(wpe.get_script(self.entity) == self)
	assert(wpe.send(self.entity, "add", 2, 3) == 5)
	assert(wpe.send(self.entity, "no_such_method") == nil)
	assert(wpe.get_script(self.ball) == nil and wpe.send(self.ball, "add", 1, 1) == nil) -- the ball has no script
	assert(wpe.broadcast("on_tour_ping", 7) >= 1 and self.pinged == 7)

	-- Saved data: tables of numbers, strings, booleans and vectors, nested.
	local data = { level = 3, name = "tour", alive = true, spot = vector.create(1, 2, 3), items = { "a", "b" }, deep = { x = { y = 1 } } }
	wpe.save_data("tour", data)
	local loaded = wpe.load_data("tour")
	assert(loaded.level == 3 and loaded.name == "tour" and loaded.alive == true and vnear(loaded.spot, data.spot))
	assert(#loaded.items == 2 and loaded.items[2] == "b" and loaded.deep.x.y == 1)
	assert(wpe.delete_data("tour") and wpe.load_data("tour") == nil and not wpe.delete_data("tour"))
	assert(not pcall(wpe.save_data, "bad name!", 1))
	assert(not pcall(wpe.save_data, "tour", { f = print }))
	assert(not pcall(wpe.save_data, "tour", { [2] = 1 }))
	assert(not pcall(wpe.save_data, "tour", nil))

	-- Post effects at runtime: read, change, add and remove (the scene file's list is the starting point).
	local effects = wpe.get_postfx()
	local before = #effects
	assert(before >= 1 and type(effects[1].effect) == "string" and type(effects[1].enabled) == "boolean")
	wpe.set_postfx("vignette", { strength = 0.4, enabled = true })
	local vignette
	for _, effect in wpe.get_postfx() do
		if effect.effect == "vignette" then
			vignette = effect
		end
	end
	assert(vignette and near(vignette.strength, 0.4) and vignette.enabled)
	local wave = wpe.add_postfx({ effect = "shockwave", radius = 0.1, strength_pixels = 4 })
	assert(wave == before + 1 and wpe.get_postfx()[wave].effect == "shockwave")
	assert(near(wpe.get_postfx()[wave].radius, 0.1) and near(wpe.get_postfx()[wave].width, 0.08)) -- defaults fill in
	wpe.set_postfx(wave, { radius = 9 }) -- clamped to its range
	assert(near(wpe.get_postfx()[wave].radius, 4))
	assert(not pcall(wpe.set_postfx, wave, { bloom = 1 }))
	assert(not pcall(wpe.set_postfx, 99, {}) and not pcall(wpe.set_postfx, "radial_blur", {}))
	assert(not pcall(wpe.add_postfx, { effect = "bloom" }))
	wpe.remove_postfx(wave)
	assert(#wpe.get_postfx() == before)
	-- Trails: a ribbon between two moving entities through where they were lately.
	local trail_base, trail_tip = wpe.spawn("tour trail base"), wpe.spawn("tour trail tip")
	wpe.set_position(trail_tip, vector.create(0, 1, 0))
	wpe.set_trail(trail_base, { base = trail_base, tip = trail_tip, lifetime = 0.3, blend = "alpha",
		color_start = wpe.color(1, 1, 1, 1), color_end = wpe.color(1, 1, 1, 0), width_end = 0.5, smoothing = 2 })
	local trail = wpe.get_trail(trail_base)
	assert(trail.tip == trail_tip and trail.blend == "alpha" and trail.emitting and near(trail.lifetime, 0.3))
	wpe.set_trail(trail_base, { emitting = false })
	assert(not wpe.get_trail(trail_base).emitting and wpe.get_trail(trail_base).smoothing == 2)
	assert(not pcall(wpe.set_trail, trail_base, { blend = "glow" }) and not pcall(wpe.set_trail, trail_base, { lifetime = 0 }))
	wpe.clear_trail(trail_base)
	assert(wpe.get_trail(trail_base).samples == 0)
	wpe.remove_trail(trail_base)
	assert(wpe.get_trail(trail_base) == nil and not pcall(wpe.clear_trail, trail_base))
	wpe.destroy(trail_base)
	wpe.destroy(trail_tip)	-- Slashes: a ribbon sweeping along a Bezier curve when played; its head entity follows the head.
	local slash, slash_head = wpe.spawn("tour slash"), wpe.spawn("tour slash head")
	wpe.set_parent(slash_head, slash, false)
	wpe.set_slash(slash, { points = { vector.create(0, 0, 0), vector.create(1, 1, 0), vector.create(2, 1, 0), vector.create(3, 0, 0) },
		width = 0.2, length = 0.4, duration = 0.25, head = slash_head, blend = "alpha", segments = 8 })
	local info = wpe.get_slash(slash)
	assert(not info.playing and info.head == slash_head and info.blend == "alpha" and info.segments == 8)
	assert(near(info.points[4].x, 3) and near(info.length, 0.4))
	wpe.play_slash(slash)
	assert(wpe.get_slash(slash).playing and wpe.get_slash(slash).time == 0)
	assert(not pcall(wpe.set_slash, slash, { length = 0 }) and not pcall(wpe.set_slash, slash, { points = { vector.create(0, 0, 0) } }))
	wpe.remove_slash(slash)
	assert(wpe.get_slash(slash) == nil and not pcall(wpe.play_slash, slash))
	wpe.destroy(slash)	-- Captured frames are image assets by path; slots 1 to 8.
	assert(wpe.capture_screen(2) == "capture/2" and not pcall(wpe.capture_screen, 9))
	self.cut_phases = {}
	-- A flashback: grayscale with a warm (sepia) tint, faded in by strength.
	local memory = wpe.add_postfx({ effect = "grayscale", strength = 0.5, tint_b = 0.7 })
	assert(near(wpe.get_postfx()[memory].strength, 0.5) and near(wpe.get_postfx()[memory].tint_r, 1))
	wpe.remove_postfx(memory)

	-- Screen positions: a point straight ahead of the camera is in the middle of the screen.
	local camera = wpe.get_camera()
	local ahead = wpe.get_world_position(camera) + wpe.forward(camera) * 5
	local sx, sy, in_front = wpe.world_to_screen(ahead)
	local width, height = wpe.screen_size()
	assert(in_front and near(sx, width / 2, 0.5) and near(sy, height / 2, 0.5),
		string.format("world_to_screen: %s %s %s of %d x %d", tostring(sx), tostring(sy), tostring(in_front), width, height))
	assert(select(3, wpe.world_to_screen(wpe.get_world_position(camera) - wpe.forward(camera) * 5)) == false)

	-- A character controller, walked in a circle at the fixed rate.
	self.walker = wpe.spawn("tour_walker")
	wpe.set_position(self.walker, vector.create(-12, 0.5, 12))
	wpe.add_character(self.walker, { radius = 0.3, height = 1.6, offset = vector.create(0, 0.8, 0), max_slope = 40, step_height = 0.25, snap_distance = 0.2 })
	local character = wpe.get_character(self.walker)
	assert(not character.grounded and near(character.radius, 0.3) and near(character.max_slope, 40))
	assert(near(character.step_height, 0.25) and near(character.snap_distance, 0.2))
	assert(vnear(character.offset, vector.create(0, 0.8, 0)) and character.ground == nil)
	self.walker_velocity = vector.create(0, 0, 0)
	self.walker_start = wpe.get_position(self.walker)
	assert(not pcall(wpe.add_character, self.walker, { radius = -1 }))
	assert(not pcall(wpe.move_character, self.ball, vector.create(0, 0, 0), 0.1)) -- not a character

	-- Audio.
	local voice = wpe.play_sound("sounds/chime.wav", {
		volume = 0.5,
		pitch = 1.2,
		bus = wpe.BUS_EFFECTS,
		position = vector.create(0, 1, 0),
		min_distance = 1,
		max_distance = 20,
		loop = false,
	})
	assert(voice > 0)
	wpe.stop_sound(voice)
	self.speaker = wpe.spawn("tour_speaker")
	wpe.add_audio_source(self.speaker, {
		sound = "sounds/thud.wav",
		loop = true,
		volume = 0.2,
		pitch = 1,
		spatial = true,
		min_distance = 1,
		max_distance = 30,
		bus = wpe.BUS_EFFECTS,
		play = false,
	})
	assert(not wpe.is_audio_playing(self.speaker))
	wpe.set_audio_playing(self.speaker, true)
	assert(wpe.is_audio_playing(self.speaker))
	wpe.set_volume(wpe.BUS_EFFECTS, 0.8)
	assert(near(wpe.get_volume(wpe.BUS_EFFECTS), 0.8))

	-- Input. The scene test runs headless (no keyboard or mouse); gamepads are optional, so their
	-- results are only checked for type. Player 4 has neither the keyboard nor (normally) a pad.
	assert(wpe.action_value("move_x", 4) == 0 and not wpe.action_down("jump", 4))
	assert(not wpe.action_pressed("release_mouse") and not wpe.action_released("release_mouse"))
	assert(not pcall(wpe.action_value, "no_such_action"))
	assert(not wpe.key_down("space") and not wpe.key_pressed("w") and not wpe.key_released("escape"))
	assert(not pcall(wpe.key_down, "not a key"))
	assert(not wpe.mouse_down("left") and not wpe.mouse_pressed("right") and not wpe.mouse_released("middle"))
	local mx, my = wpe.mouse_position()
	local dx, dy = wpe.mouse_delta()
	assert(type(mx) == "number" and type(my) == "number" and dx == 0 and dy == 0 and wpe.mouse_wheel() == 0)
	wpe.set_mouse_captured(false)
	assert(not wpe.is_mouse_captured())
	assert(wpe.gamepad_count() >= 0)
	assert(type(wpe.gamepad_connected(1)) == "boolean")
	assert(wpe.gamepad_connected(1) or wpe.gamepad_name(1) == nil)
	assert(type(wpe.gamepad_down("south")) == "boolean" and type(wpe.gamepad_pressed("start", 2)) == "boolean")
	assert(type(wpe.gamepad_released("dpad_up", 1)) == "boolean")
	assert(math.abs(wpe.gamepad_axis("left_x")) <= 1 and wpe.gamepad_axis("right_trigger", 3) >= 0)
	assert(type(wpe.rumble(0, 0, 0)) == "boolean") -- whether a pad accepted it
	assert(not pcall(wpe.gamepad_axis, "left_z"))

	-- Application services: canvas size and the camera rendered from.
	local width, height = wpe.screen_size()
	assert(width > 0 and height > 0)
	local camera = wpe.get_camera()
	assert(camera ~= nil)
	wpe.set_camera(camera)
	assert(not pcall(wpe.set_camera, e)) -- the tour has no camera component
	assert(not pcall(wpe.load_scene, "scenes/missing.scene"))

	-- 2D drawing (also every frame in on_update).
	local tw, th = wpe.text_size("tour", 2)
	assert(tw > 0 and th > 0)
	assert(wpe.color(1, 0, 0) == 0xFFFF0000)

	-- Billboards (3D sprites): read, change, add, swap the image, remove.
	local coin = wpe.find("coin")
	local b = wpe.get_billboard(coin)
	assert(b.image == "sprites/coin.png" and b.columns == 4 and b.frame == 1 and b.fps == 8)
	assert(b.mode == "axis_aligned_y" and b.blend == "alpha" and b.loop and not b.lit and b.visible)
	assert(near(b.size.x, 0.8) and near(b.size.y, 0.8) and near(b.pivot.x, 0.5))
	wpe.set_billboard(coin, { frame = 3, fps = 0 }) -- hold the third frame
	assert(wpe.get_billboard(coin).frame == 3)
	assert(not pcall(wpe.set_billboard, coin, { frame = 5 })) -- the sheet has 4 frames
	assert(not pcall(wpe.set_billboard, coin, { mode = "sideways" }))
	wpe.set_billboard(coin, { fps = 8 })
	self.marker = wpe.spawn("tour_marker")
	wpe.set_position(self.marker, vector.create(2, 3, 2))
	wpe.set_billboard(self.marker, { image = "sprites/glow.png", blend = "additive", color = wpe.color(0.4, 1, 0.4) })
	assert(wpe.get_billboard(self.marker).image == "sprites/glow.png")
	wpe.set_billboard(self.marker, { image = "sprites/tree.png", lit = true, size = vector.create(1, 1.5, 0) })
	local swapped = wpe.get_billboard(self.marker)
	assert(swapped.image == "sprites/tree.png" and swapped.blend == "additive" and swapped.lit)
	assert(not pcall(wpe.set_billboard, self.marker, { image = "sprites/missing.png" }))
	wpe.remove_billboard(self.marker)
	assert(wpe.get_billboard(self.marker) == nil)
	wpe.set_billboard(self.marker, { color = wpe.color(1, 0, 0) }) -- a plain quad, no image
	assert(wpe.get_billboard(self.marker).image == nil)
	self.coin = coin
	self.coin_frames = {}

	-- Particles: read and change an emitter, emit on demand, stop and restart, add and remove.
	local fire = wpe.find("fire")
	local p = wpe.get_particles(fire)
	assert(p.rate == 40 and p.max_particles == 128 and p.playing and p.loop and p.world_space)
	assert(near(p.lifetime[1], 0.5) and near(p.lifetime[2], 0.9) and near(p.size[1], 0.5) and near(p.size[2], 0.1))
	assert(near(p.spread, math.rad(12)) and p.direction == vector.create(0, 1, 0))
	wpe.set_particles(fire, { rate = 60, size = { 0.6, 0.1 } })
	assert(wpe.get_particles(fire).rate == 60)
	assert(not pcall(wpe.set_particles, fire, { lifetime = { 2, 1 } })) -- low above high
	assert(not pcall(wpe.set_particles, fire, { spread = 4 })) -- more than pi
	assert(wpe.emit_particles(fire, 5) == 5)
	assert(wpe.particle_count(fire) >= 5)
	wpe.stop_particles(fire, true)
	assert(wpe.particle_count(fire) == 0 and not wpe.get_particles(fire).playing)
	wpe.play_particles(fire)
	assert(wpe.get_particles(fire).playing)
	self.puff = wpe.spawn("tour_puff")
	wpe.set_particles(self.puff, { rate = 0, burst = 10, duration = 0.5, loop = false })
	assert(wpe.get_particles(self.puff).burst == 10)
	assert(not pcall(wpe.emit_particles, self.child, 1)) -- no emitter
	self.fire = fire

	-- UI: the scene's HUD (a nine-sliced panel with wrapped text and a button). The UI was laid out at the
	-- start of this frame, so rectangles are known and buttons can take the focus.
	local hud, hud_text, button = wpe.find("hud"), wpe.find("hud text"), wpe.find("hud button")
	assert(wpe.get_text(hud_text) == "UI over 3D")
	wpe.set_text(hud_text, "API tour")
	assert(wpe.get_text(hud_text) == "API tour")
	wpe.set_text_color(hud_text, wpe.color(1, 1, 0.6))
	wpe.set_sprite_color(hud, wpe.color(0.1, 0.2, 0.4, 0.9))
	assert(not pcall(wpe.set_text, hud, "x")) -- the panel has no Text
	assert(not pcall(wpe.set_sprite_color, hud_text, 0)) -- and the text no Sprite
	local x0, y0, x1, y1 = wpe.get_ui_offsets(hud)
	assert(x0 == -110 and y0 == 14 and x1 == -6 and y1 == 50)
	local rx, ry, rw, rh = wpe.ui_rect(hud)
	assert(rw == 104 and rh == 36 and ry == 14 and rx == width - 110)
	wpe.set_ui_offsets(hud, x0, y0 + 4, x1, y1 + 4) -- takes effect at the next layout
	assert(select(2, wpe.get_ui_offsets(hud)) == 18)
	assert(wpe.is_ui_visible(hud))
	wpe.set_ui_visible(hud, false)
	assert(not wpe.is_ui_visible(hud))
	wpe.set_ui_visible(hud, true)
	assert(wpe.ui_focused() == nil)
	assert(wpe.ui_focus(button) and wpe.ui_focused() == button)
	wpe.set_interactable(button, false) -- the next update takes the focus away
	assert(not wpe.ui_focus(button))
	wpe.set_interactable(button, true)
	assert(wpe.ui_focus(nil) and wpe.ui_focused() == nil)
	self.hud = hud

	self.updates = 0
	self.fixed = 0
end

function Tour:on_update(dt)
	self.updates += 1
	if self.updates == 10 then
		wpe.destroy(self.temp)
		wpe.stop_animation(self.figure)
	elseif self.updates == 11 then
		assert(not wpe.is_alive(self.temp), "destroy should have completed at the end of the previous update")
	elseif self.updates == 20 then
		wpe.play_music("music/ambient.wav", 0.5, true) -- crossfades from the spawner's track
	elseif self.updates == 25 then
		wpe.remove_audio_source(self.speaker)
		wpe.stop_music(0.25)
		wpe.play_music("music/ambient.wav", 0, true)
	elseif self.updates == 2 then
		assert(select(2, wpe.ui_rect(self.hud)) == 18, "the moved HUD should be laid out again")
	elseif self.updates == 3 then
		-- The puff's single burst went out at its first update; it emits nothing more.
		assert(wpe.particle_count(self.puff) == 10, "the burst should be out")
	elseif self.updates == 60 then
		assert(wpe.particle_count(self.puff) == 0, "the puff's particles should have expired")
		assert(not wpe.get_particles(self.puff).playing)
		assert(wpe.particle_count(self.fire) > 10, "the fire should be burning")
		wpe.remove_particles(self.puff)
		assert(wpe.get_particles(self.puff) == nil)
	elseif self.updates == 120 then
		local shown = 0
		for _ in self.coin_frames do
			shown += 1
		end
		assert(shown >= 3, "the coin's sprite sheet should animate")
	elseif self.updates == 130 then
		assert(wpe.get_position(self.ball).y < 0.55, "the ball should rest on the ground")
		local walker = wpe.get_character(self.walker)
		assert(walker.grounded and walker.ground_normal.y > 0.99, "the walker should be on the ground")
		assert(vector.magnitude(wpe.get_position(self.walker) - self.walker_start) > 0.5, "the walker should walk")
		assert(self.timer_hits == 1, "a one-shot timer fires once")
		assert(self.ticks >= 6, "a repeating timer keeps firing")
		assert(wpe.cancel(self.ticker))
	elseif self.updates == 140 then
		wpe.remove_character(self.walker)
		assert(not pcall(wpe.get_character, self.walker))
	elseif self.updates == 150 then
		-- A Judgement Cut through every phase (scripts/lib/judgement_cut.luau): cuts, hold, shatter.
		self.effects_before_cut = #wpe.get_postfx()
		self.cut = JudgementCut.start(0.5, 0.5, { lines = 6, interval = 0.02, hold = 0.05, shatter_time = 0.3, slot = 4 })
	elseif self.updates == 260 then
		assert(self.cut_phases.cutting and self.cut_phases.holding and self.cut_phases.shattering, "every phase ran")
		assert(self.cut == nil and #wpe.get_postfx() == self.effects_before_cut, "the cut ended and cleaned up")
	end
	if self.cut then
		self.cut_phases[self.cut.phase] = true
		if not self.cut:update(dt) then
			self.cut = nil
		end
	end
	self.coin_frames[wpe.get_billboard(self.coin).frame] = true
	assert(wpe.time() >= 0 and wpe.frame() >= 0)
	wpe.draw_rect(256, 0, 64, 10, wpe.color(0.1, 0.2, 0.4, 0.7))
	wpe.draw_text(258, 2, "tour " .. self.updates, wpe.color(1, 1, 1), 1)
end

-- Called by wpe.send and wpe.broadcast in on_start.
function Tour:add(a, b)
	return a + b
end

function Tour:on_tour_ping(value)
	self.pinged = value
end

function Tour:on_fixed_update(dt)
	self.fixed += 1
	wpe.rotate(self.entity, vector.create(0, 1, 0), dt)
	if self.updates < 140 then
		local t = self.fixed * dt
		local v = vector.create(math.cos(t) * 1.5, self.walker_velocity.y - 9.81 * dt, math.sin(t) * 1.5)
		local grounded
		self.walker_velocity, grounded = wpe.move_character(self.walker, v, dt)
		assert(type(grounded) == "boolean")
	end
	-- Report progress through the entity name so the host side can check both callbacks ran.
	wpe.set_name(self.entity, string.format("tour:%d:%d", self.updates, self.fixed))
end

function Tour:on_destroy()
	wpe.destroy(self.pointer)
	wpe.destroy(self.child)
	wpe.destroy(self.figure)
	wpe.destroy(self.ball)
	wpe.destroy(self.pillar)
	wpe.destroy(self.walker)
	wpe.destroy(self.speaker)
	wpe.destroy(self.marker)
	wpe.destroy(self.puff)
end

return Tour
