#include "pbd_test.h"

#include "Emu/PBD/pbd_practice_presentation.h"
#include "Emu/Cell/pbd_practice_controls.h"
#include "Emu/Cell/pbd_practice_mouse.h"
#include "Emu/PBD/pbd_movement_observation.h"
#include "Emu/Cell/pbd_game_overlay.h"
#include "Emu/PBD/pbd_practice_frames.h"
#include "Emu/PBD/pbd_practice_menu.h"
#include "Emu/PBD/pbd_practice_move_list.h"
#include "Emu/PBD/pbd_practice_match_prompt.h"
#include "Emu/PBD/pbd_practice_start.h"
#include "Emu/PBD/pbd_practice_reference.h"
#include "Emu/PBD/pbd_practice_wakeup.h"
#include "Emu/PBD/pbd_move_readiness.h"

#include <algorithm>
#include <atomic>
#include <map>
#include <limits>
#include <string_view>
#include <thread>
#include <vector>

namespace
{
#include "practice_menu_pointer_checks.h"

#include "practice_menu_settings_checks.h"

#include "practice_menu_reference_checks.h"

} // namespace

PBD_TEST(PbdPractice, PointerChecks)
{
	using namespace pbd::practice::mouse;
	layout view;
	view.owner = {1, 0, 1, 1};
	view.active = true;
	view.row_count = 1;
	view.page_count = 1;
	view.rows[0] = {204, 251, 872, 36, 1};
	view.pages[0] = {494, 168, 286, 32, 1};
	pointer_identity_changes(view);
	view.rows[0].value = 8;
	publish(view);
	pointer(600, 260, 1280, 720, true);
	const auto scrolled = take(view.owner);
	check(scrolled && scrolled->value == 8, "scrolling must retain the logical setting rather than reusing the visible row number");
	wheel(false);
	const auto scroll = take(view.owner);
	check(scroll && scroll->kind == action::scroll && scroll->value == 1, "wheel must stay a scoped menu action");
	back();
	const auto previous = take(view.owner);
	check(previous && previous->kind == action::back, "right-click uses the menu's Back action");
	back();
	++view.owner.revision;
	publish(view);
	check(!take(view.owner), "a right-click cannot back out of a replacement decision");
	check(!pointer(-1, 260, 1280, 720, true) && !pointer(600, 260, 0, 720, true) &&
			  !pointer(std::numeric_limits<float>::quiet_NaN(), 260, 1280, 720, true) && !take(view.owner),
		"invalid pointer positions cannot activate a row");
	pointer(600, 260, 1280, 720, true);
	captured_buttons.store(true);
	publish({});
	check(!take(view.owner) && !pointer(600, 260, 1280, 720, true), "closing the menu clears pending actions and all hit targets");
	back();
	check(!take(view.owner), "right-click on a closed menu cannot become a game input");
	check(captured_buttons.load(), "a closing click must still own its held button until Qt receives release");
	captured_buttons.store(false);

	auto menu = pointer_rail_layout();
	pointer_control_bar(menu);
	pointer_frequency_picker(menu);
	pointer_dropdown(menu);
	pointer_question(menu);
}

PBD_TEST(PbdPractice, SettingsChecks)
{
	using namespace pbd::practice;
	namespace category = pbd::practice::categories;
	// Every settings category ends with Restore Defaults. Practice Settings starts with Training Mode, then the mode's rows.
	check(practice_rows::mode == 0 && practice_rows::first == 1 && practice_rows::wakeup == practice_rows::first + practice_rows::game_options &&
			  practice_rows::throw_escape == practice_rows::wakeup + 1 && practice_rows::movement == practice_rows::throw_escape + 1 &&
			  practice_rows::wavedash_speed == practice_rows::movement + 1 && practice_rows::after_contact == practice_rows::wavedash_speed + 1 &&
			  practice_rows::counted == practice_rows::after_contact + 1 &&
			  practice_rows::times == practice_rows::counted + 1 && practice_rows::input_delay == practice_rows::times + 1 &&
			  practice_rows::restore == practice_rows::input_delay + 1 && practice_rows::count == practice_rows::restore + 1,
		"Offense lists Training Mode, the game's five options, Wake-up, Throw Escape, Movement, Wavedash Speed, Action After a Hit or Block, Count, After, Input Delay Frames, then Restore Defaults");
	movement_and_reaction_options();
	training_modes_and_navigation();
	display_defaults_and_confirmation();
	selection_gates_and_loading();
	packed_game_options_and_prompts();
	dummy_action_shortcuts();
}

PBD_TEST(PbdPractice, DropdownChecks)
{
	using namespace pbd::practice;
	check(dropdown_top(250.f, 0, 2) == 249.f && dropdown_top(250.f, 1, 2) == 219.f, "the option in effect must line up with its row");
	check(dropdown_top(160.f, 5, 6) == 153.f, "a stack that would rise above the list area must be pushed down into it");
	check(dropdown_top(580.f, 0, 2) == 548.f, "a stack that would pass the panel's bottom edge must be pushed up");
	check(dropdown_top(160.f, 0, 11) == 159.f && dropdown_top(400.f, 0, 11) == 278.f, "an eleven-option list must fit inside the panel");
	// The longest list, Wake-up's fifteen choices with Random last, fills the list area (y 153 to 608) and no more; the pad
	// and the pointer reach only the options a list shows.
	check(pbd::practice::pbd_wakeup_option_count <= max_options && max_options == 15, "every Wake-up choice fits the option list");
	check(dropdown_top(160.f, 0, 15) == 158.f && dropdown_top(400.f, 14, 15) == 153.f && 153.f + 30.f * static_cast<float>(max_options) <= 608.f,
		"a fifteen-option list must fit inside the panel");
	check(pbd::practice::mouse::layout{}.options.size() >= max_options, "every option a list shows must be a pointer target");
	check(step_frequency(1, false) == 0 && step_frequency(1, true) == 2 && step_frequency(10, true) == 10 && step_frequency(10, false) == 9,
		"frequency must step by one inside 0..10");
	check(step_frequency(0, false) == 0 && step_frequency(0, true) == 1 && min_frequency == 0,
		"Frequency 0 is the bottom: the action stays recorded and is never chosen");
	check(step_frequency(42, false) == 9 && step_frequency(42, true) == 10, "an out-of-range weight must be clamped before stepping");
	// Every recorded action at 0 is reported; an empty slot's 0 means nothing, and one action above 0 is enough to play.
	{
		const std::array<bool, 8> two{true, false, true, false, false, false, false, false}, none{};
		check(every_action_at_zero(two, {0, 7, 0, 0, 0, 0, 0, 0}) && !every_action_at_zero(two, {0, 0, 3, 0, 0, 0, 0, 0}) &&
				  !every_action_at_zero(none, {}),
			"playback says so only when actions are recorded and all are at Frequency 0");
		// The weighted pick never chooses a slot at 0: the percentages give it nothing, and the others share 100.
		const auto shares = pbd::practice::controls::percentages({0, 0, 3, 0, 0, 0, 1, 0});
		check(shares[0] == 0 && shares[2] == 75 && shares[6] == 25, "a Frequency 0 action has no share");
		check(!pbd::practice::controls::playback_available({0, 0, 0, 0, 0, 0, 0, 0}, 2, 0) &&
				  pbd::practice::controls::playback_available({0, 0, 1, 0, 0, 0, 0, 0}, 2, 0),
			"with every weight 0 nothing is available to play");
	}
	{
		pbd::game_overlay::row slot_row;
		check(!slot_row.frequency, "a row without a slot shows no frequency");
		slot_row.frequency = 0u;
		check(slot_row.frequency && *slot_row.frequency == 0, "a recorded action at Frequency 0 still shows its picker");
	}
	check(wrap_step(0, 4, false) == 3 && wrap_step(3, 4, true) == 0 && wrap_step(1, 4, true) == 2, "menu highlights must wrap at both ends");
	check(wrap_step(7, 4, true) == 0 && wrap_step(5, 0, true) == 0, "a stale highlight or an empty list must stay in range");
}

