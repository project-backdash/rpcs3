#include "pbd_test.h"

#include "Emu/Cell/pbd_hidden_selection.h"
#include "Emu/Cell/pbd_game_overlay.h"
#include "Emu/Cell/pbd_update_script.h"
#include "select_tables_fixture.h"

using pbd::test::check;

namespace
{
	using namespace pbd::hidden_selection;

	observation selecting(u32 role = 0)
	{
		observation value;
		value.scene = 3;
		value.subscene = 44;
		value.mode = 4;
		value.grid_mode = 1;
		value.state = 3;
		value.pad_role = role;
		value.cursor = 14;
		value.base_character = 5;
		value.verified = true;
		return value;
	}
	void selection_card_checks()
	{
		pbd::game_overlay::snapshot panel;
		check(!panel.selection_transition);
		const auto card = pbd::game_overlay::selection_card(panel);
		check(card.visible && card.selection_transition && !card.close_on_action && !card.row_count && card.generation == 1);
		check(panel.generation == 0 && !panel.visible);
		check(!pbd::game_overlay::parse("PBD-MENU-1\ngeneration=1\nvisible=1\nselection_transition=1\n").value);
	}

	void row_resolution_checks(const tables& mapping)
	{
		check(resolve(mapping, 12)->cursor == 14 && resolve(mapping, 12)->confirm == 0x80);
		check(resolve(mapping, 31)->cursor == 23 && resolve(mapping, 31)->confirm == 0x40);
		check(resolve(mapping, 77)->cursor == 5 && resolve(mapping, 79)->cursor == 6);
		check(resolve(mapping, 18)->confirm == 0x800 && resolve(mapping, 19)->confirm == 0x10 && resolve(mapping, 20)->confirm == 0x20);
		for (u32 row = 0; row < 84; ++row)
			check(resolve(mapping, row));
		check(!resolve(mapping, 84) && !resolve(mapping, 85));
		auto locked = mapping;
		locked.availability &= ~(1ull << 5);
		check(!resolve(locked, 12));
	}

	void locked_boss_checks(const tables& boss_locked)
	{
		check(resolve(boss_locked, 12) && resolve(boss_locked, 32));
		driver ordinary;
		check(ordinary.begin({12, 32, 17, 3, 0}, boss_locked, 0));
		check(!resolve(boss_locked, 81) && !resolve(boss_locked, 82) && !resolve(boss_locked, 83));
		check(directional_neighbor(boss_locked, 34, 3) == 25u); // Right skips Jinpachi and random, in the same direction.
		check(directional_neighbor(boss_locked, 25, 2) == 34u); // Left skips random and Jinpachi.
		check(!move_toward(boss_locked, 34, 35) && !move_toward(boss_locked, 25, 24));
	}

	void banned_route_checks(const tables& mapping, const tables& boss_locked)
	{
		for (auto banned : {mapping, boss_locked})
		{
			banned.ban_jinpachi = true;
			check(!resolve(banned, 81) && !resolve(banned, 82) && !resolve(banned, 83));
			check(cursor_available(banned, 24) && !cursor_available(banned, 35));
			check(directional_neighbor(banned, 34, 3) == 24u && directional_neighbor(banned, 24, 2) == 34u);
			const auto saved = pbd::character_rule::initial_selection(banned, 35, 34, 34, 81, 0x7ffffffffull);
			check(saved && saved->cursor == 34 && saved->base == 30 && saved->row == 75 && saved->panel == 0);
			for (u32 start = 0; start < 36; ++start)
				for (u32 target = 0; target < 36; ++target)
				{
					if (!cursor_available(banned, target))
						continue;
					auto cursor = start;
					unsigned moves = 0;
					while (cursor != target && moves++ < 36)
					{
						const auto button = move_toward(banned, cursor, target);
						check(button);
						const auto direction = std::find(directions.begin(), directions.end(), *button);
						check(direction != directions.end());
						const auto next = directional_neighbor(banned, cursor, static_cast<std::size_t>(direction - directions.begin()));
						check(next && *next != 35);
						cursor = *next;
					}
					check(cursor == target && moves < 36);
				}
		}
	}

