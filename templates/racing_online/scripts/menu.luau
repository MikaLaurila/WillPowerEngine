-- The title menu, drawn over the track while the race script is in its "menu" phase: practice alone,
-- host a game, join one (on this computer, at an address you type, or found on the local network),
-- and the name the other players see.
--
--   Up/Down (d-pad) choose, Return (A) selects; the address and the name are text fields (Return or a
--   click types, Return again is done). The mouse works too. Esc cancels joining.
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
	self.panel = wpe.find("menu panel")
	self.fields = { address = wpe.find("address field"), name = wpe.find("name field") }
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

-- The menu's rows: {text, action} or {text, field} (a text field after the text). A row with neither is a label.
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
		{ text = "Join:", field = "address" },
		{ text = "Find games on the network", action = function()
			self.page = "lan"
			self.selected = 1
		end },
		{ text = "Name:", field = "name" },
		{ text = "Quit", action = function()
			wpe.quit()
		end },
	}
end

-- ---- The text fields ------------------------------------------------------------------------------
-- The address and the name are text fields (UI entities in the scene, scripts/menu_field.luau): select
-- the row and press Return (or click it) to type; Return again joins or keeps the name, Esc puts the old
-- text back. While a field takes the keyboard, keys type instead of driving actions.

-- From the fields' script: every change, and Return.
function Menu:typed(setting: string, text: string)
	self[setting] = text
end

function Menu:submitted(setting: string, text: string)
	self[setting] = text
	self:save()
	if setting == "address" then
		self:join(text)
	end
end

-- Shows the fields on their rows (or hides them all with nil), on only while their row is selected.
function Menu:place_fields(rows, selected: number?)
	local editing = wpe.ui_editing()
	for setting, field in self.fields do
		local row = rows and rows[setting]
		wpe.set_ui_visible(field, row ~= nil)
		local on = row ~= nil and row.index == selected
		wpe.set_interactable(field, on)
		if row then
			wpe.set_ui_offsets(field, row.x, row.y - 2, row.right, row.y + ROW - 3)
			wpe.set_text_color(field, if on then wpe.color(1, 0.85, 0.3) else wpe.color(1, 1, 1))
			if editing ~= field and wpe.ui_value(field) ~= self[setting] then
				wpe.ui_value(field, self[setting]) -- set elsewhere (the template's test does)
			end
			if on and wpe.ui_focused() ~= field then
				wpe.ui_focus(field)
			end
		end
	end
	wpe.set_ui_visible(self.panel, rows ~= nil)
end

-- ---- Every frame ---------------------------------------------------------------------------------

function Menu:on_update(dt)
	local race = wpe.get_script(self.race)
	local phase = if race then race.phase else "menu"
	if phase ~= "menu" then
		self:place_fields(nil)
		if phase == "joining" then
			self:draw_joining()
		end
		return
	end
	local items = self:items()
	self.selected = math.clamp(self.selected, 1, #items)
	local width, height = wpe.screen_size()
	local top = 76
	local left, right = 70, width - 70
	local editing = wpe.ui_editing()

	-- The mouse picks the row under it (not while typing, unless it clicks).
	local mx, my = wpe.mouse_position()
	local hovered = nil
	if mx >= left and mx < right and my >= top - 2 and my < top - 2 + #items * ROW then
		hovered = (my - top + 2) // ROW + 1
	end
	local moved = self.mouse_x and (mx ~= self.mouse_x or my ~= self.mouse_y)
	if hovered and (wpe.mouse_pressed("left") or (moved and not editing)) then
		self.selected = hovered
	end
	self.mouse_x, self.mouse_y = mx, my
	if not editing then -- (Keys type while editing; a gamepad's d-pad would still move.)
		if wpe.action_pressed("menu_up") then
			self.selected = if self.selected > 1 then self.selected - 1 else #items
		elseif wpe.action_pressed("menu_down") then
			self.selected = if self.selected < #items then self.selected + 1 else 1
		end
	end
	local item = items[self.selected]
	local clicked = hovered == self.selected and wpe.mouse_pressed("left")
	local activate = (not editing and wpe.action_pressed("menu_select")) or clicked
	if not editing and wpe.action_pressed("leave") and self.page == "lan" then
		items[#items].action() -- Back
		return
	end

	-- Draw: the panel and the fields are UI entities (drawn before this), the rows are drawn here.
	local white, yellow, dim = wpe.color(1, 1, 1), wpe.color(1, 0.85, 0.3), wpe.color(1, 1, 1, 0.6)
	local title = if self.page == "lan" then "Games nearby" else "Racing Online"
	local tw = wpe.text_size(title, 2)
	wpe.draw_text((width - tw) // 2, 40, title, yellow, 2)
	wpe.set_ui_offsets(self.panel, left - 8, top - 8, right + 8, top + #items * ROW + 4)
	local field_rows = {}
	for i, row in items do
		local y = top + (i - 1) * ROW
		local color = if row.action or row.field then white else dim
		if i == self.selected then
			wpe.draw_rect(left - 4, y - 2, right - left + 8, ROW - 1, wpe.color(1, 1, 1, 0.18))
			color = yellow
		end
		wpe.draw_text(left, y, row.text, color)
		if row.field then
			field_rows[row.field] = { index = i, x = left + wpe.text_size(row.text) + 4, y = y, right = right }
		end
	end
	self:place_fields(if self.page == "main" then field_rows else nil, self.selected)
	local hint = "Up/Down choose, Return selects"
	if editing then
		hint = if item.field == "address" then "Return joins, Esc cancels" else "Return keeps it, Esc cancels"
	elseif item.field == "name" then
		hint = "Return: type your name"
	elseif item.field == "address" then
		hint = "Return: type the host's address"
	end
	local hw = wpe.text_size(hint)
	wpe.draw_text((width - hw) // 2, height - 26, hint, dim)
	if Notice.text then
		local nw = wpe.text_size(Notice.text)
		wpe.draw_text((width - nw) // 2, top + #items * ROW + 14, Notice.text, wpe.color(1, 0.55, 0.4))
	end

	if activate and item.field then
		wpe.ui_edit(self.fields[item.field]) -- (the UI may have started already: Return on a focused field)
	elseif activate and item.action then
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
