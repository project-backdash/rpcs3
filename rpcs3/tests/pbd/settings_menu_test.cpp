#include "pbd_test.h"

#include "Emu/PBD/pbd_json_reader.h"
#include "Emu/PBD/pbd_settings_runtime.h"

#include <array>
#include <string>
#include <string_view>
#include <vector>

namespace
{
	using pbd::test::check;

	using namespace pbd::settings_menu;

	model sample_model()
	{
		model m;
		m.start.volume = 80;
		m.start.resolution = 100;
		m.start.fullscreen = true;
		m.start.vsync = false;
		m.start.audio_output = std::string(default_audio_output);
		m.start.display = "monitor:\\\\?\\DISPLAY#DEL4321#5&1&UID4353#{e6f07b5f-ee97-4a90-b076-33f57bf4eaa7}";
		m.applied = m.start;
		m.current = m.start;
		m.audio_outputs = {{std::string(default_audio_output), "Windows default"},
			{"{0.0.0.00000000}.{11111111-2222-3333-4444-555555555555}", "Headphones (USB Audio)"},
			{"{0.0.0.00000000}.{AAAAAAAA-2222-3333-4444-555555555555}", "Speakers (Realtek(R) Audio)"}};
		m.displays = {{m.start.display, "1: 2560 x 1440"}, {"monitor:\\\\?\\DISPLAY#GSM5B7F#5&2&UID4354#{e6f07b5f-ee97-4a90-b076-33f57bf4eaa7}", "2: 1920 x 1080"}};
		return m;
	}

	std::uint32_t at(row r)
	{
		return static_cast<std::uint32_t>(r);
	}
	std::uint32_t at_entry(entry e)
	{
		return static_cast<std::uint32_t>(e);
	}

} // namespace

void settings_page_return_checks(model& m, controller& c, auto& v)
{
	// Settings opens the settings page; Circle and its Back row go back up, applying the video rows; START closes.
	c.top_focus = at_entry(entry::settings);
	check(c.poll(m, cross, 40) == effect::none && c.level == page::settings && c.open, "Cross on Settings opens the page");
	c.focus = at(row::resolution);
	c.poll(m, 0, 41);
	c.poll(m, cross, 42);
	c.poll(m, 0, 43);
	check(c.poll(m, cross, 44) == effect::apply && c.level == page::top && c.open, "Confirm applies and leaves Settings for the pause menu");
	c.poll(m, 0, 45);
	c.poll(m, cross, 46);
	c.poll(m, 0, 47);
	c.poll(m, 0, 50);
	v = make_view(m, c, "Practice");
	check(!v.top && v.has_top, "under the top level the last row is Back");
	check(c.poll(m, circle, 60) == effect::apply && c.level == page::top && c.open, "Circle goes back up and applies the video rows");
	c.poll(m, 0, 70);
	c.poll(m, cross, 80);
	c.poll(m, 0, 90);
	c.focus = at(row::close);
	check(c.poll(m, cross, 100) == effect::apply && c.level == page::top, "the Back row goes up too");
	c.poll(m, 0, 110);
	c.poll(m, cross, 120);
	c.poll(m, 0, 130);
	check(c.poll(m, start, 140) == effect::close && !c.open, "START closes from the settings page");
}

std::uint32_t top_row_navigation_checks(model& m, controller& c, auto& v)
{
	// The pause menu's rows, in order (without clips, no recording row); Down from Return to Game reaches each in turn and stops
	// at Exit Game, and Up stops at Return to Game, so Exit Game is never one press from where the menu opens.
	const auto rows = shown_entries(m);
	const auto shown = static_cast<std::uint32_t>(rows.size());
	check(shown == 4 && rows[2] == entry::launcher && rows[3] == entry::quit, "without clips the recording row is not shown");
	c.show(0, true);
	v = make_view(m, c, "Practice");
	check(v.entries.size() == rows.size() && v.top_focus == 0,
		"the view lists the rows shown");
	for (std::uint32_t i = 1; i <= shown; ++i)
	{
		c.poll(m, down, 150 + i * 2);
		c.poll(m, 0, 151 + i * 2);
		check(c.top_focus == at_entry(rows[std::min(i, shown - 1)]), "Down moves through the rows and stops at Exit Game");
	}
	for (std::uint32_t i = 1; i <= shown; ++i)
	{
		c.poll(m, up, 170 + i * 2);
		c.poll(m, 0, 171 + i * 2);
		check(c.top_focus == at_entry(rows[shown - 1 - std::min(i, shown - 1)]), "Up moves back and stops at Return to Game");
	}
	// A held Up repeats but still stops at Return to Game.
	c.poll(m, up, 190);
	c.poll(m, up, 190 + 2000);
	check(c.top_focus == at_entry(entry::resume), "a held Up never wraps to Exit Game");
	c.poll(m, 0, 2200);
	check(c.wheel(m, true) == effect::none && c.top_focus == at_entry(entry::resume), "the wheel stops at Return to Game too");

	return shown;
}

void launcher_navigation_checks(model& m, controller& c, auto& v)
{
	// Return to Launcher: the menu stays up saying so and takes nothing more until the run ends.
	c.show(0, true);
	c.top_focus = at_entry(entry::launcher);
	check(c.poll(m, cross, 200) == effect::navigate && c.leaving && c.open && c.chosen() == entry::launcher, "Return to Launcher asks the launcher");
	c.poll(m, 0, 210);
	check(c.poll(m, circle, 220) == effect::none && c.poll(m, down, 230) == effect::none && c.top_focus == at_entry(entry::launcher) && c.open,
		"no input once a leaving row is chosen");
	v = make_view(m, c, "Replay");
	check(v.leaving && v.prompts.empty(), "leaving says so, with no controls");
	const std::string notice = "launcher request failed";
	c.cancel_leave(notice);
	v = make_view(m, c, "Replay");
	check(!c.leaving && v.help == notice, "a failed request says so and takes input again");
	c.poll(m, 0, 235);
	c.poll(m, down, 240);
	check(c.top_focus == at_entry(entry::quit),
		"the next press clears the notice");

	// Exit Game asks at once, with no question of its own: the menu always opens on Return to Game.
	c.poll(m, 0, 245);
	check(c.poll(m, cross, 250) == effect::navigate && c.leaving && c.chosen() == entry::quit, "Exit Game asks the launcher");
}

