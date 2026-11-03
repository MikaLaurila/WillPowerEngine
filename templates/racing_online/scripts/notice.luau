-- A message for the menu that outlives a scene load ("The host closed the game"). Modules are loaded
-- once and kept while the game runs, so every script that requires this one shares the same table.
return { text = nil :: string? }
