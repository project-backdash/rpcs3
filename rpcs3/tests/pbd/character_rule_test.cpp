#include "pbd_test.h"

#include "Emu/PBD/pbd_character_rule.h"
#include "Emu/Cell/pbd_hidden_selection.h"

using pbd::test::check;

static void selection_call_guards()
{
	using namespace pbd::character_rule;
	call value{1, 1, 0x01000000, 0x470540, 4, 3, 44, 1, 0xaf538, jinpachi, 0x10093f58, random_cursor, true};
	check(selection_predicate(rule::ban_jinpachi, value) == predicate::available);
	value.cursor = jinpachi_cursor;
	check(selection_predicate(rule::ban_jinpachi, value) == predicate::unavailable);
	check(selection_predicate(rule::stock, value) == predicate::stock);
	value.link = 0xb0b6c;
	check(selection_predicate(rule::ban_jinpachi, value) == predicate::unavailable);
	for (const auto link : {0x5a468u, 0x5a498u, 0x5a4c8u, 0x5a514u, 0x5ba74u, 0x615f8u, 0xafcd4u, 0x1033c4u})
	{
		value.link = link;
		check(selection_predicate(rule::ban_jinpachi, value) == predicate::stock);
	}
	value.link = 0xaf538;
	auto wrong = value;
	wrong.active_generation++;
	check(selection_predicate(rule::ban_jinpachi, wrong) == predicate::unverified);
	wrong = value;
	wrong.mode = 5;
	check(selection_predicate(rule::ban_jinpachi, wrong) == predicate::unverified);
	wrong = value;
	wrong.owner++;
	check(selection_predicate(rule::ban_jinpachi, wrong) == predicate::unverified);
	wrong = value;
	wrong.record += 4;
	check(selection_predicate(rule::ban_jinpachi, wrong) == predicate::unverified);
	wrong = value;
	wrong.toc += std::uint64_t{1} << 32;
	check(selection_predicate(rule::ban_jinpachi, wrong) == predicate::unverified);
	wrong = value;
	wrong.record += std::uint64_t{1} << 32;
	check(selection_predicate(rule::ban_jinpachi, wrong) == predicate::unverified);
	value.link = 0xb16c8;
	value.cursor = 34;
	check(saved_character_predicate(rule::ban_jinpachi, value, 0) == predicate::unavailable);
	check(saved_character_predicate(rule::stock, value, 0) == predicate::stock);
	check(saved_character_predicate(rule::ban_jinpachi, value, 1) == predicate::unverified);
	value.record = 0x10093fd0;
	check(saved_character_predicate(rule::ban_jinpachi, value, 1) == predicate::unavailable);
	value.cursor = jinpachi_cursor;
	check(saved_character_predicate(rule::ban_jinpachi, value, 1) == predicate::unverified);
}

PBD_TEST(PbdCharacterRule, Checks)
{
	using namespace pbd::character_rule;
	check(for_session(mode::friend_room) == rule::stock);
	check(for_session(mode::lobby) == rule::ban_jinpachi);
	check(for_session(mode::lobby, true) == rule::stock);
	check(for_session(mode::ranked, true) == rule::ban_jinpachi);
	check(candidates(rule::stock, roster_mask) == roster_mask);
	check(candidates(rule::ban_jinpachi, roster_mask) == 0x3ffffffffull);
	pbd::hidden_selection::tables table;
	table.verified = true;
	table.availability = roster_mask;
	for (std::size_t i = 0; i < table.grid.size(); ++i)
	{
		table.grid[i].character = static_cast<std::uint16_t>(i % 34);
		table.grid[i].neighbor[2] = static_cast<std::uint8_t>((i + 35) % 36);
	}
	table.grid[random_cursor].character = 35;
	table.grid[jinpachi_cursor].character = jinpachi;
	for (const auto mask : {roster_mask, roster_mask & ~jinpachi_bit})
	{
		table.availability = mask;
		for (std::uint32_t cursor = 0; cursor < 36; ++cursor)
			check(initial_cursor(rule::ban_jinpachi, table, cursor) == (cursor == jinpachi_cursor ? 34u : cursor));
		check(cursor_available(rule::ban_jinpachi, table, random_cursor));
	}
	table.availability = roster_mask;
	check(initial_cursor(rule::stock, table, jinpachi_cursor) == jinpachi_cursor);
	check(!initial_cursor(rule::ban_jinpachi, table, 36));
	table.grid[jinpachi_cursor].neighbor[2] = jinpachi_cursor;
	check(!initial_cursor(rule::ban_jinpachi, table, jinpachi_cursor));
	table.grid[jinpachi_cursor].neighbor[2] = 255;
	check(!initial_cursor(rule::ban_jinpachi, table, jinpachi_cursor));
	table.verified = false;
	check(!initial_cursor(rule::ban_jinpachi, table, 0));
	table.verified = true;
	table.grid[jinpachi_cursor].neighbor[2] = 34;
	table.grid[34].character = 30;
	for (std::uint32_t base = 0; base < 34; ++base)
		table.costumes[base].rows = {base};
	auto saved = initial_selection(table, jinpachi_cursor, jinpachi, jinpachi, 81, roster_mask);
	check(saved && saved->cursor == 34 && saved->panel == 0 && saved->base == 30 && saved->row == 30);
	check(saved->move && saved->preview && saved->reroute);
	auto random = initial_selection(table, random_cursor, jinpachi, jinpachi, 83, roster_mask);
	check(random && random->cursor == random_cursor && random->panel == 0 && random->base == 0 && random->row == 0);
	check(!random->move && random->preview && random->reroute);
	auto unchanged = initial_selection(table, 1, 33, 1, 1, roster_mask);
	check(unchanged && !unchanged->move && !unchanged->preview && !unchanged->reroute);
	check(initial_selection(table, 1, 33, 1, 0xffffffffu, roster_mask));
	check(!initial_selection(table, 1, 35, 1, 1, roster_mask));
	check(!initial_selection(table, random_cursor, jinpachi, jinpachi, 83, jinpachi_bit));
	table.costumes[30].rows.clear();
	check(!initial_selection(table, jinpachi_cursor, 0, jinpachi, 81, roster_mask));
	for (std::uint32_t row = 0; row < 90; ++row)
	{
		check(row_available(rule::ban_jinpachi, row) == (row < 81));
		check(row_available(rule::stock, row) == (row < 85));
	}
	for (std::uint32_t first = 0; first < 35; ++first)
	{
		const auto panel = initial_panel(rule::ban_jinpachi, first, roster_mask, roster_mask);
		check(panel && *panel == (first == jinpachi ? 0u : first));
		check(initial_panel(rule::stock, first, roster_mask, roster_mask) == first);
	}
	check(initial_panel(rule::ban_jinpachi, 33, roster_mask, (std::uint64_t{1} << 7) | jinpachi_bit) == 7);
	check(!initial_panel(rule::ban_jinpachi, jinpachi, roster_mask, jinpachi_bit));
	check(!initial_panel(rule::ban_jinpachi, 35, roster_mask, roster_mask));
	check(!initial_panel(rule::ban_jinpachi, 0, roster_mask << 1, roster_mask));

	selection_call_guards();
}

