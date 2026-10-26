-- Falls under gravity (fixed step), tumbles, and removes itself after landing.
local Falling = {}

function Falling:on_start()
	self.velocity = vector.create(0, 0, 0)
	self.landed_at = nil
end

function Falling:on_fixed_update(dt)
	if self.landed_at then
		return
	end
	self.velocity += vector.create(0, -9.81, 0) * dt
	local p = wpe.get_position(self.entity) + self.velocity * dt
	if p.y <= 0.175 then
		p = vector.create(p.x, 0.175, p.z)
		self.landed_at = wpe.time()
	end
	wpe.set_position(self.entity, p)
	wpe.rotate(self.entity, vector.create(1, 0, 0), dt * 3)
end

function Falling:on_update(dt)
	if self.landed_at and wpe.time() - self.landed_at > 2 then
		wpe.destroy(self.entity)
	end
end

return Falling
