-- A physics box: tumbles onto the ground (or the spinning helmets) and removes itself a few
-- seconds after its first collision.
local Falling = {}

function Falling:on_start()
	wpe.add_rigidbody(self.entity, { type = wpe.BODY_DYNAMIC })
	-- Size is in the entity's local units; the spawner's scale applies on top.
	wpe.add_collider(self.entity, { shape = wpe.COLLIDER_BOX, size = vector.create(1, 1, 1), restitution = 0.3 })
	self.landed_at = nil
end

function Falling:on_collision_enter(other)
	if not self.landed_at then
		self.landed_at = wpe.time()
	end
end

function Falling:on_update(dt)
	if self.landed_at and wpe.time() - self.landed_at > 4 then
		wpe.destroy(self.entity)
	end
end

return Falling
