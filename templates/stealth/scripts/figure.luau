-- The animated model of a character, and playing it by speed: a small module the player and the guards
-- share (require("scripts/figure")).
--
-- The model is a child entity (named by the character's `model` property) holding an animated glTF
-- instance. With one clip, as the sample figure has, it plays while moving (faster with speed) and stands
-- on its first frame otherwise. Name your own model's clips in `move` and `idle` to use them.
local Figure = {}

-- The first entity named `name` under `root`, searched depth first.
local function find_child(root: number, name: string): number?
	for _, child in wpe.children(root) do
		if wpe.get_name(child) == name then
			return child
		end
		local found = find_child(child, name)
		if found then
			return found
		end
	end
	return nil
end

-- The entity at or below `root` that plays the model's animations (has an Animator), or nil.
local function find_animated(root: number): number?
	if pcall(wpe.get_animation, root) then
		return root
	end
	for _, child in wpe.children(root) do
		local found = find_animated(child)
		if found then
			return found
		end
	end
	return nil
end

-- The animated entity of the model named `model` under `owner`, or nil.
function Figure.find(owner: number, model: string): number?
	local root = find_child(owner, model)
	return if root then find_animated(root) else nil
end

-- Plays the move clip at a rate following `speed` (1 at `walk_speed`), or stands still. `state` keeps what
-- was playing between calls (a table the caller owns).
function Figure.animate(figure: number?, state: { [string]: any }, speed: number, walk_speed: number,
	move: string?, idle: string?)
	if not figure then
		return
	end
	local moving = speed > 0.3
	local wanted = if moving then "move" else "idle"
	if state.playing ~= wanted then
		state.playing = wanted
		local name = if moving then move else idle
		wpe.play_animation(figure, if name and name ~= "" then name else 1, true, 0.2)
	end
	-- Without an idle clip of its own, the figure holds the first frame of the first one.
	local hold = not moving and (idle == nil or idle == "")
	wpe.animation_speed(figure, if hold then 0 else (if moving then speed / walk_speed else 1))
end

return Figure