	void unavailable_table_checks(const tables& mapping, const tables& boss_locked)
	{
		auto unsupported = boss_locked;
		unsupported.availability &= ~(1ull << 5);
		check(!resolve(unsupported, 32) && !move_toward(unsupported, 0, 22));
		auto bad_neighbor = boss_locked;
		bad_neighbor.grid[34].neighbor[3] = 36;
		check(!directional_neighbor(bad_neighbor, 34, 3));
		driver unavailable;
		check(!unavailable.begin({12, 81, 17, 3, 0}, boss_locked, 0));
		check(std::string_view(unavailable.error()) == "selection_row_unavailable");
		auto changed = mapping;
		changed.costumes[5].rows = {13};
		check(!resolve(changed, 12));
	}

	void grid_route_checks(const tables& mapping, const tables& boss_locked)
	{
		// Every start/target pair reaches the real cell by bounded single-direction neighbors.
		for (u32 start = 0; start < 36; ++start)
			for (u32 target = 0; target < 36; ++target)
			{
				auto cursor = start;
				unsigned moves = 0;
				while (cursor != target && moves++ < 8)
				{
					const auto button = move_toward(mapping, cursor, target);
					check(button);
					const auto direction = std::find(directions.begin(), directions.end(), *button);
					check(direction != directions.end());
					cursor = mapping.grid[cursor].neighbor[static_cast<std::size_t>(direction - directions.begin())];
				}
				check(cursor == target && moves <= 7);
			}
		for (u32 start = 0; start < 36; ++start)
			for (u32 target = 0; target < 36; ++target)
			{
				if (!cursor_available(boss_locked, target))
					continue;
				auto cursor = start;
				unsigned moves = 0;
				while (cursor != target && moves++ < 36)
				{
					const auto button = move_toward(boss_locked, cursor, target);
					check(button);
					const auto direction = std::find(directions.begin(), directions.end(), *button);
					check(direction != directions.end());
					const auto next = directional_neighbor(boss_locked, cursor, static_cast<std::size_t>(direction - directions.begin()));
					check(next);
					cursor = *next;
				}
				check(cursor == target && moves < 36);
			}
	}

	void fixed_selection_checks(const tables& mapping)
	{
		{
			driver test;
			check(test.begin({12, 32, 17, 3, 0}, mapping, 0));
			auto now = selecting();
			check(test.sample(0, now, 0xffff, &mapping) == 0x80); // Square means Nina row 12; Cross would select row 13.
			now.cursor = 0;
			check(test.sample(0, now, 0xffff, &mapping) == 0x80); // frozen same source
			now = selecting();
			check(test.sample(6, now, 0xffff, &mapping) == 0);
			now.state = 9;
			now.committed_row = 12;
			check(test.sample(24, now, 0xffff) == 0x40);
			now.state = 21;
			check(test.sample(48, now, 0xffff) == 0);
			now.scene = 8;
			check(test.sample(72, now, 0xffff) == 0);
			now.scene = 17;
			now.applied_verified = true;
			now.applied_left = 12;
			now.applied_right = 32;
			now.applied_stage = 17;
			now.applied_rounds = 3;
			check(test.sample(96, now, 0xffff) == 0 && test.complete());
			check(test.sample(97, now, 0x1000) == 0x1000);
		}
	}

	void stage_steering_checks(const tables& mapping)
	{
		{
			driver test;
			check(test.begin({12, 31, 0, 3, 1}, mapping, 0));
			auto now = selecting(1);
			now.cursor = 23;
			now.base_character = 11;
			check(test.sample(0, now, 0xffff, &mapping) == 0x40); // Panda costume, owned right side only
			now.state = 13;
			now.stage_owner = 1;
			now.stage_mode = 1;
			now.new_stages = {17, 18};
			now.old_stages = {0, 1};
			check(test.sample(24, now, 0xffff) == 0x1000);
			now.stage_mode = 2;
			now.stage_cursor = 1;
			check(test.sample(48, now, 0xffff) == 0x2000);
			now.stage_cursor = 0;
			check(test.sample(72, now, 0xffff) == 0x40);
		}
	}

