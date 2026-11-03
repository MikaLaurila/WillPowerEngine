-- The title menu, drawn over the track while the race script is in its "menu" phase: practice alone,
-- host a game, join one (on this computer, at an address you type, or found on the local network),
-- and the name the other players see.
--
--   Up/Down (d-pad) choose, Return (A) selects, typing edits the address and the name; the mouse works
--   too. Esc cancels joining.
--
-- Hosting keeps this scene: the host's current scene is the game's, and players who join load it. The
-- race script hears `on_net_joined` and opens the lobby. A joiner loads the host's copy of the scene
-- once it is in, which starts the race script there afresh.
local Menu = {}

local SAVE = "player"
local Notice = require("scripts/notice")
local ROW = 12

function Menu:on_start()
	local saved = wpe.load_data(SAVE)
	self.name = if type(saved) == "table" and type(saved.name) == "string" then saved.name else "Player"
	self.address = if type(saved) == "table" and type(saved.address) == "string" then saved.address else "192.168.1.2"
	self.selected = 1
	self.page = "main" -- or "lan": games found on the local network
	self.race = wpe.find("race")
end

function Menu:save()
	wpe.save_data(SAVE, { name = self.name, address = self.address })
end

-- ---- What the items do (also called directly by the template's test) ----------------------------

function Menu:practice()
	Notice.text = nil
	wpe.send(self.race, "practice")
end

function Menu:host()
	Notice.text = nil
	self:save()
	-- The race script's on_net_joined follows (this frame), and opens the lobby.
	if not wpe.net_host({ name = self.name }) then
		Notice.text = "Couldn't host: is the game already running here?"
	end
end

function Menu:join(address: string)
	Notice.text = nil
	self:save()
	wpe.net_stop_finding()
	if wpe.net_join(address, { name = self.name }) then
		self.joining = address
		local race = wpe.get_script(self.race)
		if race then
			race.phase = "joining"
		end
	else
		Notice.text = "That is not an address: " .. address
	end
end

function Menu:cancel_join()
	wpe.net_leave()
	local race = wpe.get_script(self.race)
	if race then
		race.phase = "menu"
	end
end

-- The menu's rows: {text, action, field}. A row without an action is a label.
function Menu:items()
	if self.page == "lan" then
		local items = {}
		for _, game in wpe.net_find_games() do
			local text = string.format("%s's game  %d/%d", game.host, game.players, game.max_players)
			if game.accepting and game.players < game.max_players then
				table.insert(items, { text = text, action = function()
					self:join(game.address)
				end })
			else
				table.insert(items, { text = text .. " (full)" })
			end
		end
		if #items == 0 then
			table.insert(items, { text = "Looking for games..." })
		end
		table.insert(items, { text = "Back", action = function()
			wpe.net_stop_finding()
			self.page = "main"
			self.selected = 1
		end })
		return items
	end
	return {
		{ text = "Practice", action = function()
			self:practice()
		end },
		{ text = "Host a game", action = function()
			self:host()
		end },
		{ text = "Join a game on this computer", action = function()
			self:join("127.0.0.1")
		end },
		{ text = "Join: " .. self.address, field = "address", action = function()
			self:join(self.address)
		end },
		{ text = "Find games on the network", action = function()
			self.page = "lan"
			self.selected = 1
		end },
		{ text = "Name: " .. self.name, field = "name" },
		{ text = "Quit", action = function()
			wpe.quit()
		end },
	}
end

-- ---- Typing --------------------------------------------------------------------------------------

local LETTERS = "abcdefghijklmnopqrstuvwxyz"
local DIGITS = "0123456789"

-- What was typed this frame (keys are physical, US layout).
local function typed(): string
	local shift = wpe.key_down("lshift") or wpe.key_down("rshift")
	local text = ""
	for i = 1, #LETTERS do
		local c = LETTERS:sub(i, i)
		if wpe.key_pressed(c) then
			text ..= if shift then c:upper() else c
		end
	end
	for i = 1, #DIGITS do
		local c = DIGITS:sub(i, i)
		if wpe.key_pressed(c) or wpe.key_pressed("kp_" .. c) then
			text ..= c
		end
	end
	if wpe.key_pressed("period") or wpe.key_pressed("kp_period") then
		text ..= "."
	end
	if wpe.key_pressed("minus") then
		text ..= if shift then "_" else "-"
	end
	if wpe.key_pressed("semicolon") and shift then
		text ..= ":" -- address:port
	end
	return text
end

function Menu:edit(field: string)
	local value = self[field]
	local limit = if field == "name" then 15 else 40
	if wpe.key_pressed("backspace") and #value > 0 then
		value = value:sub(1, #value - 1)
	end
	local text = typed()
	if field == "name" then
		text = text:gsub("[^%w_%-]", "")
	end
	value = (value .. text):sub(1, limit)
	if value ~= self[field] then
		self[field] = value
		self:save()
	end
end

-- ---- Every frame ---------------------------------------------------------------------------------

function Menu:on_update(dt)
	local race = wpe.get_script(self.race)
	local phase = if race then race.phase else "menu"
	if phase == "joining" then
		self:draw_joining()
		return
	end
	if phase ~= "menu" then
		return
	end
	local items = self:items()
	self.selected = math.clamp(self.selected, 1, #items)
	local width, height = wpe.screen_size()
	local top = 76
	local left, right = 70, width - 70

	-- The mouse picks the row under it.
	local mx, my = wpe.mouse_position()
	local hovered = nil
	if mx >= left and mx < right and my >= top - 2 and my < top - 2 + #items * ROW then
		hovered = (my - top + 2) // ROW + 1
	end
	if hovered and (wpe.mouse_pressed("left") or (self.mouse_x and (mx ~= self.mouse_x or my ~= self.mouse_y))) then
		self.selected = hovered
	end
	self.mouse_x, self.mouse_y = mx, my
	if wpe.action_pressed("menu_up") then
		self.selected = if self.selected > 1 then self.selected - 1 else #items
	elseif wpe.action_pressed("menu_down") then
		self.selected = if self.selected < #items then self.selected + 1 else 1
	end
	local item = items[self.selected]
	if item.field then
		self:edit(item.field)
		item = self:items()[self.selected]
	end
	local activate = wpe.action_pressed("menu_select") or (hovered == self.selected and wpe.mouse_pressed("left"))
	if wpe.action_pressed("leave") and self.page == "lan" then
		items[#items].action() -- Back
		return
	end

	-- Draw.
	local white, yellow, dim = wpe.color(1, 1, 1), wpe.color(1, 0.85, 0.3), wpe.color(1, 1, 1, 0.6)
	local title = if self.page == "lan" then "Games nearby" else "Racing Online"
	local tw = wpe.text_size(title, 2)
	wpe.draw_text((width - tw) // 2, 40, title, yellow, 2)
	wpe.draw_rect(left - 8, top - 8, right - left + 16, #items * ROW + 12, wpe.color(0, 0, 0, 0.55))
	for i, row in items do
		local y = top + (i - 1) * ROW
		local color = if row.action or row.field then white else dim
		if i == self.selected then
			wpe.draw_rect(left - 4, y - 2, right - left + 8, ROW - 1, wpe.color(1, 1, 1, 0.18))
			color = yellow
		end
		local text = row.text
		if row.field and i == self.selected and (wpe.time() * 2) % 2 < 1 then
			text ..= "_" -- the cursor
		end
		wpe.draw_text(left, y, text, color)
	end
	local hint = if item.field == "name" then "Type your name" elseif item.field == "address" then "Type the host's address, Return joins" else "Up/Down choose, Return selects"
	local hw = wpe.text_size(hint)
	wpe.draw_text((width - hw) // 2, height - 26, hint, dim)
	if Notice.text then
		local nw = wpe.text_size(Notice.text)
		wpe.draw_text((width - nw) // 2, top + #items * ROW + 14, Notice.text, wpe.color(1, 0.55, 0.4))
	end

	if activate and item.action then
		item.action()
	end
end

function Menu:draw_joining()
	if wpe.action_pressed("leave") then
		self:cancel_join()
		return
	end
	local width, height = wpe.screen_size()
	local text = "Joining " .. (self.joining or "the game") .. "..."
	local w = wpe.text_size(text)
	wpe.draw_text((width - w) // 2, height // 3, text, wpe.color(1, 1, 1))
	local hint = "Esc: cancel"
	local hw = wpe.text_size(hint)
	wpe.draw_text((width - hw) // 2, height // 3 + 14, hint, wpe.color(1, 1, 1, 0.6))
end

return Menu
