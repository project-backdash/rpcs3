#include "pbd_test.h"
#include "select_tables_fixture.h"
#include "Emu/PBD/pbd_costume_select.h"
#include "Emu/PBD/pbd_costume_panel.h"

using pbd::test::check;
using namespace pbd::costume_select;

PBD_TEST(PbdCostumePanel, UsesStockNamesAndPublishesOnlyChangedViews)
{
	using namespace pbd::costume_panel;
	check(fighter_name(77) == "Lili" && fighter_name(78) == "Lili" && fighter_name(31) == "Panda");
	for (u32 selected = 0; selected < 84; ++selected)
		check(!fighter_name(selected).empty());
	check(label(card{pbd::custom_look::look{77}, 0, {}}) == "DEFAULT 01");
	check(label(card{pbd::custom_look::look{78}, 0, 9}) == "CUSTOM 10");
	publish({42, true, true, 0, 0, {{pbd::custom_look::look{77}, 0x80, {}}}});
	const auto first = load();
	const auto serial = revision();
	publish(first);
	check(revision() == serial);
	publish({43});
	check(revision() == serial + 1 && !load().visible && load().generation == 43);
}

namespace
{
	pbd::hidden_selection::observation selecting(u32 fighter = 31)
	{
		pbd::hidden_selection::observation out;
		out.verified = true;
		out.mode = 10;
		out.scene = 3;
		out.subscene = 44;
		out.grid_mode = 1;
		out.state = 3;
		out.base_character = fighter;
		return out;
	}
} // namespace

PBD_TEST(PbdCostumeSelect, EveryStockCostumeHasItsOwnConfirm)
{
	const auto tables = pbd::test::select_tables();
	u32 total = 0;
	for (u32 fighter = 0; fighter < 35; ++fighter)
	{
		const auto list = cards(tables, fighter);
		check(list.size() == tables.costumes[fighter].rows.size());
		for (u32 i = 0; i < list.size(); ++i)
		{
			check(list[i].look.row == tables.costumes[fighter].rows[i] && !list[i].slot);
			check(list[i].confirm == pbd::hidden_selection::resolve(tables, list[i].look.row)->confirm);
			++total;
		}
	}
	check(total == 84 && cards(tables, 7).size() == 5 && cards(tables, 24).size() == 4);
	auto unavailable = tables;
	unavailable.verified = false;
	check(cards(unavailable, 31).empty() && cards(tables, 35).empty());
	unavailable = tables;
	unavailable.ban_jinpachi = true;
	check(cards(unavailable, 34).empty());
}

PBD_TEST(PbdCostumeSelect, CrossAlwaysPicksDefaultOneAndSquareRemembersTheLastPick)
{
	const auto tables = pbd::test::select_tables();
	const auto seen = selecting(7);
	row menu;
	check(menu.step(tables, seen, square).word == 0 && menu.open());
	for (u32 i = 0; i < 4; ++i)
	{
		menu.step(tables, seen, 0);
		menu.step(tables, seen, right);
	}
	menu.step(tables, seen, 0);
	const auto fifth = menu.step(tables, seen, cross);
	check(fifth.picked && fifth.picked->look.row == 20 && fifth.word == circle && !menu.open());
	check(menu.step(tables, seen, cross).word == 0);
	menu.step(tables, seen, 0);
	menu.step(tables, seen, square);
	check(menu.open() && menu.focus() == 4);
	menu.step(tables, seen, 0);
	menu.step(tables, seen, circle);
	check(!menu.open());
	menu.step(tables, seen, 0);
	const auto plain = menu.step(tables, seen, cross);
	check(plain.picked && plain.picked->look.row == 16 && plain.word == square);
}

PBD_TEST(PbdCostumeSelect, ThePanelHoldsEveryButtonAndLeavesTheRosterCursorAlone)
{
	const auto tables = pbd::test::select_tables();
	const auto seen = selecting();
	row menu;
	check(menu.step(tables, seen, left | l1 | r1 | start | circle).word == left);
	menu.step(tables, seen, 0);
	const auto setup = menu.step(tables, seen, triangle);
	check(setup.controller_setup && setup.word == 0);
	menu.step(tables, seen, 0);
	menu.step(tables, seen, square);
	check(menu.step(tables, seen, square).word == 0 && menu.focus() == 0);
	menu.step(tables, seen, left);
	check(menu.focus() == 1);
	check(menu.step(tables, seen, left).word == 0 && menu.focus() == 1);
	menu.step(tables, seen, circle);
	check(!menu.open() && menu.step(tables, seen, circle | cross).word == 0);
	menu.step(tables, seen, 0);
	check(menu.step(tables, seen, right).word == right);
}

PBD_TEST(PbdCostumeSelect, OtherModesAndUnverifiedScreensKeepTheirInput)
{
	const auto tables = pbd::test::select_tables();
	for (u32 mode : {0u, 5u, 12u})
	{
		row menu;
		auto seen = selecting();
		seen.mode = mode;
		check(!menu.step(tables, seen, square).word && !menu.open());
	}
	row menu;
	auto seen = selecting();
	menu.step(tables, seen, square);
	seen.scene = 17;
	check(!menu.step(tables, seen, cross).word && !menu.open());
	seen = selecting();
	seen.verified = false;
	check(!menu.step(tables, seen, square).word);
}

PBD_TEST(PbdCostumeSelect, VersusCountdownChoosesTheHighlightOrDefaultOne)
{
	const auto tables = pbd::test::select_tables();
	auto seen = selecting(7);
	seen.mode = 4;
	row menu;
	check(menu.step(tables, seen, square).word == 0 && menu.open());
	menu.step(tables, seen, right);
	check(menu.focus() == 1);
	const auto expired = menu.step(tables, seen, right, {}, true);
	check(expired.picked && expired.picked->look.row == 17 && !menu.open());
	for (u32 frame = 0; frame < 24; ++frame)
		check(menu.step(tables, seen, 0, {}, true).picked == expired.picked);
	menu.step(tables, seen, 0);
	menu.step(tables, seen, square);
	menu.step(tables, seen, circle);
	const auto closed = menu.step(tables, seen, circle, {}, true);
	check(closed.picked && closed.picked->look.row == 16);
}

PBD_TEST(PbdCostumeSelect, CustomsAreBoundedValidatedAndGroupedAfterDefaults)
{
	const auto tables = pbd::test::select_tables();
	std::array<std::optional<pbd::custom_look::look>, 11> slots;
	slots[0] = pbd::custom_look::look{77, {61, 67, 66, 50}};
	slots[2] = pbd::custom_look::look{78, {51, 56, 52, 50}};
	slots[3] = pbd::custom_look::look{0};
	slots[4] = pbd::custom_look::look{77, {61, 62, 66, 50}};
	slots[10] = slots[0];
	const auto list = cards(tables, 31, slots);
	check(list.size() == 4 && list[2].slot == 0 && list[3].slot == 2);
	row menu;
	const auto seen = selecting();
	menu.step(tables, seen, square, slots);
	menu.step(tables, seen, r1, slots);
	check(menu.focus() == 2);
	menu.step(tables, seen, l1, slots);
	check(menu.focus() == 0);
	menu.step(tables, seen, r1, slots);
	const auto chosen = menu.step(tables, seen, cross, slots);
	check(chosen.picked && chosen.picked->slot == 0 && chosen.word == square);
	menu.step(tables, seen, 0, slots);
	slots[0].reset();
	menu.step(tables, seen, square, slots);
	check(menu.focus() == 0);
}