PBD_TEST(PbdPractice, RailChecks)
{
	using namespace pbd::practice;
	constexpr std::uint16_t up = 0x1000, down = 0x4000, left = 0x8000, right = 0x2000, cross = 0x0040, circle = 0x0020, l1 = 0x0004, r1 = 0x0008;
	column_focus focus;
	check(focus.rail && focus.page == 0 && focus.row == 0, "the pause menu must open with the category rail focused");
	focus.row = 5;
	check(rail_step(focus, down, 3) == rail_action::moved && focus.page == 1 && focus.row == 0 && focus.rail,
		"Down on the rail must show the next category's list from its first row and keep the rail focused");
	check(rail_step(focus, up, 3) == rail_action::moved && focus.page == 0 && rail_step(focus, up, 3) == rail_action::moved && focus.page == 2,
		"Up on the rail must wrap from the first category to the last");
	check(rail_step(focus, l1, 3) == rail_action::none && rail_step(focus, r1, 3) == rail_action::none && focus.page == 2 && focus.rail,
		"L1/R1 must no longer change category");
	check(rail_step(focus, left, 3) == rail_action::none && focus.rail, "Left on the rail has nowhere to go");
	check(rail_step(focus, right, 3) == rail_action::entered && !focus.rail && focus.page == 2, "Right must move focus into the shown list");
	check(rail_step(focus, down, 3) == rail_action::none && focus.page == 2, "rail steps must not act while the list has focus");
	focus.rail = true;
	check(rail_step(focus, cross, 3) == rail_action::entered && !focus.rail, "Cross on the rail must also move focus into the list");
	focus.rail = true;
	check(rail_step(focus, circle, 3) == rail_action::resume && focus.rail, "Circle on the rail must resume the fight");

	// The categories (the dummy recording setup lives in Practice Settings' Defense mode).
	namespace category = pbd::practice::categories;
	check(category::count == 8 && category::count <= max_pages && category::count <= pbd::practice::mouse::layout{}.pages.size(),
		"every rail category must be drawn and answer the pointer");
	check(category::restart_settings == 1 && category::display_settings == 2, "Display Settings follows Restart Settings");
	check(category::practice_settings == 0 && category::exit_practice == category::count - 1 &&
			  category::move_list > category::display_settings && category::character_selection == category::move_list + 1 &&
			  category::stage_selection == category::character_selection + 1 && category::settings == category::stage_selection + 1,
		"the rail must keep its order, with Settings just before Exit Practice");
	column_focus rail{true, category::display_settings, 0};
	check(rail_step(rail, down, category::count, true) == rail_action::moved && rail.page == category::move_list,
		"Down must reach Move List");
	check(rail_step(rail, right, category::count) == rail_action::entered && !rail.rail && rail.page == category::move_list,
		"Right enters Move List as it enters every other category");
	rail.rail = true;
	check(rail_step(rail, cross, category::count) == rail_action::entered && !rail.rail, "Cross also enters Move List");
	rail.rail = true;
	check(rail_step(rail, circle, category::count) == rail_action::resume, "Circle on Move List must still resume");
	check(rail_step(rail, down, category::count) == rail_action::moved && rail.page == category::character_selection,
		"Down must pass Move List to Character Selection");
	rail.page = category::exit_practice;
	check(rail_step(rail, down, category::count) == rail_action::moved && rail.page == category::practice_settings,
		"Down on Exit Practice must wrap to Practice Settings");
	check(pbd::practice::restart_rows::restart == 0 && pbd::practice::restart_rows::position == 1 &&
			  pbd::practice::restart_rows::remember == 2 && pbd::practice::restart_rows::restore == 5 && pbd::practice::restart_rows::count == 6,
		"Restart Settings lists Restart, Restart Position, Remember Position, the two restart choices and Restore Defaults");
	check(list_back(circle) && list_back(left) && !list_back(right) && !list_back(cross) && !list_back(static_cast<std::uint16_t>(up | down)) && !list_back(static_cast<std::uint16_t>(l1 | r1)),
		"Circle or Left in the list must return focus to the rail, and nothing else may");
}

PBD_TEST(PbdPractice, PromptChecks)
{
	using namespace pbd::practice;
	prompt_bar bar;
	bar.add("{cross}", "Confirm", prompt_action::confirm);
	bar.add("{circle}/{start}", "Resume", prompt_action::resume);
	bar.add("{dpad:udr}", "Move");
	check(bar.count == 3 && bar.items[2].action == prompt_action::none, "a group without an action stays plain");
	check(bar.offers(prompt_action::confirm) && bar.offers(prompt_action::resume) && !bar.offers(prompt_action::back) &&
			  !bar.offers(prompt_action::frequency),
		"the bar must report only the actions it shows");
	check(!bar.offers(prompt_action::none), "a plain group must never count as clickable");
	for (int i = 0; i < 10; ++i)
		bar.add("{start}", "Extra", prompt_action::back);
	check(bar.count == max_prompts && bar.offers(prompt_action::back), "groups past the bar's capacity are dropped");
	check((static_cast<std::uint32_t>(prompt_action::frequency) | prompt_second_key) >> 8 == 1 &&
			  ((static_cast<std::uint32_t>(prompt_action::frequency) | prompt_second_key) & 0xff) == static_cast<std::uint32_t>(prompt_action::frequency),
		"the second-key flag must not collide with an action value");
}

