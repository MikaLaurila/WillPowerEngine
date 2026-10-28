-- A title-menu button: clicking it (mouse, Return/Space, or the pad's A) loads `scene`, or quits the
-- game when `scene` is "quit". With `focus` set, the button takes the focus when the menu opens, so
-- keyboard and gamepad players see where they are.
local MenuButton = {}
MenuButton.properties = { scene = "", focus = false }

function MenuButton:on_start()
	if self.focus then
		wpe.ui_focus(self.entity)
	end
end

function MenuButton:on_click()
	if self.scene == "quit" then
		wpe.quit()
	else
		wpe.load_scene(self.scene)
	end
end

return MenuButton
