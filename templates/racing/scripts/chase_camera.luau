-- A chase camera behind the entity named `target`, following the heading of its script (`yaw`) with a
-- little lag, so turns and drifts show. It pulls back a little at speed.
local ChaseCamera = {}
ChaseCamera.properties = {
	target = "car",
	distance = 6.5, -- behind the car, at rest
	speed_distance = 2, -- added at top speed
	height = 2.6,
	look_height = 1, -- the point looked at, above the car's origin
	follow = 5, -- how quickly the camera swings behind the car (per second)
}

function ChaseCamera:on_start()
	self.followed = wpe.find(self.target)
	local car = if self.followed then wpe.get_script(self.followed) else nil
	self.yaw = if car and car.yaw then car.yaw else 0
	wpe.set_camera(self.entity)
end

-- The shortest turn from angle `from` to angle `to` (radians), in [-pi, pi).
local function angle_delta(from: number, to: number): number
	return (to - from + math.pi) % (2 * math.pi) - math.pi
end

function ChaseCamera:on_update(dt)
	if not self.followed or not wpe.is_alive(self.followed) then
		return
	end
	local car = wpe.get_script(self.followed)
	local heading = if car and car.yaw then car.yaw else self.yaw
	self.yaw += angle_delta(self.yaw, heading) * math.min(1, dt * self.follow)
	local speed = if car and car.speed and car.top_speed then math.abs(car.speed) / car.top_speed else 0
	local distance = self.distance + self.speed_distance * speed
	local target = wpe.get_world_position(self.followed)
	-- Behind: the car looks down -Z at yaw 0, so behind is +Z turned by the yaw.
	local back = vector.create(math.sin(self.yaw), 0, math.cos(self.yaw))
	wpe.set_position(self.entity, target + back * distance + vector.create(0, self.height, 0))
	wpe.look_at(self.entity, target + vector.create(0, self.look_height, 0))
end

return ChaseCamera
