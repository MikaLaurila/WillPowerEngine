-- A title-menu button: clicking it (mouse, Return/Space, or the pad's A) loads `scene`, or quits the
-- game when `scene` is "quit". With `focus` set, the button takes the focus when the menu opens, so
-- keyboard and gamepad players see where they are. Buttons slide in when the menu opens (lower ones a
-- little later), and the chosen scene opens through an iris closing on the button.
local MenuButton = {}
MenuButton.properties = { scene = "", focus = false }

function MenuButton:on_start()
	if self.focus then
		wpe.ui_focus(self.entity)
	end
	-- Slide in from off the left edge, overshooting a little, staggered by height on the screen.
	local min_x, min_y, max_x, max_y = wpe.get_ui_offsets(self.entity)
	local _, y = wpe.ui_rect(self.entity)
	local _, height = wpe.screen_size()
	wpe.tween_value(-320, 0, { duration = 0.45, ease = "out_back", delay = 0.25 * y / height }, function(shift)
		wpe.set_ui_offsets(self.entity, min_x + shift, min_y, max_x + shift, max_y)
	end)
end

function MenuButton:on_click()
	if self.scene == "quit" then
		wpe.quit()
		return
	end
	local x, y, w, h = wpe.ui_rect(self.entity)
	local width, height = wpe.screen_size()
	wpe.load_scene(self.scene, {
		transition = "iris",
		duration = 0.45,
		center_x = (x + w / 2) / width,
		center_y = (y + h / 2) / height,
	})
end

return MenuButton
