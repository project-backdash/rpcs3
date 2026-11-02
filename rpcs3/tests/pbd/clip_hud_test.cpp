#include "pbd_test.h"

#include "Emu/PBD/pbd_clip_buffer.h"
#include "Emu/PBD/pbd_clip_hud.h"

using pbd::test::check;

namespace
{
	namespace pad = pbd::controllers;

	pad::layout sony_layout()
	{
		pad::layout value;
		constexpr std::array<unsigned, 14> order{pad::square, pad::cross, pad::circle, pad::triangle, pad::l1, pad::r1, pad::l2, pad::r2,
			pad::select, pad::start, pad::l3, pad::r3, pad::home, pad::touchpad};
		for (unsigned i = 0; i < order.size(); ++i)
			value[order[i]] = pad::binding{pad::binding::button, i};
		return value;
	}

	void pad_name_checks()
	{
		using namespace pbd::clips;
		const pad::layout none{};
		const auto name = [&](std::string_view choice, pad_buttons buttons, bool versus = false, const pad::layout& defaults = {})
		{
			return prompt_name(parse_pad_button(choice), buttons, defaults, none, versus);
		};
		check(name("create", {"playstation", true}) == "Mute", "the default button is a DualSense's Mute button, never its touchpad");
		check(name("create", {"xbox", true}) == "Share", "an Xbox pad's Share button");
		check(name("create", {"switch", true}) == "Capture", "a Switch pad's Capture button");
		check(name("create", {"steamdeck", true}) == "...", "a Steam Deck's Quick Access button, by its print");
		check(name("create", {"xbox", false}).empty() && name("create", {"playstation", false}).empty(),
			"a pad without MISC1, a DualShock 4 or a stick among them, has no clip button to show");
		for (const auto older : {"share", "touchpad"})
			check(name(older, {"playstation", true}) == "Mute" && name(older, {"playstation", false}).empty(),
				"the older Share and Touchpad choices are the default button");
		check(name("home", {"playstation"}) == "PS" && name("home", {"xbox"}) == "Home", "Home by its name on the pad");
		check(name("home", {"xbox"}, true).empty(), "in a match Home is the game's Start, so it is no clip button");

		const auto sony = sony_layout();
		check(name("button-10", {"playstation"}, false, sony) == "{l3}", "a picked button shows as the glyph of its job");
		check(name("button-8", {"playstation"}, false, sony).empty() && name("button-13", {"playstation"}, false, sony).empty(),
			"a button the game gets as Select or Start is no clip button");
		check(name("button-12", {"playstation"}, false, sony) == "PS" && name("button-12", {"playstation"}, true, sony).empty(),
			"a raw Home is Home outside a match");
		check(name("button-15", {"playstation"}, false, sony) == "Button 16", "a button without a job shows its number");
		pad::layout dpad = sony;
		dpad[pad::up] = pad::binding{pad::binding::button, 14};
		check(name("button-14", {"playstation"}, false, dpad) == "{dpad:u}", "a direction button shows its arrow");
		pad::layout taught{};
		taught[pad::cross] = pad::binding{pad::binding::button, 10};
		check(prompt_name(parse_pad_button("button-10"), {"playstation"}, sony, taught, false) == "{cross}", "a taught job wins");
		check(!misc1_free(taught, pad::binding{pad::binding::button, 10}) && misc1_free(taught, std::nullopt),
			"a MISC1 the player taught a game job is no clip button");
	}

	void legend_checks()
	{
		using namespace pbd::clips::hud;
		check(settings{}.save_key == "Alt+F10" && settings{}.record_key == "Alt+F9",
			"before the Clips settings arrive, or with no settings file, the keys are NVIDIA Instant Replay's");
		settings clips{true, true, 30};
		const keys save_keys{false, false, "Alt+F10"}, record_keys{false, false, "Alt+F9"};
		pad::display keyboard;
		keyboard.active = true;
		keyboard.family = "keyboard";
		auto shown = legend_for(clips, keyboard);
		check(shown.shown && shown.save == save_keys && shown.record == record_keys, "the keyboard shows the player's own keys");
		check(shown.save_button.text.empty() && shown.record_button.text.empty(), "and no pad button while the keyboard is in use");
		check(stop_keys(shown) == record_keys, "the keyboard's Record keys stop a recording");

		pad::display xbox;
		xbox.active = true;
		xbox.family = "xbox";
		xbox.clip_button = "Share";
		shown = legend_for(clips, xbox);
		check(shown.save == save_keys && shown.record == record_keys, "the keyboard keys show while a pad is in use");
		check(shown.save_button == keys{true, false, "Share"} && shown.record_button == keys{true, true, "Share"},
			"beside the pad's button: a tap saves, a hold records");
		check(stop_keys(shown) == keys{true, true, "Share"}, "a recording started from the pad says to hold its button to stop");

		xbox.clip_button.clear();
		shown = legend_for(clips, xbox);
		check(shown.save == save_keys && shown.record == record_keys && shown.save_button.text.empty() && shown.record_button.text.empty(),
			"a pad without a clip button shows the keyboard keys alone");
		check(stop_keys(shown) == record_keys, "and its recording stops with them");
		shown = legend_for(clips, pad::display{});
		check(shown.save == save_keys && shown.save_button.text.empty(), "before any controller reports, the keys");

		settings custom = clips;
		custom.save_key = "F8";
		custom.record_key = "Shift+F8";
		check(legend_for(custom, keyboard).save.text == "F8" && legend_for(custom, keyboard).record.text == "Shift+F8", "changed keys show as changed");
		check(legend_for(custom, xbox).record.text == "Shift+F8", "on a pad as well");

		settings quiet = clips;
		quiet.background = false;
		xbox.clip_button = "Capture";
		shown = legend_for(quiet, xbox);
		check(shown.shown && shown.save.text.empty() && shown.save_button.text.empty() && shown.record == record_keys &&
				  shown.record_button == keys{true, true, "Capture"},
			"without Background recording there is nothing to save, but Record still works");

		settings off = clips;
		off.available = false;
		check(!legend_for(off, keyboard).shown && legend_for(off, keyboard) == legend{} && legend_for(off, xbox) == legend{},
			"no clip keys while this PC cannot make clips");

		const auto parts = key_parts("Ctrl+Shift+F11");
		check(parts.size() == 3 && parts[0] == "Ctrl" && parts[1] == "Shift" && parts[2] == "F11", "a shortcut is one keycap per key");
		check(key_parts("F8").size() == 1 && key_parts("").empty(), "a single key is one keycap");
	}

