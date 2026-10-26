-- Spins its entity around an axis and bobs it up and down.
local Spinner = {}

function Spinner:on_start()
	self.axis = vector.normalize(vector.create(0.3, 1, 0.2))
	self.speed = 1.2
	self.base = wpe.get_position(self.entity)
	self.phase = (self.entity % 7) * 0.9
end

function Spinner:on_update(dt)
	wpe.rotate(self.entity, self.axis, self.speed * dt)
	local bob = math.sin(wpe.time() * 2 + self.phase) * 0.15
	wpe.set_position(self.entity, self.base + vector.create(0, bob, 0))
end

-- Called after hot reload; state such as self.base is preserved.
function Spinner:on_reload()
	wpe.log("spinner reloaded on entity", self.entity)
end

return Spinner
