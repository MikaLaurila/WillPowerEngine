-- A camera orbiting the entity named `target`: mouse look while the mouse is captured (click the window
-- to capture it, Escape to release), right stick on a gamepad, mouse wheel to zoom. When a wall comes
-- between the camera and the target, the camera moves in front of it instead of looking through it.
local OrbitCamera = {}
OrbitCamera.properties = {
	target = "player",
	distance = 5, -- meters from the target (the mouse wheel changes it)
	min_distance = 2,
	max_distance = 10,
	height = 1.5, -- the point looked at, above the target's origin (its feet)
	mouse_sensitivity = 0.006, -- radians per screen pixel
	stick_speed = 2.5, -- radians per second at full deflection
}

local MIN_PITCH, MAX_PITCH = -1.2, 0.5 -- radians: from steeply above to slightly below
local CLEARANCE = 0.25 -- how far the camera stays from walls

function OrbitCamera:on_start()
	self.followed = wpe.find(self.target)
	-- Start behind the target, looking the way it does.
	self.yaw = if self.followed then wpe.get_rotation(self.followed).y else 0
	self.pitch = -0.3
	wpe.set_camera(self.entity)
end

function OrbitCamera:on_update(dt)
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
	end
	-- The right stick: up looks up (the stick's Y axis points down).
	self.yaw -= wpe.action_value("look_x") * self.stick_speed * dt
	self.pitch -= wpe.action_value("look_y") * self.stick_speed * dt
	self.pitch = math.clamp(self.pitch, MIN_PITCH, MAX_PITCH)
	self.distance = math.clamp(self.distance - wpe.mouse_wheel() * 0.5, self.min_distance, self.max_distance)

	if not self.followed or not wpe.is_alive(self.followed) then
		self.followed = wpe.find(self.target)
		if not self.followed then
			return
		end
	end
	local focus = wpe.get_world_position(self.followed) + vector.create(0, self.height, 0)
	-- Behind the focus at yaw and pitch: the camera looks down its -Z axis, so it sits on +Z.
	local cp = math.cos(self.pitch)
	local back = vector.create(math.sin(self.yaw) * cp, -math.sin(self.pitch), math.cos(self.yaw) * cp)
	local distance = self.distance
	local hit = wpe.shape_cast({ shape = wpe.COLLIDER_SPHERE, radius = CLEARANCE }, focus, back, distance,
		{ ignore = self.followed })
	if hit then
		distance = math.max(hit.distance, 0.1)
	end
	wpe.set_position(self.entity, focus + back * distance)
	wpe.set_rotation(self.entity, vector.create(self.pitch, self.yaw, 0))

	local width, height = wpe.screen_size()
	if not wpe.is_mouse_captured() then
		local text = "Click to look around"
		local tw = wpe.text_size(text)
		wpe.draw_text((width - tw) // 2, 16, text)
	end
	wpe.draw_text(4, height - 12, "WASD move  Shift sprint  Space jump  Wheel zoom", wpe.color(1, 1, 1, 0.7))
end

return OrbitCamera
