-- A checkpoint across the track: when this player's car drives through its trigger, it tells the race
-- which one it was. Index 0 is the start/finish line; the others are passed in order 1, 2, 3... around
-- the lap.
--
-- Every machine sees every car pass, but only your own car counts here: the other cars are counted by
-- their own drivers' machines, which see them exactly (here they are drawn a little in the past), and
-- which tell everyone how far they got (race.luau, "progress").
local Checkpoint = {}
Checkpoint.properties = { index = 0 }

function Checkpoint:on_trigger_enter(other)
	local car = wpe.get_script(other)
	if car and car.is_mine and car:is_mine() then
		local race = wpe.find("race")
		if race then
			wpe.send(race, "checkpoint", self.index)
		end
	end
end

return Checkpoint
