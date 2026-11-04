-- Tobin at his market stall: no quest of his own, but his lines follow the story.
return {
	name = "Tobin",
	voice = 0.65,
	start = {
		{ node = "done", when = "ring_returned" },
		{ node = "hint", when = "ring_quest", unless = "ring_found" },
		{ node = "hello" },
	},
	nodes = {
		hello = {
			text = "Tobin's Trinkets! Everything a hero needs, most of it only slightly used.",
			choices = {
				{ text = "What do you sell?", go = "wares" },
				{ text = "Heard any news?", go = "news" },
				{ text = "Goodbye." },
			},
		},
		wares = {
			text = {
				"Rope. Lanterns. A lute with three strings, which is plenty for most songs.",
				"Come back when your purse is heavier than your boots.",
			},
			next = "more",
		},
		news = {
			text = "Old Maren looks troubled. Lost something, I'd wager. She sits by the fountain.",
			next = "more",
		},
		more = {
			text = "Anything else?",
			choices = {
				{ text = "What do you sell?", go = "wares" },
				{ text = "Heard any news?", go = "news" },
				{ text = "Goodbye." },
			},
		},
		hint = {
			text = "Looking for Maren's ring? Little Pip collects shiny things. Just saying.",
			choices = {
				{ text = "Thanks, Tobin." },
			},
		},
		done = {
			text = "Word is you found Maren's ring. Ashford owes you one! Well, I don't. But Ashford does.",
		},
	},
}
