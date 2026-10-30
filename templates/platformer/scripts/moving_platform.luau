-- A platform moving back and forth between its start and start + offset, easing at the ends. It is a
-- kinematic body: moving its transform pushes what it runs into. It keeps its `velocity` up to date,
-- which the player reads to ride along.
local MovingPlatform = {}
MovingPlatform.properties = {
	offset = vector.create(5, 0, 0), -- where the far end is, from the start
	period = 4, -- seconds for a whole trip there and back
}

function MovingPlatform:on_start()
	self.start = wpe.get_position(self.entity)
	self.clock = 0
	self.velocity = vector.create(0, 0, 0)
end

function MovingPlatform:on_fixed_update(dt)
	self.clock += dt
	-- 0 at the start, 1 at the far end, following a cosine so it slows down at both ends.
	local t = 0.5 - 0.5 * math.cos(self.clock * 2 * math.pi / self.period)
	local position = self.start + self.offset * t
	self.velocity = (position - wpe.get_position(self.entity)) / dt
	wpe.set_position(self.entity, position)
end

return MovingPlatform
