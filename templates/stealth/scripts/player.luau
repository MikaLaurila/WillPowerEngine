-- The player: a third-person character on a character controller (as in the third_person template),
-- which moves relative to the camera (move_x / move_y), sprints (sprint) and jumps (jump).
--
-- Stealth adds noise: sprinting and landing from a jump make sounds the guards hear (wpe.broadcast of
-- on_noise, with where and how far it carries). Walking is silent. The game freezes the player (`frozen`)
-- once it is caught or escapes.
local Figure = require("scripts/figure")

local Player = {}
Player.properties = {
	walk_speed = 3, -- m/s
	sprint_speed = 6,
	jump_speed = 6,
	gravity = 18,
	turn_speed = 12,
	sprint_noise = 9, -- how far (m) sprinting footsteps carry
	landing_noise = 6, -- and a landing
	model = "figure",
}

local STEP_INTERVAL = 0.3 -- seconds between sprinting footsteps

-- Horizontal unit vectors of the camera's view: forward along the ground, and right.
local function camera_axes(): (vector, vector)
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

local function angle_delta(from: number, to: number): number
	return (to - from + math.pi) % (2 * math.pi) - math.pi
end

function Player:on_start()
	self.figure = Figure.find(self.entity, self.model)
	self.animation = {}
	self.yaw = wpe.get_rotation(self.entity).y
	self.velocity = vector.create(0, 0, 0)
	self.jump_requested = false
	self.step_timer = 0
	self.was_grounded = true
	self.frozen = false
	self.noise = 0 -- the last noise's radius, fading (the HUD shows it)
end

-- A sound the guards may hear: they come to look if they are within `radius`.
function Player:make_noise(radius: number)
	self.noise = math.max(self.noise, radius)
	wpe.broadcast("on_noise", wpe.get_world_position(self.entity), radius)
end

function Player:on_update(dt)
	if wpe.action_pressed("jump") and not self.frozen then
		self.jump_requested = true
	end
	self.noise = math.max(0, self.noise - dt * 12)
	local speed = vector.magnitude(vector.create(self.velocity.x, 0, self.velocity.z))
	Figure.animate(self.figure, self.animation, speed, self.walk_speed)
end

function Player:on_fixed_update(dt)
	local forward, right = camera_axes()
	local wish = right * wpe.action_value("move_x") + forward * wpe.action_value("move_y")
	if self.frozen then
		wish = vector.create(0, 0, 0)
	end
	if vector.magnitude(wish) > 1 then
		wish = vector.normalize(wish)
	end
	local sprint = if self.frozen then 0 else math.clamp(wpe.action_value("sprint"), 0, 1)
	local speed = self.walk_speed + (self.sprint_speed - self.walk_speed) * sprint
	local character = wpe.get_character(self.entity)
	local grounded = character.grounded

	local target = wish * speed
	local blend = math.min(1, dt * (grounded and 12 or 3))
	local v = self.velocity
	v = vector.create(v.x + (target.x - v.x) * blend, v.y - self.gravity * dt, v.z + (target.z - v.z) * blend)
	if self.jump_requested and grounded then
		v = vector.create(v.x, self.jump_speed, v.z)
	end
	self.jump_requested = false
	local falling = self.velocity.y
	self.velocity = wpe.move_character(self.entity, v, dt)
	grounded = wpe.get_character(self.entity).grounded

	-- Noise: sprinting footsteps, and landing after a real fall.
	local ground_speed = vector.magnitude(vector.create(self.velocity.x, 0, self.velocity.z))
	self.step_timer -= dt
	if grounded and sprint > 0.5 and ground_speed > self.walk_speed + 0.5 and self.step_timer <= 0 then
		self.step_timer = STEP_INTERVAL
		self:make_noise(self.sprint_noise)
	end
	if grounded and not self.was_grounded and falling < -4 then
		self:make_noise(self.landing_noise)
	end
	self.was_grounded = grounded

	if vector.magnitude(wish) > 0.1 then
		local goal = math.atan2(-wish.x, -wish.z)
		self.yaw += angle_delta(self.yaw, goal) * math.min(1, dt * self.turn_speed)
		wpe.set_rotation(self.entity, vector.create(0, self.yaw, 0))
	end
end

return Player
