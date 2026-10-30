#pragma once

// Scenarios run by spectator_channel_test.cpp.
static void watch_screen_and_game_visibility()
{
	{
		// The watcher's screen: watch_ui sends the whole screen, the watched game's phase decides when it shows, and the watcher's
		// presses become spectator-pad-N.json files with their own serial.
		namespace ui = pbd::spectator_ui;
		sample_document f(26, true);
		f.prepare();
		f.begin(1, 0, nullptr, true, "");
		f.control("\"type\":\"presentation\",\"hidden\":false,\"catchUp\":false,\"mode\":\"watch\",\"leftName\":\"Kai\",\"rightName\":\"Rook\",\"score\":{\"left\":1,\"right\":0},\"delayFrames\":180");
		f.control("\"type\":\"watch_ui\",\"revision\":1,\"bar\":true,\"firstTo\":2,\"unread\":2,\"preview\":{\"id\":\"m1\",\"author\":\"Velvet\",\"text\":\"close one\"}");
		check(f.input.screen() && f.input.screen()->unread == 2 && f.input.screen()->first_to == 2);
		const auto first = ui::load(9);
		check(first.present && !first.shown && first.bar && first.game == 1 && first.left_name == "Kai" && first.left_score == 1);
		// The preview waits to show: its fade starts when the screen first shows it, not while the cover is up.
		check(first.new_line && first.new_line->id == "m1" && !first.new_line_since_ms);
		// Before the intro nothing shows, whatever the phase, and only Circle reports.
		f.input.observe_fight(true);
		check(!ui::load(9).shown);
		f.input.read_pad(0);
		f.input.read_pad(ui::pad::square | ui::pad::circle);
		check(f.input.pad_events() == 1);
		// Exposed, the screen shows in a fight and hides at a K.O.
		f.packet(1, 1, 0, 0);
		check(f.input.poll_input(test_hash));
		f.play(0, true, {5, 40, 41});
		check(ui::load(9).shown);
		const auto since = ui::load(9).new_line_since_ms;
		check(since);
		f.input.observe_fight(false);
		check(!ui::load(9).shown);
		f.input.observe_fight(true);
		check(ui::load(9).shown && ui::load(9).new_line_since_ms == since);
		f.input.read_pad(0);
		f.input.read_pad(ui::pad::square);
		check(f.input.pad_events() == 2);
		// The in-game menu (Esc) holds the controller: the screen gives way to it, and its Circle (back, close) writes no pad event,
		// so it never also stops the watch; Circle still down when the menu lets go is no press, and the screen shows again.
		f.input.read_pad(0, true);
		check(!ui::load(9).shown);
		f.input.read_pad(ui::pad::square | ui::pad::r2, true);
		f.input.read_pad(ui::pad::circle, true);
		check(f.input.pad_events() == 2 && !ui::load(9).shown);
		f.input.read_pad(ui::pad::circle);
		check(f.input.pad_events() == 2 && ui::load(9).shown && ui::load(9).new_line_since_ms == since);
		const auto pad_file = [&](u64 serial)
		{
			std::ifstream in(f.directory / ("spectator-pad-" + std::to_string(serial) + ".json"), std::ios::binary);
			check(in);
			auto parsed = json_parser(std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>())).parse();
			check(parsed.value);
			return std::move(*parsed.value);
		};
		const auto circle = pad_file(1), square = pad_file(2);
		std::uint32_t number = 0;
		bool flag = false;
		check(is(circle.find("schema"), "pbd-spectator-pad-1") && number32(circle.find("serial"), 1, 1, number) &&
			  is(circle.find("button"), "circle") && pbd::json::detail::boolean(circle.find("shown"), flag) && !flag &&
			  number32(circle.find("uiRevision"), 1, 1, number));
		check(is(square.find("button"), "square") && pbd::json::detail::boolean(square.find("shown"), flag) && flag &&
			  pbd::json::detail::boolean(square.find("chatOpen"), flag) && !flag && !square.find("quickLine"));
		// The chat opens: the same preview keeps its time; the highlight starts on selected=, the directions move it, and Cross
		// reports the highlighted Quick line.
		f.control("\"type\":\"watch_ui\",\"revision\":2,\"bar\":true,\"firstTo\":2,\"unread\":0,\"preview\":{\"id\":\"m1\",\"author\":\"Velvet\",\"text\":\"close one\"},"
				  "\"chat\":{\"quick\":[\"gg\",\"Nice!\"],\"selected\":0}");
		check(ui::load(9).new_line_since_ms == since && ui::load(9).chat && ui::load(9).highlight == 0);
		f.input.read_pad(0);
		f.input.read_pad(ui::pad::right);
		check(ui::load(9).highlight == 1 && f.input.pad_events() == 2);
		f.input.read_pad(0);
		f.input.read_pad(ui::pad::cross);
		const auto cross = pad_file(3);
		check(f.input.pad_events() == 3 && is(cross.find("button"), "cross") && pbd::json::detail::boolean(cross.find("chatOpen"), flag) && flag &&
			  number32(cross.find("quickLine"), 1, 1, number) && number32(cross.find("uiRevision"), 2, 2, number));
		// A revision that does not grow refuses like any invalid control.
		f.control("\"type\":\"watch_ui\",\"revision\":2,\"bar\":true,\"firstTo\":2,\"unread\":0", false);
		check(f.input.reason() == "watch_ui_invalid" && !ui::load(9).shown);
	}
}

