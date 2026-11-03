-- A guard: patrols a route, sees with a cone of sight, hears noises, and chases, searches and gives up
-- like a guard in a stealth game. It walks with the navigation system (docs/navigation.md): the entity
-- has a character controller and a nav_agent, and this script only decides where to go.
--
-- What it does, as states:
--   patrol       walks the route's points in order, waiting a moment at each
--   suspicious   saw something: stops and stares while suspicion fills (faster the closer you are)
--   investigate  heard a noise, or lost sight while suspicious: walks over to look
--   look         at the spot: looks around for a moment
--   chase        suspicion is full: runs after the player (and calls the others); catching ends the game
--   search       lost the player: goes to where it saw them last and checks around there
--   return       gave up: back to the nearest point of its route
--
-- The route is an entity (named by `route`) whose children are the points, in order. Tune the senses in
-- the inspector: `view_distance` and `view_angle` make the cone, `notice_time` how long you can stay in it.
local Figure = require("scripts/figure")

local Guard = {}
Guard.properties = {
	route = "", -- the entity whose children are the patrol points
	walk_speed = 1.8, -- m/s on patrol
	chase_speed = 4.3, -- m/s chasing (the player sprints at 6)
	view_distance = 9, -- m
	view_angle = 45, -- half the cone's width, degrees
	eye_height = 1.6,
	notice_time = 1.2, -- seconds in sight at the cone's far end before it gives chase (less when closer)
	lose_time = 2.5, -- seconds out of sight before a chase turns into a search
	search_time = 8, -- seconds spent searching
	search_radius = 5,
	hearing = 1, -- how well it hears: times each noise's own radius
	alarm_radius = 16, -- guards this near come to help when it starts a chase
	wait = 1.5, -- seconds at each patrol point
	catch_distance = 1,
	model = "figure",
}

local CALM_AFTER = 1.2 -- seconds out of sight before a suspicious guard goes to look

-- The horizontal angle (radians) an entity looking down -Z must turn to face along `d`.
local function yaw_of(d: vector): number
	return math.atan2(-d.x, -d.z)
end

local function flat_distance(a: vector, b: vector): number
	return vector.magnitude(vector.create(a.x - b.x, 0, a.z - b.z))
end

function Guard:on_start()
	self.figure = Figure.find(self.entity, self.model)
	self.animation = {}
	self.player = wpe.find("player")
	self.points = {}
	local route = if self.route ~= "" then wpe.find(self.route) else nil
	if route then
		for _, point in wpe.children(route) do
			table.insert(self.points, wpe.get_world_position(point))
		end
	end
	if #self.points == 0 then
		self.points = { wpe.get_world_position(self.entity) } -- No route: it guards where it stands.
	end
	self.next_point = 1
	self.suspicion = 0
	self.unseen = 0
	self.waiting = 0
	self.yaw = yaw_of(wpe.forward(self.entity)) -- (Euler angles would flip past 90 degrees.)
	self:patrol()
end

-- ---- What it sees -------------------------------------------------------------------------------------

-- Whether the player is in the cone and nothing solid is in between; and how far away they are.
function Guard:sees_player(position: vector): (boolean, number)
	if not self.player or not wpe.is_alive(self.player) then
		return false, math.huge
	end
	local eye = position + vector.create(0, self.eye_height, 0)
	local chest = wpe.get_world_position(self.player) + vector.create(0, 1.0, 0)
	local to = chest - eye
	local distance = vector.magnitude(to)
	if distance > self.view_distance or distance < 1e-3 then
		return false, distance
	end
	local f = wpe.forward(self.entity)
	local facing = vector.normalize(vector.create(f.x, 0, f.z))
	local toward = vector.normalize(vector.create(to.x, 0, to.z))
	if vector.dot(facing, toward) < math.cos(math.rad(self.view_angle)) then
		return false, distance
	end
	-- The first thing a ray from the eye meets must be the player (walls and crates hide them).
	local hit = wpe.raycast(eye, to, distance + 0.5, { ignore = self.entity })
	return hit ~= nil and hit.entity == self.player, distance
end

-- ---- States ---------------------------------------------------------------------------------------------

function Guard:go(state: string, speed: number, where: vector?)
	self.state = state
	self.waiting = 0
	wpe.add_nav_agent(self.entity, { speed = speed }) -- Only the setting changes; it keeps walking.
	if where then
		wpe.nav_move_to(self.entity, where)
	end
end

function Guard:patrol()
	self:go("patrol", self.walk_speed, self.points[self.next_point])
end

function Guard:suspect()
	if self.state ~= "suspicious" then
		self.state = "suspicious"
		wpe.nav_stop(self.entity) -- Stand and stare.
	end
end

function Guard:investigate(where: vector)
	self.last_seen = where
	self:go("investigate", self.walk_speed * 1.3, where)
end

function Guard:chase()
	self.suspicion = 1
	self:go("chase", self.chase_speed)
	wpe.nav_follow(self.entity, self.player)
	wpe.broadcast("on_alarm", self.entity, self.last_seen)
end

function Guard:search()
	self.search_left = self.search_time
	self:go("search", self.walk_speed * 1.5, self.last_seen)
end

function Guard:return_to_route()
	-- The nearest point of the route, and on from there.
	local here = wpe.get_world_position(self.entity)
	local best, best_distance = 1, math.huge
	for i, point in self.points do
		local d = flat_distance(here, point)
		if d < best_distance then
			best, best_distance = i, d
		end
	end
	self.next_point = best
	self:go("return", self.walk_speed, self.points[best])
