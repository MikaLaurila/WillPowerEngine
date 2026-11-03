-- An arcade car that can be raced over the network. It drives like the one in the `racing` template:
-- its physics body is a sphere (the "car N" entity, a dynamic rigid body that can't rotate), the script
-- sets the velocity each step from the controls, the child "body" turns to the car's heading, and its
-- child "shell" (the visible car) leans in the turns.
--
--   Throttle: W, Up, right trigger      Brake / reverse: S, Down, left trigger
--   Steer: A/D, arrows, left stick      Handbrake (drift): Space, B (east)
--
-- Multiplayer. Every machine in a game has all four cars, but each car is simulated by one of them, its
-- owner (scene key `net`; the host hands the cars out, see race.luau). Only the owner runs the driving
-- code below. Everywhere else the engine's replication moves the car: the owner sends where it is about
-- 30 times a second, and the others draw it between the last positions they heard of. "body" is
-- networked too (it has its own `net` key), so the heading arrives the same way. The lean is not sent:
-- every machine works it out from how fast the car turns, which costs nothing on the network.
--
-- The property `player` says whose car this is: player 0 (the host, or you alone) drives car 1,
-- player 1 drives car 2, and so on. A car nobody drives belongs to the host, which parks it beside the
-- track (`place("pit")`).
local Car = {}
Car.properties = {
	player = 0, -- the player who drives this car
	top_speed = 30, -- m/s (108 km/h)
	reverse_speed = 8,
	acceleration = 14, -- m/s²
	braking = 30,
	drag = 0.25, -- the share of speed lost per second when coasting
	turn_rate = 2.2, -- radians per second at full steer, once moving
	grip = 6, -- how fast sideways sliding stops (per second)
	handbrake_grip = 1.2,
}

-- The shortest turn from angle `from` to angle `to` (radians), in [-pi, pi).
local function angle_delta(from: number, to: number): number
	return (to - from + math.pi) % (2 * math.pi) - math.pi
end

function Car:on_start()
	self:setup()
end

-- Finds the parts and remembers the grid slot. Called by on_start, or earlier by `place` when the race
-- script, which starts first, moves the car before its own script has started.
function Car:setup()
	if self.ready then
		return
	end
	self.ready = true
	for _, child in wpe.children(self.entity) do
		if wpe.get_name(child) == "body" then
			self.body = child
		end
	end
	for _, child in if self.body then wpe.children(self.body) else {} do
		if wpe.get_name(child) == "shell" then
			self.shell = child
		end
	end
	self.yaw = self:body_yaw()
	self.home = wpe.get_position(self.entity) -- the grid slot, where the scene file puts the car
	self.home_yaw = self.yaw
	self.pit = vector.create(-9 + 6 * self.player, 0, -22) -- a parking place on the grass inside the track
	self.controls = false -- the race script switches them on
	self.speed = 0 -- forward speed, m/s (negative when reversing)
	self.steer = 0
	self.lean = 0
	self.was_local = wpe.net_is_local(self.entity)
	self.last_position = self.home
end

-- The heading of "body", from where it faces (the car looks down -Z at yaw 0). Reading it back from the
-- direction rather than from Euler angles works at every angle.
function Car:body_yaw(): number
	if not self.body then
		return 0
	end
	local f = wpe.forward(self.body)
	return math.atan2(-f.x, -f.z)
end

-- Whether this machine drives the car: it simulates the car, and the car is this player's. (The host
-- simulates the parked cars too, but nobody drives them.) Outside a game you are player 0.
function Car:is_mine(): boolean
	return wpe.net_is_local(self.entity) and (wpe.net_player() or 0) == self.player
end

-- The race script: false holds the car (countdown, finished, parked).
function Car:set_controls(on: boolean)
	self.controls = on
end

-- Puts the car, stopped, on its grid slot ("home") or in its parking place ("pit"). Only the owner can
-- move a car: anywhere else replication would put it straight back where its owner says it is.
function Car:place(where: string)
	if not wpe.net_is_local(self.entity) then
		return
	end
	self:setup()
	wpe.set_position(self.entity, if where == "pit" then self.pit else self.home)
	wpe.set_velocity(self.entity, vector.create(0, 0, 0))
	self.yaw = self.home_yaw
	self.speed = 0
	self.steer = 0
	if self.body then
		wpe.set_rotation(self.body, vector.create(0, self.yaw, 0))
	end
