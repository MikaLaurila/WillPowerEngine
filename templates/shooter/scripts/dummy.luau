-- A training dummy: wanders around its home with the navigation system (the entity has a character
-- controller and a nav_agent), falls over when shot enough, and gets back up after a while.
--
-- Guns tell what they hit through on_shot(damage, point, direction); returning true says "a target was
-- hit" (the gun shows its hit marker). Down, it broadcasts on_target_down (the gun counts them).
-- The model is the child named by `model` (it falls over; the entity itself stays upright).
local Dummy = {}
Dummy.properties = {
	health = 3,
	wander = 6, -- how far from home it walks (m)
	rest = 1.5, -- seconds it stands at each spot
	down_time = 4, -- seconds it stays down
	model = "body",
}

function Dummy:on_start()
	self.home = wpe.get_position(self.entity)
	self.hp = self.health
	self.resting = 0.5
	self.down = 0
	for _, child in wpe.children(self.entity) do
		if wpe.get_name(child) == self.model then
			self.body = child
		end
	end
end

function Dummy:on_shot(damage: number, point: vector, direction: vector): boolean
	if self.down > 0 then
		return false -- Already down: shots go through to the floor, so to speak.
	end
	self.hp -= damage
	if self.body then
		wpe.shake(self.body, { amplitude = 0.04, rotation = 6, duration = 0.2 })
	end
	if self.hp <= 0 then
		self.down = self.down_time
		wpe.nav_stop(self.entity)
		if self.body then -- Topple away from the shot.
			local away = vector.create(direction.x, 0, direction.z)
			local tilt = if vector.magnitude(away) > 1e-3 then -math.pi / 2 else 0
			local yaw = math.atan2(-away.x, -away.z) - wpe.get_rotation(self.entity).y
			wpe.set_rotation(self.body, vector.create(0, yaw, 0))
			wpe.tween(self.body, { rotation = vector.create(tilt, yaw, 0), duration = 0.35, ease = "out_bounce" })
		end
		wpe.broadcast("on_target_down", self.entity)
	end
	return true
end

function Dummy:on_nav_arrived()
	self.resting = self.rest
end

function Dummy:on_nav_failed()
	self.resting = self.rest
end

function Dummy:on_update(dt)
	if self.down > 0 then
		self.down -= dt
		if self.down <= 0 then
			self.hp = self.health
			if self.body then
				wpe.tween(self.body, { rotation = vector.create(0, 0, 0), duration = 0.5, ease = "out_back" })
			end
			self.resting = 1
		end
		return
	end
	if wpe.nav_state(self.entity) ~= "moving" then
		self.resting -= dt
		if self.resting <= 0 then
			local spot = wpe.nav_random_point(self.home, self.wander)
			if spot then
				wpe.nav_move_to(self.entity, spot)
			end
			self.resting = self.rest
		end
	end
end

return Dummy
