#include "Emu/PBD/pbd_controller_mapper.h"
#include "Emu/PBD/pbd_controller_display.h"
#include "Emu/Cell/pbd_practice_controls.h"
#include "pbd_test.h"
#include "rpcs3qt/pbd_raw_keyboard.h"

namespace
{
	using namespace pbd::controllers;
	using pbd::test::check;
	static void controller_claim_and_hysteresis(std::array<source, 3>& inputs)
	{
		selector select_device;
		select_device.sample(inputs, true, false);
		inputs[1].value.buttons = bit(cross);
		check(select_device.sample(inputs, true, false).buttons == bit(cross), "fresh pad press reaches the next complete report");
		check(select_device.owner() == "pad-a", "fresh press claims immediately");
		inputs[2].value.buttons = bit(circle);
		check(select_device.sample(inputs, true, false).buttons == bit(circle), "second pad replaces the first whole state");
		inputs[1].value.buttons = 0;
		check(select_device.sample(inputs, true, false).buttons == bit(circle) && select_device.owner() == "pad-b", "release never claims");
		inputs[0].value.buttons = bit(square);
		check(select_device.sample(inputs, true, true).buttons == bit(circle), "chat typing does not take the pad");
		select_device.sample(inputs, true, false);
		check(select_device.owner() == "pad-b", "holding a chat key after typing never claims");
		inputs[0].value.buttons = 0;
		select_device.sample(inputs, true, false);
		inputs[0].value.buttons = bit(square);
		check(select_device.sample(inputs, true, false).buttons == bit(square) && select_device.owner() == "keyboard", "fresh keyboard press claims");
		inputs[1].value.sticks[0] = .34f;
		select_device.sample(inputs, true, false);
		check(select_device.owner() == "keyboard", "stick noise below .35 never claims");
		inputs[1].value.sticks[0] = .36f;
		select_device.sample(inputs, true, false);
		check(select_device.owner() == "pad-a", "stick past .35 claims");
		inputs[2].value.buttons |= bit(start);
		select_device.sample(inputs, true, false);
		inputs[1].value.sticks[0] = .25f;
		select_device.sample(inputs, true, false);
		inputs[1].value.sticks[0] = .4f;
		select_device.sample(inputs, true, false);
		check(select_device.owner() == "pad-b", "stick stays armed until it falls below .20");
		inputs[1].value.sticks[0] = .19f;
		select_device.sample(inputs, true, false);
		inputs[1].value.sticks[0] = .36f;
		select_device.sample(inputs, true, false);
		check(select_device.owner() == "pad-a", "neutral below .20 rearms the stick");
		inputs[1].value.connected = false;
		check(select_device.sample(inputs, true, false).buttons == 0 && select_device.sample(inputs, true, false).sticks[0] == 0, "unplugging clears the report");
		inputs[1].value.connected = true;
		inputs[1].value.buttons = bit(start);
		check(select_device.sample(inputs, true, false).buttons == 0, "reconnecting with Start held does not press it");
		select_device.sample(inputs, false, false);
		check(select_device.sample(inputs, true, false).buttons == 0, "focus return with a held control stays neutral");
		inputs[1].value.buttons = 0;
		select_device.sample(inputs, false, false);
		inputs[1].value.buttons = bit(cross);
		check(select_device.sample(inputs, false, false).buttons == bit(cross), "a controller keeps working while another window has the focus");
		check(select_device.sample(inputs, true, false).buttons == bit(cross), "regaining the focus does not drop a held controller button");
		inputs[1].value.buttons = 0;
		select_device.sample(inputs, true, false);
		inputs[2].value.buttons |= bit(l1);
		select_device.sample(inputs, true, false, "keyboard");
		check(select_device.owner() == "keyboard", "controller lock preserves the chosen player");
	}

	static void raw_controller_learning()
	{
		raw_state rest{{false, false}, {0.f, -1.f}, {0}};
		auto pressed_raw = rest;
		pressed_raw.buttons[1] = true;
		check(learn(rest, rest, pressed_raw) == binding{binding::button, 1}, "learn uses the SDL raw button index");
		layout learned{};
		learned[start] = binding{binding::button, 1};
		check(map({}, pressed_raw, learned).buttons == bit(start), "a taught Options button maps to Start");
		pressed_raw = rest;
		pressed_raw.hats[0] = 2;
		const auto hat = learn(rest, rest, pressed_raw);
		check(hat == binding{binding::hat, 0, 2}, "learn stores SDL hat mask without browser indices");
		learned[right] = hat;
		pressed_raw.hats[0] = 3;
		check(map({}, pressed_raw, learned).buttons == bit(right), "a hat's diagonal keeps its direction");
		pressed_raw = rest;
		pressed_raw.axes[1] = 1.f;
		const auto trigger = learn(rest, rest, pressed_raw);
		check(trigger == binding{binding::axis, 1, 1, -1.f}, "trigger learn keeps its rest calibration");
		check(!neutral(rest, pressed_raw) && neutral(rest, rest), "neutral waits for a calibrated trigger to release");
		check(canonical({bit(pbd::controllers::select) | bit(start) | bit(square)}) == 0x0980, "logical actions have the canonical PS3 bits");
		check(same_input({binding::axis, 2, 1, -1.f}, {binding::axis, 2, 1, 0.f}), "calibration never gives a raw trigger two jobs");
		pbd::practice::controls::touchpad_click touch;
		touch.mode = pbd::practice::controls::touchpad_mode::split;
		check(touch.update(true, .25f, true) == 0x0100, "touchpad left half remains Select");
		check(touch.update(true, .75f, true) == 0x0100, "a held touchpad click keeps its first half");
		touch.update(false, {}, true);
		check(touch.update(true, .75f, true) == 0x0800, "touchpad right half remains Start");
		touch.update(false, {}, true);
		check(touch.update(true, {}, true) == 0x0100, "a stick with no touch coordinates remains Select");
	}