void navigation_request_checks(model& m, controller& c, auto& v)
{
	// The two leaving rows name what they ask of the launcher; the two in-game rows write nothing.
	for (const auto& [destination, target] : {std::pair{entry::launcher, "launcher"}, std::pair{entry::quit, "quit"}})
	{
		const auto decoded = pbd::json::detail::json_parser(navigate_json(destination)).parse();
		check(decoded.value);
		check(pbd::json::detail::is(decoded.value->find("schema"), "pbd-navigate-1"));
		check(pbd::json::detail::is(decoded.value->find("target"), target));
	}
	check(navigate_json(entry::resume).empty() && navigate_json(entry::settings).empty() && navigate_json(entry::record).empty(),
		"the in-game rows write no request");
	const std::array<std::string_view, entry_count> targets{"", "", "", "launcher", "quit"};
	check(entry_targets == targets, "each row's launcher target");
}

void top_pointer_checks(model& m, controller& c, auto& v, std::uint32_t shown)
{
	// The pointer: hover focuses a row, a click acts on it.
	c.show(0, true);
	const auto centre = [](rect r)
	{
		return std::pair{r.x + r.w / 2.f, r.y + r.h / 2.f};
	};
	auto [sx, sy] = centre(entry_rect(1, shown));
	check(c.pointer(m, sx, sy, false) == effect::none && c.top_focus == at_entry(entry::settings) && c.level == page::top, "hovering a row focuses it");
	check(c.pointer(m, sx, sy, true) == effect::none && c.level == page::settings, "a click on Settings opens the page");
	check(c.back(m) == effect::apply && c.level == page::top, "a right click goes back up");
	auto [ex, ey] = centre(entry_rect(3, shown));
	check(c.pointer(m, ex, ey, true) == effect::navigate && c.leaving && c.chosen() == entry::quit,
		"a click on Exit Game asks the launcher");
	check(c.pointer(m, sx, sy, true) == effect::none, "the pointer does nothing once a leaving row is chosen");
	check(entry_top(1, 4) - entry_top(0, 4) == row_pitch && entry_top(2, 4) - entry_top(1, 4) == row_pitch + close_gap &&
			  entry_top(3, 4) - entry_top(2, 4) == row_pitch,
		"the two leaving rows sit apart below Return to Game and Settings");
	check(entry_top(2, 5) - entry_top(1, 5) == row_pitch && entry_top(3, 5) - entry_top(2, 5) == row_pitch + close_gap &&
			  entry_top(4, 5) - entry_top(3, 5) == row_pitch,
		"with the recording row, the gap stays above the two leaving rows");
	for (std::uint32_t i = 0; i < entry_count; ++i)
		check(entry_rect(i, entry_count).y + entry_rect(i, entry_count).h <= row_top(row_count - 1) + row_height,
			"the rows fit the settings page's list column");
}

// Offline play opens the menu only as asked for, and never beside a session: a session's own menu and rules come first.
PBD_TEST(PbdSettingsMenu, OfflinePlayChecks)
{
	using pbd::session_mode::offline_play_requested;
	check(offline_play_requested("1", nullptr) && offline_play_requested("1", "0"), "PBD_OFFLINE_PLAY=1 asks for it");
	check(!offline_play_requested(nullptr, nullptr) && !offline_play_requested("", nullptr) &&
			  !offline_play_requested("true", nullptr),
		"only the value 1 asks for it");
	check(!offline_play_requested("1", "1"), "a session is never offline play");
}

PBD_TEST(PbdSettingsMenu, NavigationChecks)
{
	auto m = sample_model();
	controller c;
	// The Cross that opened the menu from Practice's row must not act in it.
	c.show(cross);
	check(c.open && c.release_required, "a press held at open must wait for its release");
	check(c.poll(m, cross, 0) == effect::none && c.focus == 0 && !c.dropdown, "the held press must not act");
	check(c.poll(m, 0, 10) == effect::none && !c.release_required, "the release frees the menu");
	check(c.poll(m, up, 20) == effect::none && c.focus == at(row::close), "Up from the first row wraps to Close");
	c.poll(m, 0, 30);
	check(c.poll(m, down, 40) == effect::none && c.focus == at(row::volume), "Down from Close wraps to Volume");
	c.poll(m, 0, 50);

	// Volume: a 5-point grid, clamped to 0..100, heard at once.
	check(c.poll(m, right, 60) == effect::volume && m.current.volume == 85, "Right raises the volume by 5");
	check(c.poll(m, right, 100) == effect::none && m.current.volume == 85, "a held Right waits 400 ms before repeating");
	check(c.poll(m, right, 470) == effect::volume && m.current.volume == 90, "then it repeats");
	check(c.poll(m, right, 500) == effect::none, "the next repeat waits 90 ms");
	check(c.poll(m, right, 560) == effect::volume && m.current.volume == 95, "and repeats every 90 ms");
	c.poll(m, 0, 600);
	m.current.volume = 83;
	check(c.poll(m, left, 610) == effect::volume && m.current.volume == 80, "Left from 83 lands on the grid at 80");
	c.poll(m, 0, 620);
	m.current.volume = 100;
	check(c.poll(m, right, 630) == effect::none && m.current.volume == 100, "100 is the top");
	c.poll(m, 0, 640);
	check(when(row::volume) == timing::now && !pending(m, row::volume), "Volume applies now and never waits");

	// Resolution: Left/Right step through the launcher's sizes and stop at the ends; Cross opens the list on the size in use.
	c.focus = at(row::resolution);
	check(c.poll(m, right, 700) == effect::none && m.current.resolution == 133, "Right moves to the next size");
	check(pending(m, row::resolution), "a new size waits for the menu to close");
	c.poll(m, 0, 710);
	c.poll(m, right, 720);
	c.poll(m, 0, 730);
	check(m.current.resolution == 200, "the largest size");
	c.poll(m, right, 740);
	c.poll(m, 0, 750);
	check(m.current.resolution == 200, "Right stops at the largest size");
	check(c.poll(m, cross, 760) == effect::none && c.dropdown && c.option_focus == 3, "Cross opens the list on the size in use");
	c.poll(m, 0, 770);
	c.poll(m, up, 780);
	c.poll(m, 0, 790);
	check(c.option_focus == 2 && m.current.resolution == 200, "moving in the list changes nothing yet");
	check(c.poll(m, circle, 800) == effect::none && !c.dropdown && c.open && m.current.resolution == 200, "Circle leaves the list unchanged");
	c.poll(m, 0, 810);
	c.poll(m, cross, 820);
	c.poll(m, 0, 830);
	c.poll(m, down, 840);
	c.poll(m, 0, 850);
	check(c.option_focus == 0, "the list wraps");
	check(c.poll(m, cross, 860) == effect::close && !c.open && m.current.resolution == 67, "Confirm chooses and leaves Settings");
	c.poll(m, 0, 870);
	c.show(0);
	c.focus = at(row::resolution);

	// START in an open list closes the whole menu.
	c.poll(m, cross, 880);
	c.poll(m, 0, 890);
	check(c.dropdown, "the list is open");
	check(c.poll(m, start, 900) == effect::close && !c.open && !c.dropdown, "START closes the menu from an open list");

	// Close row and Circle.
	c.show(0);
	c.focus = at(row::close);
	check(c.poll(m, cross, 1000) == effect::close && !c.open, "Cross on Close closes");
	c.show(0);
	check(c.poll(m, circle, 1100) == effect::close, "Circle closes");
}

