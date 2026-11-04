-- Runs a conversation written as data (dialogues/*.luau): pages of text, choices, and the story flags
-- they test and set. The dialogue box (scripts/dialogue_box.luau) shows it; this module only keeps its
-- place, so it can be driven and tested without drawing anything.
--
-- A conversation file returns a table:
--
--   return {
--       name = "Elder",          -- who speaks (each node can name another speaker)
--       voice = 0.8,             -- the pitch of their voice blips (1 = as recorded)
--       start = {                -- where it begins: the first entry whose conditions hold
--           { node = "thanks", when = "ring_returned" },
--           { node = "hello" },
--       },
--       nodes = {
--           hello = {
--               text = { "Welcome, traveller.", "A second page." },   -- one string or a list of pages
--               choices = {
--                   { text = "Can I help?", go = "quest", unless = "ring_lost" },
--                   { text = "Goodbye." },                           -- no `go`: the talk ends
--               },
--           },
--           quest = { text = "...", set = "ring_lost", event = "on_quest_started", next = "hello" },
--       },
--   }
--
-- Conditions (`when`: every flag set; `unless`: none set) take a flag or a list of flags, on start entries
-- and choices. Effects (`set`, `clear`: flags; `event`: a method every script gets through wpe.broadcast,
-- with the talker's entity) happen when a node is shown or a choice is picked. A node goes on to `next`
-- after its last page, offers its `choices` there, or ends the talk.
local Story = require("scripts/lib/story")

local Conversation = {
	active = nil :: any, -- the conversation running now (one at a time), or nil
}
Conversation.__index = Conversation

local function list_of(value)
	if value == nil then
		return {}
	end
	return if type(value) == "table" then value else { value }
end

-- Raises with every mistake found in a conversation's data (`name` labels the messages).
function Conversation.check(data, name: string)
	local problems = {}
	local function problem(text: string)
		table.insert(problems, text)
	end
	if type(data) ~= "table" or type(data.nodes) ~= "table" then
		error(name .. ": a conversation is a table with `nodes`", 2)
	end
	local function check_flags(where: string, field: string, value)
		for _, flag in list_of(value) do
			if type(flag) ~= "string" then
				problem(string.format("%s: `%s` takes flag names", where, field))
			end
		end
	end
	local function check_target(where: string, target)
		if target ~= nil and data.nodes[target] == nil then
			problem(string.format("%s goes to '%s', which isn't a node", where, tostring(target)))
		end
	end
	local starts = if type(data.start) == "string" then { { node = data.start } } else data.start
	if type(starts) ~= "table" or #starts == 0 then
		problem("`start` names a node, or lists { node = ..., when = ... } entries")
	else
		for i, entry in starts do
			check_target(string.format("start %d", i), entry.node)
			check_flags(string.format("start %d", i), "when", entry.when)
			check_flags(string.format("start %d", i), "unless", entry.unless)
		end
	end
	for id, node in data.nodes do
		local where = "node '" .. tostring(id) .. "'"
		local pages = list_of(node.text)
		if #pages == 0 then
			problem(where .. " has no `text`")
		end
		for _, page in pages do
			if type(page) ~= "string" then
				problem(where .. ": pages are strings")
			end
		end
		check_target(where, node.next)
		check_flags(where, "set", node.set)
		check_flags(where, "clear", node.clear)
		if node.choices ~= nil and node.next ~= nil then
			problem(where .. " has both `choices` and `next`")
		end
		for i, choice in list_of(node.choices) do
			local at = string.format("%s, choice %d", where, i)
			if type(choice.text) ~= "string" then
				problem(at .. " has no `text`")
			end
			check_target(at, choice.go)
			check_flags(at, "when", choice.when)
			check_flags(at, "unless", choice.unless)
			check_flags(at, "set", choice.set)
			check_flags(at, "clear", choice.clear)
		end
	end
	if #problems > 0 then
		error(name .. ":\n  " .. table.concat(problems, "\n  "), 2)
	end
end

local function holds(entry): boolean
	return Story.all(entry.when) and Story.none(entry.unless)
end

local function apply(effects, talker: number?)
	for _, flag in list_of(effects.set) do
		Story.set(flag)
	end
	for _, flag in list_of(effects.clear) do
		Story.clear(flag)
	end
	if effects.event then
		wpe.broadcast(effects.event, talker)
	end
end

-- Starts a conversation (`data` from a dialogues file) with `talker` (the entity speaking, or nil). It
-- becomes Conversation.active. Returns nil when no start entry's conditions hold.
function Conversation.start(data, talker: number?)
	local starts = if type(data.start) == "string" then { { node = data.start } } else data.start
	for _, entry in starts do
		if holds(entry) then
			local talk = setmetatable({ data = data, talker = talker, done = false }, Conversation)
			Conversation.active = talk
			talk:_enter(entry.node)
			return talk
		end
	end
	return nil
end

function Conversation:_enter(id: string)
	self.id = id
	self.node = self.data.nodes[id]
	self.pages = list_of(self.node.text)
	self.page_index = 1
	apply(self.node, self.talker)
end

function Conversation:_finish()
	self.done = true
	if Conversation.active == self then
		Conversation.active = nil
	end
end

-- Who speaks the current page.
function Conversation:speaker(): string
	return self.node.speaker or self.data.name or ""
end

-- The text of the current page.
function Conversation:text(): string
	return self.pages[self.page_index]
end

-- Whether this is the node's last page (where its choices show).
function Conversation:last_page(): boolean
	return self.page_index >= #self.pages
end

-- The choices to offer now: on the last page, those whose conditions hold (each { text, index }).
function Conversation:choices()
	local shown = {}
	if not self:last_page() then
		return shown
	end
	for i, choice in list_of(self.node.choices) do
		if holds(choice) then
			table.insert(shown, { text = choice.text, index = i })
		end
	end
	return shown
end

-- Goes on from a page without choices: the next page, the next node, or the end.
function Conversation:advance()
	if self.done then
		return
	end
	if not self:last_page() then
		self.page_index += 1
	elseif #self:choices() > 0 then
		return -- Waits for a choice.
	elseif self.node.next then
		self:_enter(self.node.next)
	else
		self:_finish()
	end
end

-- Picks one of the choices shown (its position in :choices()).
function Conversation:choose(position: number)
	local shown = self:choices()
	local picked = shown[position]
	if self.done or not picked then
		return
	end
	local choice = self.node.choices[picked.index]
	apply(choice, self.talker)
	if choice.go then
		self:_enter(choice.go)
	else
		self:_finish()
	end
end

-- Ends the conversation now.
function Conversation:stop()
	self:_finish()
end

return Conversation