end

-- Forward and right vectors of the heading on the ground.
function Car:axes(): (vector, vector)
	local forward = vector.create(-math.sin(self.yaw), 0, -math.cos(self.yaw))
	return forward, vector.create(-forward.z, 0, forward.x)
end

function Car:on_fixed_update(dt)
	if not wpe.net_is_local(self.entity) then
		return -- another machine drives this car; replication moves it here
	end
	local driving = self.controls and self:is_mine()
	local position = wpe.get_position(self.entity)
	local ground = wpe.raycast(position + vector.create(0, 0.6, 0), vector.create(0, -1, 0), 0.8, { ignore = self.entity })
	local v = wpe.get_velocity(self.entity)
	local forward, right = self:axes()
	local speed = vector.dot(v, forward)
	local side = vector.dot(v, right)

	local throttle = if driving then math.clamp(wpe.action_value("throttle"), 0, 1) else 0
	local brake = if driving then math.clamp(wpe.action_value("brake"), 0, 1) else 1 -- held in place
	local steer = if driving then math.clamp(wpe.action_value("steer"), -1, 1) else 0
	local handbrake = driving and wpe.action_down("handbrake")

	if ground then
		if throttle > 0 then
			speed = math.min(speed + throttle * self.acceleration * dt, self.top_speed)
		end
		if brake > 0 then
			if speed > 0.5 then
				speed = math.max(speed - brake * self.braking * dt, 0)
			elseif driving then
				speed = math.max(speed - brake * self.acceleration * dt, -self.reverse_speed) -- reverse
			else
				speed = 0
			end
		end
		if throttle == 0 and brake == 0 then
			speed -= speed * math.min(1, self.drag * dt)
		end
		-- Steering turns faster with speed (up to full at 8 m/s), and the other way in reverse.
		local turning = math.clamp(speed / 8, -1, 1)
		self.yaw -= steer * self.turn_rate * turning * dt
		local grip = if handbrake then self.handbrake_grip else self.grip
		side -= side * math.min(1, grip * dt)
	end
	self.speed = speed
	self.steer = steer
	-- The new heading's axes: the speed kept along the new forward is what turns the car.
	forward, right = self:axes()
	wpe.set_velocity(self.entity, forward * speed + right * side + vector.create(0, v.y, 0))
end

function Car:on_update(dt)
	if not self.body then
		return
	end
	local is_local = wpe.net_is_local(self.entity)
	if is_local ~= self.was_local then
		-- The car changed hands: carry on from the heading its last owner left it at.
		self.was_local = is_local
		self.yaw = self:body_yaw()
		self.speed = 0
		self.steer = 0
	end
	local target_lean = 0
	if is_local then
		target_lean = -self.steer * math.clamp(self.speed / self.top_speed, -1, 1) * 0.08
		wpe.set_rotation(self.body, vector.create(0, self.yaw, 0))
	elseif dt > 0 then
		-- Replication turned the body and moved the car: how fast tells the speed and the lean.
		local yaw = self:body_yaw()
		local turn = angle_delta(self.yaw, yaw) / dt
		local position = wpe.get_position(self.entity)
		local moved = position - self.last_position
		local speed = vector.magnitude(vector.create(moved.x, 0, moved.z)) / dt
		self.speed += (speed - self.speed) * math.min(1, dt * 10)
		self.yaw = yaw
		target_lean = math.clamp(turn / self.turn_rate, -1, 1) * math.clamp(self.speed / self.top_speed, 0, 1) * 0.08
	end
	self.last_position = wpe.get_position(self.entity)
	-- The body leans out of turns.
	self.lean += (target_lean - self.lean) * math.min(1, dt * 8)
	if self.shell then
		wpe.set_rotation(self.shell, vector.create(0, 0, self.lean))
	end
end

return Car