PBD_TEST(PbdPractice, ViewChecks)
{
	pbd::practice::snapshot original;
	original.active = true;
	original.toast = "Situation saved";
	original.fighters[0].damage = 12;
	pbd::practice::publish(original);
	original.toast.clear();
	original.fighters[0].damage = 99;
	auto loaded = pbd::practice::load();
	check(loaded.toast == "Situation saved" && loaded.fighters[0].damage == 12u, "publisher must copy caller-owned data");
	loaded.toast.clear();
	check(pbd::practice::load().toast == "Situation saved", "loaded view must not mutate shared data");
	{
		// Attack Info and Frame Data: only established values; every other line reads "---".
		const auto attack = pbd::practice::attack_info_text(loaded.fighters[0]);
		check(attack[0].value == "---" && attack[1].value == "---" &&
				  attack[2].value == "---" && attack[3].value == "12" &&
				  attack[4].value == "---" && attack[5].value == "---",
			"verified damage must not fabricate the unread Attack Info lines");
		const auto frames = pbd::practice::frame_data_text(loaded.fighters[0]);
		check(frames[0].value == "---" && frames[1].value == "---",
			"Absent frame observations leave both lines unavailable");
		pbd::practice::feedback verified;
		verified.damage = 0;
		verified.total_damage = 0;
		verified.hits = 0;
		verified.combo_damage = 35;
		verified.startup = 10;
		verified.on_block = -7;
		verified.outcome = pbd::practice::contact::block;
		const auto zero = pbd::practice::attack_info_text(verified);
		check(zero[0].value == "0" && zero[1].value == "35" && zero[3].value == "0" && zero[4].value == "0" &&
				  pbd::practice::frame_data_text(verified)[1].value == "-7",
			"a verified zero and a signed advantage must be distinct from ---");
		check(zero[2].value == "---", "Hit Properties stays --- until a contact's level is read");
		verified.hit_level = pbd::practice::frames::hit_level_text(0x041f);
		const auto level = pbd::practice::attack_info_text(verified);
		check(level[2].value == verified.hit_level,
			"a read level shows by name, with a shorter label for a wide value");
		verified.hit_level = {};
		verified.on_block = 3;
		check(pbd::practice::frame_data_text(verified)[1].value == "+3", "a positive recovery shows its sign");
		verified.on_block.reset();
		verified.on_hit = 0;
		check(pbd::practice::frame_data_text(verified)[0].value == "10" && pbd::practice::frame_data_text(verified)[1].value == "0",
			"move startup and an observed even hit retain their separate units");
		verified.on_hit.reset();
		verified.on_whiff = -17;
		check(pbd::practice::frame_data_text(verified)[1].value == "-17", "a whiff shows its recovery relative to the opponent");
		verified.on_whiff.reset();
		verified.result_label = "Launch";
		check(pbd::practice::frame_data_text(verified)[1].value == "Launch" && pbd::practice::frame_data_text(verified)[0].value == "10",
			"a contact without a plain advantage shows its label, and the startup stays");
		verified.on_hit = 5;
		check(pbd::practice::frame_data_text(verified)[1].value == "+5", "a number wins over a label");
		verified.on_hit.reset();
		verified.result_label = {};
		check(pbd::practice::snapshot{}.attack_info && pbd::practice::snapshot{}.frame_data && !pbd::practice::snapshot{}.fight_live,
			"both panels are on by default, and nothing shows before a fight boundary has been read");
		check(pbd::practice::practice_fight(10, 17, 17), "Practice's fight is pending phase 17 on scene 17");
		check(!pbd::practice::practice_fight(10, 17, 5), "VS's fight phase 5 is not Practice's");
		check(!pbd::practice::practice_fight(10, 12, 17), "the loading scene already reads phase 17 before the arena exists");
		check(!pbd::practice::practice_fight(4, 17, 17) && !pbd::practice::practice_fight(10, 17, 37),
			"another mode, or any other phase (a K.O.), is not the Practice fight");
	}
	{
		// Hits and Combo Damage follow the latest combo, as the game's own counter does: a player's 3-hit, 22-damage combo
		// reads 3 and 22 however many hits came before it.
		using pbd::practice::combo_tracker;
		const auto feed = [](combo_tracker& combo, std::initializer_list<std::uint32_t> dealt, std::optional<bool> rest)
		{
			for (const auto each : dealt)
				combo.frame(each, rest);
		};
		combo_tracker combo;
		for (int earlier = 0; earlier < 147; ++earlier)
		{
			combo.frame(8, std::nullopt);
			for (int f = 0; f < 50; ++f)
				combo.frame(0, std::nullopt);
		}
		check(!combo.active && combo.hits == 1, "147 jabs spaced out are 147 one-hit combos, not a 147-hit count");
		feed(combo, {10, 0, 0, 0, 0, 0, 0, 0, 0, 0, 7, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 5}, false);
		check(combo.active && combo.hits == 3 && combo.damage == 22, "three hits before the dummy recovers are one 3-hit, 22-damage combo");
		feed(combo, {0, 0, 0, 0}, true);
		check(!combo.active && combo.hits == 3 && combo.damage == 22, "back at rest the combo ends, and its values stay to read");
		combo.frame(9, false);
		check(combo.active && combo.hits == 1 && combo.damage == 9, "the next hit starts the count again");
		// A rest reading inside the input delay is too fresh to end a combo; without a reading a long pause ends it.
		combo_tracker fresh;
		fresh.frame(10, false);
		fresh.frame(0, true);
		check(fresh.active, "a rest reading on the frame after a hit does not end the combo");
		combo_tracker juggle;
		juggle.frame(12, std::nullopt);
		for (std::uint32_t f = 0; f < pbd::practice::combo_gap_frames; ++f)
			juggle.frame(0, std::nullopt);
		juggle.frame(15, std::nullopt);
		check(juggle.active && juggle.hits == 2 && juggle.damage == 27, "without a reading, a juggle's wait for its next hit stays one combo");
		for (std::uint32_t f = 0; f <= pbd::practice::combo_gap_frames; ++f)
			juggle.frame(0, std::nullopt);
		check(!juggle.active, "and a longer pause ends it");
		juggle.reset();
		check(!juggle.active && juggle.hits == 0 && juggle.damage == 0, "a restart clears it");
		combo_tracker stuck;
		stuck.frame(10, false);
		for (std::uint32_t f = 0; f < pbd::practice::combo_cap_frames; ++f)
			stuck.frame(0, false);
		check(stuck.active, "a dummy still down keeps its combo open for the cap");
		stuck.frame(0, false);
		stuck.frame(6, false);
		check(stuck.active && stuck.hits == 1 && stuck.damage == 6, "a reading stuck away from rest ends the combo at the cap");
	}
	{
		// The panels' layout: side by side along the bottom, clear of the game's top HUD and combo counter on either side, of
		// a shown command history column and of the control bar, inside the canvas, and never meeting the other side's.
		using namespace pbd::practice;
		for (const bool history : {false, true})
			for (const bool info : {false, true})
				for (const bool data : {false, true})
				{
					std::vector<panel_rect> all;
					for (std::size_t side = 0; side < 2; ++side)
					{
						const auto placed = place_panels(side, history, info, data);
						check(placed.attack_info.has_value() == info && placed.frame_data.has_value() == data, "a panel shows only when it is on");
						const panel_rect hud = side ? mirrored(game_top_hud) : game_top_hud, combo = side ? mirrored(game_combo_counter) : game_combo_counter;
						const panel_rect column = side ? mirrored(history_column) : history_column;
						for (const auto& rect : {placed.attack_info, placed.frame_data})
						{
							if (!rect)
								continue;
							check(!rect->overlaps(hud) && !rect->overlaps(combo) && (!history || !rect->overlaps(column)),
								"no panel covers the game's HUD or a shown command history");
							check(!rect->overlaps(game_capture_prompt) && !rect->overlaps(mirrored(game_capture_prompt)),
								"no panel covers the game's COMMAND CAPTURE prompt, and both sides stay level");
							check(rect->x >= 0.f && rect->x + rect->w <= 1280.f && rect->y >= 0.f && rect->y + rect->h <= control_bar_top,
								"every panel stays on the canvas, above the control bar");
							check(side ? rect->x >= 640.f : rect->x + rect->w <= 640.f, "each side keeps to its half");
							all.push_back(*rect);
						}
						if (placed.attack_info && placed.frame_data)
							check(side ? placed.frame_data->x + placed.frame_data->w <= placed.attack_info->x :
										 placed.attack_info->x + placed.attack_info->w <= placed.frame_data->x,
								"Attack Info sits nearest the screen edge");
					}
					for (std::size_t a = 0; a < all.size(); ++a)
						for (std::size_t b = a + 1; b < all.size(); ++b)
							check(!all[a].overlaps(all[b]), "no two panels overlap");
				}
		const auto p1 = place_panels(0, true, true, true);
		check(p1.attack_info->x == 176.f && p1.attack_info->y == 472.f && p1.frame_data->y + p1.frame_data->h == panels_bottom &&
				  p1.attack_info->y + p1.attack_info->h == panels_bottom,
			"P1's panels: right of its history, along the bottom; Frame Data is two rows tall");
		for (const bool history : {false, true})
			for (std::size_t side = 0; side < 2; ++side)
			{
				const auto rect = *place_panels(side, history, false, true, replay_panels_bottom).frame_data;
				const panel_rect wins{0.f, 550.f, 1280.f, 62.f};
				check(!rect.overlaps(wins) && !rect.overlaps(side ? mirrored(game_combo_counter) : game_combo_counter),
					"replay frame data sits between the combo counter and WINS streak on both sides");
				check(!history || !rect.overlaps(side ? mirrored(history_column) : history_column),
					"raising replay frame data keeps it clear of command history");
			}
	}
	pbd::practice::publish({});
	std::atomic<bool> done{false}, coherent{true};
	std::thread writer([&]
		{
			for (std::uint32_t generation = 1; generation <= 1000; ++generation)
			{
				pbd::practice::snapshot value;
				value.active = true;
				value.menu_generation = generation;
				value.section = std::to_string(generation);
				value.fighters[0].damage = generation;
				value.fighters[1].damage = generation;
				pbd::practice::publish(value);
			}
			done.store(true);
		});
	do
	{
		const auto value = pbd::practice::load();
		if (value.active && (value.section != std::to_string(value.menu_generation) ||
								value.fighters[0].damage != value.menu_generation || value.fighters[1].damage != value.menu_generation))
			coherent.store(false);
	} while (!done.load());
	writer.join();
	check(coherent.load(), "reader must receive one coherent generation during concurrent publication");
	pbd::practice::publish({});
}

PBD_TEST(PbdPractice, CalculatedReference)
{
	using namespace pbd::practice::reference;
	calculated_frame_formulas();
	std::map<std::uint32_t, std::uint32_t> words;
	constexpr std::uint32_t bank = 0x20000000, moves = 0x21000000, commands = 0x22000000,
							reactions = 0x23000000, conditions = 0x24000000, requirements = 0x25000000, extra = 0x26000000;
	words[bank + 0x1e0] = moves;
	words[bank + 0x1e4] = 3;
	words[bank + 0x1b0] = commands;
	words[bank + 0x1b4] = 3;
	words[bank + 0x188] = reactions;
	words[bank + 0x18c] = 1;
	words[bank + 0x198] = conditions;
	words[bank + 0x19c] = 1;
	words[bank + 0x190] = requirements;
	words[bank + 0x194] = 1;
	words[bank + 0x1c0] = extra;
	words[bank + 0x1c4] = 1;
	words[extra] = 0x80;
	words[requirements] = 327u << 16;
	words[conditions] = requirements;
	words[conditions + 8] = reactions;
	words[reactions + 0x34] = 2u << 16;
	words[reactions + 0x48] = 1u << 16;
	for (std::uint32_t side = 0; side < 3; ++side)
	{
		const auto allocation = moves + side * 0x4c, command = commands + side * 24;
		words[allocation + 0x10] = side ? 0u : 1u;
		words[allocation + 0x14] = command;
		words[allocation + 0x20] = conditions;
		words[allocation + 0x24] = 60u << 16;
		words[allocation + 0x30] = 0;
		words[allocation + 0x44] = (10u << 16) | 11u;
		words[command] = 0x80000000;
		words[command + 8] = 0x80010000;
		words[command + 12] = extra;
		words[command + 20] = 0x50;
		words[command + 16] = ((side == 0 ? 30u : side == 1 ? 20u :
															  40u)
								  << 16) |
		                      255u;
	}
	const auto word = [&](std::uint32_t at)
	{
		const auto found = words.find(at);
		return found == words.end() ? ~0u : found->second;
	};
	const auto half = [&](std::uint32_t at)
	{
		return (word(at & ~3u) >> ((at & 2) ? 0 : 16)) & 65535u;
	};
	const auto decoded = pbd::move_list::read_timing(bank, moves, word, half);
	check(decoded.startup == 10u && decoded.active == 2u && decoded.recovery == 19u && decoded.on_block == 0 && decoded.on_hit == 20,
		"bounded native tables establish the reference without a published-number lookup");
	words[moves + 0x10] = 0;
	const auto changed_level = pbd::move_list::read_timing(bank, moves, word, half);
	check(changed_level.startup == decoded.startup && changed_level.on_block == decoded.on_block && changed_level.on_hit == decoded.on_hit,
		"an attack window still establishes timing when a property supplies the hit level later");
	words[moves + 0x44] = 0;
	check(!pbd::move_list::read_timing(bank, moves, word, half).startup,
		"a move without an attack window cannot acquire startup from a hit level");
	words[moves + 0x44] = (10u << 16) | 11u;
	words[moves + 0x10] = 1;
	words[requirements] = 326u << 16;
	check(!pbd::move_list::read_timing(bank, moves, word, half).on_block, "conditional hit records never become an unconditional block reference");
	words[requirements] = 327u << 16;
	words[reactions + 0x34] = 0x80010000;
	check(!pbd::move_list::read_timing(bank, moves, word, half).on_hit && pbd::move_list::read_timing(bank, moves, word, half).on_block == 0,
		"reaction aliases stay unavailable independently of the valid block branch");
	words[commands] = 0;
	const auto automatic = pbd::move_list::read_timing(bank, moves, word, half);
	check(automatic.startup == 10u && !automatic.recovery && !automatic.on_block,
		"an automatic transition before the terminal can alter timing and never becomes fixed recovery");
	words[bank + 0x1e4] = ~0u;
	check(!pbd::move_list::read_timing(bank, moves, word, half).startup, "an oversized allocation bank cannot establish reference data");
}

