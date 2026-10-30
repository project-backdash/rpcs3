#pragma once

// Native table and frame-observation scenarios, included in the test's anonymous namespace.
std::optional<pbd::practice::frames::readiness_reading> free_on(std::uint32_t cursor)
{
	pbd::practice::frames::readiness_reading out;
	out.free = cursor;
	return out;
}

void calculated_frame_formulas()
{
	using namespace pbd::practice::reference;
	const terminal block{19, 255, 0x8001, true}, jab_hit{25, 255, 0x8001, true}, df1_hit{30, 255, 0x8001, true};
	const auto jab = calculate({10, 10, 39, true, true, {26, 255, 0x8001, true}}, block, jab_hit);
	check(jab.startup == 10u && jab.active == 1u && jab.recovery == 16u && jab.block_stun == 19u && jab.hit_stun == 25u && jab.on_block == 3 && jab.on_hit == 9,
		"jab reference comes from the attack's terminal and separate standing reactions");
	const auto df1 = calculate({13, 14, 48, true, true, {34, 255, 0x8001, true}}, block, df1_hit);
	check(df1.active == 2u && df1.recovery == 20u && df1.on_block == -2 && df1.on_hit == 9,
		"recovery follows the final active frame while advantage assumes first-active contact");
	const auto hop = calculate({15, 17, 51, true, true, {46, 255, 0x8001, true}}, block, {64, 255, 0x8004, true});
	check(hop.on_block == -12 && !hop.on_hit && !hop.hit_stun, "a floor terminal never masquerades as standing hit advantage");
	check(!calculate({10, 10, 39, true, true, {39, 255, 0x8001, true}}, {}, {}).on_block,
		"a missing block reaction is unavailable even when attacker recovery is known");
	check(!calculate({10, 10, 39, true, false, {26, 255, 0x8001, true}}, block, jab_hit).recovery,
		"a timing script cannot be flattened into a static reference");
	check(!calculate({10, 11, 39, true, true, {9, 255, 0x8001, true}}, block, jab_hit).recovery,
		"an endpoint before the active window is not recovery");
	check(!calculate({0, 0, 39, true, true, {26, 255, 0x8001, true}}, block, jab_hit).startup,
		"malformed attack timing cannot establish any calculated frame data");
	const auto even = calculate({10, 10, 39, true, true, {26, 255, 0x8001, true}}, {16, 255, 0x8001, true});
	check(even.on_block == 0, "a calculated even advantage is a number, not unavailable");
}

void catalog_category_titles(const std::map<std::uint32_t, std::uint32_t>& words, std::uint32_t bank, std::uint32_t rows, std::uint32_t name)
{
	using namespace pbd::move_list;
	{
		auto titled = words;
		titled[rows + 4] = name + 0x100;
		titled[name + 0x100] = (u32{'T'} << 16) | 'h';
		titled[name + 0x104] = (u32{'r'} << 16) | 'o';
		titled[name + 0x108] = (u32{'w'} << 16) | 's';
		titled[name + 0x10c] = 0;
		const auto titled_word = [&](u32 at)
		{
			const auto found = titled.find(at);
			return found == titled.end() ? ~0u : found->second;
		};
		const auto titled_half = [&](u32 at)
		{
			return (titled_word(at & ~3u) >> ((at & 2) ? 0 : 16)) & 65535u;
		};
		const auto with_title = read_catalog(bank, 9, titled_word, titled_half);
		check(with_title && with_title->entries.size() == 1 && with_title->headers.size() == 1 && with_title->headers[0].position == 0 &&
				  with_title->headers[0].name == u"Throws",
			"a titled row without commands is the category of the moves after it");
	}
}

