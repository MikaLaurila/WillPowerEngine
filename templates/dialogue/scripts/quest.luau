-- The quest log in the top-left corner: it hears the events conversations send (`event` in the dialogue
-- files reaches every script through wpe.broadcast) and shows the current goal, with a jingle when the
-- quest is done. Any script can react to those events the same way: a door that opens on_quest_done.
local Story = require("scripts/lib/story")

local Quest = {}

function Quest:on_start()
	self.banner = nil
	self.banner_time = 0
end

function Quest:announce(text: string)
	self.banner = text
	self.banner_time = 3
end

function Quest:on_quest_started(talker: number?)
	self:announce("New quest: The Silver Ring")
end

function Quest:on_ring_found(talker: number?)
	self:announce("Found Maren's ring")
end

function Quest:on_quest_done(talker: number?)
	self:announce("Quest complete: The Silver Ring")
	wpe.play_sound("sounds/quest.wav", { volume = 0.6 })
end

-- The goal for where the story is (nil once it's over).
function Quest:goal(): string?
	if Story.has("ring_returned") then
		return nil
	elseif Story.has("ring_found") then
		return "Bring the ring back to Maren"
	elseif Story.has("ring_quest") then
		return "Find Maren's silver ring"
	end
	return "Talk to the villagers"
end

function Quest:on_update(dt)
	local text = self:goal()
	if text then
		wpe.draw_text(6, 6, text, wpe.color(1, 1, 1, 0.85))
	end
	if self.banner then
		self.banner_time -= dt
		if self.banner_time <= 0 then
			self.banner = nil
			return
		end
		local width = wpe.screen_size()
		local w = wpe.text_size(self.banner)
		local alpha = math.min(1, self.banner_time)
		wpe.draw_rect((width - w) // 2 - 6, 30, w + 12, 14, wpe.color(0, 0, 0, 0.6 * alpha))
		wpe.draw_text((width - w) // 2, 33, self.banner, wpe.color(1, 0.85, 0.4, alpha))
	end
end

return Quest