PBD_TEST(PbdCharacterRule, SelectionConfirmation)
{
	using namespace pbd::character_rule;
	// Selection confirmation accepts subscenes 2, 4 and 44 at the verified handler call.
	call value{1, 1, 0x01000000, 0x470540, 4, 3, 4, 1, 0xb0b6c, jinpachi, 0x10093f58, 7, true};
	check(selection_predicate(rule::ban_jinpachi, value) == predicate::unavailable);
	value.record = 0x10093fd0;
	check(selection_predicate(rule::ban_jinpachi, value) == predicate::unavailable);
	value.link = 0xaf538;
	value.cursor = random_cursor;
	check(selection_predicate(rule::ban_jinpachi, value) == predicate::unverified);
	value.link = 0xb16c8;
	check(saved_character_predicate(rule::ban_jinpachi, value, 1) == predicate::unverified);
	value.link = 0xb0b6c;
	for (std::uint32_t phase = 0; phase < 64; ++phase)
	{
		value.subscene = phase;
		check(selection_predicate(rule::ban_jinpachi, value) ==
			  (phase == 2 || phase == 4 || phase == 44 ? predicate::unavailable : predicate::unverified));
	}
	value.link = 0xaf538;
	value.subscene = 2;
	check(selection_predicate(rule::ban_jinpachi, value) == predicate::unverified);
	value.link = 0xb0b6c;
	value.subscene = 4;
	for (const auto phase : {4u, 44u})
	{
		value.subscene = phase;
		auto wrong = value;
		wrong.active_generation++;
		check(selection_predicate(rule::ban_jinpachi, wrong) == predicate::unverified);
		wrong = value;
		wrong.owner++;
		check(selection_predicate(rule::ban_jinpachi, wrong) == predicate::unverified);
		wrong = value;
		wrong.toc++;
		check(selection_predicate(rule::ban_jinpachi, wrong) == predicate::unverified);
		wrong = value;
		wrong.mode = 5;
		check(selection_predicate(rule::ban_jinpachi, wrong) == predicate::unverified);
		wrong = value;
		wrong.scene = 2;
		check(selection_predicate(rule::ban_jinpachi, wrong) == predicate::unverified);
		wrong = value;
		wrong.grid_mode = 0;
		check(selection_predicate(rule::ban_jinpachi, wrong) == predicate::unverified);
		wrong = value;
		wrong.record += 4;
		check(selection_predicate(rule::ban_jinpachi, wrong) == predicate::unverified);
		wrong = value;
		wrong.cursor = 36;
		check(selection_predicate(rule::ban_jinpachi, wrong) == predicate::unverified);
		wrong = value;
		wrong.pinned = false;
		check(selection_predicate(rule::ban_jinpachi, wrong) == predicate::unverified);
	}
	// A cleared scene permits continuation only while the subscene remains at select (44).
	value.scene = 0;
	value.subscene = 44;
	value.cursor = 5;
	check(selection_predicate(rule::ban_jinpachi, value) == predicate::unavailable);
	value.subscene = 4;
	check(selection_predicate(rule::ban_jinpachi, value) == predicate::unverified);
	value.subscene = 44;
	value.link = 0xaf538;
	check(selection_predicate(rule::ban_jinpachi, value) == predicate::unverified);
}
