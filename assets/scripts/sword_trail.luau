-- A weapon trail on Beowulf's katana. The katana (the skinned mesh "Katana2") moves with the "right_hand"
-- bone, so the trail follows two points under that bone, at the blade's base and tip: the empties
-- "blade_base" and "blade_tip" made in Blender (parented to the bone), or, for a model without them, points
-- added here (in the hand's own space the blade runs along +Z, from the guard to the tip at about 1.95).
--
-- Each slash shows the drawn katana (hiding the one in the saya), records the trail while the slash plays,
-- then goes back to Idle with the katana sheathed. Fire slashes; with `auto` it also slashes by itself.
local util = require("scripts/lib/util")

local SwordTrail = {}
SwordTrail.properties = {
	auto = true, -- slash by itself every `interval` seconds
	interval = 2.2,
	fade = 0.15, -- crossfade between animations, seconds
}

local SLASHES = { "Slash0", "Slash1", "Slash2", "Slash3", "Slash4", "Slash5" }

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
	wpe.set_trail(self.entity, {
		base = base, tip = tip, emitting = false, lifetime = 0.22, width_end = 0.25,
		color_start = wpe.color(0.8, 0.9, 1, 0.85), color_end = wpe.color(0.35, 0.45, 1, 0),
	})
	self.next_slash = 1
	self.wait = 0.6
	self:sheathe(true)
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
	elseif self.auto then
		self.wait -= dt
		if self.wait <= 0 then
			self:slash()
		end
	end
end

return SwordTrail
