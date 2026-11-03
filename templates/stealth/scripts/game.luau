-- The game: get from the start to the exit (the green pad) without being caught. Shows the goal and the
-- guards' alertness, and what happened at the end; then the level starts over.
--
-- Guards broadcast on_caught when one reaches the player; the exit's trigger broadcasts on_escaped.
local Game = {}
Game.properties = {
	restart_after = 3, -- seconds the result shows before the level starts over
}

function Game:on_start()
	self.player = wpe.find("player")
	self.time = 0
	self.result = nil -- "caught" or "escaped"
	self.restart_in = 0
end

function Game:finish(result: string)
	if self.result then
		return
	end
	self.result = result
	self.restart_in = self.restart_after
	local player = wpe.get_script(self.player)
	if player then
		player.frozen = true
	end
	wpe.broadcast("on_game_over", result)
end

function Game:on_caught(guard: number)
	self:finish("caught")
end

function Game:on_escaped()
	self:finish("escaped")
end

-- The most alert guard: "chasing", "searching", "suspicious" or nil.
local function alert_level(): string?
	local worst = nil
	for _, name in { "guard 1", "guard 2", "guard 3" } do
		local guard = wpe.get_script(wpe.find(name) or 0)
		local state = guard and guard.state
		if state == "chase" then
			return "chasing"
		elseif state == "search" then
			worst = "searching"
		elseif (state == "suspicious" or state == "investigate" or state == "look") and worst == nil then
			worst = "suspicious"
		end
	end
	return worst
end

function Game:on_update(dt)
	local width, height = wpe.screen_size()
	if not self.result then
		self.time += dt
		wpe.draw_text(6, 22, "Reach the green exit unseen", wpe.color(1, 1, 1), 1)
		wpe.draw_text(6, 32, "Guards hear sprinting and landings", wpe.color(0.75, 0.75, 0.75), 1)
		local alert = alert_level()
		if alert then
			local color = if alert == "chasing" then wpe.color(1, 0.3, 0.2) else wpe.color(1, 0.85, 0.2)
			wpe.draw_text(width - 8 * #alert - 6, 22, alert, color, 1)
		end
		local noise = wpe.get_script(self.player)
		if noise and noise.noise > 0.5 then
			wpe.draw_rect(6, height - 10, noise.noise * 6, 4, wpe.color(1, 0.6, 0.2, 0.8))
		end
		return
	end
	local text = if self.result == "caught" then "Caught!" else string.format("Escaped in %.1f s!", self.time)
	local color = if self.result == "caught" then wpe.color(1, 0.3, 0.2) else wpe.color(0.4, 1, 0.5)
	local w = wpe.text_size(text, 2)
	wpe.draw_rect(0, height / 2 - 14, width, 28, wpe.color(0, 0, 0, 0.6))
	wpe.draw_text((width - w) / 2, height / 2 - 8, text, color, 2)
	self.restart_in -= dt
	if self.restart_in <= 0 then
		wpe.load_scene("scenes/main.scene")
	end
end

return Game
