-- Helpers shared by the demo scripts:  local util = require("scripts/lib/util")
local util = {}

-- Moves `value` toward `target` by at most `step`.
function util.approach(value: number, target: number, step: number): number
	if value < target then
		return math.min(value + step, target)
	end
	return math.max(value - step, target)
end

-- The shortest turn from angle `from` to angle `to` (radians), in [-pi, pi).
function util.angle_delta(from: number, to: number): number
	return (to - from + math.pi) % (2 * math.pi) - math.pi
end

-- The first entity named `name` under `root` (searched depth first), or nil. Unlike wpe.find, it stays
-- within one instance, so each copy of a model finds its own parts.
function util.find_child(root: number, name: string): number?
	for _, child in wpe.children(root) do
		if wpe.get_name(child) == name then
			return child
		end
		local found = util.find_child(child, name)
		if found then
			return found
		end
	end
	return nil
end

-- Horizontal unit vectors of the camera's view: forward along the ground, and right.
function util.camera_axes(): (vector, vector)
	local camera = wpe.get_camera()
	local forward = vector.create(0, 0, -1)
	if camera then
		local f = wpe.forward(camera)
		local flat = vector.create(f.x, 0, f.z)
		if vector.magnitude(flat) > 1e-3 then
			forward = vector.normalize(flat)
		end
	end
	return forward, vector.create(-forward.z, 0, forward.x)
end

return util