end

-- ---- Events ---------------------------------------------------------------------------------------------

-- The navigation system says it got there (or as near as it could get: on_nav_failed).
function Guard:on_nav_arrived()
	if self.state == "patrol" then
		self.next_point = self.next_point % #self.points + 1
		self.waiting = self.wait
	elseif self.state == "investigate" then
		self.state = "look"
		self.waiting = 2.5
	elseif self.state == "search" then
		self.waiting = 1
	elseif self.state == "return" then
		self:patrol()
	end
end

function Guard:on_nav_failed()
	self:on_nav_arrived()
end

-- The player made a noise at `where`, heard up to `radius` away.
function Guard:on_noise(where: vector, radius: number)
	if self.state == "chase" or self.state == "search" or self.done then
		return
	end
	if flat_distance(wpe.get_world_position(self.entity), where) <= radius * self.hearing then
		self.suspicion = math.max(self.suspicion, 0.4)
		self:investigate(where)
	end
end

-- Another guard started a chase.
function Guard:on_alarm(caller: number, where: vector?)
	if caller == self.entity or self.state == "chase" or self.done or not where then
		return
	end
	if flat_distance(wpe.get_world_position(self.entity), where) <= self.alarm_radius then
		self.last_seen = where
		self:search()
	end
end

-- The game ended (caught or escaped): everyone stops.
function Guard:on_game_over()
	self.done = true
	wpe.nav_stop(self.entity)
end

-- ---- Every frame ----------------------------------------------------------------------------------------

-- Turns toward a point, for a guard standing still (walking, the nav agent turns it).
function Guard:face(target: vector, dt: number)
	local here = wpe.get_world_position(self.entity)
	local d = vector.create(target.x - here.x, 0, target.z - here.z)
	if vector.magnitude(d) < 0.1 then
		return
	end
	local delta = (yaw_of(d) - self.yaw + math.pi) % (2 * math.pi) - math.pi
	self.yaw += delta * math.min(1, dt * 6)
	wpe.set_rotation(self.entity, vector.create(0, self.yaw, 0))
end

function Guard:on_update(dt)
	local agent = wpe.get_nav_agent(self.entity)
	local speed = vector.magnitude(vector.create(agent.velocity.x, 0, agent.velocity.z))
	Figure.animate(self.figure, self.animation, speed, self.walk_speed)
	self.yaw = yaw_of(wpe.forward(self.entity)) -- (Euler angles would flip past 90 degrees.)
	if self.done then
		return
	end
	local position = wpe.get_world_position(self.entity)
	local seen, distance = self:sees_player(position)
	if seen then
		self.last_seen = wpe.get_world_position(self.player)
		self.unseen = 0
		if self.state ~= "chase" then
			local closeness = 1 + 2 * (1 - distance / self.view_distance)
			self.suspicion = math.min(1, self.suspicion + dt / self.notice_time * closeness)
			if self.suspicion >= 1 then
				self:chase()
			else
				self:suspect()
			end
		end
	else
		self.unseen += dt
		if self.state ~= "chase" then
			self.suspicion = math.max(0, self.suspicion - dt * 0.25)
		end
	end

	if self.state == "chase" then
		if flat_distance(position, wpe.get_world_position(self.player)) <= self.catch_distance then
			wpe.broadcast("on_caught", self.entity)
		elseif self.unseen > self.lose_time then
			self:search()
		end
	elseif self.state == "suspicious" then
		if self.last_seen then
			self:face(self.last_seen, dt)
		end
		if self.unseen > CALM_AFTER and self.last_seen then
			self:investigate(self.last_seen)
		end
	elseif self.state == "look" then
		-- Looking around: a slow turn, then back to work.
		self.yaw += dt * 1.5
		wpe.set_rotation(self.entity, vector.create(0, self.yaw, 0))
		self.waiting -= dt
		if self.waiting <= 0 then
			self:return_to_route()
		end
	elseif self.state == "search" then
		self.search_left -= dt
		if self.search_left <= 0 then
			self:return_to_route()
		elseif self.waiting > 0 then
			self.waiting -= dt
			if self.waiting <= 0 then
				local spot = wpe.nav_random_point(self.last_seen, self.search_radius)
				if spot then
					wpe.nav_move_to(self.entity, spot)
				end
			end
		end
	elseif self.state == "patrol" and self.waiting > 0 then
		self.waiting -= dt
		if self.waiting <= 0 then
			self:patrol()
		end
	end
	self:draw_mark(position)
end

-- "?" over a guard that's suspicious (with how close it is to noticing), "!" over one chasing.
function Guard:draw_mark(position: vector)
	local mark, color = nil, 0
	if self.state == "chase" then
		mark, color = "!", wpe.color(1, 0.25, 0.2)
	elseif self.state ~= "patrol" and self.state ~= "return" then
		mark, color = "?", wpe.color(1, 0.85, 0.2)
	end
	if not mark then
		return
	end
	local x, y, in_front = wpe.world_to_screen(position + vector.create(0, 2.4, 0))
	if not in_front then
		return
	end
	wpe.draw_text(x - 4, y - 8, mark, color, 2)
	if self.state == "suspicious" then
		wpe.draw_rect(x - 8, y + 10, 16, 3, wpe.color(0, 0, 0, 0.6))
		wpe.draw_rect(x - 8, y + 10, 16 * self.suspicion, 3, color)
	end
end

return Guard