	void toast_text_checks()
	{
		using namespace pbd::clips::hud;
		check(title(toast_kind::clip_saved) == "CLIP SAVED" && title(toast_kind::recording) == "RECORDING" &&
				  title(toast_kind::recording_saved) == "RECORDING SAVED" && title(toast_kind::none).empty(),
			"the toast titles");
		check(length_text(15) == "15 s" && length_text(30) == "30 s" && length_text(60) == "1 min" && length_text(120) == "2 min" &&
				  length_text(300) == "5 min",
			"clip lengths read as the Clips settings name them");
		check(duration_text(102'000) == "1:42" && duration_text(9'999) == "0:09" && duration_text(3'600'000) == "60:00",
			"a recording's length in minutes and seconds");
	}

	void toast_timing_checks()
	{
		using namespace pbd::clips::hud;
		auto at = frame_at(0);
		check(at.shown && at.alpha == 0.f && at.offset == -6.f && at.dot, "a toast starts clear, 6 units up");
		at = frame_at(fade_in_ms);
		check(at.alpha == 1.f && at.offset == 0.f, "and settles in 135 ms");
		check(frame_at(1999).alpha == 1.f && frame_at(1999).shown, "it holds for 2 s");
		at = frame_at(2125);
		check(at.shown && at.alpha > .49f && at.alpha < .51f, "then fades out over 250 ms");
		check(!frame_at(2250).shown && !frame_at(60'000).shown, "and is gone");
		check(frame_at(499).dot && !frame_at(500).dot && frame_at(1000).dot, "the recording dot blinks in two steps a second");
	}

	void event_checks()
	{
		using namespace pbd::clips::hud;
		events seen;
		check(seen.update(false, "", 0).kind == toast_kind::none, "a fresh recorder says nothing");
		auto result = seen.update(true, "", 1000);
		check(result.kind == toast_kind::recording, "a recording starts");
		check(seen.update(true, "", 1100).kind == toast_kind::none, "and runs quietly");
		check(seen.update(true, "a.mp4", 5000).kind == toast_kind::clip_saved, "a clip saved while recording is a clip");
		result = seen.update(false, "b.mp4", 103'000);
		check(result.kind == toast_kind::recording_saved && result.length_ms == 102'000, "the recording saved, with its length");
		check(seen.update(false, "b.mp4", 104'000).kind == toast_kind::none, "an unchanged status says nothing again");
		check(seen.update(false, "c.mp4", 105'000).kind == toast_kind::clip_saved, "a new file while idle is a clip");
		seen.update(true, "c.mp4", 106'000);
		check(seen.update(false, "c.mp4", 107'000).kind == toast_kind::none, "a stop that saved nothing says nothing");
	}

	void channel_checks()
	{
		using namespace pbd::clips::hud;
		publish({});
		show(toast_kind::clip_saved, 1000);
		check(current().kind == toast_kind::none && !toast_showing(1000), "no toast while clips cannot record here");
		publish({true, true, 60, "Ctrl+F10", "Ctrl+Shift+F11"});
		show(toast_kind::clip_saved, 2000);
		const auto shown = current();
		check(shown.kind == toast_kind::clip_saved && shown.seconds == 60 && shown.at_ms == 2000, "a saved clip keeps the length it saved with");
		check(toast_showing(2000) && toast_showing(4249) && !toast_showing(4250), "the toast takes the slot for 2.25 s");
		show(toast_kind::recording_saved, 3000, 102'000);
		check(current().kind == toast_kind::recording_saved && current().serial == shown.serial + 1, "a new toast replaces the one showing");
		show(toast_kind::none, 3500);
		check(current().kind == toast_kind::recording_saved, "nothing to say replaces nothing");
		publish({});
	}
} // namespace

PBD_TEST(PbdClipHud, PadButtonNames)
{
	pad_name_checks();
}
PBD_TEST(PbdClipHud, LegendFollowsDeviceAndSettings)
{
	legend_checks();
}
PBD_TEST(PbdClipHud, ToastText)
{
	toast_text_checks();
}
PBD_TEST(PbdClipHud, ToastTiming)
{
	toast_timing_checks();
}
PBD_TEST(PbdClipHud, RecorderEvents)
{
	event_checks();
}
PBD_TEST(PbdClipHud, SharedChannel)
{
	channel_checks();
}
