-- A 2.5D platformer hero on a character controller, moving along X only (the level's plane is Z = 0).
--
--   Move: A/D, arrows, left stick, d-pad     Jump: Space, W, Up, A (south); hold for higher jumps
--   Restart the level: R, Back
--
-- Platformer feel: a jump pressed just before landing still happens (jump buffer), a jump just after
-- running off a ledge still works (coyote time), and letting go of jump early cuts the jump short.
-- The child "visual" (pivot at the feet) faces the way the hero runs and squashes and stretches.
--
-- Other scripts call into it: collect_coin() (coins), hurt() (hazards), set_checkpoint(position)
-- (checkpoints) and finish() (the goal). Standing on something whose script has a `velocity` (the
-- moving platform) carries the hero along.
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
	self.since_ground = 0
	self.since_jump_press = math.huge
	self.jump_held = false
	self.was_grounded = true
	self.coins = 0
	self.total_coins = nil -- counted once every coin has started
	self.time = 0
	self.finished = false
	self.flash = 0 -- the red flash after getting hurt
end

-- ---- Called by other scripts ----------------------------------------------------------------------

function Player:collect_coin()
	self.coins += 1
end

function Player:hurt()
	if self.finished then
		return
	end
	wpe.set_position(self.entity, self.checkpoint)
	self.velocity = vector.create(0, 0, 0)
	self.flash = 1
end

function Player:set_checkpoint(position: vector)
	self.checkpoint = position
end

function Player:finish()
	if not self.finished then
		self.finished = true
		self.velocity = vector.create(0, self.velocity.y, 0)
	end
end

-- ---- Frame update: input edges, visuals, HUD ---------------------------------------------------

function Player:on_update(dt)
	if self.total_coins == nil then
		self.total_coins = wpe.broadcast("is_coin") -- every coin's script answers once it has started
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
			"Press Jump to play again",
		}
		for i, line in lines do
			local w = wpe.text_size(line, i == 1 and 2 or 1)
			wpe.draw_text((width - w) // 2, height // 3 + (i - 1) * 22, line, wpe.color(1, 1, 1), i == 1 and 2 or 1)
		end
	else
		wpe.draw_text(6, height - 12, "A/D move  Space jump (hold: higher)  R restart", wpe.color(1, 1, 1, 0.7))
	end
end

-- ---- Fixed update: movement ------------------------------------------------------------------------

function Player:on_fixed_update(dt)
	local character = wpe.get_character(self.entity)
	local grounded = character.grounded
	self.since_ground = if grounded then 0 else self.since_ground + dt
	self.since_jump_press += dt

	-- Run toward the wished speed.
	local wish = if self.finished then 0 else math.clamp(wpe.action_value("move_x"), -1, 1)
	if math.abs(wish) > 0.1 then
		self.facing = if wish > 0 then 1 else -1
	end
	local accel = if grounded then self.acceleration else self.air_acceleration
	local v = self.velocity
	local vx = approach(v.x, wish * self.run_speed, accel * dt)
	local vy = v.y - self.gravity * dt

	-- Jump: buffered presses and coyote time; letting go early cuts the rise.
	if self.since_jump_press <= self.jump_buffer and self.since_ground <= self.coyote_time and not self.finished then
		vy = self.jump_speed
		self.since_jump_press = math.huge
		self.since_ground = math.huge -- no second jump from the same ledge
		self.squash = 0.3
	elseif vy > 0 and not self.jump_held then
		vy *= self.jump_cut ^ (dt * 60) -- smooth: about jump_cut per frame at 60 Hz
	end

	-- A moving platform under the hero carries it.
	local carry = vector.create(0, 0, 0)
	if grounded and character.ground then
		local ground_script = wpe.get_script(character.ground)
		if ground_script and typeof(ground_script.velocity) == "vector" then
			carry = vector.create(ground_script.velocity.x, 0, 0)
		end
	end
	local moved, now_grounded = wpe.move_character(self.entity, vector.create(vx, vy, 0) + carry, dt)
	self.velocity = vector.create(moved.x - carry.x, moved.y, 0)
	if now_grounded and not self.was_grounded and vy < -4 then
		self.squash = -0.3 -- landing
	end
	self.was_grounded = now_grounded

	-- Stay on the level's plane.
	local p = wpe.get_position(self.entity)
	if math.abs(p.z) > 1e-3 then
		wpe.set_position(self.entity, vector.create(p.x, p.y, 0))
	end
end

return Player
