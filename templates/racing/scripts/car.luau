-- An arcade car. Its physics body is a sphere (the "car" entity, a dynamic rigid body that can't
-- rotate), so walls and bumps are handled by the physics; the script sets the velocity each step
-- from the controls. The child "body" turns to the car's heading, and its child "shell" (the visible
-- car) leans in the turns.
--
--   Throttle: W, Up, right trigger      Brake / reverse: S, Down, left trigger
--   Steer: A/D, arrows, left stick      Handbrake (drift): Space, B (east)
--
-- Grip pulls the sideways part of the velocity toward zero: high grip corners on rails, low grip
-- (the handbrake) slides. The race script turns the controls off during the countdown and after the
-- finish (set_controls).
local Car = {}
Car.properties = {
	top_speed = 30, -- m/s (108 km/h)
	reverse_speed = 8,
	acceleration = 14, -- m/s²
	braking = 30,
	drag = 0.25, -- the share of speed lost per second when coasting
	turn_rate = 2.2, -- radians per second at full steer, once moving
	grip = 6, -- how fast sideways sliding stops (per second)
	handbrake_grip = 1.2,
}

function Car:on_start()
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
	self.yaw = if self.body then wpe.get_rotation(self.body).y else 0
	self.controls = true
	self.speed = 0 -- forward speed, m/s (negative when reversing)
	self.steer = 0
	self.lean = 0
end

-- The race script: false holds the car (countdown, finished).
function Car:set_controls(on: boolean)
	self.controls = on
end

-- Forward and right vectors of the heading on the ground (the car looks down -Z at yaw 0).
function Car:axes(): (vector, vector)
	local forward = vector.create(-math.sin(self.yaw), 0, -math.cos(self.yaw))
	return forward, vector.create(-forward.z, 0, forward.x)
end

function Car:on_fixed_update(dt)
	local position = wpe.get_position(self.entity)
	local ground = wpe.raycast(position + vector.create(0, 0.6, 0), vector.create(0, -1, 0), 0.8, { ignore = self.entity })
	local v = wpe.get_velocity(self.entity)
	local forward, right = self:axes()
	local speed = vector.dot(v, forward)
	local side = vector.dot(v, right)

	local throttle = if self.controls then math.clamp(wpe.action_value("throttle"), 0, 1) else 0
	local brake = if self.controls then math.clamp(wpe.action_value("brake"), 0, 1) else 0
	local steer = if self.controls then math.clamp(wpe.action_value("steer"), -1, 1) else 0
	local handbrake = self.controls and wpe.action_down("handbrake")
	if not self.controls then
		brake = 1 -- held in place
	end

	if ground then
		if throttle > 0 then
			speed = math.min(speed + throttle * self.acceleration * dt, self.top_speed)
		end
		if brake > 0 then
			if speed > 0.5 then
				speed = math.max(speed - brake * self.braking * dt, 0)
			elseif self.controls then
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
	-- The body leans out of turns.
	local target_lean = -self.steer * math.clamp(self.speed / self.top_speed, -1, 1) * 0.08
	self.lean += (target_lean - self.lean) * math.min(1, dt * 8)
	wpe.set_rotation(self.body, vector.create(0, self.yaw, 0))
	if self.shell then
		wpe.set_rotation(self.shell, vector.create(0, 0, self.lean))
	end
end

return Car
