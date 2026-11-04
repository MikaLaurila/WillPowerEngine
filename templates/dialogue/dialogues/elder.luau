-- Old Maren by the fountain: she gives the quest (the lost ring) and takes it back at the end.
-- The format is explained at the top of scripts/lib/conversation.luau.
return {
	name = "Maren",
	voice = 0.8,
	start = {
		{ node = "thanks", when = "ring_returned" },
		{ node = "found", when = "ring_found" },
		{ node = "waiting", when = "ring_quest" },
		{ node = "hello" },
	},
	nodes = {
		hello = {
			text = {
				"Ah, a new face in Ashford. Welcome, traveller.",
				"Forgive an old woman's fretting. I have lost something dear to me.",
			},
			choices = {
				{ text = "What did you lose?", go = "ring" },
				{ text = "Tell me about Ashford.", go = "village" },
				{ text = "Goodbye." },
			},
		},
		village = {
			text = {
				"Ashford is small: the fountain, the market, and far too many cats.",
				"Tobin at the market knows everyone's business. Ask him anything.",
			},
			next = "more",
		},
		more = {
			text = "Was there something else?",
			choices = {
				{ text = "What did you lose?", go = "ring", unless = "ring_quest" },
				{ text = "Goodbye." },
			},
		},
		ring = {
			text = {
				"My mother's silver ring. I had it here at the fountain this morning.",
				"Would you keep an eye out for it?",
			},
			choices = {
				{ text = "I'll find it.", go = "promise", set = "ring_quest", event = "on_quest_started" },
				{ text = "Not now, sorry.", go = "later" },
			},
		},
		promise = {
			text = "Bless you. The children play around here; perhaps one of them saw something.",
		},
		later = {
			text = "Of course. Mind the cats on your way.",
		},
		waiting = {
			text = "Any luck with my ring? The children play around the fountain...",
			choices = {
				{ text = "Still looking." },
			},
		},
		found = {
			text = "Is that... my ring? You found it!",
			choices = {
				{
					text = "Here you are.",
					go = "returned",
					set = "ring_returned",
					clear = "ring_found",
					event = "on_quest_done",
				},
				{ text = "Not just yet." },
			},
		},
		returned = {
			text = {
				"Thank you, dear. Pip had it? That rascal. I'll bake him a pie anyway.",
				"Here, take this old map. It's all I have, but it has never led me wrong.",
			},
		},
		thanks = {
			text = "Thank you again for my ring. Come by for pie sometime.",
		},
	},
}
