-- Exercises every function of the `wpe` scripting API. Used by the scripting integration test
-- (which runs it for 60 simulated seconds) and as a living reference. Fails loudly via assert.
local Tour = {}

local function near(a, b, eps)
	return math.abs(a - b) <= (eps or 1e-4)
end

local function vnear(a, b, eps)
	return near(a.x, b.x, eps) and near(a.y, b.y, eps) and near(a.z, b.z, eps)
end

function Tour:on_start()
	local e = self.entity
	wpe.log("api tour starting on", e)

	-- Names and liveness.
	wpe.set_name(e, "tour")
	assert(wpe.get_name(e) == "tour")
	assert(wpe.is_alive(e))

	-- Transforms.
	wpe.set_position(e, vector.create(1, 2, 3))
	assert(wpe.get_position(e) == vector.create(1, 2, 3))
	wpe.set_rotation(e, vector.create(0.1, 0.2, 0.3))
	assert(vnear(wpe.get_rotation(e), vector.create(0.1, 0.2, 0.3)))
	wpe.set_scale(e, vector.create(1, 2, 1))
	assert(wpe.get_scale(e) == vector.create(1, 2, 1))
	wpe.set_scale(e, 1)

	-- Hierarchy and world-space queries.
	self.child = wpe.spawn("tour_child")
	wpe.set_parent(self.child, e, false)
	wpe.set_position(self.child, vector.create(0, 1, 0))
	assert(wpe.get_parent(self.child) == e)
	assert(#wpe.children(e) == 1)
	assert(wpe.get_world_position(self.child).y > 2)
	wpe.set_parent(self.child, nil) -- keeps its world placement by default
	assert(wpe.get_parent(self.child) == nil)

	-- Orientation helpers.
	self.pointer = wpe.spawn("tour_pointer")
	wpe.look_at(self.pointer, vector.create(0, 0, -5))
	assert(vnear(wpe.forward(self.pointer), vector.create(0, 0, -1)))
	wpe.rotate(self.pointer, vector.create(0, 1, 0), math.pi / 2)

	-- Lights.
	wpe.set_light(self.pointer, { type = wpe.LIGHT_SPOT, intensity = 2, range = 5, inner_cone = 0.2, outer_cone = 0.4 })
	local light = wpe.get_light(self.pointer)
	assert(light.type == wpe.LIGHT_SPOT and light.range == 5)
	wpe.remove_light(self.pointer)
	assert(wpe.get_light(self.pointer) == nil)

	-- Prefabs, visibility, animation.
	self.figure = wpe.instantiate("figure", vector.create(-2, 0, 0))
	wpe.set_visible(self.figure, false)
	wpe.set_visible(self.figure, true)
	wpe.play_animation(self.figure, 1, true)
	wpe.animation_time(self.figure, 0.25)
	assert(near(wpe.animation_time(self.figure), 0.25))

	-- Scripts attaching scripts, and deferred destruction.
	self.temp = wpe.spawn("tour_temp")
	assert(wpe.attach_script(self.temp, "scripts/spinner.luau"))
	assert(not pcall(wpe.get_position, 4294967295)) -- invalid handles raise errors

	-- Physics (the host provides a ground collider with its top at y = 0).
	self.ball = wpe.spawn("tour_ball")
	wpe.set_position(self.ball, vector.create(8, 3, 8))
	wpe.add_rigidbody(self.ball, {
		type = wpe.BODY_DYNAMIC,
		linear_damping = 0.1,
		angular_damping = 0.1,
		gravity_scale = 1,
		lock_rotation = false,
		continuous = true,
	})
	wpe.add_collider(self.ball, { shape = wpe.COLLIDER_SPHERE, radius = 0.5, density = 2, friction = 0.5, restitution = 0.1 })
	wpe.set_velocity(self.ball, vector.create(0, 1, 0))
	assert(vnear(wpe.get_velocity(self.ball), vector.create(0, 1, 0)))
	wpe.apply_impulse(self.ball, vector.create(0.1, 0, 0))
	wpe.apply_force(self.ball, vector.create(0, 0, 1))

	self.pillar = wpe.spawn("tour_pillar")
	wpe.set_position(self.pillar, vector.create(-8, 1, -8))
	wpe.add_rigidbody(self.pillar, { type = wpe.BODY_KINEMATIC })
	wpe.add_collider(self.pillar, { shape = wpe.COLLIDER_CAPSULE, radius = 0.3, height = 2, offset = vector.create(0, 0, 0) })
	wpe.remove_collider(self.pillar)
	wpe.remove_rigidbody(self.pillar)
	-- Without a rigid body a collider is static level geometry; this one is a trigger hull from a prefab.
	wpe.add_collider(self.pillar, { shape = wpe.COLLIDER_CONVEX, model = "box", mesh = 1, trigger = true })

	local gravity = wpe.get_gravity()
	assert(gravity.y < 0)
	wpe.set_gravity(gravity)
	local hit = wpe.raycast(vector.create(20, 5, 20), vector.create(0, -1, 0), 10)
	assert(hit and near(hit.distance, 5, 1e-3) and hit.normal.y > 0.99 and wpe.get_name(hit.entity) == "ground")
	assert(wpe.raycast(vector.create(20, 5, 20), vector.create(0, 1, 0)) == nil)

	self.updates = 0
	self.fixed = 0
end

function Tour:on_update(dt)
	self.updates += 1
	if self.updates == 10 then
		wpe.destroy(self.temp)
		wpe.stop_animation(self.figure)
	elseif self.updates == 11 then
		assert(not wpe.is_alive(self.temp), "destroy should have completed at the end of the previous update")
	elseif self.updates == 120 then
		assert(wpe.get_position(self.ball).y < 0.55, "the ball should rest on the ground")
	end
	assert(wpe.time() >= 0 and wpe.frame() >= 0)
end

function Tour:on_fixed_update(dt)
	self.fixed += 1
	wpe.rotate(self.entity, vector.create(0, 1, 0), dt)
	-- Report progress through the entity name so the host side can check both callbacks ran.
	wpe.set_name(self.entity, string.format("tour:%d:%d", self.updates, self.fixed))
end

function Tour:on_destroy()
	wpe.destroy(self.pointer)
	wpe.destroy(self.child)
	wpe.destroy(self.figure)
	wpe.destroy(self.ball)
	wpe.destroy(self.pillar)
end

return Tour