PBD_TEST(PbdSettingsMenu, ChoiceChecks)
{
	auto m = sample_model();
	check(value_text(m, row::audio_output) == m.audio_outputs[0].label, "the default output");
	// Endpoint IDs compare without letter case: the launcher may save another case than Windows lists.
	m.current.audio_output = "{0.0.0.00000000}.{aaaaaaaa-2222-3333-4444-555555555555}";
	check(value_text(m, row::audio_output) == m.audio_outputs[2].label, "a device is found whatever its letter case");
	check(pending(m, row::audio_output), "a new device waits for the next session");
	m.current.audio_output = "{0.0.0.00000000}.{99999999-2222-3333-4444-555555555555}";
	check(chosen_index(m, m.current, row::audio_output) == m.audio_outputs.size(), "an unplugged device has no listed choice");
	m.current = m.start;
	m.current.resolution = 150;
	check(value_text(m, row::resolution) == "150%", "a size the launcher does not offer shows its percentage");
	check(value_text(m, row::display) == m.displays[0].label, "the window's monitor");
	check(when(row::display) == timing::next_session && when(row::window) == timing::on_close && when(row::close) == timing::none,
		"each row says when it applies");
	model none;
	check(options(none, row::display).empty(), "no monitor list");
	controller c;
	c.show(0);
	c.focus = at(row::display);
	check(c.poll(none, cross, 0) == effect::none && !c.dropdown, "a row without choices does not open a list");
}

PBD_TEST(PbdSettingsMenu, ChangesChecks)
{
	using namespace pbd::json;
	using namespace pbd::json::detail;
	auto m = sample_model();
	auto decoded = json_parser(changes_json(m)).parse();
	check(decoded.value);
	check(is(decoded.value->find("schema"), "pbd-game-settings-changes-1"));
	const auto* patch = decoded.value->find("patch");
	check(patch && patch->kind == json_kind::object && patch->fields.empty(), "nothing changed: an empty patch");
	m.current.volume = 55;
	m.current.resolution = 133;
	m.current.fullscreen = false;
	m.current.vsync = true;
	m.current.audio_output = "{0.0.0.00000000}.{11111111-2222-3333-4444-555555555555}";
	m.current.display = m.displays[1].id;
	decoded = json_parser(changes_json(m)).parse();
	check(decoded.value);
	check(is(decoded.value->find("schema"), "pbd-game-settings-changes-1"));
	patch = decoded.value->find("patch");
	check(patch);
	const auto* video = patch->find("video");
	const auto* audio = patch->find("audio");
	check(video && audio);
	std::uint32_t value = 0;
	bool flag = false;
	check(number32(video->find("resolutionScale"), 0, 200, value) && value == 133);
	check(is(video->find("windowMode"), "windowed"));
	check(boolean(video->find("vsync"), flag) && flag);
	check(is(video->find("gameDisplay"), m.current.display), "the display's backslashes survive JSON encoding");
	check(number32(audio->find("masterVolume"), 0, 100, value) && value == 55);
	check(is(audio->find("outputDevice"), m.current.audio_output));
	m.current = m.start;
	m.current.fullscreen = false;
	m.start.fullscreen = false;
	m.borderless = true;
	m.current.fullscreen = true;
	decoded = json_parser(changes_json(m)).parse();
	check(decoded.value);
	patch = decoded.value->find("patch");
	check(patch);
	video = patch->find("video");
	check(video && is(video->find("windowMode"), "borderless"), "fullscreen saves Borderless when the session drew borderless");
	m = sample_model();
	m.current.audio_output = "{0.0.0.00000000}.{aaaaaaaa-2222-3333-4444-555555555555}";
	m.start.audio_output = "{0.0.0.00000000}.{AAAAAAAA-2222-3333-4444-555555555555}";
	decoded = json_parser(changes_json(m)).parse();
	check(decoded.value);
	patch = decoded.value->find("patch");
	check(patch && !patch->find("audio"), "the same device in another case is no change");
	m.current.resolution = 150;
	decoded = json_parser(changes_json(m)).parse();
	check(decoded.value);
	patch = decoded.value->find("patch");
	check(patch && !patch->find("video"), "a size the launcher does not offer is never saved");
}

