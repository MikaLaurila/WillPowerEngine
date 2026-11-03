-- The race, alone (practice) or with up to four players over the network.
--
-- Phases (`self.phase`):
--   "menu"       not in a game: scripts/menu.luau shows the menu and the camera circles the track
--   "joining"    asked to join a game, waiting to get in
--   "waiting"    in the game, waiting for the host to say what is going on
--   "lobby"      warming up: drive around while players join; the host starts the race (Return)
--   "countdown"  3-2-1: each racer's car is back on its grid slot, held
--   "racing"     laps counted through the checkpoints in order
--   "finished"   the results; the host goes back to the lobby (Return)
--
-- Who decides what is the main design question of a multiplayer game. Here:
-- * The host decides the phase and who races, and tells everyone with a "race" message (the whole
--   state, so a player who joins late catches up with one message). The others follow it. In practice
--   this machine is its own host.
-- * Each racer counts its own laps. Only a car's owner sees exactly where it is (the others draw it
--   about 100 ms in the past), so its owner is the one to say when it crossed a line, and it tells
--   everyone how far it got with "progress" messages.
-- * Each racer times itself from its own GO. The start message takes a moment to arrive, longer for a
--   player far from the host; timing from when each one saw GO keeps that delay out of the result. The
--   host ranks the finish times.
-- * The host hands out the cars: player N drives car N+1 (wpe.net_set_owner), and parks the free ones.
--
-- Everything is sent reliably: these messages happen rarely and must not be lost. The cars' positions
-- go through replication (see car.luau), not through this script.
local Race = {}
Race.properties = {
	laps = 3,
	checkpoints = 3, -- checkpoints besides the finish line (indices 1..checkpoints)
	countdown = 3, -- seconds
	finish_wait = 30, -- seconds the others get after the first racer finishes
}

local SAVE = "best_lap"
local MAX_PLAYERS = 4
local Notice = require("scripts/notice")
local COLORS = { [0] = wpe.color(1, 0.35, 0.3), wpe.color(0.4, 0.6, 1), wpe.color(0.4, 0.9, 0.45), wpe.color(1, 0.85, 0.2) }
local END_REASONS = {
	closed = "The host closed the game",
	["timed out"] = "Lost the connection to the host",
	kicked = "The host removed you from the game",
	["game full"] = "That game is full",
	["no answer"] = "No game answered at that address",
	["different game or build"] = "That host runs a different version of the game",
}

