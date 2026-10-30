-- A checkpoint: when the player passes its trigger, the player comes back here after getting hurt, and
-- the flag turns from red ("flag off") to green ("flag on").
local Checkpoint = {}

function Checkpoint:on_start()
	for _, child in wpe.children(self.entity) do
		local name = wpe.get_name(child)
		if name == "flag off" then
			self.off = child
		elseif name == "flag on" then
			self.on = child
		end
	end
	self:show(false)
end

function Checkpoint:show(reached: boolean)
	if self.off then
		wpe.set_visible(self.off, not reached)
	end
	if self.on then
		wpe.set_visible(self.on, reached)
	end
end

function Checkpoint:on_trigger_enter(other)
	if wpe.get_name(other) ~= "player" or self.reached then
		return
	end
	self.reached = true
	self:show(true)
	wpe.send(other, "set_checkpoint", wpe.get_world_position(self.entity))
end

return Checkpoint
