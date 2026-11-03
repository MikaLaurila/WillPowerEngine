-- A character that wanders by itself: it picks a random spot it can walk to near its home, walks there,
-- rests a moment, and picks another. The walking is the navigation system's (docs/navigation.md): the
-- entity has a character controller and a nav_agent, and this script only says where to go and plays the
-- model's animation by speed.
local Wanderer = {}
Wanderer.properties = {
	radius = 6, -- how far from home it wanders (m)
	rest = 2, -- seconds it stands at each spot (up to twice that)
	model = "figure", -- the child entity with the animated model
	move_animation = "", -- clip names; empty: the model's first clip, held still for idle
	idle_animation = "",
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

function Wanderer:on_start()
	self.home = wpe.get_position(self.entity)
	for _, child in wpe.children(self.entity) do
		if wpe.get_name(child) == self.model then
			self.figure = find_animated(child)
		end
	end
	self.resting = 1
	self.walk_speed = wpe.get_nav_agent(self.entity).speed
end

-- Arrived (or got as near as it could): rest, then go somewhere else.
function Wanderer:on_nav_arrived()
	self.resting = self.rest * (1 + math.random())
end

function Wanderer:on_nav_failed()
	self:on_nav_arrived()
end

function Wanderer:on_update(dt)
	if wpe.nav_state(self.entity) ~= "moving" then
		self.resting -= dt
		if self.resting <= 0 then
			local spot = wpe.nav_random_point(self.home, self.radius)
			if spot then
				wpe.nav_move_to(self.entity, spot)
			end
			self.resting = self.rest
		end
	end
	if not self.figure then
		return
	end
	local v = wpe.get_nav_agent(self.entity).velocity
	local speed = vector.magnitude(vector.create(v.x, 0, v.z))
	local moving = speed > 0.3
	if moving ~= self.moving then
		self.moving = moving
		local name = if moving then self.move_animation else self.idle_animation
		wpe.play_animation(self.figure, if name ~= "" then name else 1, true, 0.2)
	end
	local hold = not moving and self.idle_animation == ""
	wpe.animation_speed(self.figure, if hold then 0 else (if moving then speed / self.walk_speed else 1))
end

return Wanderer