PBD_TEST(PbdPractice, MoveListCatalog)
{
	using namespace pbd::move_list;
	std::map<u32, u32> words;
	constexpr u32 bank = 0x20000000, rows = 0x21000000, mapping = 0x22000000,
				  records = 0x23000000, samples = 0x24000000, name = 0x25000000, notation = 0x26000000;
	words[bank + 0x218] = rows;
	words[bank + 0x21c] = 2;
	words[bank + 0x200] = mapping;
	words[bank + 0x204] = 3;
	words[bank + 0x208] = records;
	words[bank + 0x20c] = 1;
	words[bank + 0x210] = samples;
	words[bank + 0x214] = 1;
	words[rows + 44] = 0xffff0000;
	words[rows + 60 + 4] = name;
	words[rows + 60 + 16] = notation;
	words[rows + 60 + 44] = 0x00010001;
	words[rows + 60 + 52] = 2;
	words[rows + 60 + 56] = 3u << 16;
	words[name] = (u32{'J'} << 16) | 'a';
	words[name + 4] = u32{'b'} << 16;
	words[notation] = 0xe0150000;
	words[mapping] = 0;
	words[mapping + 4] = 0;
	words[records] = 1500;
	words[records + 4] = 0;
	words[records + 8] = 0;
	words[records + 12] = 1;
	words[records + 16] = samples;
	words[samples] = 0x00800301;
	words[samples + 4] = 0;
	const auto word = [&](u32 at)
	{
		const auto found = words.find(at);
		return found == words.end() ? ~0u : found->second;
	};
	const auto half = [&](u32 at)
	{
		return (word(at & ~3u) >> ((at & 2) ? 0 : 16)) & 65535u;
	};
	const auto list = read_catalog(bank, 9, word, half);
	check(list && list->character == 9 && list->bank == bank && list->entries.size() == 1 &&
			  list->entries[0].id == 1 && list->entries[0].name == u"Jab" && list->entries[0].notation == u"\ue015" &&
			  list->entries[0].command_id == 1 && list->entries[0].variants == 1 && list->entries[0].conditions == 2 && list->entries[0].execution == 3,
		"catalog ids retain their character and stock descriptor ordinal; empty headers are not choices");
	check(list->headers.empty(), "a row without commands and without a readable title is no category");
	catalog_category_titles(words, bank, rows, name);
	const auto command = read_command(bank, 1, word, half);
	check(command && command->record == 0 && command->samples.size() == 1 && command->samples[0][0] == 0x80 &&
			  command->samples[0][1] == 0x301,
		"the reader preserves stock command flags for the executor");
	check(!read_command(bank, 3, word, half) && !read_catalog(bank, 64, word, half), "out-of-range command and character ids are refused");
	words[records + 16] = samples + 2;
	check(!read_command(bank, 1, word, half) && !read_catalog(bank, 9, word, half), "a command outside an aligned sample-table entry cannot become a picker choice");
	words[records + 16] = samples;
	words[records + 12] = 4097;
	check(!read_command(bank, 1, word, half), "an oversized command cannot allocate an unbounded tape");
	words[bank + 0x21c] = 4097;
	check(!read_catalog(bank, 9, word, half), "an oversized descriptor list cannot allocate unbounded rows");
	words[bank + 0x21c] = 2;
	words[rows + 60 + 44] = 0x000100ff;
	check(!read_catalog(bank, 9, word, half), "every advertised variant stays inside the command map");
	words[rows + 60 + 44] = 0x00010001;
	words[name] = 0xd8000000;
	check(!read_catalog(bank, 9, word, half), "a broken surrogate is not a display label");
	words[name] = (u32{'J'} << 16) | 'a';
	const auto changed = [&](u32 at)
	{
		static unsigned reads = 0;
		return at == bank + 0x218 && ++reads > 1 ? rows + 60 : word(at);
	};
	check(!read_catalog(bank, 9, changed, half), "a replaced bank table cannot publish stale descriptor ids");
}

void preset_start_input_and_readback()
{
	using namespace pbd::practice::start;
	input_held = true;
	check(holds_port(0) && holds_port(1) && !holds_port(2), "startup holds both ports neutral during the direct resource load");
	input_held = false;
	check(!holds_port(0) && !holds_port(1), "normal controller ownership resumes after startup releases");
	watch sample{{{26, 25}, 23}};
	snapshot now{10, 3, 44, 23, 0, {26, 25}, false};
	check(sample.step(now) == progress::waiting, "a selector is not a verified Practice fight");
	now.scene = now.pending = 17;
	now.models_ready = true;
	for (std::uint32_t counter = 0; counter < 3; ++counter)
	{
		now.counter = counter;
		check(sample.step(now) == progress::waiting, "all three advancing frames must be observed");
	}
	now.counter = 3;
	check(sample.step(now) == progress::ready, "matching live actors and three advancing frames admit the start");
	now.rows[1] = 0;
	check(sample.step(now) == progress::refused, "a wrong dummy cannot acknowledge a requested Practice fight");
	now.rows[1] = 25;
	now.mode = 4;
	check(sample.step(now) == progress::refused, "an online fight cannot acknowledge a Practice start");
}

PBD_TEST(PbdPractice, PresetStartReadback)
{
	using namespace pbd::practice::start;
	preset_lifetime_and_boundary_checks();
	const auto parsed = parse(R"({"schema":"pbd-practice-start-1","id":"one","characterId":"jin_kazama","characterRow":26,"costume":0,"stage":23})");
	check(parsed.value && parsed.value->character_row == 26u, "a bounded preset request carries a reviewed row");
	check(!parse(R"({"schema":"pbd-practice-start-1","id":"one","characterId":"jin_kazama","characterRow":85,"costume":0,"stage":23})").value,
		"an out-of-range resource row cannot reach the stock loader");
	const std::string jin = R"({"schema":"pbd-practice-start-1","id":"one",)"
							R"("characterId":"jin_kazama","characterRow":26,"costume":0,)";
	check(!parse(jin + R"("stage":14})").value, "a stage outside the supported pool is refused");
	check(parse(jin + R"("stage":0})").value.has_value(), "an original Tekken 5 stage is a Practice stage");
	pbd::hidden_selection::tables tables;
	tables.verified = true;
	tables.availability = 0x7ffffffffull;
	tables.grid[5].character = 9;
	for (const std::uint32_t row : {25u, 26u, 27u})
		tables.characters[row].character = 9;
	tables.costumes[9].rows = {25, 26, 27};
	tables.buttons[10] = 0;
	tables.buttons[11] = 1;
	tables.buttons[12] = 2;
	preset_costume_resolution(parsed, tables);
	preset_start_input_and_readback();
}

PBD_TEST(PbdPractice, MatchPromptOwnership)
{
	using namespace pbd::practice::match_prompt;
	pbd::game_overlay::snapshot panel;
	panel.kind = pbd::game_overlay::panel_kind::prompt;
	panel.row_count = 2;
	panel.timer = {true, 16000, 15000};
	check(interrupts(panel, true, false), "Practice prompt ownership and deadline policy");
	check(!interrupts(panel, false, false), "Practice prompt ownership and deadline policy");
	check(!interrupts(panel, true, true), "Practice prompt ownership and deadline policy");
	check(!expired(panel, 15999), "Practice prompt ownership and deadline policy");
	check(expired(panel, 16000), "Practice prompt ownership and deadline policy");
	check(expired(panel, 16001), "Practice prompt ownership and deadline policy");
	panel.practice = true;
	check(!interrupts(panel, true, false), "Practice prompt ownership and deadline policy");
	panel.practice = false;
	panel.kind = pbd::game_overlay::panel_kind::menu;
	check(!interrupts(panel, true, false), "Practice prompt ownership and deadline policy");
	panel.kind = pbd::game_overlay::panel_kind::prompt;
	panel.row_count = 3;
	check(!interrupts(panel, true, false), "Practice prompt ownership and deadline policy");
	panel.row_count = 2;
	panel.timer.total_ms = 60000;
	check(!interrupts(panel, true, false), "Practice prompt ownership and deadline policy");
	panel.timer.total_ms = 15000;
	panel.timer.present = false;
	check(!interrupts(panel, true, false), "Practice prompt ownership and deadline policy");
	check(!expired(panel, 16001), "Practice prompt ownership and deadline policy");
	// The press present when the card opens cannot answer it or reach either Practice port.
	const auto held = pbd::game_overlay::read_claim(true, 0x0040, true, false);
	check(held.claimed && !held.navigate, "Practice prompt ownership and deadline policy");
	check(pbd::game_overlay::blanks_executed_port(held.claimed, false, true), "Practice prompt ownership and deadline policy");
	const auto release = pbd::game_overlay::read_claim(held.ignore_until_release, 0, true, false);
	check(!release.navigate, "Practice prompt ownership and deadline policy");
	const auto answer = pbd::game_overlay::read_claim(release.ignore_until_release, 0x0040, true, false);
	check(answer.navigate && answer.claimed, "Practice prompt ownership and deadline policy");
	const auto closing = pbd::game_overlay::read_claim(true, 0x0040, false, false);
	check(closing.claimed && !closing.navigate, "Practice prompt ownership and deadline policy");
}

