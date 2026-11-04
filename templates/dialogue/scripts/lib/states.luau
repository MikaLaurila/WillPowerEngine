-- A small state machine for characters: the player, enemies, villagers. Each state is a table of
-- functions, all optional, called with the owning script (`self`) first:
--
--   enter(self, previous, ...)   when the machine comes into the state (`...` from go/push)
--   exit(self, next)             when it leaves
--   update(self, dt)             from machine:update (on_update)
--   fixed_update(self, dt)       from machine:fixed_update (on_fixed_update)
--   any other name               an event, from machine:send(name, ...)
--
-- A function that returns a state's name moves the machine there (exit, then enter). An event the current
-- state has no function for is ignored, so a state simply doesn't react to what it doesn't handle.
--
--   local States = require("scripts/lib/states")
--   Guard.states = {
--       patrol = { update = function(self, dt) if self:sees_player() then return "chase" end end },
--       chase = { enter = function(self) wpe.play_sound("sounds/alert.wav") end, ... },
--   }
--   function Guard:on_start() self.machine = States.new(self, Guard.states, "patrol") end
--   function Guard:on_update(dt) self.machine:update(dt) end
--   function Guard:on_noise(where) self.machine:send("on_noise", where) end
--
-- Shared behaviour: States.extend(base, {...}) makes a state that falls back on `base` for what it doesn't
-- define (an "on the ground" base for idle and run). Interruptions: machine:push("hurt") enters a state
-- on top, and machine:pop() goes back to the one below (with its enter called again).
--
-- machine.state is the current state's name, machine.previous the one before, machine.time the seconds
-- spent in it (counted by update).
local States = {}

local Machine = {}
Machine.__index = Machine

local MAX_CHAIN = 16 -- Transitions in one go before it's surely a loop (a enters b, b enters a, ...).

-- A machine for `owner` (the script: its functions get it as `self`) over the `states` table, starting in
-- `initial` (its enter runs now, with `...`).
function States.new(owner, states: { [string]: any }, initial: string, ...)
	assert(type(states) == "table", "States.new: states must be a table of state tables")
	local machine = setmetatable({
		owner = owner,
		states = states,
		state = nil :: string?,
		previous = nil :: string?,
		time = 0,
		stack = {},
		chain = 0,
	}, Machine)
	machine:go(initial, ...)
	return machine
end

-- A state that uses `base` for every function it doesn't define itself.
function States.extend(base, state)
	return setmetatable(state or {}, { __index = base })
end

function Machine:_state(name: string)
	local state = self.states[name]
	if type(state) ~= "table" then
		local known = {}
		for key in self.states do
			table.insert(known, key)
		end
		table.sort(known)
		error(string.format("no state '%s' (states: %s)", tostring(name), table.concat(known, ", ")), 3)
	end
	return state
end

-- Follows what a state function returned: a state's name moves there.
function Machine:_follow(result)
	if type(result) == "string" then
		self:go(result)
	end
end

-- Moves to state `name`: the current state's exit, then the new one's enter (with `...`). Going to the state
-- it is in runs exit and enter again (a restart).
function Machine:go(name: string, ...)
	local state = self:_state(name)
	self.chain += 1
	if self.chain > MAX_CHAIN then
		self.chain = 0
		error(string.format("states keep moving each other on ('%s' -> '%s'): a loop?", tostring(self.state), name), 2)
	end
	local from = self.state
	if from then
		local current = self.states[from]
		if current and current.exit then
			current.exit(self.owner, name)
		end
	end
	self.previous = from
	self.state = name
	self.time = 0
	local result = if state.enter then state.enter(self.owner, from, ...) else nil
	self:_follow(result)
	self.chain = math.max(0, self.chain - 1)
end

-- Runs the current state's update; it may return the next state's name.
function Machine:update(dt: number)
	self.time += dt
	local state = self.states[self.state]
	if state and state.update then
		self:_follow(state.update(self.owner, dt))
	end
end

-- Runs the current state's fixed_update (from on_fixed_update); it may return the next state's name.
function Machine:fixed_update(dt: number)
	local state = self.states[self.state]
	if state and state.fixed_update then
		self:_follow(state.fixed_update(self.owner, dt))
	end
end

-- Gives an event to the current state: its function of that name, if it has one, called with `...`; it may
-- return the next state's name. Returns whether the state handled it.
function Machine:send(event: string, ...): boolean
	local state = self.states[self.state]
	local handler = state and state[event]
	if type(handler) ~= "function" then
		return false
	end
	self:_follow(handler(self.owner, ...))
	return true
end

-- Enters `name` on top of the current state, which waits below (its exit runs; its enter runs again on pop).
function Machine:push(name: string, ...)
	table.insert(self.stack, self.state)
	self:go(name, ...)
end

-- Leaves the state pushed last for the one below it (or `fallback` when nothing is below).
function Machine:pop(fallback: string?)
	local below = table.remove(self.stack)
	self:go(below or fallback or self.state)
end

-- Whether the machine is in `name`.
function Machine:is(name: string): boolean
	return self.state == name
end

return States
