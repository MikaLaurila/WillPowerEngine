-- A checkpoint across the track: when the car drives through its trigger, it tells the race which one it
-- was. Index 0 is the start/finish line; the others are passed in order 1, 2, 3... around the lap.
local Checkpoint = {}
Checkpoint.properties = { index = 0 }

function Checkpoint:on_trigger_enter(other)
	if wpe.get_name(other) == "car" then
		local race = wpe.find("race")
		if race then
			wpe.send(race, "checkpoint", self.index)
		end
	end
end

return Checkpoint