PBD_TEST(PbdPractice, FrameDataObservations)
{
	using namespace pbd::practice::frames;
	check(startup(10, 11, 30, true) == 10u && !startup(0, 0, 30, true) && !startup(11, 10, 30, true) && !startup(10, 31, 30, true) && !startup(10, 11, 65535, true),
		"only an ordered attack window inside the move supplies startup");
	check(!startup(10, 11, 30, false), "a nonattack animation cannot supply startup from unrelated window fields");
	tracker sample;
	auto advance = [&](tick from, tick to)
	{
		for (tick frame = from; frame <= to; ++frame)
			sample.boundary(frame);
	};
	observed_contact_endpoints(sample, advance);
	overlap_and_counter_gap(sample);
	sample.boundary(200);
	sample.commit(1, 40, 12);
	sample.applied_contact(1, 50, contact::hit);
	sample.boundary(201);
	sample.commit(0, 50, {});
	sample.boundary(201);
	check(sample.latest[1].impact == 200u && !sample.latest[1].on_hit, "a pause poll preserves the observation without inventing elapsed frames");
	sample.boundary(202);
	sample.actionable(0, 50);
	sample.actionable(1, 40);
	check(sample.latest[1].on_hit == 0 && !sample.latest[0].on_hit, "an even contact on the other side is a measured zero");
	reaction_binding_refusals(sample);
	sample.reset();
	sample.boundary(600);
	sample.commit(0, 60, 15);
	sample.commit(1, 70, {});
	sample.applied_contact(0, 0, contact::hit);
	sample.boundary(601);
	sample.commit(1, 80, {});
	sample.boundary(602);
	sample.actionable(0, 60);
	sample.reject_recovery(1, 80);
	sample.boundary(603);
	sample.actionable(1, 80);
	check(sample.latest[0].startup == 15u && !sample.latest[0].on_hit && !sample.latest[0].attacker_ready && !sample.latest[0].defender_ready,
		"a floor or unsupported recovery endpoint cannot publish advantage, while the move's startup remains known");
	sample.reset();
	sample.boundary(700);
	sample.commit(0, 60, 10);
	sample.commit(1, 70, {});
	sample.applied_contact(0, 80, contact::hit);
	sample.applied_contact(0, 80, contact::hit);
	sample.applied_contact(0, 80, contact::hit);
	sample.boundary(701);
	sample.commit(1, 80, {});
	sample.boundary(702);
	sample.actionable(0, 60);
	sample.actionable(1, 80);
	check(!sample.latest[0].on_hit, "a third contact cannot rearm a move whose overlapping contacts were rejected");
	sample.boundary(703);
	sample.commit(0, 61, 10);
	sample.applied_contact(0, 81, contact::hit);
	sample.boundary(704);
	sample.commit(1, 81, {});
	sample.boundary(705);
	sample.actionable(0, 61);
	sample.actionable(1, 81);
	check(sample.latest[0].on_hit == 0, "the next attack can establish a fresh observation after a rejected move");
}

PBD_TEST(PbdPractice, ObservedWhiffRecovery)
{
	using namespace pbd::practice::frames;
	tracker sample;
	auto advance = [&](tick from, tick to)
	{
		for (tick frame = from; frame <= to; ++frame)
			sample.boundary(frame);
	};
	sample.boundary(100);
	sample.commit(0, 10, 10, true, 10);
	advance(101, 125);
	sample.actionable(0, 10, 26);
	check(sample.latest[0].on_whiff == -17 && sample.latest[0].whiff_last_active == 109u && sample.latest[0].whiff_ready == 126u &&
			  !sample.latest[0].on_block && !sample.latest[0].on_hit,
		"whiff counts from the last active frame to the first stock action frame, independently of contact advantage");
	sample.boundary(126);
	sample.actionable(0, 10, 27);
	check(sample.latest[0].on_whiff == -17, "later idle animation callbacks cannot lengthen the first free frame");
	sample.boundary(127);
	sample.commit(0, 11, 13, true, 14);
	check(!sample.latest[0].on_whiff, "the next attack clears the preceding whiff result");
	advance(128, 160);
	sample.actionable(0, 11, 34);
	check(sample.latest[0].on_whiff == -21, "a two-frame active window starts recovery after its final active frame");
	sample.reset();
	sample.boundary(200);
	sample.commit(0, 20, 10, true, 10);
	advance(201, 210);
	sample.applied_contact(0, 30, contact::hit);
	sample.boundary(211);
	sample.commit(1, 30, {});
	advance(212, 225);
	sample.actionable(0, 20, 26);
	check(!sample.latest[0].on_whiff, "a hit is not a whiff even before hit advantage finishes measuring");
	sample.reset();
	sample.boundary(300);
	sample.commit(0, 40, 10, true, 10);
	advance(301, 325);
	sample.actionable(0, 40, 26);
	sample.applied_contact(0, 50, contact::block);
	check(!sample.latest[0].on_whiff, "a late contact in the same update removes a provisional whiff");
	sample.reset();
	sample.boundary(400);
	sample.commit(0, 60, 10, true, 10);
	advance(401, 410);
	sample.commit(0, 61, {});
	advance(411, 425);
	sample.actionable(0, 60, 26);
	check(!sample.latest[0].on_whiff, "an interrupted or cancelled move cannot borrow its former recovery window");
	whiff_clock_and_terminal_refusals(sample, advance);
}

PBD_TEST(PbdPractice, MovementTechniques)
{
	namespace movement = pbd::practice::movement;
	using movement::preset;
	// Offense's Movement Technique lists the techniques only, each option its preset.
	const auto list = pbd::practice::menu::technique_choices(preset::backdash, std::nullopt);
	const std::array<std::string_view, 5> names{"Neutral", "Wavedash", "Continuous Backdash", "Continuous Forward Dash", "Backdash"};
	check(list.options.size() == names.size() && movement::technique_count == names.size(), "five techniques");
	for (std::size_t i = 0; i < names.size(); ++i)
		check(list.options[i].text == names[i] && movement::preset_name(static_cast<preset>(i)) == names[i], "techniques in preset order");
	check(list.options[0].is_default && list.current == static_cast<std::uint32_t>(preset::backdash), "Neutral is the default and the option in effect is shown");
	for (const auto plain : {preset::sidestep_left, preset::sidestep_right, preset::sidewalk_left, preset::sidewalk_right, preset::crouch, preset::jump_back})
	{
		check(!movement::is_technique(plain), "a plain input is not a technique");
		check(pbd::practice::menu::technique_choices(plain, std::nullopt).current == 0, "a plain input saved before shows as Neutral");
	}
	check(!pbd::practice::menu::technique_choices(preset::none, 0x10u).options[1].enabled &&
			  pbd::practice::menu::technique_choices(preset::none, 0x24u).options[1].enabled,
		"Wavedash is for the Mishimas only");

	// Backdash as a technique backdashes again and again: one whole b, n, b, then neutral, each pass from rest.
	const auto backdash = movement::technique_sequence(preset::backdash);
	check(!backdash.once && backdash.rest_each_pass && movement::plays_alone(backdash) && !backdash.flow_at,
		"the technique plays on its own, each backdash waiting for the dummy at rest");
	check(backdash.frames.size() == movement::repeated_backdash_frames && backdash.frames[0] == movement::back && backdash.frames[1] == 0 &&
			  backdash.frames[2] == movement::back && backdash.frames[movement::backdash_frames - 1] == movement::back &&
			  backdash.frames[movement::backdash_frames] == 0 && backdash.frames.back() == 0,
		"b, n, b held, then neutral until the next");
	check(movement::generate(preset::backdash).once, "Defense's Backdash stays a one-shot before each action");
	check(movement::technique_sequence(preset::wavedash, 5).frames == movement::generate(preset::wavedash, 5).frames &&
			  movement::technique_sequence(preset::backdash_cancel).frames == movement::generate(preset::backdash_cancel).frames,
		"the other techniques play their own tapes");
	check(movement::technique_sequence(preset::crouch).empty(), "a plain input never plays as a technique");
}