PBD_TEST(PbdSettingsMenu, PointerChecks)
{
	auto m = sample_model();
	controller c;
	c.show(0);
	const auto centre = [](rect r)
	{
		return std::pair{r.x + r.w / 2.f, r.y + r.h / 2.f};
	};
	auto [vx, vy] = centre(row_rect(at(row::vsync)));
	check(c.pointer(m, vx, vy, false) == effect::none && c.focus == at(row::vsync) && !c.dropdown, "hovering a row focuses it");
	check(c.pointer(m, vx, vy, true) == effect::none && c.dropdown && c.option_focus == 1, "a click opens its list on the value in use");
	const float top = stack_top(m, row::vsync, 2);
	auto [ox, oy] = centre(option_rect(top, 0));
	check(c.pointer(m, ox, oy, false) == effect::none && c.option_focus == 0 && !m.current.vsync, "hovering an option highlights it");
	check(c.pointer(m, ox, oy, true) == effect::close && !c.open && m.current.vsync, "a click confirms and leaves Settings");
	c.show(0);
	c.pointer(m, vx, vy, true);
	check(c.dropdown, "the list is open again");
	check(c.pointer(m, 100.f, 100.f, true) == effect::none && !c.dropdown && m.current.vsync, "a click outside closes the list unchanged");
	auto [volume_x, volume_y] = centre(row_rect(at(row::volume)));
	c.pointer(m, volume_x, volume_y, false);
	auto [rx, ry] = centre(stepper_rect(true));
	check(c.pointer(m, rx, ry, true) == effect::volume && m.current.volume == 85, "the > beside the focused volume raises it");
	auto [lx, ly] = centre(stepper_rect(false));
	check(c.pointer(m, lx, ly, true) == effect::volume && m.current.volume == 80, "the < lowers it");
	check(c.pointer(m, volume_x - 300.f, volume_y, true) == effect::none && !c.dropdown, "a click on the Volume row itself opens nothing");
	auto [cx, cy] = centre(row_rect(at(row::close)));
	check(c.pointer(m, cx, cy, true) == effect::close && !c.open, "a click on Close closes");
	c.show(0);
	check(c.focus == at(row::close), "reopening keeps the last focused row");
	c.focus = 0;
	check(c.wheel(m, false) == effect::none && c.focus == 1, "the wheel moves the focus");
	check(c.back(m) == effect::close && !c.open, "a right click goes back out of the menu");
	check(row_top(at(row::close)) - row_top(at(row::controls)) == row_pitch + close_gap, "Close sits apart from the settings");
}

PBD_TEST(PbdSettingsMenu, ViewChecks)
{
	auto m = sample_model();
	controller c;
	c.show(0);
	auto v = make_view(m, c, "Practice");
	check(v.open && v.context == "Practice" && v.rows[at(row::volume)].value == "80" && v.rows[at(row::volume)].when == timing::now,
		"the view carries the rows");
	check(v.prompts.size() == 3 && v.prompts[0].keys == "{dpad:lr}", "Volume's control bar");
	c.focus = at(row::audio_output);
	m.current.audio_output = m.audio_outputs[1].id;
	v = make_view(m, c, "Replay");
	check(v.rows[at(row::audio_output)].pending && v.prompts[0].keys == "{cross}" && v.help == helps[at(row::audio_output)],
		"a pending device, its help and its control bar");
	c.press(m, cross);
	v = make_view(m, c, "Replay");
	check(v.dropdown && v.options.size() == 3 && v.option_current == 1, "an open list");
	c.focus = at(row::close);
	c.dropdown = false;
	v = make_view(m, c, "Spectating");
	check(v.prompts.size() == 3 && v.rows[at(row::close)].value.empty(), "Close's control bar");
}

PBD_TEST(PbdSettingsMenu, TopLevelChecks)
{
	auto m = sample_model();
	controller c;
	// Esc opens on the top level, always on Return to Game.
	c.top_focus = 3;
	c.show(0, true);
	check(c.open && c.has_top && c.level == page::top && c.top_focus == 0, "Esc opens on Return to Game");
	auto v = make_view(m, c, "Practice");
	check(v.top && v.top_focus == 0 && v.prompts.size() == 3,
		"the top level's view");
	check(c.poll(m, cross, 0) == effect::close && !c.open, "Cross on Return to Game closes");
	c.show(0, true);
	check(c.poll(m, circle, 10) == effect::close && !c.open, "Circle on the top level returns to the game");
	c.show(0, true);
	check(c.poll(m, up, 20) == effect::none && c.top_focus == at_entry(entry::resume), "Up on Return to Game stays there");
	c.poll(m, 0, 30);

	settings_page_return_checks(m, c, v);
	const auto shown = top_row_navigation_checks(m, c, v);
	launcher_navigation_checks(m, c, v);
	navigation_request_checks(m, c, v);
	top_pointer_checks(m, c, v, shown);
	// Opened from Practice's Settings row (or with no request file) there is no top level: Circle closes.
	c.show(0, false);
	check(!c.has_top && c.level == page::settings && c.poll(m, circle, 300) == effect::close, "no top level: Circle closes");
}

// Where clips can record, the top level offers Start recording below Settings, with the rows that stay in the game. Choosing it
// closes the menu as Return to Game does and asks for the recording; while one runs the row says Stop recording.
PBD_TEST(PbdSettingsMenu, RecordingChecks)
{
	auto m = sample_model();
	controller c;
	check(set_clips(m, true, false) && !set_clips(m, true, false), "a change of the clips state is reported once");
	const auto rows = shown_entries(m);
	check(rows.size() == entry_count && rows[2] == entry::record, "with clips every row shows, recording third");
	c.show(0, true);
	auto v = make_view(m, c, "Practice");
	check(v.entries.size() == entry_count, "the row says Start recording");
	c.poll(m, down, 10);
	c.poll(m, 0, 20);
	c.poll(m, down, 30);
	c.poll(m, 0, 40);
	v = make_view(m, c, "Practice");
	check(c.chosen() == entry::record && v.top_focus == 2 && v.help == entry_help(entry::record, "Practice"), "Down twice reaches it");
	check(c.poll(m, cross, 50) == effect::record && !c.open, "Cross closes the menu and asks for the recording");

	check(set_clips(m, true, true) && m.recording, "a recording runs");
	c.show(0, true);
	v = make_view(m, c, "Replay");
	check(entry_help(entry::record, "Replay", true) != entry_help(entry::record, "Replay"),
		"while it runs the row says Stop recording");
	const rect r = entry_rect(2, entry_count);
	check(c.pointer(m, r.x + r.w / 2.f, r.y + r.h / 2.f, true) == effect::record && !c.open, "a click on the row acts as Cross");

	// Clips that can no longer record take the row away; a focus left on it is on Settings, the row above.
	c.show(0, true);
	c.top_focus = at_entry(entry::record);
	check(set_clips(m, false, true) && !m.clips && !m.recording, "no clips, no recording");
	v = make_view(m, c, "Practice");
	check(v.entries.size() == entry_count - 1 && v.top_focus == 1 && c.focused_entry(m) == entry::settings, "the focus falls back to Settings");
	check(c.poll(m, cross, 60) == effect::none && c.level == page::settings, "Cross then opens Settings");
}