	void invalid_state_checks(const tables& mapping)
	{
		for (const auto bad_state : {2u, 7u, 16u, 17u, 18u, 19u, 20u, 23u})
		{
			driver test;
			check(test.begin({12, 32, 17, 3, 0}, mapping, 0));
			auto now = selecting();
			now.state = bad_state;
			check(test.sample(0, now, 0xffff, &mapping) == 0 && !test.healthy());
		}
	}

	void transition_readback_checks(const tables& mapping)
	{
		{
			driver test;
			check(test.begin({12, 32, 17, 3, 0}, mapping, 0));
			auto now = selecting();
			u32 source = 0;
			for (const auto scene : {4u, 5u, 0u, 1u, 2u, 6u, 7u, 8u, 9u, 10u, 11u, 12u, 13u, 14u, 16u})
			{
				now.scene = scene;
				check(test.sample(source++, now, 0xffff) == 0 && test.healthy() && !test.complete());
			}
			now.scene = 17;
			now.applied_verified = true;
			now.applied_left = 12;
			now.applied_right = 32;
			now.applied_stage = 18;
			now.applied_rounds = 3;
			check(test.sample(source, now, 0xffff) == 0 && !test.healthy() && !test.complete());
		}
	}

	void invalid_scene_checks(const tables& mapping)
	{
		for (const auto scene : {15u, 18u, 39u, 44u})
		{
			driver test;
			check(test.begin({12, 32, 17, 3, 0}, mapping, 0));
			auto now = selecting();
			now.scene = scene;
			check(test.sample(0, now, 0xffff) == 0 && !test.healthy());
		}
	}

	void unavailable_stage_checks(const tables& mapping)
	{
		{
			driver test;
			check(test.begin({12, 32, 0x25, 3, 0}, mapping, 0));
			auto now = selecting();
			now.state = 13;
			now.new_stages = {17, 18, 0x23};
			check(test.sample(0, now, 0xffff) == 0 && !test.healthy()); // no unlock or stage normalization guess
		}
	}

	void fighter_readback_checks(const tables& mapping)
	{
		{
			driver test;
			check(test.begin({12, 32, 17, 3, 0}, mapping, 0));
			auto now = selecting();
			now.scene = 17;
			now.applied_verified = true;
			now.applied_left = 13;
			now.applied_right = 32;
			now.applied_stage = 17;
			now.applied_rounds = 3;
			check(test.sample(0, now, 0xffff) == 0 && !test.healthy() && !test.complete());
		}
	}

	void select_timeout_checks(const tables& mapping)
	{
		{
			driver test;
			check(test.begin({12, 32, 17, 3, 0}, mapping, 0));
			auto now = selecting();
			check(test.sample(3601, now, 0xffff, &mapping) == 0 && !test.healthy());
		}
	}

	void loading_timeout_checks(const tables& mapping)
	{
		{
			driver test;
			check(test.begin({12, 32, 17, 3, 0}, mapping, 0));
			auto now = selecting();
			now.scene = 0;
			check(test.sample(0, now, 0xffff) == 0 && test.healthy() && !test.complete());
			check(test.sample(3601, now, 0xffff) == 0 && !test.healthy() && !test.complete());
		}
	}

	void offset_timeout_checks(const tables& mapping)
	{
		{
			driver test;
			check(test.begin({12, 32, 17, 3, 0}, mapping, 8624));
			auto now = selecting();
			now.scene = 1;
			check(test.sample(8625, now, 0xffff) == 0 && test.healthy() && !test.complete());
			check(test.sample(12225, now, 0xffff) == 0 && !test.healthy() && !test.complete());
		}
	}

