-- Game flow for the demo scenes: mouse capture (click to capture, Escape to release, Escape again
-- to quit), scene switching (switch_scene), and a heads-up display showing the controls and every
-- connected gamepad's live state (handy for testing controllers).
local Game = {}
-- help: the scene's control lines, separated by ";" (empty: the ball demo's).
Game.properties = { next_scene = "scenes/test_full.scene", title = "WillPowerEngine", help = "" }

local BUTTONS = {
	"south", "east", "west", "north", "back", "guide", "start", "left_stick", "right_stick",
	"left_shoulder", "right_shoulder", "dpad_up", "dpad_down", "dpad_left", "dpad_right", "misc1", "touchpad",
}
local SHORT = {
	south = "A", east = "B", west = "X", north = "Y", back = "Back", guide = "Guide", start = "Start",
	left_stick = "LS", right_stick = "RS", left_shoulder = "LB", right_shoulder = "RB", dpad_up = "Up",
	dpad_down = "Down", dpad_left = "Left", dpad_right = "Right", misc1 = "Misc", touchpad = "Pad",
}

function Game:on_start()
	self.frames = 0
	self.fps_time = 0
	self.fps = 0
	self.show_help = true
end

function Game:on_update(dt)
	self.frames += 1
	self.fps_time += dt
	if self.fps_time >= 0.5 then
		self.fps = self.frames / self.fps_time
		self.frames = 0
		self.fps_time = 0
	end

	if wpe.is_mouse_captured() then
		if wpe.action_pressed("release_mouse") then
			wpe.set_mouse_captured(false)
		end
	elseif wpe.mouse_pressed("left") then
		wpe.set_mouse_captured(true)
	elseif wpe.action_pressed("release_mouse") then
		wpe.quit()
	end
	if wpe.action_pressed("switch_scene") then
		wpe.load_scene(self.next_scene, { transition = "dissolve", duration = 0.35 })
	end
	if wpe.key_pressed("f1") then
		self.show_help = not self.show_help
	end
	self:draw_hud()
end

local function bar(x, y, value, color)
	-- A small horizontal meter for an axis value in [-1, 1] (or [0, 1]).
	wpe.draw_rect(x, y, 41, 5, wpe.color(0, 0, 0, 0.6))
	local w = math.floor(math.abs(value) * 20 + 0.5)
	if value >= 0 then
		wpe.draw_rect(x + 20, y + 1, w + 1, 3, color)
	else
		wpe.draw_rect(x + 20 - w, y + 1, w + 1, 3, color)
	end
end

function Game:draw_hud()
	local width, height = wpe.screen_size()
	local white, dim, yellow = wpe.color(1, 1, 1), wpe.color(0.7, 0.75, 0.8), wpe.color(1, 0.85, 0.3)
	wpe.draw_rect(0, 0, width, 10, wpe.color(0, 0, 0, 0.55))
	wpe.draw_text(2, 2, string.format("%s  %3.0f fps", self.title, self.fps), white)

	local y = 13
	if self.show_help then
		local lines = {
			"WASD/stick move  Space/A jump  F,LMB/RT throw",
			"Shift/LT boost  T/Y rumble  R/Back reset",
		}
		if self.help ~= "" then
			lines = string.split(self.help, ";")
		end
		table.insert(lines, "Tab/Start next scene  F1 help")
		table.insert(lines, wpe.is_mouse_captured() and "Esc: release mouse" or "Click: mouse look  Esc: quit")
		for _, line in lines do
			wpe.draw_text(2, y, line, dim)
			y += 8
		end
	end

	-- Gamepads: one panel per connected player.
	if wpe.gamepad_count() == 0 then
		wpe.draw_text(2, height - 10, "No gamepad connected", dim)
		return
	end
	local panel_y = height - 4
	for player = 4, 1, -1 do
		if wpe.gamepad_connected(player) then
			panel_y -= 30
			local x = 2
			wpe.draw_rect(0, panel_y - 1, width, 29, wpe.color(0, 0, 0, 0.45))
			wpe.draw_text(x, panel_y, string.format("P%d %s", player, wpe.gamepad_name(player) or "?"), yellow)
			bar(x, panel_y + 9, wpe.gamepad_axis("left_x", player), white)
			bar(x + 44, panel_y + 9, wpe.gamepad_axis("left_y", player), white)
			bar(x + 88, panel_y + 9, wpe.gamepad_axis("right_x", player), white)
			bar(x + 132, panel_y + 9, wpe.gamepad_axis("right_y", player), white)
			bar(x + 176, panel_y + 9, wpe.gamepad_axis("left_trigger", player), yellow)
			bar(x + 220, panel_y + 9, wpe.gamepad_axis("right_trigger", player), yellow)
			local held = {}
			for _, name in BUTTONS do
				if wpe.gamepad_down(name, player) then
					table.insert(held, SHORT[name])
				end
			end
			wpe.draw_text(x, panel_y + 17, #held > 0 and table.concat(held, " ") or "-", white)
		end
	end
end

return Game