static void input_deadline_and_wait_controls()
{
	{
		// The input wait: 60 s with nothing ends the watch; a waiting control for the game the watch waits for moves the deadline
		// to its arrival plus 60 s; a stall after the last one, and ten minutes without a game, end it with their own reasons.
		using clock = std::chrono::steady_clock;
		const auto from = clock::time_point{} + std::chrono::hours(1);
		const auto at = [&](int seconds)
		{
			return from + std::chrono::seconds(seconds);
		};
		check(input_wait_outcome(from, std::nullopt, at(59)) == wait_outcome::waiting);
		check(input_wait_outcome(from, std::nullopt, at(60)) == wait_outcome::buffer_timeout);
		check(input_wait_outcome(from, at(50), at(100)) == wait_outcome::waiting);
		check(input_wait_outcome(from, at(50), at(110)) == wait_outcome::source_stalled);
		// A marker from before this wait does not count.
		check(input_wait_outcome(from, from - std::chrono::seconds(1), at(60)) == wait_outcome::buffer_timeout);
		check(input_wait_outcome(from, at(590), at(600)) == wait_outcome::wait_limit);
		check(std::string_view(wait_reason(wait_outcome::buffer_timeout)) == "spectator_buffer_timeout" &&
			  std::string_view(wait_reason(wait_outcome::source_stalled)) == "spectator_source_stalled" &&
			  std::string_view(wait_reason(wait_outcome::wait_limit)) == "spectator_wait_limit");
	}
	{
		// The waiting control counts only for the game the watch waits for, while it waits; the cover says what it waits for, and
		// that game's begin_game clears it.
		sample_document f(27, true);
		f.prepare();
		f.begin(1, 0, nullptr, true, "");
		f.packet(1, 1, 0, 0);
		check(f.input.poll_input(test_hash));
		f.play(0, true, {5, 40, 41});
		check(!f.input.between_games());
		f.control("\"type\":\"waiting\",\"gameNumber\":2,\"phase\":\"between_games\"");
		check(!f.input.alive_at() && load_presentation(9).waiting_title.empty());
		f.play(1);
		f.control("\"type\":\"end_game\",\"gameNumber\":1,\"finalFrame\":2");
		check(f.input.end(2, true));
		check(f.input.between_games());
		f.control("\"type\":\"waiting\",\"gameNumber\":3,\"phase\":\"next_game\"");
		check(!f.input.alive_at());
		f.control("\"type\":\"waiting\",\"gameNumber\":2,\"phase\":\"between_games\"");
		const auto between = load_presentation(9);
		check(f.input.alive_at() && between.hidden && !between.waiting_title.empty() && between.waiting_text.find("2") != std::string::npos);
		const auto first = *f.input.alive_at();
		f.control("\"type\":\"waiting\",\"gameNumber\":2,\"phase\":\"next_game\"");
		const auto choosing = load_presentation(9);
		check(*f.input.alive_at() >= first && choosing.hidden && !choosing.waiting_title.empty() && choosing.waiting_text.find("2") != std::string::npos);
		f.begin(2, 2, nullptr, true, "");
		check(!f.input.alive_at() && load_presentation(9).waiting_title.empty() && load_presentation(9).waiting_text.empty() && !f.input.between_games());
		// A malformed marker refuses.
		f.control("\"type\":\"waiting\",\"gameNumber\":3,\"phase\":\"lunch\"", false);
		check(f.input.reason() == "waiting_invalid");
	}
}

static void publication_and_receipt_collisions()
{
	{
		// Complete publication refuses an occupied serial; a dropped display press can retry the same serial after release.
		namespace ui = pbd::spectator_ui;
		sample_document f(28, true);
		f.prepare();
		f.begin(1, 0, nullptr, true, "");
		const auto name = "spectator-pad-1.json";
		f.write(name, "Occupied serial");
		f.input.read_pad(0);
		f.input.read_pad(ui::pad::start);
		check(f.input.healthy() && f.input.pad_events() == 0);
		std::ifstream old(f.directory / name, std::ios::binary);
		check(std::string(std::istreambuf_iterator<char>(old), std::istreambuf_iterator<char>()) == "Occupied serial");
		old.close();
		check(std::filesystem::remove(f.directory / name));
		f.input.read_pad(0);
		f.input.read_pad(ui::pad::start);
		check(f.input.pad_events() == 1);
		std::ifstream published(f.directory / name, std::ios::binary);
		const std::string text(std::istreambuf_iterator<char>(published), {});
		const auto report = json_parser(text).parse();
		bool shown = true;
		check(report.value && is(report.value->find("button"), "start") && pbd::json::detail::boolean(report.value->find("shown"), shown) && !shown &&
			  is(report.value->find("generation"), "123") && is(report.value->find("bindingDigest"), hex(identity().binding)));
		f.input.read_pad(ui::pad::start);
		check(f.input.pad_events() == 1);
	}
	{
		// An occupied native receipt cannot be replaced or advance the actual consumption cursor.
		sample_document f(29, true);
		f.prepare();
		f.begin(1, 0, nullptr, true, "");
		f.write("spectator-native-2.json", "Occupied serial");
		f.packet(1, 1, 0, 0);
		check(!f.input.poll_input(test_hash) && !f.input.healthy() && f.input.consumed() == 0);
		check(f.report(2) == "Occupied serial");
	}
	{
		// A pre-existing temporary report also refuses publication without replacing its bytes.
		sample_document f(30, true);
		f.prepare();
		f.begin(1, 0, nullptr, true, "");
		const auto name = ".spectator-native-2.tmp";
		f.write(name, "Occupied temporary");
		f.packet(1, 1, 0, 0);
		check(!f.input.poll_input(test_hash) && !f.input.healthy() && f.input.consumed() == 0);
		std::ifstream old(f.directory / name, std::ios::binary);
		check(std::string(std::istreambuf_iterator<char>(old), std::istreambuf_iterator<char>()) == "Occupied temporary");
	}
}
