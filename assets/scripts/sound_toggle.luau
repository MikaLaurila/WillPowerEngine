-- A UI toggle that turns all sound on and off (the master bus), remembered between runs in the saved
-- "settings".
local SoundToggle = {}

function SoundToggle:on_start()
	local settings = wpe.load_data("settings")
	if settings and type(settings.sound) == "boolean" then
		wpe.ui_value(self.entity, settings.sound)
	end
	self:apply(wpe.ui_value(self.entity))
end

function SoundToggle:apply(on)
	wpe.set_volume(wpe.BUS_MASTER, on and 1 or 0)
end

function SoundToggle:on_change(on)
	self:apply(on)
	local settings = wpe.load_data("settings") or {}
	settings.sound = on
	wpe.save_data("settings", settings)
end

return SoundToggle
