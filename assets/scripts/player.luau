-- A rolling ball the player drives: moves relative to the camera (move_x / move_y), jumps when it
-- stands on something (jump), boosts (boost), throws crates (fire) and rumbles the gamepad on hard
-- landings. Properties (set in the scene file): speed, jump_speed, player (1-4: whose input).
local Player = {}

function Player:on_start()
	self.speed = self.speed or 6
	self.jump_speed = self.jump_speed or 6.5
	self.player = self.player or 1
	self.spawn = wpe.get_position(self.entity)
	self.jump_requested = false
	self.fall_speed = 0
	self.cooldown = 0
	self.thrown = {}
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

local function grounded(self)
	-- Cast from just below the ball's center so the ray starts outside its own collider.
	local p = wpe.get_world_position(self.entity) - vector.create(0, 0.45, 0)
	local hit = wpe.raycast(p, vector.create(0, -1, 0), 0.2)
	return hit ~= nil
end

function Player:on_update(dt)
	-- Edges ("pressed") are per frame, so they are read here and consumed by the fixed update.
	if wpe.action_pressed("jump", self.player) then
		self.jump_requested = true
	end
	if wpe.action_pressed("reset", self.player) or wpe.get_position(self.entity).y < -20 then
		wpe.set_position(self.entity, self.spawn)
		wpe.set_velocity(self.entity, vector.create(0, 0, 0))
	end
	self.cooldown = math.max(0, self.cooldown - dt)
	if wpe.action_down("fire", self.player) and self.cooldown == 0 then
		self.cooldown = 0.25
		self:throw()
	end
	if wpe.action_pressed("rumble_test", self.player) then
		wpe.rumble(0.4, 0.8, 0.3, self.player)
	end
end

function Player:throw()
	local forward = camera_axes()
	local origin = wpe.get_world_position(self.entity) + forward * 0.9 + vector.create(0, 0.6, 0)
	local crate = wpe.instantiate("crate", origin)
	wpe.set_scale(crate, 0.4)
	wpe.add_rigidbody(crate, { type = wpe.BODY_DYNAMIC, velocity = forward * 11 + vector.create(0, 3, 0) })
	wpe.add_collider(crate, { shape = wpe.COLLIDER_BOX, size = vector.create(1, 1, 1), density = 0.5 })
	wpe.attach_script(crate, "scripts/falling.luau")
	wpe.play_sound("sounds/chime.wav", { volume = 0.25, pitch = 1.6 })
end

function Player:on_fixed_update(dt)
	local forward, right = camera_axes()
	local wish = right * wpe.action_value("move_x", self.player) + forward * wpe.action_value("move_y", self.player)
	if vector.magnitude(wish) > 1 then
		wish = vector.normalize(wish)
	end
	local boost = 1 + 0.8 * wpe.action_value("boost", self.player)
	local v = wpe.get_velocity(self.entity)
	local on_ground = grounded(self)
	-- Steer the horizontal velocity toward the wished one (less control in the air).
	local target = wish * self.speed * boost
	local blend = math.min(1, dt * (on_ground and 10 or 2.5))
	local vy = v.y
	if self.jump_requested and on_ground then
		vy = self.jump_speed
		wpe.play_sound("sounds/thud.wav", { volume = 0.3, pitch = 1.8 })
	end
	self.jump_requested = false
	wpe.set_velocity(self.entity, vector.create(v.x + (target.x - v.x) * blend, vy, v.z + (target.z - v.z) * blend))
	self.fall_speed = -math.min(v.y, 0)
end

function Player:on_collision_enter(other)
	-- Hard landings rumble: stronger and longer for bigger falls.
	if self.fall_speed > 4 then
		local strength = math.clamp((self.fall_speed - 4) / 8, 0.15, 1)
		wpe.rumble(strength * 0.6, strength, 0.08 + strength * 0.2, self.player)
		wpe.play_sound("sounds/thud.wav", { volume = strength, pitch = 0.7 })
	end
end

return Player