	static void live_attack_mapping()
	{
		settings::values live;
		live.device = "keyboard";
		live.mapping[4] = "1+2+3";
		settings::publish(live, {{"keyboard", "Keyboard"}});
		check(settings::remap_attacks(0x1004) == 0x10d0, "Take Control starts with the saved macro and keeps its direction");
		const std::array<std::uint16_t, 5> masks{208, 176, 224, 112, 240};
		for (unsigned i = 0; i < masks.size(); ++i)
		{
			live.mapping[4] = settings::assignments[11 + i];
			check(settings::submit(live), "a live macro change is accepted for a connected controller");
			check(settings::remap_attacks(0x8004) == (0x8000 | masks[i]), "Take Control uses every attack in the new macro");
			check(pbd::practice::controls::map_attacks(0x8004, pbd::practice::controls::defaults) == (0x8000 | masks[i]),
				"Practice and Take Control use the same live mapping");
		}
		settings::publish(settings::values{}, {{"keyboard", "Keyboard"}});
		check(settings::remap_attacks(0x0004) == 0x00f0, "an older backend snapshot cannot undo a live mapping change");
		check(settings::read().current.mapping[4] == "1+2+3+4", "reopening Controls retains the submitted value before its next poll");
		live.mapping[4] = "1+4";
		const auto reopened = settings::current_revision();
		settings::publish(live, {{"keyboard", "Keyboard"}}, reopened);
		check(settings::remap_attacks(0x0004) == 0x00a0, "a new backend or a saved-file change takes the current launcher mapping");
		// The stick clicks: with no action a click stays the click (Take Control, Remember); with one, a fight reads that action's
		// attacks in its place.
		live.mapping = settings::values{}.mapping;
		live.mapping[9] = "1+2";
		check(settings::submit(live), "R3 can be given an action");
		check(settings::remap_attacks(0x0400 | 0x1000) == (0x1000 | 0x0090) && settings::remap_attacks(0x0200) == 0x0200,
			"a taken fighter's R3 is 1+2 and its L3 the click");
		check(settings::clicks_with_actions() == 0x0400, "only R3 has an action");
		check(pbd::practice::controls::map_attacks(0x0400 | 0x0080, pbd::practice::controls::defaults) == 0x0090,
			"Practice's fight reads R3 as 1+2 too");
		check(!pbd::practice::controls::r3_remembers(pbd::practice::controls::defaults), "so R3 no longer remembers the position");
		live.mapping[9] = "off";
		live.mapping[8] = "4";
		check(settings::submit(live) && pbd::practice::controls::r3_remembers(pbd::practice::controls::defaults) &&
				  settings::remap_attacks(0x0400) == 0x0400 && settings::remap_attacks(0x0200 | 0x0020) == 0x0020,
			"R3 off is Remember again; L3 with 4 is a second Right Kick");
		auto seen = settings::current_revision();
		settings::publish(live, {{"pad-a", "First pad"}, {"pad-b", "Second pad"}}, seen);
		settings::values first, second;
		first.device = "pad-a";
		first.mapping[0] = "1+2";
		second.device = "pad-b";
		second.mapping[1] = "3+4";
		check(settings::submit(first) && settings::submit(second), "both offline controllers can change before the backend polls");
		first.mapping[0] = "1+4";
		check(settings::submit(first), "a pending layout can change again before the backend polls");
		const auto changes = settings::take(seen);
		check(changes.size() == 2 && changes[0] == second && changes[1] == first,
			"the backend takes each device's latest mapping, with the last submitted device last");
		check(seen == settings::current_revision() && settings::take(seen).empty(), "one poll acknowledges the whole batch");
		check(settings::submit(second), "another layout can be pending when a backend opens");
		seen = settings::current_revision();
		check(settings::submit(first), "a new change can arrive before the new backend's first poll");
		const auto fresh = settings::take(seen);
		check(fresh.size() == 1 && fresh[0] == first, "a new backend ignores changes older than its starting revision");
		{
			std::lock_guard lock(settings::mutex);
			settings::current = {};
			settings::requested = {};
			settings::revision = 0;
			settings::mapping_changed.store(false);
			settings::attack_masks.store(0);
		}
	}

} // namespace