// Where the update latch injects the ports' words, the claim blanks only Practice's own controller: a replay's recorded
// fighters and a watcher's two ports keep the recorded input while the menu is open (the watcher's ports are checked
// against the source's applied words every frame, so a blanked frame would fault the watch).
PBD_TEST(PbdSettingsMenu, PortChecks)
{
	for (std::uint32_t port = 0; port < 2; ++port)
	{
		check(!blanks_port(false, true, port, false) && !blanks_port(false, true, port, true), "no claim blanks nothing");
		check(!blanks_port(true, false, port, false) && !blanks_port(true, false, port, true), "a replay's or a watcher's ports keep the recorded input");
		check(blanks_port(true, true, port, false), "Practice's ports read neutral while claimed");
	}
	check(blanks_port(true, true, 0, true) && !blanks_port(true, true, 1, true), "the dummy's playback keeps feeding P2 while it plays");
}

// The game window lets only the launcher's own process take the foreground: a malformed PBD_LAUNCHER_PID names nobody, and
// in particular never 0xFFFFFFFF (ASFW_ANY, every process).
PBD_TEST(PbdSettingsMenu, LauncherProcessChecks)
{
	check(parse_launcher_process_id("13584") == std::optional<std::uint32_t>(13584u), "a decimal process ID is read");
	check(parse_launcher_process_id("4294967294") == std::optional<std::uint32_t>(4294967294u), "the largest process ID is read");
	check(!parse_launcher_process_id("4294967295"), "ASFW_ANY is never a launcher");
	check(!parse_launcher_process_id("4294967296") && !parse_launcher_process_id("99999999999"), "beyond 32 bits names nobody");
	check(!parse_launcher_process_id("") && !parse_launcher_process_id("0"), "no ID or zero names nobody");
	check(!parse_launcher_process_id("-1") && !parse_launcher_process_id("+4") && !parse_launcher_process_id(" 4") &&
			  !parse_launcher_process_id("4 ") && !parse_launcher_process_id("0x10") && !parse_launcher_process_id("12a"),
		"only plain decimal digits");
}

// Settings' Controls page is the launcher's Button Layout: the controller, the game's actions in Tekken 8's order with the
// buttons that do them, Reset to default and Back, in one column.
static model controls_model()
{
	auto m = sample_model();
	m.current.input.device = m.start.input.device = "keyboard";
	m.controllers = {{"keyboard", "Keyboard"}, {"pad:test", "Arcade stick"}};
	return m;
}

// The menu open on the Controls page with `focus` on row `row_index`, and that row waiting for its button: Cross pressed on it
// and let go, as the player does.
static controller waiting_on(model& m, std::uint32_t row_index, std::uint64_t& now)
{
	controller c;
	c.show(0);
	c.level = page::controls;
	c.focus_controls(row_index);
	check(c.poll(m, cross, now += 10) == effect::none && c.capture.active, "Cross on an action makes its row wait");
	check(c.poll(m, cross, now += 10) == effect::none && c.capture.masked == cross, "the Cross that did it is not its button");
	check(c.poll(m, 0, now += 10) == effect::none && c.capture.masked == 0, "once let go, the next press of it counts");
	return c;
}

// A tap of `button` while a row waits: down, then let go.
static effect tap(controller& c, model& m, word button, std::uint64_t& now)
{
	check(c.poll(m, button, now += 10) == effect::none, "a button acts when it is let go, not before");
	return c.poll(m, 0, now += 10);
}

// Offline publication and menu changes preserve each device's layout; unmapped legacy devices retain the shown layout.
PBD_TEST(PbdSettingsMenu, OwnLayoutChecks)
{
	namespace input = pbd::controllers::settings;
	struct restore_settings
	{
		input::snapshot current;
		std::vector<input::change> requested;
		std::uint64_t revision, attacks;
		bool changed;
		restore_settings()
		{
			std::lock_guard lock(input::mutex);
			current = input::current;
			requested = input::requested;
			revision = input::revision;
			attacks = input::attack_masks.load();
			changed = input::mapping_changed.load();
		}
		~restore_settings()
		{
			std::lock_guard lock(input::mutex);
			input::current = std::move(current);
			input::requested = std::move(requested);
			input::revision = revision;
			input::attack_masks.store(attacks);
			input::mapping_changed.store(changed);
		}
	} restore;
	auto m = controls_model();
	input::layouts layouts;
	layouts.first[4] = "1+3";
	layouts.saved["pad:two"] = input::values{}.mapping;
	layouts.saved["pad:two"][0] = "1+2";
	layouts.saved["keyboard"] = input::values{}.mapping;
	layouts.saved["keyboard"][5] = "3+4";
	const auto devices = std::vector<input::device>{{"keyboard", "Keyboard"}, {"pad:test", "Arcade stick"}, {"pad:two", "Pad two"}};
	auto seen = input::current_revision();
	const auto reopen = [&](std::string_view device)
	{
		auto view = layouts.presentation(device, devices, "pad:two");
		input::publish(std::move(view.current), std::move(view.devices), seen);
		const auto published = input::read();
		m.current.input = published.current;
		m.controllers = published.devices;
	};
	reopen("pad:two");
	check(m.current.input.mapping == layouts.saved["pad:two"], "reopening takes the selected pad's effective mapping");
	controller c;
	c.show(0);
	c.level = page::controls;
	c.focus_controls(0);
	check(c.press(m, right) == effect::controls && m.current.input.device == "keyboard" && m.current.input.mapping == layouts.saved["keyboard"],
		"switching to Keyboard restores its own mapping");
	check(input::submit(m.current.input));
	for (const auto& change : input::take(seen))
		layouts.saved[change.device] = change.mapping;
	reopen("keyboard");
	check(m.current.input.mapping[5] == "3+4" && m.current.input.mapping[0] == "1", "Keyboard survives publication and reopen without a pad's macro");
	check(c.press(m, left) == effect::controls && m.current.input.device == "pad:two" && m.current.input.mapping == layouts.saved["pad:two"]);
	m.current.input.mapping[6] = "2+4";
	check(input::submit(m.current.input));
	for (const auto& change : input::take(seen))
		layouts.saved[change.device] = change.mapping;
	reopen("pad:two");
	check(m.current.input.mapping[0] == "1+2" && m.current.input.mapping[6] == "2+4", "editing one action preserves the pad's other macro");
	check(c.press(m, right) == effect::controls && m.current.input.mapping == layouts.saved["keyboard"] && m.current.input.mapping[5] == "3+4",
		"the pad's edit leaves Keyboard's macro intact");
	check(layouts.for_player("unmodified", 0)[4] == "1+3" && layouts.for_player("unmodified", 1)[4] == "off",
		"unedited devices keep the first-player launcher and second-player default layouts");
	m.controllers.push_back({"legacy", "Legacy controller"});
	const auto before_legacy = m.current.input.mapping;
	show_controller(m, "legacy");
	check(m.current.input.mapping == before_legacy, "a genuinely unmapped legacy device retains the shown layout");
	show_controller(m, "pad:two");
	check(m.current.input.device == "pad:two", "Button setup opens on its controller");
}

