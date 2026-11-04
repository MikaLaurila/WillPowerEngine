-- The pause menu (Esc, or Start on a pad) and its controls screen, where players pick their own keys and
-- buttons. Each change is saved for the player (wpe.set_bindings), and the game starts with it next time.
--
--   Up/Down choose, Left/Right switch between the keyboard and gamepad columns, Return or Space (A) selects,
--   Esc (B) goes back. Changing a binding waits for the key or button to use; Esc (Start on a pad) cancels.
--   A key another row had moves there in exchange. The mouse works too.
local State = require("scripts/menu_state")
local Controls = {}

local ROW = 13
local LEFT, KEYBOARD_X, GAMEPAD_X = 26, 112, 212 -- the label column and the two binding columns
local TOP = 70

-- The controls screen's rows: an action and, for the halves of an axis action, the way the row pushes it.
local ROWS = {
	{ text = "Move left", action = "move_x", direction = -1 },
	{ text = "Move right", action = "move_x", direction = 1 },
	{ text = "Jump", action = "jump", direction = 0 },
	{ text = "Restart", action = "restart", direction = 0 },
}
local DEVICES = { "keyboard", "gamepad" }

-- ---- Which bindings a row shows, and changing them -------------------------------------------------
-- A cell is a row on one device. Pad cells hold buttons, triggers and stick halves; a stick read whole
-- (move_x's left stick) isn't anyone's cell and stays as it is.

local function on_device(b, device: string): boolean
	if device == "keyboard" then
		return b.key ~= nil or b.mouse_button ~= nil
	end
	if b.pad_button then
		return true
	end
	local trigger = b.pad_axis == "left_trigger" or b.pad_axis == "right_trigger"
	return b.pad_axis ~= nil and (trigger or b.range ~= nil)
end

-- A binding pushes its action the way of its scale (a stick half reads 0..1 on its side).
local function in_cell(b, row, device: string): boolean
	if not on_device(b, device) then
		return false
	end
	return row.direction == 0 or (if (b.scale or 1) < 0 then -1 else 1) == row.direction
end

-- The same key, button or stick half, whatever its scale.
local function same_input(a, b): boolean
	return a.key == b.key and a.mouse_button == b.mouse_button and a.pad_button == b.pad_button
		and a.pad_axis == b.pad_axis and (a.range or "full") == (b.range or "full")
end

-- An input as a binding of `row`: pushing the row's way.
local function for_row(input, row)
	return {
		key = input.key,
		mouse_button = input.mouse_button,
		pad_button = input.pad_button,
		pad_axis = input.pad_axis,
		range = input.range,
		scale = if row.direction < 0 then -1 else nil,
	}
end

local function cell_bindings(row, device: string)
	local list = {}
	for _, b in wpe.get_bindings(row.action) do
		if in_cell(b, row, device) then
			table.insert(list, b)
		end
	end
	return list
end

-- Puts `input` in a row's cell, in place of what the cell had. A row that had the same input on the same
-- device gets the cell's old binding in exchange (or loses the input, when the cell was empty).
function Controls:rebind(row, device: string, input)
	local old = cell_bindings(row, device)[1]
	for _, other in ROWS do
		if other ~= row then
			local list = wpe.get_bindings(other.action)
			for i, b in list do
				if in_cell(b, other, device) and same_input(b, input) then
					if old then
						list[i] = for_row(old, other)
					else
						table.remove(list, i)
					end
					wpe.set_bindings(other.action, list)
					break
				end
			end
		end
	end
	local list, placed = {}, false
	for _, b in wpe.get_bindings(row.action) do
		if not in_cell(b, row, device) then
			table.insert(list, b)
		elseif not placed then
			table.insert(list, for_row(input, row))
			placed = true
		end
	end
	if not placed then
		table.insert(list, for_row(input, row))
	end
	wpe.set_bindings(row.action, list)
end

local function cell_text(row, device: string): string
	local labels = {}
	for _, b in cell_bindings(row, device) do
		table.insert(labels, wpe.binding_label(b))
	end
	return if #labels > 0 then table.concat(labels, ", ") else "-"
end

-- ---- The menu ------------------------------------------------------------------------------------

function Controls:on_start()
	State.open = false
	self.page = "main" -- or "controls"
	self.selected = 1
	self.column = 1 -- on the controls page: 1 keyboard, 2 gamepad
	self.waiting = nil -- {row, device} while listening for a new binding
end

function Controls:open()
	State.open = true
	self.page = "main"
	self.selected = 1
	wpe.set_time_scale(0) -- the game stands still; scripts' on_update and the 2D drawing go on
end

function Controls:close()
	State.open = false
	self.waiting = nil
	wpe.stop_listening()
	wpe.set_time_scale(1)
end

function Controls:items()
	if self.page == "main" then
		return {
			{ text = "Resume", action = function()
				self:close()
			end },
			{ text = "Controls", action = function()
				self.page = "controls"
				self.selected = 1
			end },
			{ text = "Quit", action = function()
				wpe.quit()
			end },
		}
	end
	local items = {}
	for _, row in ROWS do
		table.insert(items, { text = row.text, row = row })
	end
	table.insert(items, { text = "Reset to defaults", action = function()
		wpe.reset_bindings()
	end })
	table.insert(items, { text = "Back", action = function()
		self.page = "main"
		self.selected = 2
	end })
	return items
end

-- Waits for the new binding (also called by the template's test).
function Controls:change(row, device: string)
	self.waiting = { row = row, device = device }
	wpe.listen_binding(device)
end

function Controls:listen_update()
	local heard = wpe.heard_binding()
	local waiting = self.waiting
	if heard then
		self.waiting = nil
		if heard.key ~= "escape" and heard.pad_button ~= "start" then -- those cancel
			self:rebind(waiting.row, waiting.device, heard)
		end
		return
	end
	-- Cancelling from the other device (the listen doesn't hear it), or a listen that ended elsewhere.
	local cancel = if waiting.device == "gamepad" then wpe.key_pressed("escape") else wpe.gamepad_pressed("start")
	if cancel or not wpe.is_listening() then
		wpe.stop_listening()
		self.waiting = nil
	end
end

function Controls:on_update(dt)
	if self.waiting then
		self:listen_update()
		self:draw(self:items())
		return
	end
	if not State.open then
		if wpe.action_pressed("pause") then
			self:open()
		end
		return
	end
	if wpe.action_pressed("menu_back") or wpe.action_pressed("pause") then
		if self.page == "controls" then
			self.page = "main"
			self.selected = 2
		else
			self:close()
			return
		end
	end
	local items = self:items()
	self.selected = math.clamp(self.selected, 1, #items)
	if wpe.action_pressed("menu_up") then
		self.selected = if self.selected > 1 then self.selected - 1 else #items
	elseif wpe.action_pressed("menu_down") then
		self.selected = if self.selected < #items then self.selected + 1 else 1
	elseif wpe.action_pressed("menu_left") then
		self.column = 1
	elseif wpe.action_pressed("menu_right") then
		self.column = 2
	end

	-- The mouse picks what is under it when it moves, and a click selects it.
	local mx, my = wpe.mouse_position()
	local moved = self.mouse_x and (mx ~= self.mouse_x or my ~= self.mouse_y)
	self.mouse_x, self.mouse_y = mx, my
	local hovered = if my >= TOP - 2 and mx >= LEFT - 4 then (my - TOP + 2) // ROW + 1 else nil
	if hovered and (hovered < 1 or hovered > #items) then
		hovered = nil
	end
	if hovered and (moved or wpe.mouse_pressed("left")) then
		self.selected = hovered
		if items[hovered].row and mx >= KEYBOARD_X - 4 then
			self.column = if mx >= GAMEPAD_X - 4 then 2 else 1
		end
	end
	local item = items[self.selected]
	if wpe.action_pressed("menu_select") or (hovered == self.selected and wpe.mouse_pressed("left")) then
		if item.row then
			self:change(item.row, DEVICES[self.column])
		elseif item.action then
			item.action()
		end
	end
	if State.open then
		self:draw(self:items())
	end
end

-- ---- Drawing -------------------------------------------------------------------------------------

local function prompt(action: string): string
	return wpe.action_prompt(action) or "?"
end

function Controls:draw(items)
	local width, height = wpe.screen_size()
	local white, yellow, dim = wpe.color(1, 1, 1), wpe.color(1, 0.85, 0.3), wpe.color(1, 1, 1, 0.6)
	wpe.draw_rect(0, 0, width, height, wpe.color(0.05, 0.07, 0.12, 0.75))
	local title = if self.page == "main" then "Paused" else "Controls"
	wpe.draw_text((width - wpe.text_size(title, 2)) // 2, 30, title, yellow, 2)
	if self.page == "controls" then
		wpe.draw_text(KEYBOARD_X, TOP - 16, "Keyboard", dim)
		wpe.draw_text(GAMEPAD_X, TOP - 16, "Gamepad", dim)
	end
	for i, item in items do
		local y = TOP + (i - 1) * ROW
		local selected = i == self.selected
		if selected then
			wpe.draw_rect(LEFT - 4, y - 2, width - 2 * LEFT + 8, ROW - 1, wpe.color(1, 1, 1, 0.12))
		end
		wpe.draw_text(LEFT, y, item.text, if selected and not item.row then yellow else white)
		if item.row then
			for column, device in DEVICES do
				local x = if column == 1 then KEYBOARD_X else GAMEPAD_X
				local here = selected and column == self.column
				local waiting = here and self.waiting ~= nil
				local text = if waiting then (if device == "keyboard" then "press a key" else "press a button")
					else cell_text(item.row, device)
				if here then
					wpe.draw_rect(x - 3, y - 2, 96, ROW - 1, wpe.color(1, 0.85, 0.3, if waiting then 0.35 else 0.2))
				end
				wpe.draw_text(x, y, text, if here then yellow else white)
			end
		end
	end
	local hint
	if self.waiting then
		hint = if self.waiting.device == "keyboard" then "Esc: cancel" else "Start: cancel"
	elseif self.page == "controls" then
		hint = string.format("%s: change   %s: back", prompt("menu_select"), prompt("menu_back"))
	else
		hint = string.format("%s: select   %s: resume", prompt("menu_select"), prompt("pause"))
	end
	wpe.draw_text((width - wpe.text_size(hint)) // 2, height - 24, hint, dim)
end

return Controls