	void rewound_source_checks(const tables& mapping)
	{
		{
			// A transport resend must not admit a skipped source after a newer dispatched source.
			// The transport must reuse its frozen/neutral word; the selector still rejects a real backwards admission.
			driver test;
			check(test.begin({12, 32, 17, 3, 0}, mapping, 8624));
			auto now = selecting();
			now.scene = 2;
			check(test.sample(8686, now, 0xffff) == 0 && test.healthy());
			check(test.sample(8672, now, 0xffff) == 0 && !test.healthy());
			check(std::string_view(test.error()) == "selection_source_rewound");
		}
	}

	void first_packet_checks(const tables& mapping)
	{
		{
			// The first metadata packet must not classify the pending current tick as a skipped past tick.
			static_assert(!pbd::update_source_was_skipped(0, 0, false));
			static_assert(!pbd::update_source_was_skipped(2, 2, false));
			// A barrier announces frontier-1; that announcement remains past even if it has no dispatched row.
			static_assert(pbd::update_source_was_skipped(1, 2, false));
			static_assert(pbd::update_source_was_skipped(8672, 8686, false));
			static_assert(!pbd::update_source_was_skipped(8672, 8686, true));
			driver test;
			check(test.begin({12, 32, 17, 3, 0}, mapping, 0));
			auto now = selecting();
			now.cursor = 0;
			const auto first = test.sample(0, now, 0xffff, &mapping);
			check(first != 0 && test.healthy());
			now.cursor = 12;
			check(test.sample(0, now, 0xffff, &mapping) == first && test.healthy());
		}
	}

	void fixed_input_ownership_checks(const tables& mapping)
	{
		{
			// A fixed selection runs behind the loading card and owns this PC's word.
			driver fixed;
			check(fixed.begin({12, 32, 17, 3, 0}, mapping, 0));
			check(fixed.active() && fixed.hidden() && fixed.owns_input());
		}
	}

	void kept_fighter_checks(const tables& mapping)
	{
		{
			// Change character, Right side chooses: the Left PC steers its kept fighter back to the frozen row and costume, waits
			// through the chooser's countdown, and its battle readback binds every pick but the chooser's row.
			const u32 limit = selection_samples + 99 * 60;
			driver kept;
			check(kept.begin({12, 32, 17, 3, 0, 1u, limit}, mapping, 0));
			check(kept.active() && !kept.hidden() && kept.owns_input());
			auto now = selecting();
			check(kept.sample(0, now, 0xffff, &mapping) == 0x80);
			now.state = 9;
			now.committed_row = 12;
			check(kept.sample(24, now, 0xffff) == 0x40);
			now.state = 12;
			for (u32 source = 48; source <= limit; source += 600)
				check(kept.sample(source, now, 0xffff) == 0 && kept.healthy());
			now.scene = 17;
			now.applied_verified = true;
			now.applied_left = 12;
			now.applied_right = 77;
			now.applied_stage = 17;
			now.applied_rounds = 3;
			check(kept.sample(limit, now, 0xffff) == 0 && kept.complete() && !kept.active() && !kept.owns_input());
			// The kept row and the stage still bind.
			driver moved;
			check(moved.begin({12, 32, 17, 3, 0, 1u, limit}, mapping, 0));
			now.applied_left = 13;
			check(moved.sample(0, now, 0xffff) == 0 && std::string_view(moved.error()) == "selection_applied_mismatch");
			driver restaged;
			check(restaged.begin({12, 32, 17, 3, 0, 1u, limit}, mapping, 0));
			now.applied_left = 12;
			now.applied_stage = 18;
			check(restaged.sample(0, now, 0xffff) == 0 && !restaged.healthy());
			// The longer limit still ends a selection that never finishes.
			driver late;
			check(late.begin({12, 32, 17, 3, 0, 1u, limit}, mapping, 0));
			check(late.sample(limit + 1, selecting(), 0xffff, &mapping) == 0 && std::string_view(late.error()) == "selection_timeout");
		}
	}