void capture_cancel_checks(model& m, controller& c, std::uint64_t& now)
{
	// Circle can be given a job by a tap; held, it cancels, and its release then does nothing in the rows.
	m = controls_model();
	c = waiting_on(m, 3, now);
	check(tap(c, m, circle, now) == effect::controls && m.current.input.mapping[3] == "3" && m.current.input.mapping[2] == "4",
		"a tap of Circle is a choice like any other");
	m = controls_model();
	c = waiting_on(m, 5, now);
	check(c.poll(m, circle, now += 10) == effect::none && c.capture.active, "Circle held");
	check(c.poll(m, circle, now += hold_cancel_ms - 20) == effect::none && c.capture.active, "not yet a cancel");
	check(c.poll(m, circle, now += 20) == effect::none && !c.capture.active, "held long enough, it cancels");
	check(c.poll(m, 0, now += 10) == effect::none && c.level == page::controls && c.open, "its release does not go back");
	check(m.current.input.mapping == controls_model().current.input.mapping, "nothing changed");

	// Start and a direction cancel at once; Esc and a right click (back) cancel without leaving the page.
	for (const word way : {start, up, right})
	{
		c = waiting_on(m, 6, now);
		check(c.poll(m, way, now += 10) == effect::none && !c.capture.active && c.level == page::controls, "Start or a direction cancels");
		c.poll(m, 0, now += 10);
	}
	c = waiting_on(m, 6, now);
	check(c.back(m) == effect::none && !c.capture.active && c.level == page::controls && c.open, "Esc or a right click stops the wait only");
}

void capture_assignment_checks(model& m, controller& c, std::uint64_t& now)
{
	// Select leaves a combination with no button; for an attack it says the attack needs one and keeps waiting.
	m.current.input.mapping[4] = "3+4";
	c = waiting_on(m, 6, now);
	check(c.poll(m, select, now += 10) == effect::controls && m.current.input.mapping[4] == "off" && !c.capture.active, "Select: 3+4 has no button");
	c.poll(m, 0, now += 10);
	c = waiting_on(m, 2, now);
	check(c.poll(m, select, now += 10) == effect::none && c.capture.active,
		"an attack keeps a button");
	c.poll(m, 0, now += 10);
	check(tap(c, m, 0x01, now) == effect::controls && m.current.input.mapping[6] == "2" && m.current.input.mapping[1] == "off",
		"L2 for Right Punch (2): Triangle is free");

	// Two buttons at once are no choice: the row waits until both are let go, then takes the next tap.
	m = controls_model();
	c = waiting_on(m, 7, now);
	check(c.poll(m, 0x04 | 0x08, now += 10) == effect::none, "two together are refused");
	c.poll(m, 0x08, now += 10);
	check(c.poll(m, 0, now += 10) == effect::none && c.capture.active && m.current.input.mapping[5] == "off", "letting go chooses nothing");
	check(c.poll(m, 0x04, now += 10) == effect::none && c.poll(m, 0x04 | 0x08, now += 10) == effect::none && c.capture.settle,
		"a second button while one is held is refused too");
	c.poll(m, 0, now += 10);
	check(tap(c, m, 0x08, now) == effect::controls && m.current.input.mapping[5] == "1+3", "then a tap counts");

	// A button that would leave an attack without one is refused; one that already does the action changes nothing.
	m = controls_model();
	c = waiting_on(m, 5, now);
	check(tap(c, m, 0x80, now) == effect::none && m.current.input.mapping[0] == "1",
		"Square is the only button for Left Punch (1)");
	c = waiting_on(m, 4, now);
	check(tap(c, m, circle, now) == effect::none, "Circle already does 4");
	auto combination = waiting_on(m, 9, now);
	check(tap(combination, m, 0x80, now) == effect::none && m.current.input.mapping[0] == "1", "the same rule for every combination");

	// A layout saved before each action had one button can give it two: the pressed one keeps it, the other is freed.
	m.current.input.mapping[5] = "4";
	c = waiting_on(m, 4, now);
	check(tap(c, m, circle, now) == effect::controls && m.current.input.mapping[5] == "off" && m.current.input.mapping[3] == "4",
		"Circle keeps 4 and R1 no longer does it");
}

void capture_stick_click_checks(model& m, controller& c, std::uint64_t& now)
{
	// L3 and R3 take an action as any button does. R3 with one no longer remembers the position in Practice, which the help
	// line says from the button's side; its own job and Take Control in a replay read the button before the layout.
	m = controls_model();
	c = waiting_on(m, 5, now);
	auto v3 = make_view(m, c, "Practice");
	check(v3.prompts.size() == 2 && v3.prompts[0].keys == "{select}",
		"a combination's row names Select for no button");
	check(tap(c, m, r3, now) == effect::controls && m.current.input.mapping[9] == "1+2",
		"R3 takes 1+2, and the player is told what it no longer does");
	c = waiting_on(m, 6, now);
	check(tap(c, m, l3, now) == effect::controls && m.current.input.mapping[8] == "3+4", "L3 takes 3+4");
	const auto decoded = pbd::json::detail::json_parser(changes_json(m)).parse();
	check(decoded.value);
	const auto* patch = decoded.value->find("patch");
	check(patch);
	const auto* controls = patch->find("controls");
	check(controls);
	const auto* mapping = controls->find("buttonMapping");
	check(mapping && pbd::json::detail::is(mapping->find("l3"), "3+4") && pbd::json::detail::is(mapping->find("r3"), "1+2"),
		"the stick clicks are saved by name");
	check(make_view(m, c, "Practice").rows[5].value == "{r3}", "R3's row shows its glyph");
}