void preset_costume_resolution(const auto& parsed, pbd::hidden_selection::tables& tables)
{
	using namespace pbd::practice::start;
	auto wanted = *parsed.value;
	check(resolve(wanted, tables, 25)->rows == std::array<std::uint32_t, 2>{26, 25}, "default costume preserves the reviewed row and default dummy");
	wanted.costume = 1;
	check(resolve(wanted, tables, 25)->rows[0] == 25u, "an alternate comes from the live verified costume table");
	check(resolve(wanted, tables, 25)->rows[1] == 26u, "a mirror dummy uses a different verified costume from the player");
	wanted.costume = 2;
	check(resolve(wanted, tables, 84)->rows == std::array<std::uint32_t, 2>{27, 25}, "an invalid default dummy falls back to a distinct costume of the selected fighter");
	wanted.costume = 3;
	check(!resolve(wanted, tables, 25), "an unavailable costume is never guessed from adjacent rows");
}

void observed_contact_endpoints(pbd::practice::frames::tracker& sample, const auto& advance)
{
	using namespace pbd::practice::frames;
	sample.boundary(100);
	sample.commit(0, 10, 10);
	sample.commit(1, 20, {});
	advance(101, 110);
	sample.applied_contact(0, 30, contact::block);
	sample.actionable(1, 20);
	check(!sample.latest[0].defender_ready, "the contact frame's preceding neutral state cannot finish recovery");
	sample.boundary(111);
	sample.commit(1, 30, {});
	advance(112, 130);
	sample.actionable(0, 10);
	check(!sample.latest[0].on_block, "both first-actionable endpoints are required");
	sample.boundary(131);
	sample.actionable(1, 30);
	check(sample.latest[0].on_block == 1 && !sample.latest[0].on_hit && sample.latest[0].attacker_ready == 130u && sample.latest[0].defender_ready == 131u,
		"block advantage is defender ready minus attacker ready");
	sample.actionable(1, 30);
	check(sample.latest[0].on_block == 1, "a repeated terminal callback cannot move the first-actionable endpoint");
	sample.boundary(132);
	sample.commit(0, 11, 15);
	check(!sample.latest[0].on_block && !sample.latest[0].on_hit, "a new attack cannot inherit an older move's advantage");
	advance(133, 142);
	sample.applied_contact(0, 31, contact::hit);
	sample.boundary(143);
	sample.commit(1, 31, {});
	advance(144, 153);
	sample.actionable(1, 31);
	advance(154, 160);
	sample.actionable(0, 11);
	check(sample.latest[0].on_hit == -7 && !sample.latest[0].on_block, "negative hit advantage stays separate from block advantage");
	sample.boundary(161);
	sample.commit(0, 12, {}, true);
	check(!sample.latest[0].startup && !sample.latest[0].on_hit, "an unsupported attack clears the preceding attack's numbers");
}

void overlap_and_counter_gap(pbd::practice::frames::tracker& sample)
{
	using namespace pbd::practice::frames;
	sample.boundary(162);
	sample.commit(0, 13, 10);
	sample.applied_contact(0, 32, contact::hit);
	sample.boundary(163);
	sample.commit(1, 32, {});
	sample.applied_contact(0, 33, contact::hit);
	sample.actionable(0, 13);
	sample.actionable(1, 32);
	check(!sample.latest[0].on_hit, "overlapping contacts do not fabricate an individual move's advantage");
	sample.boundary(164);
	sample.commit(0, 14, 10);
	sample.applied_contact(0, 34, contact::block);
	sample.boundary(165);
	sample.commit(1, 34, {});
	sample.boundary(170);
	sample.actionable(0, 14);
	sample.actionable(1, 34);
	check(!sample.latest[0].startup && !sample.latest[0].on_block, "a counter gap discards the whole observation");
	sample.reset();
	check(!sample.latest[0].startup && !sample.latest[1].on_hit, "restart clears both sides' observations");
}

