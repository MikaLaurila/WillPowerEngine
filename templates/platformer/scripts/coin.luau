-- A coin (prefabs/coin.prefab): spins, and when the player touches its trigger it tells the player,
-- bursts into sparkles and disappears.
local Coin = {}
Coin.properties = { spin = 3 } -- radians per second

-- The player counts the coins with wpe.broadcast("is_coin").
function Coin:is_coin()
	return true
end

function Coin:on_update(dt)
	wpe.rotate(self.entity, vector.create(0, 1, 0), self.spin * dt)
end

function Coin:on_trigger_enter(other)
	if wpe.get_name(other) ~= "player" or self.taken then
		return
	end
	self.taken = true
	wpe.send(other, "collect_coin")
	-- A short burst of sparkles where the coin was, removed once they have faded.
	local sparkles = wpe.spawn("coin sparkles")
	wpe.set_position(sparkles, wpe.get_world_position(self.entity))
	wpe.set_particles(sparkles, {
		rate = 0, burst = 14, duration = 0.1, loop = false, speed = { 1.5, 3 }, lifetime = { 0.3, 0.5 },
		spread = math.pi, size = { 0.15, 0.02 }, color_start = wpe.color(1, 0.9, 0.4), color_end = wpe.color(1, 0.7, 0.1, 0),
	})
	wpe.after(0.6, function()
		wpe.destroy(sparkles)
	end)
	wpe.destroy(self.entity)
end

return Coin
