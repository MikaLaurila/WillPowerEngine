-- Turns its entity into a colored point light circling the origin. Properties: radius, speed, height.
local OrbitLight = {}

function OrbitLight:on_start()
	self.radius = self.radius or 3.5
	self.speed = self.speed or 0.7
	self.height = self.height or 0.9
	self.offset = (self.entity % 3) * 2.094
	wpe.set_light(self.entity, { type = wpe.LIGHT_POINT, intensity = 2.5, range = 6 })
end

function OrbitLight:on_update(dt)
	local a = wpe.time() * self.speed + self.offset
	wpe.set_position(self.entity, vector.create(math.cos(a) * self.radius, self.height, math.sin(a) * self.radius))
	-- Slowly cycle the hue.
	local h = wpe.time() * 0.2 + self.offset
	wpe.set_light(self.entity, {
		color = vector.create(0.5 + 0.5 * math.cos(h), 0.5 + 0.5 * math.cos(h + 2.094), 0.5 + 0.5 * math.cos(h + 4.188)),
	})
end

return OrbitLight
