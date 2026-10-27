-- Exercises every function of the `wpe` scripting API. Part of the master test scene
-- (scenes/test_full.scene), which the scene test runs for 60 simulated seconds, and a living
-- reference. Fails loudly via assert. wpe.load_scene and wpe.quit end the scene, so the engine tests
-- cover them instead.
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

	self.pillar = wpe.spawn("tour_pillar")
	wpe.set_position(self.pillar, vector.create(-8, 1, -8))
	wpe.add_rigidbody(self.pillar, { type = wpe.BODY_KINEMATIC })
	wpe.add_collider(self.pillar, { shape = wpe.COLLIDER_CAPSULE, radius = 0.3, height = 2, offset = vector.create(0, 0, 0) })
	wpe.remove_collider(self.pillar)
	wpe.remove_rigidbody(self.pillar)
	-- Without a rigid body a collider is static level geometry; this one is a trigger hull from a prefab.
	wpe.add_collider(self.pillar, { shape = wpe.COLLIDER_CONVEX, model = "box", mesh = 1, trigger = true })

	local gravity = wpe.get_gravity()
	assert(gravity.y < 0)
	wpe.set_gravity(gravity)
	local hit = wpe.raycast(vector.create(20, 5, 20), vector.create(0, -1, 0), 10)
	assert(hit and near(hit.distance, 5, 1e-3) and hit.normal.y > 0.99 and wpe.get_name(hit.entity) == "ground")
	assert(wpe.raycast(vector.create(20, 5, 20), vector.create(0, 1, 0)) == nil)

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
	elseif self.updates == 120 then
		assert(wpe.get_position(self.ball).y < 0.55, "the ball should rest on the ground")
	end
	assert(wpe.time() >= 0 and wpe.frame() >= 0)
	wpe.draw_rect(256, 0, 64, 10, wpe.color(0.1, 0.2, 0.4, 0.7))
	wpe.draw_text(258, 2, "tour " .. self.updates, wpe.color(1, 1, 1), 1)
end

function Tour:on_fixed_update(dt)
	self.fixed += 1
	wpe.rotate(self.entity, vector.create(0, 1, 0), dt)
	-- Report progress through the entity name so the host side can check both callbacks ran.
	wpe.set_name(self.entity, string.format("tour:%d:%d", self.updates, self.fixed))
end

function Tour:on_destroy()
	wpe.destroy(self.pointer)
	wpe.destroy(self.child)
	wpe.destroy(self.figure)
	wpe.destroy(self.ball)
	wpe.destroy(self.pillar)
	wpe.destroy(self.speaker)
end

return Tour
