-- A trigger area: chimes and flashes its light whenever something enters, and counts visitors.
-- Properties: pitch (chime pitch).
local Zone = {}

function Zone:on_start()
	self.pitch = self.pitch or 1
	self.visits = 0
	self.flash = 0
	wpe.set_light(self.entity, { type = wpe.LIGHT_POINT, color = vector.create(0.3, 1, 0.5), intensity = 0, range = 5 })
end

function Zone:on_trigger_enter(other)
	self.visits += 1
	self.flash = 1
	wpe.play_sound("sounds/chime.wav", { position = wpe.get_world_position(self.entity), pitch = self.pitch, volume = 0.6 })
end

function Zone:on_trigger_exit(other)
	self.flash = math.max(self.flash, 0.4)
end

function Zone:on_update(dt)
	self.flash = math.max(0, self.flash - dt * 1.5)
	wpe.set_light(self.entity, { intensity = 0.3 + self.flash * 3 })
end

return Zone
