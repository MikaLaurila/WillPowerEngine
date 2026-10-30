-- Third-person player on a character controller. It moves relative to the camera (move_x / move_y),
-- sprints (sprint), jumps when standing (jump), turns to face the way it goes, and starts over at its
-- spawn point after a fall.
--
-- The model is the child entity named by `model` (here the "figure" instance). Its animations are
-- chosen by name: `move_animation` plays while walking, faster with speed; `idle_animation` while
-- standing; `jump_animation` in the air. Empty names fall back to the model's first animation (for
-- idle: held at its first frame). To use your own character, replace the figure's model and set the
-- names in the inspector.
local Player = {}
Player.properties = {
	walk_speed = 3.5, -- m/s
	sprint_speed = 6.5,
	jump_speed = 6, -- upward m/s when jumping
	gravity = 18, -- m/s²
	turn_speed = 12, -- how quickly it turns to face its way
	model = "figure", -- the child entity with the animated model
	idle_animation = "",
	move_animation = "",
	jump_animation = "",
	move_animation_speed = 1, -- the move animation's playback rate at walk_speed
	fade = 0.2, -- seconds to crossfade between animations
}

-- The first entity named `name` under `root`, searched depth first.
local function find_child(root: number, name: string): number?
	for _, child in wpe.children(root) do
		if wpe.get_name(child) == name then
			return child
		end
		local found = find_child(child, name)
		if found then
			return found
		end
	end
	return nil
end

-- The entity at or below `root` that plays the model's animations (has an Animator), or nil.
local function find_animated(root: number): number?
	if pcall(wpe.get_animation, root) then
		return root
	end
	for _, child in wpe.children(root) do
		local found = find_animated(child)
		if found then
			return found
		end
	end
	return nil
end

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

-- The shortest turn from angle `from` to angle `to` (radians), in [-pi, pi).
local function angle_delta(from: number, to: number): number
	return (to - from + math.pi) % (2 * math.pi) - math.pi
end

function Player:on_start()
	local model = find_child(self.entity, self.model)
	self.figure = if model then find_animated(model) else nil
	self.spawn = wpe.get_position(self.entity)
	self.yaw = wpe.get_rotation(self.entity).y
	self.velocity = vector.create(0, 0, 0)
	self.jump_requested = false
	self.state = nil -- "idle", "move" or "jump": the animation showing
end

-- Switches the model's animation to the one for `state` (crossfading), and sets its playback rate.
function Player:animate(state: string, rate: number)
	if not self.figure then
		return
	end
	if state ~= self.state then
		self.state = state
		local name = self[state .. "_animation"]
		wpe.play_animation(self.figure, if name ~= "" then name else 1, state ~= "jump", self.fade)
	end
	-- Without its own idle animation, the figure stands in the first frame of the first one.
	local hold = self.state == "idle" and self.idle_animation == ""
	wpe.animation_speed(self.figure, if hold then 0 else rate)
end

function Player:on_update(dt)
	-- Edges ("pressed") belong to frames: remember a jump here, for the next fixed step.
	if wpe.action_pressed("jump") then
		self.jump_requested = true
	end
	if wpe.get_position(self.entity).y < -20 then
		wpe.set_position(self.entity, self.spawn)
		self.velocity = vector.create(0, 0, 0)
	end

	local grounded = wpe.get_character(self.entity).grounded
	local speed = vector.magnitude(vector.create(self.velocity.x, 0, self.velocity.z))
	if not grounded and self.jump_animation ~= "" then
		self:animate("jump", 1)
	elseif speed > 0.3 then
		self:animate("move", self.move_animation_speed * speed / self.walk_speed)
	else
		self:animate("idle", 1)
	end
end

function Player:on_fixed_update(dt)
	local forward, right = camera_axes()
	local wish = right * wpe.action_value("move_x") + forward * wpe.action_value("move_y")
	if vector.magnitude(wish) > 1 then
		wish = vector.normalize(wish)
	end
	local sprint = math.clamp(wpe.action_value("sprint"), 0, 1)
	local speed = self.walk_speed + (self.sprint_speed - self.walk_speed) * sprint
	local grounded = wpe.get_character(self.entity).grounded

	-- Steer toward the wished velocity (with less control in the air), and fall.
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
		self.yaw += angle_delta(self.yaw, goal) * math.min(1, dt * self.turn_speed)
		wpe.set_rotation(self.entity, vector.create(0, self.yaw, 0))
	end
end

return Player