void reaction_binding_refusals(pbd::practice::frames::tracker& sample)
{
	using namespace pbd::practice::frames;
	sample.reset();
	sample.boundary(300);
	sample.commit(0, 60, 10);
	sample.commit(1, 70, {});
	sample.applied_contact(0, 0, contact::block);
	sample.boundary(301);
	sample.commit(1, 80, {});
	sample.boundary(302);
	sample.actionable(1, 80);
	sample.actionable(0, 60);
	check(sample.latest[0].on_block == 0, "a reaction not queued at the contact instruction can bind only to the following stock commit");
	sample.reset();
	sample.boundary(400);
	sample.commit(0, 60, 10);
	sample.commit(1, 70, {});
	sample.applied_contact(0, 0, contact::hit);
	sample.boundary(401);
	sample.boundary(402);
	sample.commit(1, 80, {});
	sample.actionable(1, 80);
	sample.actionable(0, 60);
	check(!sample.latest[0].on_hit, "a later unrelated allocation cannot be adopted as the contact reaction");
	sample.reset();
	sample.boundary(500);
	sample.commit(0, 60, 10);
	sample.commit(1, 70, 15);
	sample.applied_contact(0, 0, contact::hit);
	sample.boundary(501);
	sample.commit(1, 80, {});
	sample.actionable(1, 80);
	sample.actionable(0, 60);
	check(!sample.latest[0].on_hit, "an attacking defender leaves trades and armor interactions unavailable");
}

void whiff_clock_and_terminal_refusals(pbd::practice::frames::tracker& sample, const auto& advance)
{
	using namespace pbd::practice::frames;
	sample.reset();
	sample.boundary(500);
	sample.commit(0, 70, 10, true, 10);
	advance(501, 525);
	sample.actionable(0, 70, 24);
	check(!sample.latest[0].on_whiff, "nonlinear or delayed animation timing stays unavailable");
	sample.reset();
	sample.boundary(600);
	sample.commit(0, 80, 10, true, 10);
	sample.boundary(625);
	sample.actionable(0, 80, 26);
	check(!sample.latest[0].on_whiff, "a clock gap discards whiff timing");
	sample.reset();
	sample.boundary(700);
	sample.commit(0, 90, 10, true);
	advance(701, 725);
	sample.actionable(0, 90, 26);
	check(!sample.latest[0].on_whiff, "an unqualified attack window does not establish whiff recovery");
	sample.reset();
	sample.boundary(800);
	sample.commit(0, 100, 10, true, 10);
	advance(801, 825);
	sample.actionable(0, 100, 26);
	sample.reject_recovery(0, 100);
	check(!sample.latest[0].on_whiff && !sample.latest[0].whiff_ready, "an unsupported terminal removes any provisional whiff result");
	sample.reset();
	sample.boundary(900);
	sample.commit(0, 110, 10, true, 10);
	advance(901, 909);
	sample.actionable(0, 110, 10);
	check(sample.latest[0].on_whiff == -1, "a window opening on the final active frame has its first action on the next frame");
}

void actionable_move_commands(const auto& move, const auto& command, const auto& end, std::uint32_t requirements, std::uint32_t extra)
{
	move(2, 39);
	command(0x2002, 3, 1, 14, 0x50);
	end(0x8001, 26);
	move(3, 60);
	end(0x8002, 54);
	move(4, 77);
	end(0x8001, 0, 0);
	move(5, 50);
	end(0x8004, 50);
	move(6, 95);
	command(0, 7, 15, 15, 0x41);
	end(0x8001, 0, 0);
	move(7, 40);
	end(0x8001, 25);
	move(8, 30);
	command(0, 0x8001, 1, 2, 0x41, requirements + 8);
	end(0x8001, 19);
	move(9, 39);
	command(0, 3, 10, 10, 0x41, requirements, extra + 4);
	end(0x8001, 26);
	move(10, 40);
	end(0x8020, 30);
	move(11, 40);
	end(0x8011, 30);
	move(12, 40);
	end(0x8001, 70);
	move(13, 40);
	command(0, 13, 5, 5, 0x41);
	end(0x8001, 30);
}

