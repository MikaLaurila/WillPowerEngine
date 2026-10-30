-- Hurts the player when it touches this entity's trigger collider (spikes, lava...): the player goes
-- back to its last checkpoint.
local Hazard = {}

function Hazard:on_trigger_enter(other)
	if wpe.get_name(other) == "player" then
		wpe.send(other, "hurt")
	end
end

return Hazard
