-- A chase camera behind a car (the race script picks yours with `follow`), following the heading of its
-- script (`yaw`) with a little lag, so turns and drifts show. It pulls back a little at speed. With no
-- car to follow (in the menu) it circles the track.
local ChaseCamera = {}
ChaseCamera.properties = {
	distance = 6.5, -- behind the car, at rest
	speed_distance = 2, -- added at top speed
	height = 2.6,
	look_height = 1, -- the point looked at, above the car's origin
	follow_rate = 5, -- how quickly the camera swings behind the car (per second)
	orbit_radius = 75, -- circling the track
	orbit_height = 28,
	orbit_speed = 0.08, -- radians per second
}

function ChaseCamera:on_start()
	self.yaw = 0
	self.orbit = 0
	wpe.set_camera(self.entity)
end

-- The race script: the car to follow, or nil to circle the track.
function ChaseCamera:follow(car)
	if car ~= self.followed then
		self.followed = car
		local script = if car then wpe.get_script(car) else nil
		self.yaw = if script and script.yaw then script.yaw else 0
	end
end

-- The shortest turn from angle `from` to angle `to` (radians), in [-pi, pi).
local function angle_delta(from: number, to: number): number
	return (to - from + math.pi) % (2 * math.pi) - math.pi
end

function ChaseCamera:on_update(dt)
	if not self.followed or not wpe.is_alive(self.followed) then
		self.orbit += self.orbit_speed * dt
		local at = vector.create(math.cos(self.orbit), 0, math.sin(self.orbit)) * self.orbit_radius
		wpe.set_position(self.entity, at + vector.create(0, self.orbit_height, 0))
		wpe.look_at(self.entity, vector.create(0, 0, 0))
		return
	end
	local car = wpe.get_script(self.followed)
	local heading = if car and car.yaw then car.yaw else self.yaw
	self.yaw += angle_delta(self.yaw, heading) * math.min(1, dt * self.follow_rate)
	local speed = if car and car.speed and car.top_speed then math.abs(car.speed) / car.top_speed else 0
	local distance = self.distance + self.speed_distance * speed
	local target = wpe.get_world_position(self.followed)
	-- Behind: the car looks down -Z at yaw 0, so behind is +Z turned by the yaw.
	local back = vector.create(math.sin(self.yaw), 0, math.cos(self.yaw))
	wpe.set_position(self.entity, target + back * distance + vector.create(0, self.height, 0))
	wpe.look_at(self.entity, target + vector.create(0, self.look_height, 0))
end

return ChaseCamera
