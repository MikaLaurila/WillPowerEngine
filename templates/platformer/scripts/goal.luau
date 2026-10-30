-- The level's goal: touching its trigger finishes the level, and its flag spins in celebration.
local Goal = {}

function Goal:on_trigger_enter(other)
	if wpe.get_name(other) == "player" and not self.reached then
		self.reached = true
		wpe.send(other, "finish")
	end
end

function Goal:on_update(dt)
	if self.reached then
		wpe.rotate(self.entity, vector.create(0, 1, 0), 4 * dt)
	end
end

return Goal
