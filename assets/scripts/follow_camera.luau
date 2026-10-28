-- Third-person camera orbiting a target entity (found by name once). Mouse look while the mouse is
-- captured (click the window to capture, Escape to release), right stick on a gamepad.
local Camera = {}
-- target: an entity name, resolved to an entity when found.
Camera.properties = { target = "player", distance = 6, height = 2.5, sensitivity = 0.006 }

function Camera:on_start()
	self.followed = wpe.find(self.target)
	self.yaw = 0
	self.pitch = -0.35
	wpe.set_camera(self.entity)
end

function Camera:on_update(dt)
	if not self.followed or not wpe.is_alive(self.followed) then
		self.followed = wpe.find(self.target)
		if not self.followed then
			return
		end
	end
	if wpe.is_mouse_captured() then
		local dx, dy = wpe.mouse_delta()
		self.yaw -= dx * self.sensitivity
		self.pitch -= dy * self.sensitivity
	end
	self.yaw -= wpe.action_value("look_x") * 2.5 * dt
	self.pitch -= wpe.action_value("look_y") * 1.8 * dt
	self.pitch = math.clamp(self.pitch, -1.2, 0.3)

	local focus = wpe.get_world_position(self.followed) + vector.create(0, 0.5, 0)
	local cp = math.cos(self.pitch)
	local offset = vector.create(math.sin(self.yaw) * cp, -math.sin(self.pitch), math.cos(self.yaw) * cp)
	local position = focus + offset * self.distance + vector.create(0, self.height - 1.5, 0)
	wpe.set_position(self.entity, position)
	wpe.look_at(self.entity, focus)
end

return Camera
