-- A guard that patrols between two points with the navigation system (docs/navigation.md). The entity
-- has a `nav_agent` in the scene file; the agent walks itself, and this script only says where to.
-- Part of the master test scene: the API tour reads `arrivals` and `failures`.
local Guard = {}
Guard.properties = { a = vector.create(22, 0, -20), b = vector.create(26, 0, -20) }

function Guard:on_start()
	self.arrivals = 0
	self.failures = 0
	self.going_to_b = true
	wpe.nav_move_to(self.entity, self.b)
end

-- The agent got where it was going: turn around.
function Guard:on_nav_arrived()
	self.arrivals += 1
	self.going_to_b = not self.going_to_b
	wpe.nav_move_to(self.entity, if self.going_to_b then self.b else self.a)
end

-- Sent somewhere it can't reach (it got as near as it could): back to the patrol.
function Guard:on_nav_failed()
	self.failures += 1
	wpe.nav_move_to(self.entity, self.a)
end

return Guard