void capture_other_button_checks(model& m, controller& c, std::uint64_t& now)
{
	// A button held on the other controller when the row started waiting never holds it up: only a fresh press counts.
	m = controls_model();
	c = controller{};
	c.show(0);
	c.level = page::controls;
	c.focus_controls(5);
	check(c.poll(m, cross | 0x01, now += 10) == effect::none && c.capture.active && c.capture.masked == (cross | 0x01), "L2 held elsewhere");
	c.poll(m, 0x01, now += 10);
	check(c.poll(m, 0x01 | 0x08, now += 10) == effect::none && c.capture.button == 0x08, "R1 counts though L2 is still down");
	check(c.poll(m, 0x01, now += 10) == effect::controls && m.current.input.mapping[5] == "1+2", "R1 let go: 1+2's button");
	c = waiting_on(m, 6, now);
	check(c.poll(m, 0x02, now += 10) == effect::none && c.poll(m, 0x02 | 0x04, now += 10) == effect::none && c.capture.settle,
		"two together still wait to be let go");
	check(c.poll(m, 0, now += 10) == effect::none && !c.capture.settle && c.capture.active, "once let go, the row waits again");
	c = waiting_on(m, 5, now);

	// The pointer: hovering keeps the waiting row, a click on it does nothing, a click anywhere else stops the wait.
	const auto centre = [](rect r)
	{
		return std::pair{r.x + r.w / 2.f, r.y + r.h / 2.f};
	};
	auto [ox, oy] = centre(controls_rect(10));
	check(c.pointer(m, ox, oy, false) == effect::none && c.controls_focus == 5 && c.capture.active, "hovering another row moves nothing");
	auto [wx, wy] = centre(controls_rect(5));
	check(c.pointer(m, wx, wy, true) == effect::none && c.capture.active, "a click on the waiting row keeps it waiting");
	check(c.pointer(m, ox, oy, true) == effect::none && !c.capture.active && c.controls_focus == 5, "a click elsewhere stops the wait");
	check(c.pointer(m, ox, oy, true) == effect::none && c.capture.active && c.controls_focus == 10, "and a click on a row makes it wait");
	check(c.wheel(m, true) == effect::none && c.controls_focus == 10 && c.capture.active, "the wheel does nothing while a row waits");
}

void capture_keyboard_checks(model& m, controller& c, std::uint64_t& now)
{
	// The keyboard: its keys are the launcher's to change, so a row never waits for a key here; a key's button would change the
	// layout every controller shares.
	pbd::controllers::display keyboard;
	keyboard.active = true;
	keyboard.family = "keyboard";
	pbd::controllers::publish(keyboard);
	m = controls_model();
	c = controller{};
	c.show(0);
	c.level = page::controls;
	c.focus_controls(10);
	check(c.press(m, cross) == effect::none && !c.capture.active,
		"Enter on an action waits for nothing");
	check(m.current.input.mapping == controls_model().current.input.mapping, "and changes nothing");
	pbd::controllers::publish({});
	c = waiting_on(m, 5, now);
	pbd::controllers::publish(keyboard);
	check(tap(c, m, 0x08, now) == effect::none && !c.capture.active &&
			  m.current.input.mapping == controls_model().current.input.mapping,
		"a key pressed while a row waits for a controller's button ends the wait and changes nothing");
	pbd::controllers::publish({});
}

PBD_TEST(PbdSettingsMenu, ControlsChecks)
{
	pbd::controllers::publish({});
	auto m = controls_model();
	controller c;
	c.show(0);
	c.focus = at(row::controls);
	check(c.press(m, cross) == effect::none && c.level == page::controls, "Settings reaches the Button Layout without leaving the game");
	check(c.press(m, right) == effect::controls && m.current.input.device == "pad:test", "the connected controller can be changed in game");
	auto v = make_view(m, c, "Practice");
	check(v.controls && v.row_count == controls_count && v.rows[0].value == "Arcade stick", "the controller row names the controller");
	check(v.rows[1].value == "{square}", "the first attack's button");
	check(v.rows[4].value == "{circle}", "the fourth attack's button");
	check(v.rows[5].label == "1+2" && v.rows[5].value == no_button && v.rows[15].label == "1+2+3+4",
		"then every combination, a dash for no button");
	m.current.input.mapping[4] = "1";
	check(make_view(m, c, "Practice").rows[1].value == "{square}/{l1}", "an action on two buttons shows both");
	pbd::controllers::display numbers;
	numbers.active = true;
	numbers.family = "numbers";
	pbd::controllers::publish(numbers);
	check(make_view(m, c, "Practice").rows[1].value == "Square, L1", "the 1 / 2 / 3 / 4 icons give way to the buttons' names");
	pbd::controllers::publish({});
	m.current.input.mapping[4] = "off";

	// Up and Down run down the one column and wrap at its ends: the controller, the fifteen actions, Reset to default and Back.
	// Left and Right change only the controller.
	c.press(m, down);
	check(c.controls_focus == 1, "Down from the controller reaches Left Punch (1)");
	check(c.press(m, right) == effect::none && c.press(m, left) == effect::none, "Left and Right do nothing on an action's row");
	check(c.controls_focus == 1 && m.current.input.device == "pad:test", "neither the focus nor the controller moves");
	for (int i = 0; i < 14; ++i)
		c.press(m, down);
	check(c.controls_focus == 15, "the actions run down one column to 1+2+3+4");
	c.press(m, down);
	check(c.controls_focus == controls_reset, "then Reset to default");
	c.press(m, down);
	check(c.controls_focus == controls_back, "then Back");
	c.press(m, down);
	check(c.controls_focus == 0, "Down from Back wraps to the controller");
	c.press(m, up);
	check(c.controls_focus == controls_back, "and Up from the controller to Back");

	// One column centred in the list column, its rows touching and the commands under a firmer rule, all of it inside the panel
	// above its bottom edge: nothing scrolls.
	for (std::uint32_t i = 1; i < controls_count; ++i)
	{
		const rect above = controls_rect(i - 1), box = controls_rect(i);
		check(box.x == above.x && box.w == above.w, "one column");
		check(box.y == above.y + above.h + (i == controls_reset ? layout_rule : 0.f), "each row right under the one before");
	}
	const rect head = controls_rect(0), foot = controls_rect(controls_back);
	check(head.x - list_left == list_left + list_width - head.x - head.w && head.w == layout_width, "centred in the list column");
	check(head.y == list_top && foot.y + foot.h <= 610.f, "inside the panel, above its edge");
	// A click on the header's left half steps back through the controllers, on its right half forward, as its chevrons show.
	check(c.pointer(m, head.x + 10.f, head.y + 5.f, true) == effect::controls && c.controls_focus == 0, "a click on the header");
	check(m.current.input.device == "keyboard", "on its left half goes back");
	check(c.pointer(m, head.x + head.w - 10.f, head.y + 5.f, true) == effect::controls && m.current.input.device == "pad:test",
		"on its right half forward");

	// The changes file keeps the launcher's button-by-button format.
	auto now = std::uint64_t{0};
	auto w = waiting_on(m, 5, now);
	check(tap(w, m, 0x08, now) == effect::controls && m.current.input.mapping[5] == "1+2", "R1 tapped is 1+2's button");
	const auto decoded = pbd::json::detail::json_parser(changes_json(m)).parse();
	check(decoded.value);
	check(pbd::json::detail::is(decoded.value->find("controllerLastUsed"), "pad:test"));
	const auto* patch = decoded.value->find("patch");
	check(patch);
	const auto* controls = patch->find("controls");
	check(controls);
	const auto* mapping = controls->find("buttonMapping");
	check(mapping && pbd::json::detail::is(mapping->find("r1"), "1+2"), "the layout is saved by button name");
	check(c.back(m) == effect::none && c.level == page::settings, "right-click returns from Controls to Settings");
	c.level = page::controls;
	c.controls_only = true;
	m.online = true;
	check(c.back(m) == effect::close && !c.open, "online Controls return directly to the game");
}

