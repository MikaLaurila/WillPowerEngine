-- Pip, who wanders the square: he has the ring, but only gives it up once you know to ask.
return {
	name = "Pip",
	voice = 1.5,
	start = {
		{ node = "after", when = "ring_found" },
		{ node = "after", when = "ring_returned" },
		{ node = "shiny", when = "ring_quest" },
		{ node = "hello" },
	},
	nodes = {
		hello = {
			text = "Hi! I'm Pip. Do you want to see my rock collection?",
			choices = {
				{ text = "Sure.", go = "rocks" },
				{ text = "Maybe later." },
			},
		},
		rocks = {
			text = {
				"This one is round. This one is roundish.",
				"And this one is shiny! I found it by the fountain. It's my best rock.",
			},
			choices = {
				{ text = "Nice rocks, Pip." },
			},
		},
		shiny = {
			text = "You want to see my shiny rock? Nobody ever wants to see my shiny rock.",
			choices = {
				{ text = "That's Maren's ring. She misses it.", go = "give" },
				{ text = "Never mind." },
			},
		},
		give = {
			text = {
				"Oh... I didn't know it was hers.",
				"Here. Tell her I'm sorry! Can I keep the roundish one?",
			},
			set = "ring_found",
			event = "on_ring_found",
		},
		after = {
			text = "I found a new rock. It's grey. Do you think Maren will bake me a pie?",
		},
	},
}