	void chooser_input_checks(const tables& mapping)
	{
		{
			// The chooser's PC (Right, the stage owner): its own word reaches every state of its select but the stage, which is steered
			// to the frozen stage. Its own frozen row needs no costume button: only the kept row must resolve.
			driver chooser;
			check(chooser.begin({12, 84, 17, 3, 1, 1u, selection_samples + 1800}, mapping, 0));
			check(chooser.active() && !chooser.hidden() && !chooser.owns_input());
			auto now = selecting(1);
			now.cursor = 30;
			now.base_character = 24;
			check(chooser.sample(0, now, 0x2000) == 0x2000);
			now.state = 9;
			check(chooser.sample(1, now, 0x0020) == 0x0020);
			now.state = 12;
			check(chooser.sample(2, now, 0x0040) == 0x0040);
			now.state = 7;
			check(chooser.sample(3, now, 0x0020) == 0x0020 && chooser.healthy());
			now.state = 13;
			now.stage_owner = 1;
			now.stage_mode = 1;
			now.new_stages = {17, 18};
			now.old_stages = {0, 1};
			check(chooser.sample(24, now, 0x0040) == 0x1000);
			now.stage_mode = 0;
			now.stage_cursor = 1;
			check(chooser.sample(48, now, 0x0040) == 0x2000);
			now.stage_cursor = 0;
			check(chooser.sample(72, now, 0) == 0x40);
			check(chooser.sample(78, now, 0x0040) == 0);
			now.scene = 2;
			check(chooser.sample(96, now, 0x0040) == 0);
			now.scene = 17;
			now.applied_verified = true;
			now.applied_left = 12;
			now.applied_right = 61;
			now.applied_stage = 17;
			now.applied_rounds = 3;
			check(chooser.sample(120, now, 0x0040) == 0 && chooser.complete());
			// A Left chooser's record never opens the stage; the Right PC steers it.
			driver left_chooser;
			check(left_chooser.begin({12, 32, 17, 3, 0, 0u}, mapping, 0));
			auto left_now = selecting(0);
			left_now.state = 13;
			left_now.stage_owner = 1;
			left_now.new_stages = {17};
			check(left_chooser.sample(0, left_now, 0x0040) == 0 && std::string_view(left_chooser.error()) == "selection_stage_unverified");
		}
	}

	void invalid_repick_checks(const tables& mapping)
	{
		{
			// Only the kept row must resolve, and only a real side can choose.
			driver unavailable_kept;
			check(!unavailable_kept.begin({84, 32, 17, 3, 1, 1u}, mapping, 0));
			check(std::string_view(unavailable_kept.error()) == "selection_row_unavailable");
			driver outside;
			check(!outside.begin({12, 32, 17, 3, 0, 2u}, mapping, 0));
			check(std::string_view(outside.error()) == "selection_request_unsupported");
			driver no_time;
			check(!no_time.begin({12, 32, 17, 3, 0, 1u, 0}, mapping, 0));
		}
	}
} // namespace

PBD_TEST(PbdHiddenSelection, Checks)
{
	selection_card_checks();
	const auto mapping = pbd::test::select_tables();
	row_resolution_checks(mapping);
	auto boss_locked = mapping;
	boss_locked.availability = 0x3ffffffffull;
	boss_locked.costumes[34].rows.clear(); // Actual locked Jinpachi metadata has count zero.
	locked_boss_checks(boss_locked);
	banned_route_checks(mapping, boss_locked);
	unavailable_table_checks(mapping, boss_locked);
	grid_route_checks(mapping, boss_locked);
	fixed_selection_checks(mapping);
	stage_steering_checks(mapping);
	invalid_state_checks(mapping);
	transition_readback_checks(mapping);
	invalid_scene_checks(mapping);
	unavailable_stage_checks(mapping);
	fighter_readback_checks(mapping);
	select_timeout_checks(mapping);
	loading_timeout_checks(mapping);
	offset_timeout_checks(mapping);
	rewound_source_checks(mapping);
	first_packet_checks(mapping);
	fixed_input_ownership_checks(mapping);
	kept_fighter_checks(mapping);
	chooser_input_checks(mapping);
	invalid_repick_checks(mapping);
}