PBD_TEST(PbdPractice, ReactionInputs)
{
	namespace movement = pbd::practice::movement;
	using movement::preset;
	using movement::reaction_input;
	// The plain inputs play once after the counted hit or block: a one-shot its motion, a held one for one second.
	for (const auto [input, kind] : std::array<std::pair<reaction_input, preset>, 6>{{{reaction_input::backdash, preset::backdash},
			 {reaction_input::sidestep_left, preset::sidestep_left}, {reaction_input::sidestep_right, preset::sidestep_right},
			 {reaction_input::sidewalk_left, preset::sidewalk_left}, {reaction_input::sidewalk_right, preset::sidewalk_right},
			 {reaction_input::crouch, preset::crouch}}})
	{
		const auto input_sequence = movement::reaction_sequence(input);
		check(input_sequence.frames == movement::generate(kind).frames && input_sequence.once == movement::generate(kind).once && input_sequence.sideways == movement::generate(kind).sideways,
			"an input that is also a movement plays that movement's tape");
	}
	check(movement::after_contact_wait(movement::reaction_sequence(reaction_input::crouch)) == 60 && movement::reaction_sequence(reaction_input::crouch).held,
		"Crouch holds down for one second");
	check(movement::after_contact_wait(movement::reaction_sequence(reaction_input::sidewalk_left)) == 60, "a sidewalk walks for a second");
	const auto duck = movement::reaction_sequence(reaction_input::fuzzy_duck);
	check(duck.once && duck.frames.size() == movement::fuzzy_duck_frames && movement::fuzzy_duck_frames == 8 &&
			  std::all_of(duck.frames.begin(), duck.frames.end(), [](std::uint16_t word)
				  {
					  return word == movement::down;
				  }) &&
			  movement::after_contact_wait(duck) == 8,
		"Fuzzy Duck is 8 frames of down, played once");
	// Played as the gap after a contact, the duck gives its 8 frames of down, then the gap ends on neutral.
	std::uint32_t position = 0, remaining = movement::after_contact_wait(duck), overrun = 0;
	std::vector<std::uint16_t> played;
	for (int frame = 0; frame < 20; ++frame)
	{
		const auto step = movement::gap_word(duck, position, remaining, overrun, movement::screen_side::right);
		if (step.done)
		{
			check(step.word == 0, "the duck ends on neutral");
			break;
		}
		played.push_back(step.word);
	}
	check(played == std::vector<std::uint16_t>(8, movement::pad_down), "down for 8 frames, then the dummy stands");
	check(pbd::practice::menu::shortened("Spinning Demon to Left Hook Combo", 26) == "Spinning Demon to Left..." &&
			  pbd::practice::menu::shortened("Jab", 26) == "Jab" && pbd::practice::menu::shortened("ab\xC3\xA9\xC3\xA9\xC3\xA9", 7) == "ab\xC3\xA9...",
		"a long move name is cut between characters and marked");
}

PBD_TEST(PbdPractice, MoveListNotation)
{
	using pbd::move_list::notation_text;
	check(notation_text(u"\ue015") == "1" && notation_text(u"\ue017") == "1+2" && notation_text(u"\ue023") == "1+2+3+4",
		"a button glyph is its buttons");
	check(notation_text(u"\ue002\ue015") == "d/f+1", "a direction straight before a button is one input");
	check(notation_text(u"\ue015,\ue016") == "1,2" && notation_text(u"\ue004,\ue004\ue016") == "f,f+2", "the label's own commas stay");
	check(notation_text(u"\ue00a\ue01c") == "D/F+4", "a held direction is in capitals");
	check(notation_text(u"\ue004\ue004") == "f f" && notation_text(u"\ue015\ue016") == "1 2", "other glyphs that touch are kept apart");
	check(notation_text(u" \ue001 , \ue015 ") == "d, 1" && notation_text(u"\ue010\ue015") == "1", "spaces are tidied and an unknown glyph has no text");
	check(notation_text(u"WS\ue016") == "WS 2" || notation_text(u"WS\ue016") == "WS2", "text before a button stays readable");

	// The same commands as the overlay's glyph tokens: arrows facing right, held ones marked, buttons as the lit cluster.
	using pbd::move_list::notation_glyphs;
	check(notation_glyphs(u"\ue002\ue015") == "{arrow_dr}+{cluster_1}", "d/f+1 is the down-forward arrow and button 1");
	check(notation_glyphs(u"\ue017") == "{cluster_3}" && notation_glyphs(u"\ue023") == "{cluster_15}", "a chord is one cluster");
	check(notation_glyphs(u"\ue00a\ue01c") == "{held:arrow_dr}+{cluster_8}", "a held direction is marked held");
	check(notation_glyphs(u"\ue004,\ue004\ue016") == "{arrow_r},{arrow_r}+{cluster_2}" && notation_glyphs(u"\ue004\ue004") == "{arrow_r} {arrow_r}",
		"commas stay and inputs that touch are kept apart");
	check(notation_glyphs(u"\ue000\ue001\ue003\ue005\ue006\ue007") == "{arrow_dl} {arrow_d} {arrow_l} {arrow_ul} {arrow_u} {arrow_ur}",
		"every direction has its arrow");
	check(notation_glyphs(u"SS{\ue015}") == "SS {cluster_1}" || notation_glyphs(u"SS{\ue015}") == "SS{cluster_1}", "the label's own braces are left out");
}

PBD_TEST(PbdPractice, MoveListNavigation)
{
	namespace view = pbd::practice::move_list_view;
	using row = view::row;
	// Two categories of moves, the third move one the dummy cannot do.
	std::vector<row> rows{{0, "Basic", "", true, false}, {10, "Jab", "1", false, true}, {11, "Right Jab", "2", false, true},
		{12, "Hellsweep", "f,n,d,d/f+4", false, false}, {0, "Throws", "", true, false}, {20, "Throw", "1+3", false, true}};
	view::state list;
	list.open_on(view::offense_target, rows, {});
	check(list.open && list.target == view::offense_target && list.focus == 1 && list.first == 0 && list.chosen.empty(), "the list opens on its first move");
	list.step(true);
	check(list.focus == 2, "Down moves to the next move");
	list.step(true);
	list.step(true);
	check(list.focus == 5, "category titles are passed over");
	list.step(true);
	check(list.focus == 1, "Down wraps to the first move");
	list.step(false);
	check(list.focus == 5, "Up wraps to the last move");
	list.category(false);
	check(list.focus == 1, "L1 from a category's first move goes to the one before");
	list.category(true);
	check(list.focus == 5, "R1 goes to the next category's first move");
	list.category(true);
	check(list.focus == 5, "past the last category R1 stays on the last move");
	list.focus = 3;
	list.category(false);
	check(list.focus == 1, "L1 inside a category goes to its first move");

	check(list.toggle() && list.on(10) && list.chosen == std::vector<std::uint32_t>{10}, "Cross turns a move on");
	list.focus = 5;
	check(list.toggle() && list.chosen == std::vector<std::uint32_t>{10, 20} && list.chosen_name().empty(), "several moves can be on");
	check(list.toggle() && list.chosen == std::vector<std::uint32_t>{10} && list.chosen_name() == "Jab", "and Cross turns one off again");
	list.focus = 3;
	check(!list.toggle() && list.chosen == std::vector<std::uint32_t>{10}, "a move the dummy cannot do stays off");

	view::state reopened;
	reopened.open_on(view::offense_target, rows, {20, 99, 20});
	check(reopened.chosen == std::vector<std::uint32_t>{20} && reopened.focus == 5,
		"reopening keeps the moves still in the list and highlights the first one on");
	reopened.open_on(view::offense_target, rows, {12, 20});
	check(reopened.focus == 3 && reopened.on(12));
	check(reopened.toggle() && reopened.chosen == std::vector<std::uint32_t>{20}, "a saved unavailable move can be removed");
	check(!reopened.toggle() && reopened.chosen == std::vector<std::uint32_t>{20}, "an unavailable move cannot be added again");

	// A long list keeps the highlighted move in view, with its category title when it is at the top.
	std::vector<row> many{{0, "Category", "", true, false}};
	for (std::uint32_t i = 0; i < 40; ++i)
		many.push_back({100 + i, "Move " + std::to_string(i), "1", false, true});
	view::state longer;
	longer.open_on(0, many, {});
	for (std::uint32_t i = 0; i < 20; ++i)
		longer.step(true);
	check(longer.focus == 21 && longer.first + view::visible_rows - 1 == 21, "moving down scrolls the list one row at a time");
	longer.page(true);
	check(longer.focus == 35 && longer.focus < longer.first + view::visible_rows, "Right moves a page and keeps the move in view");
	longer.page(true);
	check(longer.focus == 40, "a page stops at the last move");
	longer.page(false);
	longer.page(false);
	longer.page(false);
	check(longer.focus == 1 && longer.first == 0, "back at the top the category title shows again");
	for (std::uint32_t i = 40; i <= view::max_chosen; ++i)
		many.push_back({100 + i, "Move " + std::to_string(i), "1", false, true});
	longer.open_on(view::offense_target, std::move(many), {});
	for (std::uint32_t i = 0; i < view::max_chosen; ++i)
	{
		longer.focus = 1 + i;
		check(longer.toggle());
	}
	check(longer.chosen.size() == view::max_chosen);
	const auto full = longer.chosen;
	longer.focus = 1 + view::max_chosen;
	check(!longer.toggle() && longer.chosen == full, "a full selection refuses another distinct move");
	longer.focus = 1;
	check(longer.toggle() && !longer.on(100) && longer.chosen.size() == view::max_chosen - 1, "a full selection still allows removal");
	longer.focus = 1 + view::max_chosen;
	check(longer.toggle() && longer.on(100 + view::max_chosen) && longer.chosen.size() == view::max_chosen,
		"removal makes room for the previously refused move");
}

