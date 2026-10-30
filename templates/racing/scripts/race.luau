-- Runs the race: a 3-2-1 countdown, laps counted through the checkpoints in order (a lap only counts
-- when every checkpoint was passed since the last one, so cutting across doesn't help), lap times, and
-- the best lap kept between sessions with wpe.save_data. After the last lap it shows the result;
-- Space (or Start) races again, R (or Back) restarts at any time.
local Race = {}
Race.properties = {
	laps = 3,
	checkpoints = 3, -- checkpoints besides the finish line (indices 1..checkpoints)
	countdown = 3, -- seconds
}

local SAVE = "best_lap"

local function clock(seconds: number?): string
	if not seconds then
		return "-:--.--"
	end
	return string.format("%d:%05.2f", seconds // 60, seconds % 60)
end

function Race:on_start()
	self.car = wpe.find("car")
	self.best = wpe.load_data(SAVE) -- nil until a lap has been completed once
	self.state = "countdown" -- then "racing", then "finished"
	self.timer = self.countdown
	self.lap = 1
	self.next_checkpoint = 1
	self.lap_time = 0
	self.total_time = 0
	self.last_lap = nil
	self.new_record = false
end

-- Sent by the checkpoints.
function Race:checkpoint(index: number)
	if self.state ~= "racing" then
		return
	end
	if index == self.next_checkpoint then
		self.next_checkpoint += 1
	elseif index == 0 and self.next_checkpoint > self.checkpoints then
		self:complete_lap()
	end
end

function Race:complete_lap()
	self.last_lap = self.lap_time
	if not self.best or self.lap_time < self.best then
		self.best = self.lap_time
		self.new_record = true
		wpe.save_data(SAVE, self.best)
	end
	self.lap_time = 0
	self.next_checkpoint = 1
	if self.lap == self.laps then
		self.state = "finished"
		wpe.send(self.car, "set_controls", false)
	else
		self.lap += 1
	end
end

function Race:on_update(dt)
	if wpe.action_pressed("restart") or (self.state == "finished" and wpe.action_pressed("handbrake")) then
		wpe.load_scene("scenes/main.scene")
		return
	end
	if self.state == "countdown" then
		-- Held every frame: the car's script may start after this one (and switch its controls on).
		wpe.send(self.car, "set_controls", false)
		self.timer -= dt
		if self.timer <= 0 then
			self.state = "racing"
			self.go_time = 1 -- "GO!" stays up for a second
			wpe.send(self.car, "set_controls", true)
		end
	elseif self.state == "racing" then
		self.lap_time += dt
		self.total_time += dt
	end
	if self.go_time then
		self.go_time -= dt
		if self.go_time <= 0 then
			self.go_time = nil
		end
	end
	self:draw_hud()
end

local function centered(text: string, y: number, color: number?, scale: number?)
	local width = wpe.screen_size()
	local w = wpe.text_size(text, scale or 1)
	wpe.draw_text((width - w) // 2, y, text, color or wpe.color(1, 1, 1), scale or 1)
end

function Race:draw_hud()
	local width, height = wpe.screen_size()
	local white, yellow = wpe.color(1, 1, 1), wpe.color(1, 0.85, 0.3)
	wpe.draw_text(6, 6, string.format("Lap %d/%d", self.lap, self.laps), white)
	wpe.draw_text(6, 18, "Time " .. clock(self.lap_time), white)
	wpe.draw_text(6, 30, "Last " .. clock(self.last_lap), white)
	wpe.draw_text(6, 42, "Best " .. clock(self.best), yellow)
	local car = if self.car then wpe.get_script(self.car) else nil
	if car then -- (its speed appears once its script has started)
		local kmh = string.format("%3d km/h", math.abs(car.speed or 0) * 3.6 // 1)
		wpe.draw_text(width - 6 - wpe.text_size(kmh, 2), height - 22, kmh, white, 2)
	end
	if self.state == "countdown" then
		centered(tostring(math.ceil(self.timer)), height // 3, yellow, 4)
	elseif self.go_time then
		centered("GO!", height // 3, wpe.color(0.4, 1, 0.4), 4)
	elseif self.state == "finished" then
		centered("Finished!", height // 3, white, 2)
		centered("Total " .. clock(self.total_time), height // 3 + 24)
		if self.new_record then
			centered("New best lap " .. clock(self.best), height // 3 + 36, yellow)
		end
		centered("Space to race again", height // 3 + 52)
	end
	if self.state ~= "finished" then
		wpe.draw_text(6, height - 12, "W/S drive  A/D steer  Space drift  R restart", wpe.color(1, 1, 1, 0.7))
	end
end

return Race
