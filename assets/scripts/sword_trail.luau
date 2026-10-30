-- A weapon trail on Beowulf's katana. The katana (the skinned mesh "Katana2") moves with the "right_hand"
-- bone, so the trail follows two points under that bone, at the blade's base and tip: the empties
-- "blade_base" and "blade_tip" made in Blender (parented to the bone), or, for a model without them, points
-- added here (in the hand's own space the blade runs along +Z, from the guard to the tip at about 1.95).
--
-- Each slash shows the drawn katana (hiding the one in the saya), records the trail while the slash plays,
-- then goes back to Idle with the katana sheathed. Fire slashes; with `auto` it also slashes by itself.
-- Special (Q, right shoulder) slashes and sends a wave: a Bezier slash (a crescent drawn as it flies)
-- with sparks on its head, a short radial blur and a blue tint.
local util = require("scripts/lib/util")

local SwordTrail = {}
SwordTrail.properties = {
	auto = true, -- slash by itself every `interval` seconds
	interval = 2.2,
	fade = 0.15, -- crossfade between animations, seconds
}

local SLASHES = { "Slash0", "Slash1", "Slash2", "Slash3", "Slash4", "Slash5" }
local WAVE_SECONDS = 0.7
local WAVE_SPEED = 5 -- meters per second
local WAVE_DELAY = 0.15 -- seconds into the swing it leaves the blade

-- The entity at or below `root` that plays the model's animations (has an Animator), or nil.
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

-- A named point under `parent`, at `position` in the parent's space.
local function add_point(parent: number, name: string, position: vector): number
	local point = wpe.spawn(name)
	wpe.set_parent(point, parent, false)
	wpe.set_position(point, position)
	return point
end

function SwordTrail:on_start()
	self.figure = find_animated(self.entity)
	local hand = util.find_child(self.entity, "right_hand")
	assert(self.figure and hand, "the swordsman needs Beowulf's model under it")
	self.katana = util.find_child(self.entity, "Katana2")
	self.sheathed = util.find_child(self.entity, "SayaKatana2")
	local base = util.find_child(self.entity, "blade_base") or add_point(hand, "blade base", vector.create(0, 0.03, 0.5))
	local tip = util.find_child(self.entity, "blade_tip") or add_point(hand, "blade tip", vector.create(0, 0.03, 1.95))
	self.tip = tip
	wpe.set_trail(self.entity, {
		base = base, tip = tip, emitting = false, lifetime = 0.22, width_end = 0.25,
		color_start = wpe.color(0.8, 0.9, 1, 0.85), color_end = wpe.color(0.35, 0.45, 1, 0),
	})
	self.next_slash = 1
	self.wait = 0.6
	self:sheathe(true)
	self:make_wave()
end

-- The wave: a standing crescent bulging forward (along its local +Z, its flight), and sparks that follow its
-- head.
function SwordTrail:make_wave()
	self.wave = wpe.spawn("sword wave")
	local sparks = wpe.spawn("wave sparks")
	wpe.set_parent(sparks, self.wave, false)
	wpe.set_slash(self.wave, {
		points = { vector.create(0, -1.1, 0), vector.create(0, -0.6, 0.7), vector.create(0, 0.6, 0.7), vector.create(0, 1.1, 0) },
		width = 0.4, length = 0.8, duration = WAVE_SECONDS, head = sparks,
		color_head = wpe.color(0.9, 0.95, 1, 1), color_tail = wpe.color(0.3, 0.45, 1, 0),
	})
	wpe.set_particles(sparks, {
		playing = false, rate = 160, lifetime = { 0.2, 0.45 }, speed = { 0.4, 1.4 }, spread = math.pi, size = { 0.07, 0.01 },
		color_start = wpe.color(0.85, 0.9, 1, 1), color_end = wpe.color(0.3, 0.5, 1, 0), gravity = vector.create(0, -2, 0),
	})
	self.sparks = sparks
end

-- Sends the wave a moment into the swing, from chest height beside the swordsman, flying level toward the
-- screen's right (where his swings finish, seen from this camera), the crescent facing its flight. With its
-- screen effects.
function SwordTrail:send_wave()
	wpe.after(WAVE_DELAY, function()
		local _, right = util.camera_axes()
		local start = wpe.get_world_position(self.entity) + vector.create(0, 1.1, 0) + right * 0.6
		self.wave_direction = right
		self.wave_start = start
		self.wave_time = 0
		wpe.set_position(self.wave, start)
		-- The crescent bulges along its local +Z: turn that onto the flight.
		wpe.set_rotation(self.wave, vector.create(0, math.atan2(right.x, right.z), 0))
		wpe.play_slash(self.wave)
		wpe.set_particles(self.sparks, { playing = true })
		wpe.add_postfx({ effect = "radial_blur", strength = 0.12 })
		wpe.add_postfx({ effect = "grayscale", strength = 0.3, tint_r = 0.6, tint_g = 0.8, tint_b = 1.3 })
	end)
	self.wave_pending = true
end

-- Flies the wave forward and fades its screen effects; removes them when it's done.
function SwordTrail:update_wave(dt)
	if not self.wave_time then
		return
	end
	self.wave_time += dt
	local t = math.min(self.wave_time / WAVE_SECONDS, 1)
	wpe.set_position(self.wave, self.wave_start + self.wave_direction * (WAVE_SPEED * self.wave_time))
	local effects = wpe.get_postfx()
	for i = #effects, 1, -1 do
		if effects[i].effect == "radial_blur" then
			wpe.set_postfx(i, { strength = 0.12 * (1 - t) })
		elseif effects[i].effect == "grayscale" then
			wpe.set_postfx(i, { strength = 0.3 * (1 - t) })
		end
	end
	if t >= 1 then
		self.wave_time = nil
		self.wave_pending = false
		wpe.set_particles(self.sparks, { playing = false })
		for i = #effects, 1, -1 do
			if effects[i].effect == "radial_blur" or effects[i].effect == "grayscale" then
				wpe.remove_postfx(i)
			end
		end
	end
end

function SwordTrail:sheathe(sheathed: boolean)
	if self.katana then
		wpe.set_visible(self.katana, not sheathed)
	end
	if self.sheathed then
		wpe.set_visible(self.sheathed, sheathed)
	end
end

function SwordTrail:slash()
	local name = SLASHES[self.next_slash]
	self.next_slash = self.next_slash % #SLASHES + 1
	wpe.play_animation(self.figure, name, false, self.fade)
	wpe.clear_trail(self.entity) -- a new swing doesn't join the last
	wpe.set_trail(self.entity, { emitting = true })
	self:sheathe(false)
	self.slashing = true
end

function SwordTrail:on_update(dt)
	self:update_wave(dt)
	if self.slashing then
		if not wpe.get_animation(self.figure).playing then
			-- The swing ended: the trail fades out, the katana goes back to the saya.
			self.slashing = false
			wpe.set_trail(self.entity, { emitting = false })
			wpe.play_animation(self.figure, "Idle", true, self.fade)
			self:sheathe(true)
			self.wait = self.interval
		end
	elseif wpe.action_pressed("fire") then
		self:slash()
	elseif wpe.action_pressed("special") and not self.wave_pending then
		self:slash()
		self:send_wave()
	elseif self.auto then
		self.wait -= dt
		if self.wait <= 0 then
			self:slash()
		end
	end
end

return SwordTrail