void contact_label_publication()
{
	using namespace pbd::practice::frames;
	calculator sample;
	// A launcher: the reaction ends on the floor, so the result is its label with no number.
	sample.commit_reading(1, 900, 20, 1, free_on(1), false);
	sample.commit_reading(0, 100, 39, 1, free_on(40), true, 16, 15);
	sample.applied_contact(0, contact::hit, 54, outcome_label::launch);
	sample.commit_reading(1, 200, 55, 1, {}, false);
	check(sample.latest[0].label == outcome_label::launch && !sample.latest[0].advantage && sample.latest[0].startup == 15u,
		"a launch has its label and its startup");
	// A juggle hit of a string: its own label, and the string keeps the first hit's startup.
	sample.commit_reading(0, 101, 70, 1, free_on(40), true, 12, 11);
	sample.applied_contact(0, contact::unavailable, 80, outcome_label::juggle);
	check(sample.latest[0].label == outcome_label::juggle && sample.latest[0].contacted && sample.latest[0].startup == 15u,
		"a juggle hit is Juggle and keeps the first hit's startup");
	// Carried into the wall: the wall reaction makes it Wall.
	sample.commit_reading(1, 201, 85, 1, {}, false, {}, {}, true);
	check(sample.latest[0].label == outcome_label::wall && !sample.latest[0].advantage, "a wall reaction makes it Wall");
	// A standing hit near the wall: the wall branch replaces the number it had.
	sample.commit_reading(0, 102, 200, 1, free_on(26), true);
	sample.applied_contact(0, contact::hit, 209, outcome_label::none);
	sample.commit_reading(1, 202, 210, 1, free_on(25), false);
	check(sample.latest[0].advantage == 9 && sample.latest[0].label == outcome_label::none, "a plain hit has its number");
	sample.commit_reading(1, 203, 212, 1, free_on(30), false, {}, {}, true);
	check(sample.latest[0].label == outcome_label::wall && !sample.latest[0].advantage, "a wall branch replaces the number");
	// After the fighter attacks again, a wall move is no longer part of the earlier hit.
	sample.commit_reading(0, 103, 300, 1, free_on(26), true);
	sample.applied_contact(0, contact::hit, 309, outcome_label::none);
	sample.commit_reading(1, 204, 310, 1, free_on(25), false);
	sample.commit_reading(1, 205, 340, 1, free_on(30), true);
	sample.commit_reading(1, 206, 360, 1, free_on(30), false, {}, {}, true);
	check(sample.latest[0].advantage == 9 && sample.latest[0].label == outcome_label::none, "a wall move after its own attack is not the hit's");
	// A throw: entering the thrower's half of the throw is the connection.
	sample.reset();
	sample.commit_reading(0, 110, 400, 1, free_on(40), true, 12, 12);
	sample.commit_reading(0, 111, 410, 1, {}, true);
	sample.throw_connected(0);
	check(sample.latest[0].label == outcome_label::throw_hit && sample.latest[0].contacted && sample.latest[0].startup == 12u,
		"a connected throw is Throw with the grab's startup");
	sample.boundary(500);
	check(!sample.latest[0].whiff, "a connected throw has no whiff");
	sample.commit_reading(0, 112, 600, 1, free_on(26), true);
	check(sample.latest[0].label == outcome_label::none && !sample.latest[0].contacted, "the next attack clears the label");
}

void preset_lifetime_and_boundary_checks()
{
	using namespace pbd::practice::start;
	check(selection_boundary(10, 3, 44, 6, 14), "the initial Practice selector has its own manager state and resource context");
	check(!selection_boundary(10, 3, 44, 4, 14) && !selection_boundary(10, 3, 44, 6, 10) && !selection_boundary(14, 3, 44, 6, 14),
		"fight manager state, display mode and resource context are not interchangeable");
}
