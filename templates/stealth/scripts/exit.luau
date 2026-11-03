-- The exit: a trigger collider; the player stepping in wins (the game script hears on_escaped).
local Exit = {}

function Exit:on_trigger_enter(other)
	if other == wpe.find("player") then
		wpe.broadcast("on_escaped")
	end
end

return Exit
