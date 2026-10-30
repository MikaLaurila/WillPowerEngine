-- Judgement Cut: cuts are drawn across the screen one after another around a point, hold, then end.
--
--   local JudgementCut = require("scripts/lib/judgement_cut")
--   local cut = JudgementCut.start(x, y)       -- x, y: where the cuts cross, as fractions of the screen
--   ...each frame: if cut and not cut:update(dt) then cut = nil end
--   cut:stop()                                 -- ends it at once (the "stay" ending needs it)
--
-- Options (all optional), the third argument of start:
--   lines = 10         how many cuts
--   interval = 0.045   seconds between them
--   hold = 0.45        seconds the finished cuts stay before the ending
--   ending = "shatter" "shatter": the screen breaks along the cuts and falls away;
--                      "fade": the cuts fade out; "stay": they stay until stop()
--   shatter_time = 1.4 seconds the pieces fall
--   spread = 0.15      how far from the point the cuts may pass (fraction of the screen's height)
--   dim = 0.3          how much darker the scene gets during the cuts (0: not at all)
--   glow               the rim's tint (a vector; bluish by default). The cuts look like broken glass: the
--                      image is pulled and streaked along them, with the tinted rim on one side
--   core               a color for a line drawn along each cut (none by default; white for cuts of light)
--   slot = 8           the capture slot the shatter breaks (wpe.capture_screen)
--   seed               which cuts (random by default)
--
-- It adds its own post effects (color_adjust, cracks, shatter) after the scene's, and removes them when
-- it ends. Another script removing effects meanwhile can upset it.
local JudgementCut = {}
JudgementCut.__index = JudgementCut

local DEFAULTS = {
	lines = 10,
	interval = 0.045,
	hold = 0.45,
	ending = "shatter",
	shatter_time = 1.4,
	spread = 0.15,
	dim = 0.3,
	core = vector.create(1.0, 1.0, 1.0),
	glow = vector.create(0.35, 0.55, 1),
	slot = 8,
}
local FADE_SECONDS = 0.35

-- The index of the last post effect of a kind, or nil.
local function find_effect(kind: string): number?
	local found = nil
	for i, effect in wpe.get_postfx() do
		if effect.effect == kind then
			found = i
		end
	end
	return found
end

local function remove_effect(kind: string)
	local index = find_effect(kind)
	if index then
		wpe.remove_postfx(index)
	end
end

function JudgementCut.start(x: number, y: number, options: { [string]: any }?)
	local self = setmetatable({}, JudgementCut)
	for key, value in DEFAULTS do
		self[key] = if options and options[key] ~= nil then options[key] else value
	end
	self.seed = if options and options.seed then options.seed else math.random(1, 999999)
	self.x, self.y = x, y
	self.phase = "cutting"
	self.clock = 0
	wpe.add_postfx({ effect = "color_adjust", brightness = 0 })
	local cracks = { effect = "cracks", count = 0, seed = self.seed, center_x = x, center_y = y, spread = self.spread }
	local core = options and options.core
	local glow = options and options.glow
	if core then
		cracks.width_pixels = 1
		cracks.core_r, cracks.core_g, cracks.core_b = core.x, core.y, core.z
	end
	if glow then
		cracks.glow_r, cracks.glow_g, cracks.glow_b = glow.x, glow.y, glow.z
	end
	wpe.add_postfx(cracks)
	return self
end

-- Advances the cut; returns false once it has ended (its effects are gone).
function JudgementCut:update(dt: number): boolean
	if self.phase == "done" then
		return false
	end
	self.clock += dt
	if self.phase == "cutting" then
		local count = math.min(self.clock / self.interval, self.lines)
		wpe.set_postfx(find_effect("cracks"), { count = count })
		wpe.set_postfx(find_effect("color_adjust"), { brightness = -self.dim * count / self.lines })
		if count >= self.lines then
			self.phase, self.clock = "holding", 0
		end
	elseif self.phase == "holding" then
		if self.ending == "stay" or self.clock < self.hold then
			return true
		end
		self.clock = 0
		if self.ending == "fade" then
			self.phase = "fading"
		else
			-- The frame as it is now (without the cuts' light) breaks along the same lines.
			wpe.capture_screen(self.slot)
			remove_effect("cracks")
			wpe.add_postfx({
				effect = "shatter", count = self.lines, seed = self.seed, center_x = self.x, center_y = self.y,
				spread = self.spread, time = 0, capture = self.slot, background = 0.6,
			})
			self.phase = "shattering"
		end
	elseif self.phase == "fading" then
		local left = math.max(1 - self.clock / FADE_SECONDS, 0)
		wpe.set_postfx(find_effect("cracks"), { strength = left })
		wpe.set_postfx(find_effect("color_adjust"), { brightness = -self.dim * left })
		if left == 0 then
			self:stop()
			return false
		end
	elseif self.phase == "shattering" then
		local t = self.clock / self.shatter_time
		-- The scene behind brightens back as the pieces fall away.
		wpe.set_postfx(find_effect("shatter"), { time = self.clock, background = 0.6 * (1 - t) })
		wpe.set_postfx(find_effect("color_adjust"), { brightness = -self.dim * (1 - t) })
		if t >= 1 then
			self:stop()
			return false
		end
	end
	return true
end

-- Ends it now, removing its effects.
function JudgementCut:stop()
	if self.phase == "done" then
		return
	end
	self.phase = "done"
	remove_effect("shatter")
	remove_effect("cracks")
	remove_effect("color_adjust")
end

return JudgementCut
