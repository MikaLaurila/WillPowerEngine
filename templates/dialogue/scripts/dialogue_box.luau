-- The dialogue box at the bottom of the screen. A villager (scripts/talker.luau) opens it with its
-- conversation; the box types each page out with the speaker's voice blips, then goes on with the
-- interact or select key (or a click), and offers the node's choices: Up/Down (d-pad, stick) choose,
-- select picks, the mouse hovers and clicks. A key pressed while a page is still typing shows it whole.
--
-- The blips play on the voice bus, so the music ducks under the talking and comes back after (the
-- project's audio settings say how far and how fast).
local Conversation = require("scripts/lib/conversation")

local DialogueBox = {}
DialogueBox.properties = {
	letters_per_second = 45,
	blip_every = 2, -- a voice blip every this many letters
	blip_volume = 0.35,
}

local LINE = 10 -- pixels per line of text
local PAD = 6
local MARGIN = 8

-- Splits text into lines no wider than `width` pixels (at spaces).
local function wrap(text: string, width: number): { string }
	local lines = {}
	local line = ""
	for word in string.gmatch(text, "%S+") do
		local candidate = if line == "" then word else line .. " " .. word
		if line ~= "" and wpe.text_size(candidate) > width then
			table.insert(lines, line)
			line = word
		else
			line = candidate
		end
	end
	table.insert(lines, line)
	return lines
end

function DialogueBox:on_start()
	self.talk = nil
end

-- Opens a conversation with `talker` (called by the villagers through wpe.send). False when the
-- conversation has nothing to say now, or another one is running.
function DialogueBox:open(data, talker: number?): boolean
	if Conversation.active then
		return false
	end
	local talk = Conversation.start(data, talker)
	if not talk then
		return false
	end
	self.talk = talk
	self.opened = wpe.frame() -- The key that opened it doesn't also skip the first page.
	self.page = nil
	wpe.set_mouse_captured(false) -- The mouse picks choices now.
	return true
end

-- Starts typing a new page: what it says, wrapped, and where the typing is.
function DialogueBox:start_page()
	local width = wpe.screen_size()
	self.page = { node = self.talk.id, index = self.talk.page_index }
	self.lines = wrap(self.talk:text(), width - 2 * (MARGIN + PAD))
	self.total = 0
	for _, line in self.lines do
		self.total += #line
	end
	self.shown = 0
	self.blipped = 0
	self.selected = 1
end

function DialogueBox:typing(): boolean
	return self.shown < self.total
end

function DialogueBox:on_update(dt)
	local talk = self.talk
	if not talk or talk.done then
		self.talk = nil
		return
	end
	if not self.page or self.page.node ~= talk.id or self.page.index ~= talk.page_index then
		self:start_page()
	end

	-- Type the page out, a voice blip every few letters (pitched to the speaker).
	if self:typing() then
		self.shown = math.min(self.total, self.shown + dt * self.letters_per_second)
		local letters = math.floor(self.shown / self.blip_every)
		if letters > self.blipped then
			self.blipped = letters
			local voice = talk.data.voice or 1
			wpe.play_sound("sounds/blip.wav", {
				bus = wpe.BUS_VOICE,
				volume = self.blip_volume,
				pitch = voice * (0.92 + 0.16 * math.random()),
			})
		end
	end

	local choices = if self:typing() then {} else talk:choices()
	local hovered = self:draw(choices)
	if wpe.frame() == self.opened then
		return
	end

	-- Input: choose among the choices; select (or interact, or a click) goes on.
	if #choices > 0 then
		if wpe.action_pressed("menu_up") then
			self.selected = if self.selected > 1 then self.selected - 1 else #choices
		elseif wpe.action_pressed("menu_down") then
			self.selected = if self.selected < #choices then self.selected + 1 else 1
		end
		local mx, my = wpe.mouse_position()
		local moved = mx ~= self.mouse_x or my ~= self.mouse_y
		self.mouse_x, self.mouse_y = mx, my
		if hovered and (moved or wpe.mouse_pressed("left")) then
			self.selected = hovered -- (A resting mouse doesn't fight the keys.)
		end
	end
	local clicked = wpe.mouse_pressed("left") and (#choices == 0 or hovered ~= nil)
	if wpe.action_pressed("menu_select") or wpe.action_pressed("interact") or clicked then
		if self:typing() then
			self.shown = self.total -- Show the page whole.
		elseif #choices > 0 then
			talk:choose(self.selected)
		else
			talk:advance()
		end
	end
end

-- Draws the box; returns the choice under the mouse, if any.
function DialogueBox:draw(choices): number?
	local width, height = wpe.screen_size()
	local rows = #self.lines + (if #choices > 0 then #choices + 1 else 0)
	local h = 2 * PAD + rows * LINE
	local x, y, w = MARGIN, height - MARGIN - h, width - 2 * MARGIN
	wpe.draw_rect(x, y, w, h, wpe.color(0.06, 0.07, 0.12, 0.88))
	wpe.draw_rect(x, y, w, 1, wpe.color(1, 0.85, 0.5, 0.8))

	-- The speaker's name on a tab above the box.
	local name = self.talk:speaker()
	if name ~= "" then
		local nw = wpe.text_size(name)
		wpe.draw_rect(x, y - LINE - 3, nw + 2 * PAD, LINE + 3, wpe.color(0.06, 0.07, 0.12, 0.88))
		wpe.draw_text(x + PAD, y - LINE, name, wpe.color(1, 0.85, 0.5))
	end

	-- The page, as far as it has typed.
	local left = math.floor(self.shown)
	for i, line in self.lines do
		if left <= 0 then
			break
		end
		wpe.draw_text(x + PAD, y + PAD + (i - 1) * LINE, string.sub(line, 1, left))
		left -= #line
	end
	if not self:typing() and #choices == 0 and math.floor(wpe.time() * 2) % 2 == 0 then
		wpe.draw_text(x + w - PAD - wpe.text_size(">"), y + h - PAD - LINE + 2, ">", wpe.color(1, 0.85, 0.5))
	end

	-- The choices, the selected one marked; the mouse picks the one under it.
	local hovered = nil
	local mx, my = wpe.mouse_position()
	local top = y + PAD + (#self.lines + 1) * LINE
	for i, choice in choices do
		local cy = top + (i - 1) * LINE
		if mx >= x and mx < x + w and my >= cy - 1 and my < cy + LINE - 1 then
			hovered = i
		end
		local selected = i == self.selected
		if selected then
			wpe.draw_rect(x + PAD - 2, cy - 1, w - 2 * PAD + 4, LINE, wpe.color(1, 1, 1, 0.12))
		end
		wpe.draw_text(x + PAD, cy, (if selected then "> " else "  ") .. choice.text,
			if selected then wpe.color(1, 0.85, 0.5) else wpe.color(0.85, 0.85, 0.85))
	end
	return hovered
end

return DialogueBox
