-- A volume slider (a UI slider over 0..1): sets a mixer bus's volume as the player moves it, and
-- remembers it between runs in the saved "settings".
local VolumeSlider = {}
VolumeSlider.properties = { bus = "music" } -- "master", "effects" or "music"

local BUSES = { master = wpe.BUS_MASTER, effects = wpe.BUS_EFFECTS, music = wpe.BUS_MUSIC }

function VolumeSlider:on_start()
	self.bus_id = BUSES[self.bus] or error("unknown bus '" .. tostring(self.bus) .. "'")
	local settings = wpe.load_data("settings")
	if settings and type(settings[self.bus]) == "number" then
		wpe.ui_value(self.entity, settings[self.bus])
	end
	wpe.set_volume(self.bus_id, wpe.ui_value(self.entity))
end

function VolumeSlider:on_change(value)
	wpe.set_volume(self.bus_id, value)
	local settings = wpe.load_data("settings") or {}
	settings[self.bus] = value
	wpe.save_data("settings", settings)
end

return VolumeSlider