PBD_TEST(PbdPractice, RecordingSlotChoosesOneMove)
{
	namespace view = pbd::practice::move_list_view;
	const std::vector<view::row> rows{{0, "Basic", "", true, false}, {10, "Jab", "1", false, true},
		{11, "Right Jab", "2", false, true}, {12, "Unavailable", "", false, false}};
	for (std::uint32_t slot = 0; slot < view::offense_target; ++slot)
	{
		view::state list;
		list.open_on(slot, rows, {11, 10, 99});
		check(list.single_choice() && list.chosen == std::vector<std::uint32_t>{10} && list.focus == 1,
			"A recording slot opens with one available saved choice");
		list.focus = 2;
		check(list.toggle() && list.chosen == std::vector<std::uint32_t>{11} && list.chosen_name() == "Right Jab",
			"A new move replaces the recording slot's previous move");
		list.focus = 3;
		check(!list.toggle() && list.chosen == std::vector<std::uint32_t>{11}, "An unavailable move cannot replace a saved move");
		list.focus = 2;
		check(list.toggle() && list.chosen.empty(), "The recording slot's selected move can still be turned off");
	}
}

PBD_TEST(PbdPractice, MoveActionable)
{
	using pbd::move_list::actionable;
	std::map<std::uint32_t, std::uint32_t> words;
	constexpr std::uint32_t bank = 0x20000000, moves = 0x21000000, commands = 0x22000000, requirements = 0x25000000, extra = 0x26000000;
	words[bank + 0x1e0] = moves;
	words[bank + 0x1e4] = 16;
	words[bank + 0x1b0] = commands;
	words[bank + 0x1b4] = 64;
	words[bank + 0x190] = requirements;
	words[bank + 0x194] = 4;
	words[bank + 0x1c0] = extra;
	words[bank + 0x1c4] = 2;
	// Requirement lists end with id 327: [0, 327] always holds, [135/4, 327] is a game condition.
	words[requirements] = 0;
	words[requirements + 4] = 327u << 16;
	words[requirements + 8] = (135u << 16) | 4u;
	words[requirements + 12] = 327u << 16;
	words[extra] = 0x80;
	words[extra + 4] = 0x182;
	// Aliases 1 and 2 are the standing and crouching records; 0x20 is another name for standing, 0x11 a sidestep.
	words[bank + 0xac + 4] = 14;
	words[bank + 0xac + 8] = 15;
	words[bank + 0xac + 0x20 * 4] = 14;
	words[bank + 0xac + 0x11 * 4] = 5;
	std::uint32_t next = commands;
	const auto move = [&](std::uint32_t index, std::uint32_t length)
	{
		words[moves + index * 0x4c + 0x24] = length << 16;
		words[moves + index * 0x4c + 0x14] = next;
	};
	const auto command = [&](std::uint32_t input, std::uint32_t target, std::uint32_t first, std::uint32_t last, std::uint32_t option,
							 std::uint32_t requirement = 0, std::uint32_t data = 0)
	{
		// 0 picks the always-true requirement list and the plain extra data.
		words[next] = input;
		words[next + 4] = requirement ? requirement : requirements;
		words[next + 8] = target << 16;
		words[next + 12] = data ? data : extra;
		words[next + 16] = (first << 16) | last;
		words[next + 20] = option;
		next += 24;
	};
	const auto end = [&](std::uint32_t target, std::uint32_t first, std::uint32_t last = 255)
	{
		command(0x80000000, target, first, last, 0x50);
	};
	actionable_move_commands(move, command, end, requirements, extra);
	const auto word = [&](std::uint32_t at)
	{
		const auto found = words.find(at);
		return found == words.end() ? 0u : found->second;
	};
	const auto half = [&](std::uint32_t at)
	{
		return (word(at & ~3u) >> ((at & 2) ? 0 : 16)) & 65535u;
	};
	const auto at = [&](std::uint32_t index)
	{
		return actionable(bank, moves + index * 0x4c, word, half);
	};
	check(at(2) == 26u, "a standing end can act on its window's first frame; input commands never end a move on their own");
	check(at(3) == 54u, "a crouching end is neutral too, so lows and crouching moves have recovery");
	check(at(4) == 77u, "an empty end window ends the move on its animation's last frame");
	check(!at(5), "a floor end is a knockdown, not a frame the fighter can act on");
	check(at(6) == 40u, "an always-true automatic transition hands over to the next move, which starts at cursor 1 on the following frame");
	check(at(8) == 19u, "a conditional automatic transition is left out");
	check(at(9) == 26u, "an automatic transition with extra transition data is left out");
	check(at(10) == 30u && !at(11), "an alias counts as neutral only when it names the standing or crouching record");
	check(at(12) == 40u, "an end window after the animation's last frame ends at the last frame");
	check(!at(13), "a move that keeps chaining into moves gives no value");
	check(!actionable(bank, moves + 2 * 0x4c + 4, word, half) && !actionable(0, moves + 2 * 0x4c, word, half),
		"an address that is not a move record of a valid bank gives no value");
}

