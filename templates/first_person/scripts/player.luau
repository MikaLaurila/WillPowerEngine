-- First-person player on a character controller. The entity is the body (turned by yaw); its child
-- "head" holds the camera (tilted by pitch).
--
--   Look:   the mouse (click the window to capture it, Escape to release) or the right stick
--   Move:   WASD / arrows / left stick; sprint with Shift / left trigger
--   Jump:   Space / A (south)
--   Use:    E / left mouse / right trigger: pushes the crate under the crosshair
--
-- The level resets the player when it falls off. Change the properties in the editor's inspector.
local Player = {}
Player.properties = {
	walk_speed = 4.5, -- m/s
	sprint_speed = 7.5,
	jump_speed = 5.5, -- upward m/s when jumping (with gravity 18: about 0.85 m high)
	gravity = 18, -- m/s², stronger than Earth's for snappier jumps
	mouse_sensitivity = 0.005, -- radians per screen pixel
	stick_speed = 3, -- radians per second at full deflection
	reach = 3, -- how far "use" reaches, in meters
	push = 5, -- the impulse "use" gives (a 1 m crate weighs 1 kg)
}

local MAX_PITCH = 1.5 -- radians: almost straight up and down

function Player:on_start()
	for _, child in wpe.children(self.entity) do
		if wpe.get_name(child) == "head" then
			self.head = child
		end
	end
	assert(self.head, "the player needs a child entity named 'head' with a camera")
	wpe.set_camera(self.head)
	self.spawn = wpe.get_position(self.entity)
	self.yaw = wpe.get_rotation(self.entity).y
	self.pitch = wpe.get_rotation(self.head).x
	self.velocity = vector.create(0, 0, 0)
	self.jump_requested = false
end

-- What the crosshair points at within reach: the raycast hit, and whether it can be pushed.
function Player:aim()
	local origin = wpe.get_world_position(self.head)
	local direction = wpe.forward(self.head)
	local hit = wpe.raycast(origin, direction, self.reach, { ignore = self.entity })
	if not hit then
		return nil, false
	end
	local body = wpe.get_rigidbody(hit.entity)
	return hit, body ~= nil and body.type == wpe.BODY_DYNAMIC
end

function Player:look(dt)
	local just_captured = false
	if wpe.is_mouse_captured() then
		if wpe.action_pressed("release_mouse") then
			wpe.set_mouse_captured(false)
		else
			local dx, dy = wpe.mouse_delta()
			self.yaw -= dx * self.mouse_sensitivity
			self.pitch -= dy * self.mouse_sensitivity
		end
	elseif wpe.mouse_pressed("left") then
		wpe.set_mouse_captured(true)
		just_captured = true -- this click captures the mouse rather than using things
	end
	-- The right stick: up looks up (the stick's Y axis points down).
	self.yaw -= wpe.action_value("look_x") * self.stick_speed * dt
	self.pitch -= wpe.action_value("look_y") * self.stick_speed * dt
	self.pitch = math.clamp(self.pitch, -MAX_PITCH, MAX_PITCH)
	wpe.set_rotation(self.entity, vector.create(0, self.yaw, 0))
	wpe.set_rotation(self.head, vector.create(self.pitch, 0, 0))
	return just_captured
end

function Player:draw_hud(pushable)
	local width, height = wpe.screen_size()
	local cx, cy = width // 2, height // 2
	local color = pushable and wpe.color(1, 0.65, 0.2) or wpe.color(1, 1, 1, 0.8)
	wpe.draw_rect(cx - 4, cy, 3, 1, color)
	wpe.draw_rect(cx + 2, cy, 3, 1, color)
	wpe.draw_rect(cx, cy - 4, 1, 3, color)
	wpe.draw_rect(cx, cy + 2, 1, 3, color)
	if not wpe.is_mouse_captured() then
		local text = "Click to look around"
		local tw = wpe.text_size(text)
		wpe.draw_text((width - tw) // 2, cy + 12, text)
	end
	wpe.draw_text(4, height - 12, "WASD move  Shift sprint  Space jump  E push", wpe.color(1, 1, 1, 0.7))
end

function Player:on_update(dt)
	-- Edges ("pressed") belong to frames: remember a jump here, for the next fixed step.
	if wpe.action_pressed("jump") then
		self.jump_requested = true
	end
	local just_captured = self:look(dt)

	local hit, pushable = self:aim()
	if pushable and wpe.action_pressed("use") and not just_captured then
		local direction = wpe.forward(self.head)
		wpe.apply_impulse(hit.entity, direction * self.push)
	end

	if wpe.get_position(self.entity).y < -20 then
		wpe.set_position(self.entity, self.spawn)
		self.velocity = vector.create(0, 0, 0)
	end
	self:draw_hud(pushable)
end

function Player:on_fixed_update(dt)
	-- Ground-plane axes of the body's yaw (the entity looks down -Z).
	local forward = vector.create(-math.sin(self.yaw), 0, -math.cos(self.yaw))
	local right = vector.create(math.cos(self.yaw), 0, -math.sin(self.yaw))
	local wish = right * wpe.action_value("move_x") + forward * wpe.action_value("move_y")
	if vector.magnitude(wish) > 1 then
		wish = vector.normalize(wish)
	end
	local sprint = math.clamp(wpe.action_value("sprint"), 0, 1)
	local speed = self.walk_speed + (self.sprint_speed - self.walk_speed) * sprint
	local grounded = wpe.get_character(self.entity).grounded

	-- Steer toward the wished velocity (with little control in the air), and fall.
	local target = wish * speed
	local blend = math.min(1, dt * (grounded and 14 or 2))
	local v = self.velocity
	v = vector.create(v.x + (target.x - v.x) * blend, v.y - self.gravity * dt, v.z + (target.z - v.z) * blend)
	if self.jump_requested and grounded then
		v = vector.create(v.x, self.jump_speed, v.z)
	end
	self.jump_requested = false
	-- The controller slides along what it hits and returns what is left of the velocity.
	self.velocity = wpe.move_character(self.entity, v, dt)
end

return Player
