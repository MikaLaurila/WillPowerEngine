-- A walking character on a character controller: moves relative to the camera (move_x / move_y),
-- runs (boost), jumps when standing (jump), faces the way it walks, and goes back to its start
-- (reset, or after a fall). Slopes up to the controller's max_slope are walkable, steeper ones
-- aren't; crates get pushed.
local Hero = {}
Hero.properties = { speed = 4, run_speed = 7.5, jump_speed = 6.5, gravity = 18, player = 1 }

function Hero:on_start()
	self.spawn = wpe.get_position(self.entity)
	self.velocity = vector.create(0, 0, 0)
	self.jump_requested = false
	self.yaw = wpe.get_rotation(self.entity).y
end

-- Horizontal unit vectors of the camera's view (forward along the ground, and right).
local function camera_axes()
	local camera = wpe.get_camera()
	local forward = vector.create(0, 0, -1)
	if camera then
		local f = wpe.forward(camera)
		local flat = vector.create(f.x, 0, f.z)
		if vector.magnitude(flat) > 1e-3 then
			forward = vector.normalize(flat)
		end
	end
	return forward, vector.create(-forward.z, 0, forward.x)
end

function Hero:on_update(dt)
	-- Edges ("pressed") are per frame, so they are read here and consumed by the fixed update.
	if wpe.action_pressed("jump", self.player) then
		self.jump_requested = true
	end
	if wpe.action_pressed("reset", self.player) or wpe.get_position(self.entity).y < -20 then
		wpe.set_position(self.entity, self.spawn)
		self.velocity = vector.create(0, 0, 0)
	end
end

function Hero:on_fixed_update(dt)
	local forward, right = camera_axes()
	local wish = right * wpe.action_value("move_x", self.player) + forward * wpe.action_value("move_y", self.player)
	if vector.magnitude(wish) > 1 then
		wish = vector.normalize(wish)
	end
	local speed = self.speed + (self.run_speed - self.speed) * wpe.action_value("boost", self.player)
	local grounded = wpe.get_character(self.entity).grounded

	-- Steer toward the wished velocity (less control in the air), and fall.
	local target = wish * speed
	local blend = math.min(1, dt * (grounded and 12 or 3))
	local v = self.velocity
	v = vector.create(v.x + (target.x - v.x) * blend, v.y - self.gravity * dt, v.z + (target.z - v.z) * blend)
	if self.jump_requested and grounded then
		v = vector.create(v.x, self.jump_speed, v.z)
	end
	self.jump_requested = false
	-- The controller slides along what it hits and returns what is left of the velocity.
	self.velocity = wpe.move_character(self.entity, v, dt)

	-- Turn to face the way it walks (the entity looks down -Z).
	if vector.magnitude(wish) > 0.1 then
		local goal = math.atan2(-wish.x, -wish.z)
		local turn = (goal - self.yaw + math.pi) % (2 * math.pi) - math.pi
		self.yaw += turn * math.min(1, dt * 12)
		wpe.set_rotation(self.entity, vector.create(0, self.yaw, 0))
	end
end

return Hero
