-- A villager you can talk to. It stands (or, with a `wander` radius, strolls around its home on the
-- navigation grid); when the player comes near it turns to them and shows what to press over its head;
-- pressing it opens its conversation (`conversation`: a file in dialogues/) in the dialogue box.
--
-- Its behaviour is a state machine (scripts/lib/states.luau): idle -> noticed -> talking -> resting -> idle.
-- Each state is a table of functions; a function returning a state's name moves the machine there.
local States = require("scripts/lib/states")
local Conversation = require("scripts/lib/conversation")

local Talker = {}
Talker.properties = {
	conversation = "dialogues/elder", -- the conversation's module path (without .luau)
	talk_radius = 2.5, -- how near the player must be (m)
	wander = 0, -- wander this far from home (m); 0 stands still (wandering needs a nav_agent)
	turn_speed = 6, -- how quickly it turns to face the player
	prompt_height = 2.15, -- where "Talk" shows, above its feet (m)
	model = "figure", -- the child entity with the animated model
}

local function find_animated(root: number): number?
	if pcall(wpe.get_animation, root) then
		return root
	end
	for _, child in wpe.children(root) do
		local found = find_animated(child)
		if found then
			return found
		end
	end
	return nil
end

local function angle_delta(from: number, to: number): number
	return (to - from + math.pi) % (2 * math.pi) - math.pi
end

-- Several villagers may be near at once: the nearest one (as of the last frame) is the one that answers.
local Near = { frame = -1, best = nil :: number?, best_distance = math.huge, chosen = nil :: number? }

local function offer(entity: number, distance: number): boolean
	local frame = wpe.frame()
	if frame ~= Near.frame then
		Near.chosen, Near.best, Near.best_distance, Near.frame = Near.best, nil, math.huge, frame
	end
	if distance < Near.best_distance then
		Near.best, Near.best_distance = entity, distance
	end
	return Near.chosen == entity
end

-- ---- States -------------------------------------------------------------------------------------

Talker.states = {
	-- Standing, or strolling between random spots near home.
	idle = {
		enter = function(self)
			self.rest = 0.5
		end,
		update = function(self, dt)
			if self.wander > 0 and wpe.nav_state(self.entity) ~= "moving" then
				self.rest -= dt
				if self.rest <= 0 then
					local spot = wpe.nav_random_point(self.home, self.wander)
					if spot then
						wpe.nav_move_to(self.entity, spot)
					end
					self.rest = 1.5 + 2 * math.random()
				end
			end
			if self:player_distance() < self.talk_radius and not Conversation.active then
				return "noticed"
			end
		end,
	},
	-- The player is near: stop, face them, offer to talk.
	noticed = {
		enter = function(self)
			if self.wander > 0 then
				wpe.nav_stop(self.entity)
			end
		end,
		update = function(self, dt)
			local distance = self:player_distance()
			if distance > self.talk_radius + 0.5 or Conversation.active then
				return "idle"
			end
			self:face_player(dt)
			self.answering = offer(self.entity, distance)
			if self.answering then
				self:draw_prompt()
				if wpe.action_pressed("interact") then
					return "talking"
				end
			end
		end,
		exit = function(self)
			self.answering = false
		end,
	},
	-- In conversation: the dialogue box runs it; this one keeps facing the player until it ends.
	talking = {
		enter = function(self)
			if not wpe.send(self.box, "open", self.data, self.entity) then
				return "resting" -- Nothing to say (no start entry's conditions hold).
			end
		end,
		update = function(self, dt)
			self:face_player(dt)
			if not Conversation.active or Conversation.active.talker ~= self.entity then
				return "resting"
			end
		end,
	},
	-- A moment after talking, so the key that ended it doesn't start it again.
	resting = {
		update = function(self, dt)
			self:face_player(dt)
			if self.machine.time > 0.6 then
				return "idle"
			end
		end,
	},
}

-- ---- The rest -----------------------------------------------------------------------------------

function Talker:on_start()
	self.data = require(self.conversation)
	Conversation.check(self.data, self.conversation) -- Mistakes in the file show now, not mid-talk.
	self.home = wpe.get_position(self.entity)
	self.player = wpe.find("player")
	self.box = wpe.find("dialogue box")
	for _, child in wpe.children(self.entity) do
		if wpe.get_name(child) == self.model then
			self.figure = find_animated(child)
		end
	end
	if self.figure then
		wpe.animation_speed(self.figure, 0)
	end
	self.machine = States.new(self, Talker.states, "idle")
end

function Talker:player_distance(): number
	if not self.player then
		return math.huge
	end
	local d = wpe.get_position(self.player) - wpe.get_position(self.entity)
	return vector.magnitude(vector.create(d.x, 0, d.z))
end

-- Turns toward the player (the entity looks down -Z).
function Talker:face_player(dt: number)
	local d = wpe.get_position(self.player) - wpe.get_position(self.entity)
	if math.abs(d.x) + math.abs(d.z) < 1e-3 then
		return
	end
	local yaw = wpe.get_rotation(self.entity).y -- (Walking, the navigation turned it.)
	yaw += angle_delta(yaw, math.atan2(-d.x, -d.z)) * math.min(1, dt * self.turn_speed)
	wpe.set_rotation(self.entity, vector.create(0, yaw, 0))
end

-- "E Talk to Maren" over its head: the key or button of the device the player used last.
function Talker:draw_prompt()
	local x, y, in_front = wpe.world_to_screen(wpe.get_position(self.entity) + vector.create(0, self.prompt_height, 0))
	if not in_front then
		return
	end
	local text = string.format("%s  Talk to %s", wpe.action_prompt("interact") or "?", self.data.name or "")
	local w = wpe.text_size(text)
	x, y = math.floor(x - w / 2), math.floor(y)
	wpe.draw_rect(x - 3, y - 2, w + 6, 12, wpe.color(0, 0, 0, 0.55))
	wpe.draw_text(x, y, text, wpe.color(1, 0.92, 0.6))
end

function Talker:on_update(dt)
	if self.wander > 0 and not self.machine:is("idle") then
		wpe.nav_stop(self.entity) -- (In case a path finished arriving after it stopped.)
	end
	if self.player then
		self.machine:update(dt)
	end
	if self.figure and self.wander > 0 then
		-- Walking plays the model's clip by speed; standing holds its first frame.
		local v = wpe.get_nav_agent(self.entity).velocity
		wpe.animation_speed(self.figure, vector.magnitude(vector.create(v.x, 0, v.z)) / 1.5)
	end
end

function Talker:on_nav_arrived()
	self.machine:send("on_nav_arrived")
end

return Talker
