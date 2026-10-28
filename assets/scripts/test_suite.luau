-- Edge cases for the master test scene (scenes/test_full.scene), run over 60 simulated seconds:
-- extreme transforms, a mass spawn and destroy, prefab instantiate/destroy cycles, camera
-- switching, the input API without input, and on-screen progress. Progress is reported through the
-- entity's name ("suite:<checks>:<cycles>"); any failed check raises an error, which the scene
-- test counts.
local Suite = {}
Suite.properties = { spawn_count = 2000 }

local function near(a, b, eps)
	return math.abs(a - b) <= (eps or 1e-4)
end

function Suite:check(condition, what)
	assert(condition, what)
	self.checks += 1
end

function Suite:on_start()
	self.checks = 0
	self.cycles = 0
	self.updates = 0
	self.spawned = {}
	self.prefabs = {}

	-- Scene contents are reachable by name.
	self.main_camera = wpe.find("main camera")
	self.top_camera = wpe.find("top camera")
	self:check(self.main_camera ~= nil and self.top_camera ~= nil, "cameras")
	self:check(wpe.get_camera() == self.main_camera, "the scene file selects the main camera")
	self:check(wpe.find("no such entity") == nil, "missing names")

	-- Extreme transforms survive loading and stay usable.
	local far = wpe.find("far")
	self:check(near(wpe.get_position(far).x, 100000, 0.01) and near(wpe.get_position(far).z, -100000, 0.01), "far")
	self:check(near(wpe.get_scale(wpe.find("tiny")).x, 0.0001, 1e-9), "tiny")
	self:check(near(wpe.get_scale(wpe.find("huge")).y, 1000), "huge")

	-- Hierarchy from the file.
	local group = wpe.find("group")
	self:check(#wpe.children(group) == 2, "group children")
	self:check(wpe.get_world_position(wpe.find("group child b")).x > wpe.get_position(group).x, "child placement")

	-- Every project action exists; player 4 owns no device in the test (keyboard is player 1).
	for _, action in { "move_x", "move_y", "look_x", "look_y", "jump", "fire", "boost", "reset", "switch_scene" } do
		self:check(wpe.action_value(action, 4) == 0 and not wpe.action_down(action, 4), "idle action " .. action)
	end
	local w, h = wpe.screen_size()
	self:check(w == 320 and h == 240, "screen size")
end

function Suite:on_update(dt)
	self.updates += 1
	local u = self.updates

	if u == 30 then
		-- Mass spawn: thousands of entities in one frame...
		for i = 1, self.spawn_count do
			local e = wpe.spawn()
			wpe.set_position(e, vector.create(i % 50, 0, i // 50))
			self.spawned[i] = e
		end
		self:check(wpe.is_alive(self.spawned[self.spawn_count]), "mass spawn")
	elseif u == 31 then
		-- ...destroyed again (deferred to the end of this update).
		for _, e in self.spawned do
			wpe.destroy(e)
		end
	elseif u == 32 then
		self:check(not wpe.is_alive(self.spawned[1]) and not wpe.is_alive(self.spawned[self.spawn_count]), "mass destroy")
		self.spawned = {}
	end

	-- Prefab cycles: every second, replace a figure (animated) and a ball (physics).
	if u % 30 == 0 then
		for _, e in self.prefabs do
			wpe.destroy(e)
		end
		local x = (self.cycles % 5) * 2 - 4
		local figure = wpe.instantiate("figure", vector.create(x, 0, 6))
		wpe.play_animation(figure, 1, true)
		local ball = wpe.instantiate("ball", vector.create(x, 4, 7))
		wpe.add_rigidbody(ball, { type = wpe.BODY_DYNAMIC, velocity = vector.create(0, 2, 0) })
		wpe.add_collider(ball, { shape = wpe.COLLIDER_SPHERE, radius = 0.5, restitution = 0.4 })
		self.prefabs = { figure, ball }
		self.cycles += 1
	end

	-- Camera switching: the top camera for a moment every ten seconds.
	if u % 300 == 100 then
		wpe.set_camera(self.top_camera)
		self:check(wpe.get_camera() == self.top_camera, "camera switch")
	elseif u % 300 == 110 then
		wpe.set_camera(self.main_camera)
	end

	wpe.draw_rect(0, 211, 320, 9, wpe.color(0, 0, 0, 0.5))
	wpe.draw_text(2, 212, string.format("test scene  %.1f s  %d checks  %d cycles", wpe.time(), self.checks, self.cycles))
	wpe.set_name(self.entity, string.format("suite:%d:%d", self.checks, self.cycles))
end

function Suite:on_destroy()
	for _, e in self.prefabs do
		wpe.destroy(e)
	end
end

return Suite
