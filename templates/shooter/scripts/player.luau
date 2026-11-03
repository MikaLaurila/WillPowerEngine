-- First-person player on a character controller (as in the first_person template). The entity is the
-- body (turned by yaw); its child "head" holds the camera (tilted by pitch), and the gun hangs from the
-- head (scripts/gun.luau).
--
--   Look:   the mouse (click the window to capture it, Escape to release) or the right stick
--   Move:   WASD / arrows / left stick; sprint with Shift / left trigger
--   Jump:   Space / A (south)
--   Use:    E / Y (north): pushes the crate under the crosshair
--
-- The gun reads `velocity` and `grounded` (for its bob), `just_captured` (the click that captured the
-- mouse doesn't shoot) and kicks the view up with `kick`.
local Player = {}
Player.properties = {
	walk_speed = 4.5, -- m/s
	sprint_speed = 7.5,
	jump_speed = 5.5,
	gravity = 18,
	mouse_sensitivity = 0.005, -- radians per screen pixel
	stick_speed = 3, -- radians per second at full deflection
	reach = 3, -- how far "use" reaches, in meters
	push = 5,
	recover = 8, -- how fast the view settles back after a kick
}

local MAX_PITCH = 1.5

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
	self.kick_pitch = 0 -- recoil on top of the aimed pitch, settling back to 0
	self.velocity = vector.create(0, 0, 0)
	self.grounded = true
	self.jump_requested = false
	self.just_captured = false
end

-- Recoil: the view jumps up by `radians` and settles back.
function Player:kick(radians: number)
	self.kick_pitch += radians
end

function Player:look(dt)
	self.just_captured = false
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
		self.just_captured = true -- this click captures the mouse rather than shooting
	end
	self.yaw -= wpe.action_value("look_x") * self.stick_speed * dt
	self.pitch -= wpe.action_value("look_y") * self.stick_speed * dt
	self.pitch = math.clamp(self.pitch, -MAX_PITCH, MAX_PITCH)
	self.kick_pitch -= self.kick_pitch * math.min(1, dt * self.recover)
	wpe.set_rotation(self.entity, vector.create(0, self.yaw, 0))
	wpe.set_rotation(self.head, vector.create(math.clamp(self.pitch + self.kick_pitch, -MAX_PITCH, MAX_PITCH), 0, 0))
end

function Player:on_update(dt)
	if wpe.action_pressed("jump") then
		self.jump_requested = true
	end
	self:look(dt)
	-- Use: push the crate under the crosshair.
	if wpe.action_pressed("use") then
		local direction = wpe.forward(self.head)
		local hit = wpe.raycast(wpe.get_world_position(self.head), direction, self.reach, { ignore = self.entity })
		local body = hit and wpe.get_rigidbody(hit.entity)
		if body and body.type == wpe.BODY_DYNAMIC then
			wpe.apply_impulse(hit.entity, direction * self.push)
		end
	end
	if wpe.get_position(self.entity).y < -20 then
		wpe.set_position(self.entity, self.spawn)
		self.velocity = vector.create(0, 0, 0)
	end
	local width, height = wpe.screen_size()
	if not wpe.is_mouse_captured() then
		local text = "Click to look around"
		wpe.draw_text((width - wpe.text_size(text)) // 2, height // 2 + 12, text)
	end
	wpe.draw_text(4, height - 12, "WASD move  Shift sprint  Click shoot  R reload  E push", wpe.color(1, 1, 1, 0.7))
end

function Player:on_fixed_update(dt)
	local forward = vector.create(-math.sin(self.yaw), 0, -math.cos(self.yaw))
	local right = vector.create(math.cos(self.yaw), 0, -math.sin(self.yaw))
	local wish = right * wpe.action_value("move_x") + forward * wpe.action_value("move_y")
	if vector.magnitude(wish) > 1 then
		wish = vector.normalize(wish)
	end
	local sprint = math.clamp(wpe.action_value("sprint"), 0, 1)
	local speed = self.walk_speed + (self.sprint_speed - self.walk_speed) * sprint
	local grounded = wpe.get_character(self.entity).grounded
	local target = wish * speed
	local blend = math.min(1, dt * (grounded and 14 or 2))
	local v = self.velocity
	v = vector.create(v.x + (target.x - v.x) * blend, v.y - self.gravity * dt, v.z + (target.z - v.z) * blend)
	if self.jump_requested and grounded then
		v = vector.create(v.x, self.jump_speed, v.z)
	end
	self.jump_requested = false
	self.velocity = wpe.move_character(self.entity, v, dt)
	self.grounded = wpe.get_character(self.entity).grounded
end

return Player
