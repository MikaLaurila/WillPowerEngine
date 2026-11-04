-- The story so far: named flags that conversations test (`when`, `unless`) and set (`set`, `clear`). Any
-- script can read and change them too: Story.has("ring_found"), Story.set("gate_open").
--
-- Shared by every script (modules load once per game). Flags last until the game quits; set
-- Story.save_name to keep them in the player's saved data across runs (wpe.save_data), and call
-- Story.reset() for a new game.
local Story = {
	flags = {} :: { [string]: boolean },
	save_name = nil :: string?, -- e.g. "story"; nil: not saved
	loaded = false,
}

local function load()
	if Story.loaded then
		return
	end
	Story.loaded = true
	if Story.save_name then
		local saved = wpe.load_data(Story.save_name)
		if type(saved) == "table" then
			for flag, on in saved do
				Story.flags[flag] = on == true
			end
		end
	end
end

local function save()
	if Story.save_name then
		wpe.save_data(Story.save_name, Story.flags)
	end
end

function Story.has(flag: string): boolean
	load()
	return Story.flags[flag] == true
end

function Story.set(flag: string, on: boolean?)
	load()
	Story.flags[flag] = if on == false then nil else true
	save()
end

function Story.clear(flag: string)
	Story.set(flag, false)
end

function Story.reset()
	Story.flags = {}
	Story.loaded = true
	save()
end

-- Whether a condition holds: a flag's name, or a list of them that must all be set.
function Story.all(flags: (string | { string })?): boolean
	if flags == nil then
		return true
	end
	if type(flags) == "string" then
		return Story.has(flags)
	end
	for _, flag in flags do
		if not Story.has(flag) then
			return false
		end
	end
	return true
end

-- Whether none of the flags is set.
function Story.none(flags: (string | { string })?): boolean
	if flags == nil then
		return true
	end
	if type(flags) == "string" then
		return not Story.has(flags)
	end
	for _, flag in flags do
		if Story.has(flag) then
			return false
		end
	end
	return true
end

return Story
