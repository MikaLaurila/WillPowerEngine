-- Spins its entity around the Y axis.
-- Edit this file while the game runs: the editor and `wpe_runtime --watch` reload it.
local Spin = {}
-- Declared properties: every entity using the script gets them, and the editor shows them.
Spin.properties = { speed = 1 } -- radians per second

function Spin:on_update(dt)
	wpe.rotate(self.entity, vector.create(0, 1, 0), self.speed * dt)
end

return Spin