PBD_TEST(PbdSettingsMenu, CaptureChecks)
{
	pbd::controllers::publish({});
	auto m = controls_model();
	std::uint64_t now = 0;

	// A tap: the button takes the action; the button that had it takes the tapped button's old job, so no attack is lost.
	auto c = waiting_on(m, 1, now);
	auto v = make_view(m, c, "Practice");
	check(v.capturing &&
			  v.prompts.size() == 1 && v.prompts[0].keys == "{start}",
		"the waiting row and the help line say what to do, and how to stop");
	check(tap(c, m, 0x10, now) == effect::controls && m.current.input.mapping[0] == "2" && m.current.input.mapping[1] == "1" && !c.capture.active,
		"Triangle for Left Punch (1): Square takes 2");
	v = make_view(m, c, "Practice");
	check(!v.capturing,
		"the help line says what moved");
	check(c.poll(m, cross, now += 10) == effect::none && c.capture.active, "the next Cross makes the row wait again: the release never acted");

	capture_cancel_checks(m, c, now);
	capture_assignment_checks(m, c, now);
	capture_stick_click_checks(m, c, now);
	capture_other_button_checks(m, c, now);
	capture_keyboard_checks(m, c, now);
	// Without this PC's controller settings (an older input handler) the layout is the launcher's to change.
	m.controllers.clear();
	c = controller{};
	c.show(0);
	c.level = page::controls;
	c.focus_controls(3);
	check(c.press(m, cross) == effect::none && !c.capture.active,
		"no row waits where the menu cannot change the layout");
	// Closing the menu ends a wait.
	m = controls_model();
	c = waiting_on(m, 5, now);
	c.hide();
	check(!c.capture.active, "a closed menu has no waiting row");
}

PBD_TEST(PbdSettingsMenu, LayoutRuleChecks)
{
	using mapping = pbd::settings_menu::layout;
	mapping layout{"1", "2", "3", "4", "1+2", "off", "1+2", "off", "off", "off"};
	check(assign_slot(layout, "1+2", 5) == assigned::changed && layout == mapping{"1", "2", "3", "4", "off", "1+2", "off", "off", "off", "off"},
		"a combination moves to the pressed button; a second button that held it is freed");
	check(assign_slot(layout, "1+2", 0) == assigned::changed && layout == mapping{"1+2", "2", "3", "4", "off", "1", "off", "off", "off", "off"},
		"the button it came from takes the pressed button's old job");
	check(assign_slot(layout, "3+4", 1) == assigned::last_attack && layout[1] == "2", "an attack's last button keeps it");
	check(assign_slot(layout, "1", 5) == assigned::unchanged, "a button that does the action already changes nothing");
	layout[9] = "1";
	check(assign_slot(layout, "1", 5) == assigned::changed && layout[5] == "1" && layout[9] == "off",
		"and frees another button that did it too");
	check(assign_slot(layout, "4", 9) == assigned::changed && layout[9] == "4" && layout[3] == "off", "R3 takes an attack");
	check(slot_tokens(layout, "4", false) == "{r3}" && slot_tokens(layout, "4", true) == "R3", "by its glyph, or by name");
	check(is_attack("4") && !is_attack("1+2") && !is_attack("off"), "the attacks are 1 to 4");
	check(limbs("1") == 1u && limbs("2+4") == 0b1010u && limbs("1+2+3+4") == 0b1111u && limbs("off") == 0u,
		"the limbs each action lights");
}

// Default: the launcher's layout, with 1+2 on L1 for the keyboard.
PBD_TEST(PbdSettingsMenu, ResetChecks)
{
	auto m = controls_model();
	m.current.input.mapping = {"2", "1", "3", "4", "off", "1+2", "off", "off", "off", "1+3"};
	controller c;
	c.show(0);
	c.level = page::controls;
	c.focus_controls(controls_reset);
	check(c.press(m, cross) == effect::controls && m.current.input.mapping == layout{"1", "2", "3", "4", "1+2", "off", "off", "off", "off", "off"},
		"Reset to default, with O's 1+2 for the keyboard");
	check(c.press(m, cross) == effect::none, "a second reset changes nothing");
	m.current.input.device = "pad:test";
	check(c.press(m, cross) == effect::controls && m.current.input.mapping[4] == "off", "a pad's default has L1 free");
}
