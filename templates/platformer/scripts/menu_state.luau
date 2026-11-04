-- Shared between the pause menu (scripts/controls.luau) and the hero: whether the menu is open. Modules are
-- loaded once per game, so every script that requires this gets the same table.
return { open = false }