PBD_TEST(PbdControllerMapper, OwnershipMappingAndGlyphs)
{
	std::array<source, 3> inputs{{{"keyboard", {}, true}, {"pad-a", {}}, {"pad-b", {}}}};
	controller_claim_and_hysteresis(inputs);
	std::array<source, 3> couch{{{"keyboard", {}, true}, {"pad-a", {}}, {"pad-b", {}}}};
	selector first("pad-a"), second("");
	first.sample(couch, true, false, "pad-a");
	second.sample(without(couch, first.owner()), true, false);
	couch[2].value.buttons = bit(cross);
	check(first.sample(couch, true, false, "pad-a").buttons == 0 && first.owner() == "pad-a",
		"a locked Player 1 keeps its pad");
	const auto joined = second.sample(without(couch, first.owner()), true, false);
	check(joined.buttons == bit(cross) && second.owner() == "pad-b",
		"another pad's fresh press takes the second player");
	couch[1].value.buttons = bit(circle);
	check(first.sample(couch, true, false, "pad-a").buttons == bit(circle), "Player 1 plays its own pad");
	check(second.sample(without(couch, first.owner()), true, false).buttons == bit(cross),
		"Player 1's pad never plays the second side");
	check(without(couch, "pad-a").size() == 2 && without(couch, "nobody").size() == 3,
		"only Player 1's device is left out");
	selector connected_held("pad-a");
	check(connected_held.sample(inputs, true, false).buttons == 0, "held startup input is swallowed");
	inputs[1].value.buttons = 0;
	connected_held.sample(inputs, true, false);
	inputs[1].value.buttons = bit(start);
	check(connected_held.sample(inputs, true, false).buttons == bit(start), "release then fresh press rearms startup input");

	raw_controller_learning();
	live_attack_mapping();
	display screen;
	screen.active = true;
	screen.family = "switch";
	check(glyph_label("cross", screen) == "B" && glyph_label("circle", screen) == "A", "Switch prompts name the physical positions");
	screen.family = "steamdeck";
	check(glyph_label("l1", screen) == "L1" && glyph_label("cross", screen) == "A", "Steam Deck prompts keep its shoulders");
	screen.family = "keyboard";
	screen.keys[cross] = "J";
	screen.keys[l3] = "F";
	check(glyph_label("cross", screen) == "J" && glyph_label("l3", screen) == "F", "keyboard prompts use the configured names");
	screen.family = "numbers";
	check(glyph_label("square", screen) == "1" && glyph_label("cross", screen) == "3", "arcade stick attack-number prompts");
	screen.family = "playstation";
	screen.button_type = "ps4";
	check(glyph_label("start", screen) == "OPTIONS" && glyph_label("select", screen) == "SHARE", "DualShock 4 system buttons");
	screen.button_type = "ps5";
	check(glyph_label("start", screen) == "OPTIONS" && glyph_label("select", screen) == "CREATE", "DualSense system buttons");
	for (const auto type : {"ps3", "arcade"})
	{
		screen.button_type = type;
		check(glyph_label("start", screen) == "START" && glyph_label("select", screen) == "SELECT", "PS3 and sticks name START/SELECT");
	}
	screen.family = "xbox";
	screen.button_type.clear();
	check(glyph_label("start", screen) == "Menu" && glyph_label("select", screen) == "View", "Xbox system buttons are named");
	screen.family = "keyboard";
	screen.keys[start] = "Return";
	check(glyph_label("start", screen) == "Enter", "the keyboard names Enter");
}

PBD_TEST(PbdControllerMapper, KeyboardBindingsKeepTheTwoEnterKeysDistinct)
{
	using namespace pbd::raw_keyboard;
	return_keys physical;
	std::array<std::vector<std::string>, count> keys;
	keys[cross] = {"Return"};
	keys[circle] = {"Num+Enter"};
	keys[square] = {"J"};
	keys[l1] = {"Ctrl Right"};
	const auto sample = [&]
	{
		return sample_keyboard(keys, [&](std::string_view key)
			{
				return physical.pressed(key, [](std::string_view name)
					{
						return name == "J" || name == "Ctrl Right";
					});
			});
	};
	constexpr auto other = bit(square) | bit(l1);
	check(sample().buttons == other);
	physical.observe({0x1c, 0, 0x0d, wm_keydown});
	check(sample().buttons == (other | bit(cross)), "main Enter contributes only its own binding");
	physical.observe({0x1c, key_e0, 0x0d, wm_keydown});
	check(sample().buttons == (other | bit(cross) | bit(circle)), "both physical Enter keys contribute their bindings");
	physical.observe({0x1c, key_break, 0x0d, wm_keyup});
	check(sample().buttons == (other | bit(circle)), "keypad Enter remains held after main Enter releases");
	physical.observe({0x1c, key_e0 | key_break, 0x0d, wm_keyup});
	check(sample().buttons == other);
	physical.observe({0x1c, key_e0, 0x0d, wm_keydown});
	check(sample().buttons == (other | bit(circle)), "keypad Enter alone cannot press main Enter's action");
	physical.clear();
	check(sample().buttons == other, "registration loss releases both Enter bindings");
}
