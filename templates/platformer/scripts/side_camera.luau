-- A side-on camera following the entity named `target` along the level: it looks a little ahead of the
-- way the target runs, and eases after it rather than snapping.
local SideCamera = {}
SideCamera.properties = {
	target = "player",
	distance = 11, -- in front of the level's plane (Z = 0)
	height = 2.6, -- above the target
	lead = 2, -- how far ahead of the target it looks
	smoothing = 4, -- higher follows more tightly
}

function SideCamera:on_start()
	self.followed = wpe.find(self.target)
	self.focus = if self.followed then wpe.get_world_position(self.followed) else vector.create(0, 0, 0)
	self.lead_x = 0
	wpe.set_camera(self.entity)
	wpe.set_rotation(self.entity, vector.create(-0.16, 0, 0)) -- tilted slightly down
end

function SideCamera:on_update(dt)
	if not self.followed or not wpe.is_alive(self.followed) then
		return
	end
	local target = wpe.get_world_position(self.followed)
	local player = wpe.get_script(self.followed)
	local facing = if player and player.facing then player.facing else 1
	self.lead_x += (facing * self.lead - self.lead_x) * math.min(1, dt * 2)
	local goal = vector.create(target.x + self.lead_x, target.y, 0)
	self.focus += (goal - self.focus) * math.min(1, dt * self.smoothing)
	wpe.set_position(self.entity, self.focus + vector.create(0, self.height, self.distance))
end

return SideCamera
