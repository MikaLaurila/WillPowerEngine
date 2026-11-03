-- The gun: a model held in front of the camera (this entity is a child of the player's head), which
-- shoots where the crosshair points.
--
--   Fire:    left mouse / right trigger (hold for automatic guns)
--   Reload:  R / X (west); an empty gun reloads by itself
--
-- A shot is a ray from the camera (wpe.raycast, "hitscan"): what it hits is pushed (dynamic bodies), told
-- (its script's on_shot(damage, point, direction) runs, as the dummies' does) and sparks fly from the
-- point. The gun kicks back and the view jumps up; it bobs while walking and pulls back near walls so it
-- never pokes through them.
--
-- The model is this entity's children: replace them with your own (keep the child named "muzzle" at the
-- barrel's tip: the flash shows there). Tune the gun in the inspector.
local Gun = {}
Gun.properties = {
	damage = 1,
	fire_rate = 7, -- shots per second
	automatic = false, -- hold to keep firing
	magazine = 12,
	reload_time = 1.1, -- seconds
	range = 80, -- m
	knockback = 3, -- the impulse pushing what it hits
	recoil = 0.035, -- how far the gun kicks back (m)
	view_kick = 0.03, -- how far the view jumps up (radians)
	bob = 0.012, -- walking bob (m)
	sparks = "impact sparks", -- the entity whose particles burst where shots hit
}

local FLASH_TIME = 0.05 -- seconds the muzzle flash shows

function Gun:on_start()
	self.player = wpe.find("player")
	self.rest = wpe.get_position(self.entity) -- where it sits, relative to the head
	self.ammo = self.magazine
	self.cooldown = 0
	self.reloading = 0
	self.kick = 0 -- recoil offset, settling back
	self.flash = 0
	self.hits = 0 -- dummies knocked down
	self.marker = 0 -- the hit marker's time left
	self.bob_time = 0
	self.sparks_entity = wpe.find(self.sparks)
	for _, child in wpe.children(self.entity) do
		if wpe.get_name(child) == "muzzle" then
			self.muzzle = child
		end
	end
end

function Gun:head(): number
	return wpe.get_parent(self.entity) :: number
end

-- One shot, if the gun can fire now. @return whether it fired.
function Gun:fire(): boolean
	if self.reloading > 0 or self.cooldown > 0 then
		return false
	end
	if self.ammo <= 0 then
		wpe.play_sound("sounds/empty.wav", { volume = 0.6 })
		self.cooldown = 0.25
		self:reload()
		return false
	end
	self.ammo -= 1
	self.cooldown = 1 / self.fire_rate
	self.kick = self.recoil
	self.flash = FLASH_TIME
	wpe.play_sound("sounds/shot.wav", { volume = 0.8, pitch = 0.95 + 0.1 * math.random() })
	local script = wpe.get_script(self.player)
	if script then
		script:kick(self.view_kick)
	end

	local head = self:head()
	local origin = wpe.get_world_position(head)
	local direction = wpe.forward(head)
	local hit = wpe.raycast(origin, direction, self.range, { ignore = self.player })
	if hit then
		self:impact(hit, direction)
	end
	return true
end

-- What a shot does where it lands.
function Gun:impact(hit, direction: vector)
	local body = wpe.get_rigidbody(hit.entity)
	if body and body.type == wpe.BODY_DYNAMIC then
		wpe.apply_impulse(hit.entity, direction * self.knockback)
	end
	if wpe.send(hit.entity, "on_shot", self.damage, hit.point, direction) then
		self.marker = 0.15 -- It had something to say: show the hit marker.
		wpe.play_sound("sounds/hit.wav", { volume = 0.5 })
	end
	if self.sparks_entity then
		-- Sparks fly out of the surface: the emitter shoots along its -Z, turned to face along the normal.
		wpe.set_position(self.sparks_entity, hit.point + hit.normal * 0.02)
		wpe.look_at(self.sparks_entity, hit.point + hit.normal)
		wpe.emit_particles(self.sparks_entity, 12)
	end
end

function Gun:reload()
	if self.reloading > 0 or self.ammo == self.magazine then
		return
	end
	self.reloading = self.reload_time
	wpe.play_sound("sounds/reload.wav", { volume = 0.7 })
end

-- A dummy went down (dummy.luau broadcasts it).
function Gun:on_target_down()
	self.hits += 1
end

function Gun:on_update(dt)
	self.cooldown = math.max(0, self.cooldown - dt)
	self.flash = math.max(0, self.flash - dt)
	self.marker = math.max(0, self.marker - dt)
	if self.reloading > 0 then
		self.reloading -= dt
		if self.reloading <= 0 then
			self.reloading = 0
			self.ammo = self.magazine
		end
	end
	local player = wpe.get_script(self.player)
	local captured_now = player and player.just_captured
	local wants = if self.automatic then wpe.action_down("fire") else wpe.action_pressed("fire")
	if wants and not captured_now then
		self:fire()
	end
	if wpe.action_pressed("reload") then
		self:reload()
	end
	self:place(dt, player)
	if self.muzzle then
		wpe.set_billboard(self.muzzle, { visible = self.flash > 0, rotation = math.random() * 360 })
		wpe.set_light(self.muzzle, { intensity = if self.flash > 0 then 3 else 0 })
	end
	self:draw_hud()
end

-- Where the gun sits this frame: at rest, kicked back, bobbing while walking, lowered while reloading, and
-- pulled in when a wall is close (so the barrel never goes through it).
function Gun:place(dt: number, player)
	self.kick -= self.kick * math.min(1, dt * 14)
	local speed = 0
	if player then
		speed = vector.magnitude(vector.create(player.velocity.x, 0, player.velocity.z))
		if not player.grounded then
			speed = 0
		end
	end
	self.bob_time += dt * speed * 1.6
	local amount = self.bob * math.min(1, speed / 4)
	local bob = vector.create(math.sin(self.bob_time) * amount, -math.abs(math.cos(self.bob_time)) * amount, 0)
	local head = self:head()
	local wall = wpe.raycast(wpe.get_world_position(head), wpe.forward(head), 1, { ignore = self.player })
	local pull = if wall then math.clamp(0.75 - wall.distance, 0, 0.35) else 0
	local reload_dip = if self.reloading > 0 then 0.12 * math.sin(math.pi * self.reloading / self.reload_time) else 0
	wpe.set_position(self.entity, self.rest + bob + vector.create(0, -reload_dip - pull * 0.4, self.kick + pull))
	wpe.set_rotation(self.entity, vector.create(self.kick * 4 + reload_dip * 2, 0, 0))
end

function Gun:draw_hud()
	local width, height = wpe.screen_size()
	local cx, cy = width // 2, height // 2
	local color = wpe.color(1, 1, 1, 0.85)
	wpe.draw_rect(cx - 5, cy, 3, 1, color)
	wpe.draw_rect(cx + 3, cy, 3, 1, color)
	wpe.draw_rect(cx, cy - 5, 1, 3, color)
	wpe.draw_rect(cx, cy + 3, 1, 3, color)
	if self.marker > 0 then -- The hit marker: an X around the crosshair.
		local red = wpe.color(1, 0.3, 0.25)
		for i = 3, 5 do
			wpe.draw_rect(cx - i, cy - i, 1, 1, red)
			wpe.draw_rect(cx + i, cy - i, 1, 1, red)
			wpe.draw_rect(cx - i, cy + i, 1, 1, red)
			wpe.draw_rect(cx + i, cy + i, 1, 1, red)
		end
	end
	local ammo = if self.reloading > 0 then "Reloading" else string.format("%d / %d", self.ammo, self.magazine)
	local ammo_color = if self.ammo == 0 and self.reloading == 0 then wpe.color(1, 0.35, 0.3) else wpe.color(1, 1, 1)
	wpe.draw_text(6, height - 30, ammo, ammo_color, 2)
	wpe.draw_text(6, 6, string.format("Down: %d", self.hits), wpe.color(1, 1, 1), 1)
end

return Gun
