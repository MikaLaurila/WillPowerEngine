-- A 2.5D platformer hero on a character controller, moving along X only (the level's plane is Z = 0).
--
--   Move: A/D, arrows, left stick, d-pad     Jump: Space, W, Up, A (south); hold for higher jumps
--   Restart the level: R, Back               Menu (controls, quit): Esc, Start
--
-- Players can change those keys in the menu (scripts/controls.luau); the hints at the bottom show the keys
-- or buttons of the device they last used (wpe.action_prompt).
--
-- Platformer feel: a jump pressed just before landing still happens (jump buffer), a jump just after
-- running off a ledge still works (coyote time), and letting go of jump early cuts the jump short.
-- The child "visual" (pivot at the feet) faces the way the hero runs and squashes and stretches.
--
-- Other scripts call into it: collect_coin() (coins), hurt() (hazards), set_checkpoint(position)
-- (checkpoints) and finish() (the goal). Standing on something whose script has a `velocity` (the
-- moving platform) carries the hero along.
--
-- The movement is a state machine (scripts/lib/states.luau), one state per situation, each with its own
-- rules in `fixed_update` and the way out as the name it returns:
--
--   grounded: idle, run       ground acceleration; jump (also a buffered one on landing); walk off a ledge
--   airborne: jump, fall      air control and gravity; jump cuts the rise when let go; fall allows a late
--                             (coyote) jump right after leaving the ground; landing goes back to grounded
--   respawn                   hurt: back at the checkpoint, a moment still, then falls from there
--   finished                  at the goal: no more control
--
-- idle and run extend `grounded`, jump and fall extend `airborne` (States.extend), so what they share is
-- written once. A new move (a dash, a wall slide, a ledge grab) is one more state. Turn on `show_state` in
-- the inspector to see the state and its time over the hero while tuning.
local Menu = require("scripts/menu_state")
local States = require("scripts/lib/states")

local Player = {}
Player.properties = {
	run_speed = 6, -- m/s
	acceleration = 40, -- m/s² toward the wished speed on the ground
	air_acceleration = 25,
	jump_speed = 9.5, -- with gravity 28: about 1.6 m high
	gravity = 28,
	fall_limit = -10, -- below this height the hero is hurt
	coyote_time = 0.1, -- seconds after leaving a ledge that a jump still works
	jump_buffer = 0.12, -- seconds a jump press waits for the ground
	jump_cut = 0.45, -- the share of upward speed kept when jump is let go early
	respawn_time = 0.35, -- seconds standing still at the checkpoint after getting hurt
	show_state = false, -- draw the movement state over the hero (for tuning)
}

local function approach(value: number, target: number, step: number): number
	if value < target then
		return math.min(value + step, target)
	end
	return math.max(value - step, target)
end

local FACING_YAW = 2.04 -- radians: about 117 degrees, three quarters toward the camera

-- The shortest turn from angle `from` to angle `to` (radians), in [-pi, pi).
local function angle_delta(from: number, to: number): number
	return (to - from + math.pi) % (2 * math.pi) - math.pi
end

function Player:on_start()
	for _, child in wpe.children(self.entity) do
		if wpe.get_name(child) == "visual" then
			self.visual = child
		end
	end
	self.checkpoint = wpe.get_position(self.entity)
	self.velocity = vector.create(0, 0, 0)
	self.facing = 1 -- +1 right, -1 left
	self.squash = 0 -- > 0 stretched (rising), < 0 squashed (landing); eases back to 0
	self.since_jump_press = math.huge
	self.jump_held = false
	self.wish = 0 -- the stick (or keys) along X, -1..1
	self.grounded, self.ground = true, nil
	self.impact = 0 -- vertical speed of the last step (a landing's force)
	self.coyote = 0
	self.coins = 0
	self.total_coins = nil -- counted once every coin has started
	self.time = 0
	self.finished = false
	self.flash = 0 -- the red flash after getting hurt
	self.machine = States.new(self, Player.states, "idle")
end

-- ---- Called by other scripts ----------------------------------------------------------------------

function Player:collect_coin()
	self.coins += 1
end

function Player:hurt()
	if not self.finished and not self.machine:is("respawn") then
		self.machine:go("respawn")
	end
end

function Player:set_checkpoint(position: vector)
	self.checkpoint = position
end

function Player:finish()
	if not self.finished then
		self.machine:go("finished")
	end
end

-- ---- Frame update: input edges, visuals, HUD ---------------------------------------------------

function Player:on_update(dt)
	if self.total_coins == nil then
		self.total_coins = wpe.broadcast("is_coin") -- every coin's script answers once it has started
	end
	if Menu.open then -- the game is paused: no input (the menu takes it)
		self.jump_held = false
		self:draw_hud()
		return
	end
	if wpe.action_pressed("restart") or (self.finished and wpe.action_pressed("jump")) then
		wpe.load_scene("scenes/main.scene")
		return
	end
	if wpe.action_pressed("jump") then
		self.since_jump_press = 0
	end
	self.jump_held = wpe.action_down("jump")
	if not self.finished then
		self.time += dt
	end
	if wpe.get_position(self.entity).y < self.fall_limit then
		self:hurt()
	end
	self.machine:update(dt) -- Counts machine.time; respawn's wait is frame-timed.
	if self.show_state then
		local x, y, in_front = wpe.world_to_screen(wpe.get_position(self.entity) + vector.create(0, 1.4, 0))
		if in_front then
			local label = string.format("%s %.2f", self.machine.state, self.machine.time)
			wpe.draw_text(math.floor(x - wpe.text_size(label) / 2), math.floor(y), label, wpe.color(1, 1, 0.5))
		end
	end

	-- Face the way it runs, turned partly toward the camera so its face shows (the visual looks down -Z:
	-- a yaw of -FACING_YAW looks right and toward the camera), turning around through the front; and ease
	-- the squash and stretch back.
	if self.visual then
		local turn = -FACING_YAW * self.facing
		local yaw = wpe.get_rotation(self.visual).y
		yaw += angle_delta(yaw, turn) * math.min(1, dt * 18)
		wpe.set_rotation(self.visual, vector.create(0, yaw, 0))
		self.squash = approach(self.squash, 0, dt * 3)
		local s = self.squash
		wpe.set_scale(self.visual, vector.create(1 - 0.5 * s, 1 + s, 1 - 0.5 * s))
	end
	self.flash = math.max(0, self.flash - dt * 2)
	self:draw_hud()
end

-- What to press for an action, on the device the player used last ("Space", "A", "Cross"...).
local function prompt(action: string, direction: number?): string
	return wpe.action_prompt(action, nil, direction) or "?"
end

function Player:draw_hud()
	local width, height = wpe.screen_size()
	if self.flash > 0 then
		wpe.draw_rect(0, 0, width, height, wpe.color(1, 0.1, 0.1, 0.35 * self.flash))
	end
	wpe.draw_text(6, 6, string.format("Coins %d/%d", self.coins, self.total_coins or 0), wpe.color(1, 0.85, 0.3))
	local clock = string.format("%d:%04.1f", self.time // 60, self.time % 60)
	wpe.draw_text(width - 6 - wpe.text_size(clock), 6, clock)
	if self.finished then
		local lines = {
			"Level complete!",
			string.format("Coins %d/%d   Time %s", self.coins, self.total_coins or 0, clock),
			string.format("Press %s to play again", prompt("jump")),
		}
		for i, line in lines do
			local w = wpe.text_size(line, i == 1 and 2 or 1)
			wpe.draw_text((width - w) // 2, height // 3 + (i - 1) * 22, line, wpe.color(1, 1, 1), i == 1 and 2 or 1)
		end
	elseif not Menu.open then
		-- "A/D move" on the keyboard; on a pad the stick moves both ways, so it shows once.
		local left, right = prompt("move_x", -1), prompt("move_x", 1)
		local move = if left == right then left else left .. "/" .. right
		local hint = string.format("%s move  %s jump  %s restart  %s menu", move, prompt("jump"), prompt("restart"),
			prompt("pause"))
		wpe.draw_text(6, height - 12, hint, wpe.color(1, 1, 1, 0.7))
	end
end

-- ---- Movement helpers (the states use them) ------------------------------------------------------

-- Turns the horizontal speed toward the stick (or `wish`) at `acceleration`, and pulls down with gravity.
function Player:steer(dt: number, acceleration: number, wish: number?)
	wish = wish or self.wish
	local v = self.velocity
	self.velocity = vector.create(approach(v.x, wish * self.run_speed, acceleration * dt), v.y - self.gravity * dt, 0)
end

function Player:jump_buffered(): boolean
	return self.since_jump_press <= self.jump_buffer
end

-- idle or run, by the stick; nil when already in it (no restart every step).
function Player:ground_state(): string?
	local wanted = if math.abs(self.wish) > 0.1 then "run" else "idle"
	return if self.machine:is(wanted) then nil else wanted
end

-- Moves the character by its velocity this step (a moving platform underneath carries it along).
function Player:move(dt: number)
	local carry = vector.create(0, 0, 0)
	if self.grounded and self.ground then
		local ground_script = wpe.get_script(self.ground)
		if ground_script and typeof(ground_script.velocity) == "vector" then
			carry = vector.create(ground_script.velocity.x, 0, 0)
		end
	end
	self.impact = self.velocity.y -- How fast it came down, for the landing squash next step.
	local moved = wpe.move_character(self.entity, self.velocity + carry, dt)
	self.velocity = vector.create(moved.x - carry.x, moved.y, 0)
	-- Stay on the level's plane.
	local p = wpe.get_position(self.entity)
	if math.abs(p.z) > 1e-3 then
		wpe.set_position(self.entity, vector.create(p.x, p.y, 0))
	end
end

-- ---- The states ------------------------------------------------------------------------------------

-- On the ground: run toward the stick, jump, or walk off into the air.
local grounded = {
	enter = function(self, from: string?)
		if from == "jump" or from == "fall" then
			if self.impact < -4 then
				self.squash = -0.3 -- A hard landing squashes.
			end
			if self:jump_buffered() then
				return "jump" -- Pressed just before landing: it still counts.
			end
		end
	end,
	fixed_update = function(self, dt)
		self:steer(dt, self.acceleration)
		if self:jump_buffered() then
			return "jump"
		end
		if not self.grounded then
			return "fall"
		end
		return self:ground_state()
	end,
}

-- In the air: less control, gravity, and back to the ground on landing.
local airborne = {
	fixed_update = function(self, dt)
		self:steer(dt, self.air_acceleration)
		if self.grounded and self.velocity.y <= 0 then
			return self:ground_state() or "idle"
		end
	end,
}

Player.states = {
	idle = States.extend(grounded),
	run = States.extend(grounded),
	jump = States.extend(airborne, {
		enter = function(self)
			self.velocity = vector.create(self.velocity.x, self.jump_speed, 0)
			self.since_jump_press = math.huge
			self.squash = 0.3
		end,
		fixed_update = function(self, dt)
			local landed = airborne.fixed_update(self, dt)
			if landed then
				return landed
			end
			local vy = self.velocity.y
			if vy > 0 and not self.jump_held then
				-- Let go early: the rise is cut (about jump_cut per frame at 60 Hz, smoothly).
				self.velocity = vector.create(self.velocity.x, vy * self.jump_cut ^ (dt * 60), 0)
			elseif vy <= 0 then
				return "fall"
			end
		end,
	}),
	fall = States.extend(airborne, {
		enter = function(self, from: string?)
			-- Walked off a ledge: a jump still works for a moment (coyote time). Not after a jump.
			self.coyote = if from == "idle" or from == "run" then self.coyote_time else 0
		end,
		fixed_update = function(self, dt)
			self.coyote -= dt
			if self.coyote > 0 and self:jump_buffered() then
				return "jump"
			end
			return airborne.fixed_update(self, dt)
		end,
	}),
	-- Hurt: back at the checkpoint, still for a moment, then falling from there (whatever it was doing
	-- before doesn't matter any more, so this goes on to fall rather than back with push/pop).
	respawn = {
		enter = function(self)
			wpe.set_position(self.entity, self.checkpoint)
			self.velocity = vector.create(0, 0, 0)
			self.flash = 1
		end,
		fixed_update = function(self, dt)
			self.velocity = vector.create(0, 0, 0)
		end,
		update = function(self, dt)
			if self.machine.time >= self.respawn_time then
				return "fall"
			end
		end,
	},
	-- At the goal: it comes to a stop; nothing answers the controls any more.
	finished = {
		enter = function(self)
			self.finished = true
		end,
		fixed_update = function(self, dt)
			self:steer(dt, if self.grounded then self.acceleration else self.air_acceleration, 0)
		end,
	},
}

-- ---- Fixed update: movement ------------------------------------------------------------------------

function Player:on_fixed_update(dt)
	local character = wpe.get_character(self.entity)
	self.grounded, self.ground = character.grounded, character.ground
	self.since_jump_press += dt
	self.wish = math.clamp(wpe.action_value("move_x"), -1, 1)
	if math.abs(self.wish) > 0.1 and not self.finished then
		self.facing = if self.wish > 0 then 1 else -1
	end
	self.machine:fixed_update(dt) -- The state sets the velocity (and may move on to another state)...
	self:move(dt) -- ...and the character moves by it.
end

return Player