PBD_TEST(PbdPractice, ContactAdvantage)
{
	using namespace pbd::practice::frames;
	calculator sample;
	// Jin's jab on block, from a stock trace: committed on frame 39, contact applied on 48, the block reaction
	// committed on 49; the jab can act at cursor 26 and the reaction at cursor 19.
	sample.commit_reading(1, 900, 20, 1, free_on(1), false);
	sample.commit_reading(0, 100, 39, 1, free_on(26), true);
	check(sample.latest[0].move == 100u && !sample.latest[0].contacted && !sample.latest[0].advantage, "an attack starts with no result");
	sample.applied_contact(0, contact::block, 48);
	check(sample.latest[0].contacted && sample.latest[0].outcome == contact::block && !sample.latest[0].advantage,
		"the contact is known before the reaction starts");
	sample.commit_reading(1, 200, 49, 1, free_on(19), false);
	check(sample.latest[0].advantage == 3 && sample.latest[0].attacker_ready == 64 && sample.latest[0].defender_ready == 67 &&
			  sample.latest[0].reaction == 200u,
		"the reaction's first frame publishes block advantage: defender ready minus attacker ready");
	sample.commit_reading(1, 900, 68, 1, free_on(1), false);
	sample.commit_reading(0, 901, 70, 1, free_on(1), false);
	check(sample.latest[0].advantage == 3 && sample.latest[0].move == 100u, "the value stays until that side's next attack");
	sample.commit_reading(0, 100, 100, 1, free_on(26), true);
	check(!sample.latest[0].contacted && !sample.latest[0].advantage, "the next attack clears the previous result");
	sample.applied_contact(0, contact::hit, 109);
	sample.commit_reading(1, 201, 110, 1, free_on(25), false);
	check(sample.latest[0].advantage == 9 && sample.latest[0].outcome == contact::hit, "a hit reaction gives hit advantage");
	// A low on frame 13 of a move that recovers crouching at cursor 54, blocked crouching (reaction ends crouching at 19).
	sample.commit_reading(0, 101, 200, 1, free_on(54), true);
	sample.applied_contact(0, contact::block, 212);
	sample.commit_reading(1, 202, 213, 1, free_on(19), false);
	check(sample.latest[0].advantage == -22, "a crouching reaction against a crouching recovery is a number");
	sample.commit_reading(0, 102, 300, 1, free_on(46), true);
	sample.applied_contact(0, contact::hit, 314);
	sample.commit_reading(1, 203, 315, 1, {}, false);
	check(sample.latest[0].contacted && sample.latest[0].outcome == contact::hit && !sample.latest[0].advantage,
		"a reaction that ends on the floor leaves the value unavailable");
	sample.commit_reading(0, 103, 400, 1, free_on(26), true);
	sample.applied_contact(0, contact::unavailable, 409);
	sample.commit_reading(1, 204, 410, 1, free_on(25), false);
	check(sample.latest[0].contacted && !sample.latest[0].advantage, "a juggle or other unread contact leaves the value unavailable");
	// A two-move string: each hit gives its own value and the last one stays.
	sample.commit_reading(0, 104, 500, 1, free_on(30), true);
	sample.applied_contact(0, contact::block, 512);
	sample.commit_reading(1, 205, 513, 1, free_on(19), false);
	check(sample.latest[0].advantage == 2, "the string's first hit has its value");
	sample.commit_reading(0, 105, 520, 1, free_on(40), true);
	check(sample.latest[0].move == 105u && !sample.latest[0].advantage, "the follow-up starts a new result");
	sample.applied_contact(0, contact::block, 534);
	sample.commit_reading(1, 206, 535, 1, free_on(25), false);
	check(sample.latest[0].advantage == 0, "the last hit's value replaces it, and even is a number");
	// A second contact of the same move (a multi-hit move) recalculates from its own reaction.
	sample.applied_contact(0, contact::block, 538);
	sample.commit_reading(1, 207, 539, 1, free_on(22), false);
	check(sample.latest[0].advantage == 1 && sample.latest[0].reaction == 207u, "another hit of the same move takes over");
	sample.commit_reading(0, 106, 600, 1, free_on(26), true);
	sample.applied_contact(0, contact::block, 609);
	sample.commit_reading(1, 208, 612, 1, free_on(19), false);
	check(!sample.latest[0].advantage, "a commit too long after the contact is not the reaction");
	sample.commit_reading(0, 107, 700, 1, free_on(26), true);
	sample.applied_contact(0, contact::block, 709);
	sample.commit_reading(1, 209, 710, 3, free_on(21), false);
	check(sample.latest[0].advantage == 3, "a reaction entered at a later cursor counts from that cursor");
	sample.commit_reading(0, 108, 800, 1, free_on(26), true);
	sample.applied_contact(0, contact::block, 809);
	sample.commit_reading(0, 109, 809, 1, free_on(30), true);
	sample.commit_reading(1, 210, 810, 1, free_on(19), false);
	check(sample.latest[0].move == 109u && !sample.latest[0].contacted && !sample.latest[0].advantage,
		"an attack that starts before the reaction drops the earlier contact");
	sample.commit_reading(0, 110, 900, 1, free_on(26), true);
	sample.commit_reading(0, 111, 905, 1, free_on(20), false);
	sample.applied_contact(0, contact::block, 909);
	sample.commit_reading(1, 211, 910, 1, free_on(19), false);
	check(sample.latest[0].contacted && !sample.latest[0].advantage, "a contact from a move other than the shown attack has no number");
	sample.commit_reading(0, 112, 1000, 1, {}, true);
	sample.applied_contact(0, contact::block, 1009);
	sample.commit_reading(1, 212, 1010, 1, free_on(19), false);
	check(!sample.latest[0].advantage, "an attack without readable recovery has no number");
	sample.reset();
	sample.commit_reading(0, 950, 1100, 1, free_on(1), false);
	sample.commit_reading(1, 300, 1100, 1, free_on(26), true);
	sample.applied_contact(1, contact::hit, 1109);
	sample.commit_reading(0, 301, 1110, 1, free_on(25), false);
	check(sample.latest[1].advantage == 9 && !sample.latest[0].contacted && !sample.latest[0].move, "the dummy's attacks publish on its own side");
	sample.reset();
	check(!sample.latest[0].move && !sample.latest[1].advantage, "a restart clears both sides");
	// A cancel: blocked at -10 (attack free at cursor 40 = frame 139, reaction free at its cursor 18 = frame 129), then on
	// frame 115 the attacker enters a sway (a new move free at its own cursor 3 = frame 117) and the advantage follows: +12.
	sample.commit_reading(0, 120, 100, 1, free_on(40), true, 12);
	sample.applied_contact(0, contact::block, 111);
	sample.commit_reading(1, 220, 112, 1, free_on(18), false);
	check(sample.latest[0].advantage == -10 && sample.latest[0].attacker_ready == 139, "the move's own recovery first");
	sample.commit_reading(0, 121, 115, 1, free_on(3), false);
	check(sample.latest[0].advantage == 12 && sample.latest[0].attacker_ready == 117 && sample.latest[0].move == 120u,
		"a cancel before the first free frame recalculates from the new move, and the attack stays the one shown");
	sample.commit_reading(0, 122, 150, 1, free_on(1), false);
	check(sample.latest[0].advantage == 12, "a move after the first free frame is a new action and changes nothing");

	// Whiff: last active cursor 12 (frame 211), free at cursor 40 (frame 239): -29, as a last active frame that is also
	// the free one gives -1.
	sample.reset();
	sample.commit_reading(0, 130, 200, 1, free_on(40), true, 12);
	sample.boundary(210);
	check(!sample.latest[0].whiff, "nothing while the attack can still connect");
	sample.boundary(212);
	check(sample.latest[0].whiff == -29 && !sample.latest[0].contacted, "after the last active frame the whiff recovery shows");
	sample.commit_reading(0, 131, 215, 1, free_on(5), false);
	check(sample.latest[0].whiff == -9, "a cancel out of the whiff's recovery recalculates it too");

	// The dummy hit in the middle of its own attack: its attack shows no whiff and its reaction is not a cancel.
	sample.reset();
	sample.commit_reading(1, 140, 300, 1, free_on(40), true, 14);
	sample.commit_reading(0, 141, 302, 1, free_on(26), true, 10);
	sample.applied_contact(0, contact::hit, 305);
	sample.commit_reading(1, 240, 306, 1, free_on(30), false);
	sample.boundary(330);
	check(!sample.latest[1].whiff && !sample.latest[1].advantage && sample.latest[0].advantage,
		"an interrupted attack has no whiff, and the hit that interrupted it has its advantage");

	// A string (b+2,2: 15 then 17) or a two-hit move (b+1+4) keeps its first hit's startup; the last hit gives the result.
	sample.reset();
	sample.commit_reading(0, 150, 400, 1, free_on(30), true, 16, 15);
	check(sample.latest[0].startup == 15u, "the first hit's startup");
	sample.commit_reading(0, 151, 418, 1, free_on(35), true, 18, 17);
	check(sample.latest[0].startup == 15u && sample.latest[0].move == 151u, "a hit entered before the first can act again keeps the first startup");
	sample.applied_contact(0, contact::block, 435);
	sample.commit_reading(1, 250, 436, 1, free_on(20), false);
	check(sample.latest[0].advantage == 3, "the last hit's contact gives the result");
	sample.commit_reading(0, 152, 470, 1, free_on(30), true, 12, 12);
	check(sample.latest[0].startup == 12u && !sample.latest[0].advantage, "an attack after the first free frame is a new move");
}

PBD_TEST(PbdPractice, ContactLabels)
{
	using namespace pbd::practice::frames;
	// Move data from DR's banks: standing hit sDm_KAO30M (flags 0x842, ends standing), launch sDw_UP_01_ (0x7402, lands
	// 52/51), knockdown sDw_KAO00_ (0x7402, lands 38/37), sweep sDm_ASI00LD (0x7402, 25/24), a slow collapse
	// dm_hara07_s (0x7402, 80/81), the ground reaction sDm_2K3_Fy (0x6084), a wall reaction wDm_AirF_00 (0x1007402) and a
	// juggle in the air (Dw_AIR00_Png, 0x7402) or on the floor (dwDWN_00U, 0x6084).
	const auto hit = [](std::uint32_t flags, std::uint32_t landing = 0, std::uint32_t landed = 0, bool plain = false)
	{
		contact_reading at;
		at.attacker_level = 0x217;
		at.defender_flags = 0x1952;
		at.reaction_flags = flags;
		at.reaction_landing = landing;
		at.reaction_landed = landed;
		at.reaction_plain = plain;
		return at;
	};
	check(classify(contact::hit, hit(0x842, 0, 0, true)) == outcome_label::none, "a standing reaction has a plain number");
	check(classify(contact::hit, hit(0x7402, 52, 51)) == outcome_label::launch, "a long fall through the air is a launch");
	check(classify(contact::hit, hit(0x7402, 38, 37)) == outcome_label::knockdown, "a short fall is a knockdown");
	check(classify(contact::hit, hit(0x7402, 25, 24)) == outcome_label::knockdown, "a sweep is a knockdown");
	check(classify(contact::hit, hit(0x7402, 80, 81)) == outcome_label::knockdown, "a reaction that is not a plain fall is a knockdown");
	check(classify(contact::hit, hit(0x7402, 30, 29, true)) == outcome_label::none, "a pop-up that lands standing keeps its number");
	check(classify(contact::hit, hit(0x6084)) == outcome_label::knockdown, "a reaction lying on the floor is a knockdown");
	check(classify(contact::hit, hit(0x1007402, 255, 15)) == outcome_label::wall, "a wall reaction is Wall");
	check(classify(contact::block, hit(0x7402, 52, 51)) == outcome_label::none, "a block has no label");
	auto air = hit(0x7402, 38, 37);
	air.defender_flags = 0x7402;
	check(classify(contact::unavailable, air) == outcome_label::juggle, "a hit on a fighter in the air is a juggle");
	air.defender_flags = 0x6084;
	check(classify(contact::unavailable, air) == outcome_label::knockdown, "a hit on a fighter lying down keeps them down");
	auto thrown = hit(0x6084);
	thrown.attacker_level = 0xa00;
	check(classify(contact::hit, thrown) == outcome_label::throw_hit, "the thrower's half of a throw makes it a throw");
	check(classify(contact::hit, contact_reading{}) == outcome_label::none, "an unread reaction has no label");

	contact_label_publication();
}
