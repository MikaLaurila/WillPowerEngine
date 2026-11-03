-- A text field of the title menu (the address and the name): tells the menu what the player typed.
-- The menu places the field on its row, and turns it on while that row is selected.
local Field = {}
Field.properties = {
	setting = "name", -- which of the menu's settings it edits: "name" or "address"
}

function Field:on_start()
	self.menu = wpe.find("menu")
end

-- Every change while typing (and Esc putting the old text back).
function Field:on_change(text: string)
	wpe.send(self.menu, "typed", self.setting, text)
end

-- Return.
function Field:on_submit(text: string)
	wpe.send(self.menu, "submitted", self.setting, text)
end

return Field
