-- Drops a box prefab from the sky every `interval` seconds; each box runs falling.luau (physics).
local Spawner = {}

function Spawner:on_start()
	self.interval = 0.6
	self.timer = 0
	self.count = 0
end

function Spawner:on_fixed_update(dt)
	self.timer += dt
	if self.timer < self.interval then
		return
	end
	self.timer -= self.interval
	self.count += 1
	local x = math.sin(self.count * 1.7) * 3
	local z = math.cos(self.count * 2.3) * 3
	local box = wpe.instantiate("box", vector.create(x, 6, z))
	wpe.set_scale(box, 0.35)
	-- Tilted so boxes land on edges and corners instead of flat.
	wpe.set_rotation(box, vector.create(self.count * 0.9, self.count * 0.4, self.count * 1.3))
	wpe.attach_script(box, "scripts/falling.luau")
end

return Spawner