local function clock(seconds: number?): string
	if not seconds then
		return "-:--.--"
	end
	return string.format("%d:%05.2f", seconds // 60, seconds % 60)
end

-- Whether this machine is in a multiplayer game.
local function online(): boolean
	local status = wpe.net_status()
	return status == "hosting" or status == "joined"
end

-- This machine's player id: 0 when hosting or alone.
local function me(): number
	return wpe.net_player() or 0
end

-- The players' names by id (just you, alone).
local function names(): { [number]: string }
	local list = {}
	if online() then
		for _, p in wpe.net_players() do
			list[p.id] = p.name
		end
	else
		list[0] = "You"
	end
	return list
end

function Race:on_start()
	self.cars = {}
	for id = 0, MAX_PLAYERS - 1 do
		self.cars[id] = wpe.find("car " .. (id + 1))
	end
	self.camera = wpe.find("camera")
	self.targets = { [0] = wpe.get_position(wpe.find("finish line")) } -- where each checkpoint is
	for i = 1, self.checkpoints do
		self.targets[i] = wpe.get_position(wpe.find("checkpoint " .. i))
	end
	self.best = wpe.load_data(SAVE) -- nil until a lap has been completed once
	self.phase = "menu"
	self.age = 0 -- seconds in this phase
	self.timer = 0 -- the countdown
	self.racers = {} -- [player id] = true for those in this race
	self.progress = {} -- [player id] = {lap, next, time}: how far each racer got (time once finished)
	self.results = nil -- {{id, name, time}, ...} once the race is over (time nil: did not finish)
	self:reset_lap()
	self.first_update = true
end

-- Where this scene starts depends on the game it is in. (Decided in the first update rather than in
-- on_start, so the cars' scripts have started by then.)
function Race:join_the_game()
	local status = wpe.net_status()
	if status == "hosting" then
		self:open_lobby() -- (wpe_runtime --host, or the editor's Network menu)
	elseif status == "joined" then
		self:wait_for_host() -- a player following the host into this scene
	elseif status == "connecting" then
		self.phase = "joining" -- (wpe_runtime --connect)
	end
end

function Race:reset_lap()
	self.lap = 1
	self.next_checkpoint = 1
	self.lap_time = 0
	self.total_time = 0
	self.last_lap = nil
	self.finish_time = nil
	self.new_record = false
	self.finish_deadline = nil
end

-- Whether this machine runs the race: the host, or anyone playing alone.
function Race:in_charge(): boolean
	return not online() or wpe.net_is_host()
end

function Race:my_car()
	return self.cars[me()]
end

-- ---- The host's side ------------------------------------------------------------------------------

-- The menu: practice alone.
function Race:practice()
	if self.phase == "menu" then
		self:set_phase("lobby")
	end
end

-- Hosting started (the menu, or the command line): everyone present gets their car.
function Race:open_lobby()
	if self.phase == "menu" or self.phase == "joining" then
		for _, p in wpe.net_players() do
			self:give_car(p.id)
		end
		self:set_phase("lobby")
	end
end

-- Host: player `id` drives car id+1 from now on. Its body is networked too, so it changes hands with the
-- car. The new owner puts the car on the grid itself (settle_my_car): only the owner can move it.
function Race:give_car(id: number)
	local car = self.cars[id]
	if id == 0 or not car or not wpe.net_is_host() then
		return
	end
	wpe.net_set_owner(car, id)
	local script = wpe.get_script(car)
	if script then
		script:setup() -- (it may not have started yet)
		if script.body then
			wpe.net_set_owner(script.body, id)
		end
	end
end

-- The machine in charge parks the cars nobody drives (they are its own, so it can move them).
function Race:park_free_cars()
	local present = names()
	for id = 0, MAX_PLAYERS - 1 do
		if not present[id] and self.cars[id] then
			wpe.send(self.cars[id], "place", "pit")
		end
	end
end

-- The state everyone needs to follow the race, as one message.
function Race:state()
	local racers = {}
	for id in self.racers do
		table.insert(racers, id)
	end
	table.sort(racers)
	return { phase = self.phase, timer = self.timer, racers = racers, results = self.results }
end

-- In charge: moves the race on, here and (hosting) everywhere.
function Race:set_phase(phase: string)
	local racers = {}
	if phase == "countdown" then -- everyone in the game races
		for id in names() do
			table.insert(racers, id)
		end
	elseif phase ~= "lobby" then
		for id in self.racers do
			table.insert(racers, id)
		end
	end
	self:apply_state({ phase = phase, timer = self.countdown, racers = racers, results = self.results })
	self:park_free_cars()
	if online() then
		wpe.net_send("all", "race", self:state())
	end
end

-- In charge: ends the race once every racer finished, or the others ran out of time.
function Race:check_finish()
	if self.phase ~= "racing" then
		return
	end
	local everyone, anyone = true, false
	for id in self.racers do
		local p = self.progress[id]
		if p and p.time then
			anyone = true
		else
			everyone = false
		end
	end
	if anyone and not self.finish_deadline then
		self.finish_deadline = self.finish_wait
	end
	if everyone or (self.finish_deadline and self.finish_deadline <= 0) then
		self.results = self:standings()
		self:set_phase("finished")
	end
end

-- ---- Everyone's side ------------------------------------------------------------------------------

-- A player in the game: asks the host what is going on (answered with a "race" message).
function Race:wait_for_host()
	self.phase = "waiting"
	self.age = 0
	wpe.net_send("host", "ready")
end

-- Follows a state from the host (or from set_phase).
function Race:apply_state(state)
	local before = self.phase
	self.phase = state.phase
	self.timer = state.timer or 0
	self.racers = {}
	for _, id in state.racers or {} do
		self.racers[id] = true
	end
	self.results = state.results
	if self.phase ~= before then
		self.age = 0
		if self.phase == "countdown" then
			self.progress = {}
			self:reset_lap()
		end
		self:settle_my_car()
	end
end

-- Puts this player's car where the phase wants it: on the grid to warm up and to race, parked when
-- watching a race joined too late.
function Race:settle_my_car()
	local car = self:my_car()
	if not car or not wpe.net_is_local(car) then
		return -- not ours yet: the host's ownership message is on its way
	end
	self.my_car_local = true
	if self.phase == "lobby" or (self.phase == "countdown" and self.racers[me()]) then
		wpe.send(car, "place", "home")
	elseif self.phase == "countdown" or self.phase == "racing" then
		wpe.send(car, "place", "pit")
	end
end

-- Sent by the checkpoints when this player's car passes one.
function Race:checkpoint(index: number)
	if self.phase ~= "racing" or not self.racers[me()] or self.finish_time then
		return
	end
	if index == self.next_checkpoint then
		self.next_checkpoint += 1
		self:report_progress()
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
		self.finish_time = self.total_time
	else
		self.lap += 1
	end
	self:report_progress()
end

-- Tells everyone how far this racer got (the host relays it to the other players).
function Race:report_progress()
	local p = { lap = self.lap, next = self.next_checkpoint, time = self.finish_time }
	self.progress[me()] = p
	if online() then
		wpe.net_send("all", "progress", p)
	end
	if self:in_charge() then
		self:check_finish()
	end
end

-- The racers in order: the finished by time, then the others by how far they got (laps, checkpoints,
-- then how close their car is to the next one).
function Race:standings(): { { id: number, name: string, time: number? } }
	local list = {}
	local present = names()
	for id in self.racers do
		local p = self.progress[id] or { lap = 1, next = 1 }
		local score
		if p.time then
			score = 1e6 - p.time
		else
			local target = self.targets[if p.next > self.checkpoints then 0 else p.next]
			local car = self.cars[id]
			local distance = if car and target then vector.magnitude(wpe.get_position(car) - target) else 0
			score = (p.lap - 1) * (self.checkpoints + 1) + (p.next - 1) - math.min(distance, 500) / 1000
		end
		table.insert(list, { id = id, name = present[id] or ("Player " .. id), time = p.time, score = score })
	end
	table.sort(list, function(a, b)
		return a.score > b.score
	end)
	for _, entry in list do
		entry.score = nil
	end
	return list
end

-- ---- Network callbacks ----------------------------------------------------------------------------

function Race:on_net_joined(player)
	if wpe.net_is_host() then
		self:open_lobby()
	elseif self.phase ~= "waiting" then
		self:wait_for_host()
	end
end

function Race:on_player_joined(player, name)
	if wpe.net_is_host() then
		self:give_car(player) -- a late joiner watches until the next race (settle_my_car parks it)
	end
end

function Race:on_player_left(player, name, reason)
	-- Its cars are the host's again (the engine does that); the race goes on without it.
	self.racers[player] = nil
	self.progress[player] = nil
	if self:in_charge() then
		self:park_free_cars()
		self:check_finish()
	end
end

function Race:on_net_ended(reason)
	Notice.text = END_REASONS[reason] or ("Left the game: " .. reason)
	wpe.load_scene("scenes/main.scene") -- back to the menu, everything as the file has it
end

function Race:on_net_message(from, name, value)
	if name == "ready" and wpe.net_is_host() then
		wpe.net_send(from, "race", self:state())
	elseif name == "race" and from == 0 and not wpe.net_is_host() and type(value) == "table" then
		-- (Only the host's word counts. `from` can be trusted: the host fills it in.)
		self:apply_state(value)
	elseif name == "progress" and type(value) == "table" and self.racers[from] then
		self.progress[from] = { lap = tonumber(value.lap) or 1, next = tonumber(value.next) or 1, time = tonumber(value.time) }
		if self:in_charge() then
			self:check_finish()
		end
	end
end

-- Leaves the game (or practice) for the menu.
function Race:leave()
	if online() then
		wpe.net_leave()
	end
	wpe.load_scene("scenes/main.scene")
end

-- ---- Every frame ----------------------------------------------------------------------------------

function Race:on_update(dt)
	if self.first_update then
		self.first_update = false
		self:join_the_game()
	end
	self.age += dt
	local playing = self.phase ~= "menu" and self.phase ~= "joining"
	if playing and wpe.action_pressed("leave") then
		self:leave()
		return
	end
	-- Our car may become ours some frames after the host said so: put it in place then.
	local car = self:my_car()
	local mine = car ~= nil and wpe.net_is_local(car)
	if mine and not self.my_car_local then
		self:settle_my_car()
	end
	self.my_car_local = mine

	if self:in_charge() and self.age > 0.25 and wpe.action_pressed("start") then
		if self.phase == "lobby" then
			self:set_phase("countdown")
		elseif self.phase == "finished" then
			self.results = nil
			self:set_phase("lobby")
		end
	end
	if self.phase == "countdown" then
		self.timer -= dt
		if self.timer <= 0 then
			self.phase = "racing" -- on every machine, at its own GO
			self.age = 0
			self.go_time = 1 -- "GO!" stays up for a second
		end
	elseif self.phase == "racing" then
		if self.racers[me()] and not self.finish_time then
			self.lap_time += dt
			self.total_time += dt
		end
		if self:in_charge() and self.finish_deadline then
			self.finish_deadline -= dt
			self:check_finish()
		end
	end
	if self.go_time then
		self.go_time -= dt
		if self.go_time <= 0 then
			self.go_time = nil
		end
	end

	if car then
		local driving = self.phase == "lobby" or (self.phase == "racing" and self.racers[me()] and not self.finish_time)
		wpe.send(car, "set_controls", driving == true)
	end
	if self.camera then
		wpe.send(self.camera, "follow", if playing then car else nil)
	end
	if playing then
		self:draw_hud()
	end
end

-- ---- The HUD --------------------------------------------------------------------------------------

local function centered(text: string, y: number, color: number?, scale: number?)
	local width = wpe.screen_size()
	local w = wpe.text_size(text, scale or 1)
	wpe.draw_text((width - w) // 2, y, text, color or wpe.color(1, 1, 1), scale or 1)
end

-- Text on a dark band, readable over the sky and the track.
local function label(x: number, y: number, text: string, color: number)
	local w, h = wpe.text_size(text)
	wpe.draw_rect(x - 2, y - 1, w + 4, h + 2, wpe.color(0, 0, 0, 0.5))
	wpe.draw_text(x, y, text, color)
end

-- The other drivers' names above their cars.
function Race:draw_names(present)
	local mine = me()
	for id, name in present do
		local car = self.cars[id]
		if id ~= mine and car then
			local at = wpe.get_world_position(car)
			local x, y, in_front = wpe.world_to_screen(at + vector.create(0, 2, 0))
			local camera = if self.camera then wpe.get_world_position(self.camera) else at
			if in_front and vector.magnitude(at - camera) < 80 then
				local w = wpe.text_size(name)
				label(x - w // 2, y - 8, name, COLORS[id])
			end
		end
	end
end

function Race:draw_hud()
	local width, height = wpe.screen_size()
	local white, yellow, dim = wpe.color(1, 1, 1), wpe.color(1, 0.85, 0.3), wpe.color(1, 1, 1, 0.7)
	local present = names()
	self:draw_names(present)
	local racing = self.racers[me()] and (self.phase == "countdown" or self.phase == "racing")
	if racing then
		wpe.draw_text(6, 6, string.format("Lap %d/%d", self.lap, self.laps), white)
		wpe.draw_text(6, 18, "Time " .. clock(self.lap_time), white)
		wpe.draw_text(6, 30, "Last " .. clock(self.last_lap), white)
		wpe.draw_text(6, 42, "Best " .. clock(self.best), yellow)
		local standings = self:standings()
		if #standings > 1 then
			for place, entry in standings do
				if entry.id == me() then
					local text = string.format("%d/%d", place, #standings)
					wpe.draw_text(width - 6 - wpe.text_size(text, 2), 6, text, white, 2)
				end
			end
		end
	end
	local car = self:my_car()
	local script = if car then wpe.get_script(car) else nil
	if script and self.phase ~= "finished" then
		local kmh = string.format("%3d km/h", math.abs(script.speed or 0) * 3.6 // 1)
		wpe.draw_text(width - 6 - wpe.text_size(kmh, 2), height - 22, kmh, white, 2)
	end

	local y = height // 3
	if self.phase == "waiting" then
		centered("Joining the race...", y, white)
	elseif self.phase == "lobby" then
		centered(if online() then "Lobby" else "Practice", 36, yellow, 2)
		local row = 58
		for id = 0, MAX_PLAYERS - 1 do
			if present[id] then
				local text = present[id] .. (if id == me() then " (you)" else "")
				label((width - wpe.text_size(text)) // 2, row, text, COLORS[id])
				row += 11
			end
		end
		local hint = if self:in_charge() then "Return: start the race" else "The host starts the race"
		label((width - wpe.text_size(hint)) // 2, row + 5, hint, white)
	elseif self.phase == "countdown" then
		if racing then
			centered(tostring(math.ceil(self.timer)), y, yellow, 4)
		else
			centered("You race in the next round", y, white)
		end
	elseif self.phase == "racing" then
		if self.go_time and racing then
			centered("GO!", y, wpe.color(0.4, 1, 0.4), 4)
		elseif not self.racers[me()] then
			centered("You race in the next round", 30, white)
		elseif self.finish_time then
			centered("Finished! " .. clock(self.finish_time), y, white, 2)
			if self.finish_deadline then
				centered(string.format("The others have %d s", math.ceil(self.finish_deadline)), y + 22)
			else
				centered("Waiting for the others", y + 22)
			end
		end
	elseif self.phase == "finished" then
		centered("Results", 40, yellow, 2)
		local row = 64
		for place, entry in self.results or {} do
			local text = string.format("%d. %-12s %s", place, entry.name, if entry.time then clock(entry.time) else "DNF")
			label((width - wpe.text_size(text)) // 2, row, text, COLORS[entry.id] or white)
			row += 12
		end
		if self.new_record then
			local text = "New best lap " .. clock(self.best)
			label((width - wpe.text_size(text)) // 2, row + 4, text, yellow)
			row += 12
		end
		local hint = if self:in_charge() then "Return: back to the lobby" else "Waiting for the host"
		label((width - wpe.text_size(hint)) // 2, row + 8, hint, white)
	end
	if self.phase ~= "finished" then
		wpe.draw_text(6, height - 12, "W/S drive  A/D steer  Space drift  Esc leave", dim)
	end
end

return Race
