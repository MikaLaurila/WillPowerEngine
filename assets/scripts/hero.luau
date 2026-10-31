-- A walking character on a character controller: moves relative to the camera (move_x / move_y),
-- runs (boost), jumps when standing (jump), faces the way it walks, and goes back to its start
-- (reset, or after a fall). Slopes up to the controller's max_slope are walkable, steeper ones
-- aren't; crates get pushed. "fire" sends a shockwave out from it (post effects driven by script) with a
-- hit-stop and a camera shake; "special" cuts the space in front of it (Judgement Cut,
-- scripts/lib/judgement_cut.luau) in slow motion.
local util = require("scripts/lib/util")
local JudgementCut = require("scripts/lib/judgement_cut")

local Hero = {}
Hero.properties = { speed = 4, run_speed = 7.5, jump_speed = 6.5, gravity = 18, player = 1 }

function Hero:on_start()
	self.spawn = wpe.get_position(self.entity)
	self.velocity = vector.create(0, 0, 0)
	self.jump_requested = false
	self.yaw = wpe.get_rotation(self.entity).y
	-- The "fire" action's shockwave: two post effects, off until used.
	wpe.add_postfx({ effect = "shockwave", enabled = false, width = 0.06 })
	wpe.add_postfx({ effect = "radial_blur", enabled = false })
end

local WAVE_SECONDS = 0.4

-- Animates the shockwave: a ring growing out from the hero while it and a radial blur fade.
function Hero:update_wave(dt)
	if not self.wave then
		return
	end
	self.wave += dt
	local t = self.wave / WAVE_SECONDS
	if t >= 1 then
		self.wave = nil
		wpe.set_postfx("shockwave", { enabled = false })
		wpe.set_postfx("radial_blur", { enabled = false })
		return
	end
	-- Centered on the hero's chest; effect positions are fractions of the screen.
	local x, y = wpe.world_to_screen(wpe.get_world_position(self.entity) + vector.create(0, 1, 0))
	local width, height = wpe.screen_size()
	local cx, cy = x / width, y / height
	wpe.set_postfx("shockwave", {
		enabled = true, center_x = cx, center_y = cy, radius = 0.05 + 0.55 * t, strength_pixels = 8 * (1 - t),
	})
	wpe.set_postfx("radial_blur", { enabled = true, center_x = cx, center_y = cy, strength = 0.12 * (1 - t) })
end

function Hero:on_update(dt)
	-- Edges ("pressed") are per frame, so they are read here and consumed by the fixed update.
	if wpe.action_pressed("jump", self.player) then
		self.jump_requested = true
	end
	if wpe.action_pressed("reset", self.player) or wpe.get_position(self.entity).y < -20 then
		wpe.set_position(self.entity, self.spawn)
		self.velocity = vector.create(0, 0, 0)
	end
	if wpe.action_pressed("fire", self.player) then
		self.wave = 0
		wpe.play_sound("sounds/thud.wav", { volume = 0.4, pitch = 0.6 })
		-- Impact: the game holds for a few frames while the camera jolts (shakes run on through a hit-stop).
		wpe.hit_stop(0.07)
		wpe.shake(wpe.get_camera(), { amplitude = 0.06, rotation = 0.8, duration = 0.3, frequency = 28 })
	end
	if wpe.action_pressed("special", self.player) and not self.cut then
		-- Where the cuts cross: a point four meters ahead of the hero, at chest height.
		local ahead = wpe.get_world_position(self.entity) + wpe.forward(self.entity) * 4 + vector.create(0, 1.2, 0)
		local x, y = wpe.world_to_screen(ahead)
		local width, height = wpe.screen_size()
		self.cut = JudgementCut.start(x / width, y / height)
		wpe.play_sound("sounds/thud.wav", { volume = 0.5, pitch = 1.4 })
		-- Time crawls while the space is cut, easing back to full speed; the camera rolls with it.
		wpe.slow_motion(0.35, 1.2, 0.4)
		wpe.shake(wpe.get_camera(), { amplitude = 0.04, rotation = 1.5, duration = 0.8, frequency = 12 })
	end
	if self.cut and not self.cut:update(dt) then
		self.cut = nil
	end
	self:update_wave(dt)
end

function Hero:on_fixed_update(dt)
	local forward, right = util.camera_axes()
	local wish = right * wpe.action_value("move_x", self.player) + forward * wpe.action_value("move_y", self.player)
	if vector.magnitude(wish) > 1 then
		wish = vector.normalize(wish)
	end
	local speed = self.speed + (self.run_speed - self.speed) * wpe.action_value("boost", self.player)
	local grounded = wpe.get_character(self.entity).grounded

	-- Steer toward the wished velocity (less control in the air), and fall.
	local target = wish * speed
	local blend = math.min(1, dt * (grounded and 12 or 3))
	local v = self.velocity
	v = vector.create(v.x + (target.x - v.x) * blend, v.y - self.gravity * dt, v.z + (target.z - v.z) * blend)
	if self.jump_requested and grounded then
		v = vector.create(v.x, self.jump_speed, v.z)
	end
	self.jump_requested = false
	-- The controller slides along what it hits and returns what is left of the velocity.
	self.velocity = wpe.move_character(self.entity, v, dt)

	-- Turn to face the way it walks (the entity looks down -Z).
	if vector.magnitude(wish) > 0.1 then
		local goal = math.atan2(-wish.x, -wish.z)
		self.yaw += util.angle_delta(self.yaw, goal) * math.min(1, dt * 12)
		wpe.set_rotation(self.entity, vector.create(0, self.yaw, 0))
	end
end

return Hero
