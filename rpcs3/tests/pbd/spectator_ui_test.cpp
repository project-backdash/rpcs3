#include "pbd_test.h"

#include "Emu/Cell/pbd_spectator_ui.h"
#include <string>

using pbd::test::check;

namespace
{
	using namespace pbd::spectator_ui;
	pbd::set_channel::json_value json(const std::string& text)
	{
		auto parsed = pbd::set_channel::detail::json_parser(text).parse();
		check(parsed.value);
		return *parsed.value;
	}
	pbd::set_channel::result<state> parse_text(const std::string& fields, u64 previous = 0)
	{
		return parse(json("{\"type\":\"watch_ui\"," + fields + "}"), previous);
	}
	const std::string base = "\"revision\":1,\"bar\":true,\"firstTo\":2,\"unread\":0";
	const std::string chat = ",\"chat\":{\"tabs\":[\"All\",\"Lobby\",\"Direct\"],\"tab\":1,\"to\":\"To: Lobby\","
							 "\"lines\":[{\"author\":\"Rook\",\"text\":\"gg\"},{\"text\":\"Kai beat LowParry 2-1.\",\"system\":true}],"
							 "\"quick\":[\"gg\",\"One more?\",\"Nice!\"],\"selected\":1}";
	void minimal_state_checks()
	{
		// The minimal screen: the four required fields, nothing open.
		const auto plain = parse_text(base);
		check(plain.value && plain.value->revision == 1 && plain.value->bar && plain.value->first_to == 2 && plain.value->unread == 0);
		check(!plain.value->new_line && !plain.value->chat && !plain.value->profiles && !plain.value->cards);
		// null means absent.
		check(parse_text(base + ",\"preview\":null,\"chat\":null,\"profiles\":null,\"cards\":null").value);
		// Every required field, and a revision that only grows.
		check(!parse_text("\"bar\":true,\"firstTo\":2,\"unread\":0").value);
		check(!parse_text("\"revision\":1,\"firstTo\":2,\"unread\":0").value);
		check(!parse_text("\"revision\":1,\"bar\":1,\"firstTo\":2,\"unread\":0").value);
		check(!parse_text(base, 1).value && parse_text("\"revision\":2,\"bar\":false,\"firstTo\":0,\"unread\":99", 1).value);
		check(!parse_text("\"revision\":2,\"bar\":false,\"firstTo\":11,\"unread\":0", 1).value);
		check(!parse_text("\"revision\":2,\"bar\":false,\"firstTo\":2,\"unread\":100", 1).value);
		check(parse_text(base).error.empty() && parse_text(base, 5).error == "watch_ui_invalid");
	}

	void panel_state_checks()
	{
		// The preview, the chat panel, the profiles and the VS cards' origins, with their bounds.
		const auto shown = parse_text("\"revision\":1,\"bar\":true,\"firstTo\":2,\"unread\":3,\"preview\":{\"id\":\"msg-7\",\"author\":\"Velvet\",\"text\":\"Rook is one hit away\"}" + chat +
									  ",\"profiles\":{\"left\":{\"name\":\"Kai\",\"lines\":[\"EN \xC2\xB7 N. America\",\"Tonight: 3 sets, 2 won\"]},\"right\":{\"name\":\"Rook\"}}" +
									  ",\"cards\":{\"left\":{\"language\":\"EN\",\"region\":\"N. America\"},\"right\":{\"language\":\"\",\"region\":\"\"}}");
		check(shown.value);
		const auto& ui = *shown.value;
		check(ui.new_line && ui.new_line->id == "msg-7" && ui.new_line->author == "Velvet" && ui.new_line->text == "Rook is one hit away");
		check(ui.chat && ui.chat->tabs.size() == 3 && ui.chat->tab == 1 && ui.chat->to == "To: Lobby" && ui.chat->lines.size() == 2);
		check(ui.chat->lines[0].author == "Rook" && !ui.chat->lines[0].system && ui.chat->lines[1].system && ui.chat->lines[1].author.empty());
		check(ui.chat->quick.size() == 3 && ui.chat->selected == 1);
		check(ui.profiles && (*ui.profiles)[0].name == "Kai" && (*ui.profiles)[0].lines.size() == 2 && (*ui.profiles)[1].lines.empty());
		check(ui.cards && (*ui.cards)[0].region == "N. America" && (*ui.cards)[1].language.empty());
		// A preview needs an identifier and a text; control characters and over-long text refuse the whole control.
		check(!parse_text(base + ",\"preview\":{\"author\":\"V\",\"text\":\"hi\"}").value);
		check(!parse_text(base + ",\"preview\":{\"id\":\"a b\",\"text\":\"hi\"}").value);
		check(!parse_text(base + ",\"preview\":{\"id\":\"m\",\"text\":\"\"}").value);
		check(!parse_text(base + ",\"preview\":{\"id\":\"m\",\"text\":\"a\\u0007b\"}").value);
		check(!parse_text(base + ",\"preview\":{\"id\":\"m\",\"text\":\"" + std::string(line_bytes + 1, 'x') + "\"}").value);
		check(parse_text(base + ",\"preview\":{\"id\":\"m\",\"text\":\"" + std::string(line_bytes, 'x') + "\"}").value);
		// The chat's tab and selected= name entries that exist; at most six Quick lines and eight log lines.
		check(!parse_text(base + ",\"chat\":{\"tabs\":[\"All\"],\"tab\":1}").value);
		check(parse_text(base + ",\"chat\":{}").value);
		check(!parse_text(base + ",\"chat\":{\"quick\":[\"gg\"],\"selected\":1}").value);
		check(!parse_text(base + ",\"chat\":{\"quick\":[\"1\",\"2\",\"3\",\"4\",\"5\",\"6\",\"7\"]}").value);
		check(!parse_text(base + ",\"chat\":{\"quick\":[\"\"]}").value);
		check(!parse_text(base + ",\"chat\":{\"lines\":[{\"author\":\"x\"}]}").value);
		std::string nine = ",\"chat\":{\"lines\":[";
		for (int i = 0; i < 9; ++i)
			nine += std::string(i ? "," : "") + "{\"text\":\"l\"}";
		check(!parse_text(base + nine + "]}").value);
		// Profiles and cards name both sides.
		check(!parse_text(base + ",\"profiles\":{\"left\":{\"name\":\"Kai\"}}").value);
		check(!parse_text(base + ",\"profiles\":{\"left\":{\"name\":\"\"},\"right\":{\"name\":\"R\"}}").value);
		check(!parse_text(base + ",\"cards\":{\"left\":{\"language\":\"EN\"},\"right\":{\"language\":\"\",\"region\":\"\"}}").value);
		check(!parse_text(base + ",\"cards\":{\"left\":{\"language\":\"" + std::string(language_bytes + 1, 'x') + "\",\"region\":\"\"},\"right\":{\"language\":\"\",\"region\":\"\"}}").value);
	}

	void fight_visibility_checks()
	{
		// The phase: only a fight on screen shows the watcher's screen (from round 1, never in intros or a K.O.).
		check(fight_on_screen(16, 5) && fight_on_screen(17, 5));
		for (const u32 pending : {0u, 1u, 2u, 3u, 4u, 6u, 9u, 12u, 37u})
			check(!fight_on_screen(16, pending) && !fight_on_screen(17, pending));
		check(!fight_on_screen(8, 5) && !fight_on_screen(3, 5) && !fight_on_screen(12, 5));
	}

	void local_button_checks()
	{
		// Only new presses count; a watcher's Start remains local display input.
		pad_reader reader;
		const auto closed = parse_text(base).value;
		check(reader.read(pad::circle, &*closed, true).empty());
		check(reader.read(pad::circle, &*closed, true).empty());
		check(reader.read(0, &*closed, true).empty());
		const auto stop = reader.read(pad::circle, &*closed, true);
		check(stop.size() == 1 && stop[0].which == button::circle && !stop[0].chat_open && !stop[0].quick_line);
		const auto skip = reader.read(pad::circle | pad::start | pad::select | pad::l2, &*closed, true);
		check(skip.size() == 1 && skip[0].which == button::start && button_name(skip[0].which) == "start");
		check(reader.read(pad::circle | pad::start, &*closed, true).empty());
		check(reader.read(0, &*closed, true).empty());
		// Several at once come back in a fixed order; Cross with no chat open is nothing.
		const auto many = reader.read(pad::cross | pad::r1 | pad::square | pad::r2 | pad::l1 | pad::triangle, &*closed, true);
		check(many.size() == 5 && many[0].which == button::square && many[1].which == button::r2 && many[2].which == button::triangle &&
			  many[3].which == button::l1 && many[4].which == button::r1);
		check(reader.read(0, &*closed, true).empty());
		// Intro and win screens must still allow local skip and stop.
		const auto hidden = reader.read(pad::circle | pad::start | pad::square | pad::r2, &*closed, false);
		check(hidden.size() == 2 && hidden[0].which == button::circle && hidden[1].which == button::start);
		check(reader.read(0, nullptr, true).empty());
		check(reader.read(pad::square, nullptr, true).size() == 1);
	}

	void held_start_checks()
	{
		pad_reader reader;
		check(reader.read(pad::start, nullptr, false).empty());
		check(reader.read(pad::start, nullptr, false).empty());
		reader.read(0, nullptr, false);
		check(reader.read(pad::start, nullptr, false, true).empty());
		check(reader.read(pad::start, nullptr, false).empty());
		reader.read(0, nullptr, false);
		const auto skip = reader.read(pad::start, nullptr, false);
		check(skip.size() == 1 && skip[0].which == button::start && !skip[0].quick_line);
	}

	void chat_navigation_checks()
	{
		// The open chat: the highlight starts on selected=, the directions move it within the Quick lines, Cross reports it.
		pad_reader reader;
		const auto open = parse_text("\"revision\":1,\"bar\":true,\"firstTo\":2,\"unread\":0" + chat).value;
		reader.apply(*open);
		check(reader.highlight() == 1);
		check(reader.read(0, &*open, true).empty());
		check(reader.read(pad::right, &*open, true).empty() && reader.highlight() == 2);
		check(reader.read(0, &*open, true).empty());
		check(reader.read(pad::down, &*open, true).empty() && reader.highlight() == 2); // the last line stays
		check(reader.read(0, &*open, true).empty());
		const auto send = reader.read(pad::cross, &*open, true);
		check(send.size() == 1 && send[0].which == button::cross && send[0].chat_open && send[0].quick_line == 2u);
		check(reader.read(0, &*open, true).empty());
		check(reader.read(pad::left, &*open, true).empty() && reader.highlight() == 1);
		check(reader.read(0, &*open, true).empty());
		const auto end = reader.read(pad::circle, &*open, true);
		check(end.size() == 1 && end[0].which == button::circle && end[0].chat_open);
		// The same panel sent again (a new log line) keeps the watcher's highlight; other Quick lines or another selected= start
		// over; a closed chat clears it.
		auto again = *open;
		again.revision = 2;
		again.chat->lines.push_back({"Kai", "nice", false});
		reader.apply(again);
		check(reader.highlight() == 1);
		check(reader.read(0, &again, true).empty() && reader.read(pad::left, &again, true).empty() && reader.highlight() == 0);
		reader.apply(again);
		check(reader.highlight() == 0);
		auto other = again;
		other.chat->selected = 2;
		reader.apply(other);
		check(reader.highlight() == 2);
		auto closed = again;
		closed.chat.reset();
		reader.apply(closed);
		check(reader.highlight() == 0);
		// A chat with no Quick lines sends nothing on Cross.
		auto empty = again;
		empty.chat->quick.clear();
		empty.chat->selected = 0;
		reader.apply(empty);
		check(reader.read(0, &empty, true).empty() && reader.read(pad::cross, &empty, true).empty());

		std::string wide_chat = ",\"chat\":{\"quick\":[";
		for (u32 i = 0; i < max_quick; ++i)
			wide_chat += std::string(i ? "," : "") + "\"" + std::string(quick_bytes - 1, 'W') + std::to_string(i) + "\"";
		const auto wide = parse_text(base + wide_chat + "],\"selected\":5}").value;
		check(wide);
		reader.apply(*wide);
		check(reader.highlight() == 5);
		// Six wide replies fit two at a time with these font measurements, so navigation must move the visible window.
		constexpr std::array<float, max_quick> widths{300.f, 300.f, 300.f, 300.f, 300.f, 300.f};
		const auto visible = [&]
		{
			const auto chips = quick_layout(widths, reader.highlight(), 452.f, 140.f);
			check(chips.size() == 2);
			bool highlighted = false;
			for (const auto& chip : chips)
			{
				check(chip.row < 2 && chip.x >= 0.f && chip.x + widths[chip.index] <= 452.f);
				highlighted |= chip.index == reader.highlight();
			}
			check(highlighted);
			return chips;
		};
		check(visible().front().index == 4);
		const auto send_highlight = [&]
		{
			reader.read(0, &*wide, true);
			const auto sent = reader.read(pad::cross, &*wide, true);
			check(sent.size() == 1 && sent[0].which == button::cross && sent[0].quick_line == reader.highlight());
		};
		send_highlight();
		for (const auto direction : {pad::left, pad::right})
		{
			for (u32 step = 1; step < max_quick; ++step)
			{
				reader.read(0, &*wide, true);
				check(reader.read(direction, &*wide, true).empty());
				check(reader.highlight() == (direction == pad::left ? max_quick - 1 - step : step));
				visible();
				send_highlight();
			}
		}
	}

	void menu_capture_checks()
	{
		// The in-game menu captures Circle and directions so they cannot also stop the watch or move the Quick lines.
		// A button held across menu closure needs a release before another press can be reported.
		pad_reader reader;
		const auto open = parse_text("\"revision\":1,\"bar\":true,\"firstTo\":2,\"unread\":0" + chat).value;
		reader.apply(*open);
		check(reader.read(0, &*open, true).empty());
		check(reader.read(pad::circle, &*open, true, true).empty());
		check(reader.read(pad::down | pad::cross, &*open, true, true).empty() && reader.highlight() == 1);
		check(reader.read(pad::circle, &*open, false, true).empty());
		// The menu has let go with Circle still down.
		check(reader.read(pad::circle, &*open, true).empty());
		check(reader.read(0, &*open, true).empty());
		const auto end = reader.read(pad::circle, &*open, true);
		check(end.size() == 1 && end[0].which == button::circle && end[0].chat_open);
		// A first read under the menu primes the reader too: a held button is never a press when the menu closes.
		pad_reader fresh;
		check(fresh.read(pad::square, nullptr, true, true).empty() && fresh.read(pad::square, nullptr, true).empty());
		check(fresh.read(0, nullptr, true).empty() && fresh.read(pad::square, nullptr, true).size() == 1);
	}

	void score_and_bar_checks()
	{
		// The score line, the bar and the preview's fade.
		view shown;
		check(score_line(shown).empty());
		shown.game = 7;
		shown.first_to = 5;
		shown.left_score = 3;
		shown.right_score = 4;
		shown.left_name = "Kai";
		shown.right_name = "Rook";
		const auto named_score = score_line(shown);
		check(named_score.find(shown.left_name) != std::string::npos && named_score.find(shown.right_name) != std::string::npos);
		shown.left_name.clear();
		shown.right_name.clear();
		const auto anonymous_score = score_line(shown);
		check(anonymous_score.find("Kai") == std::string::npos && anonymous_score.find("Rook") == std::string::npos);
		for (const auto value : {shown.game, shown.first_to, shown.left_score, shown.right_score})
			check(named_score.find(std::to_string(value)) != std::string::npos && anonymous_score.find(std::to_string(value)) != std::string::npos);
		shown.unread = 3;
		const auto bar = bar_items(shown);
		check(bar.size() == 3 && bar[0].keys == "{circle}" && bar[1].keys == "{square}" && bar[1].badge == 3 && bar[2].keys == "{r2}");
		shown.chat = chat_panel{};
		shown.chat->tabs = {"All", "Lobby"};
		shown.chat->quick = {"gg"};
		const auto open = bar_items(shown);
		check(open.size() == 4 && open[0].keys == "{cross}" && open[1].keys == "{circle}" && open[2].keys == "{triangle}" && open[3].keys == "{l1}/{r1}");
		shown.chat->tabs.clear();
		shown.chat->quick.clear();
		check(bar_items(shown).size() == 2);
		shown.chat.reset();
		shown.profiles = std::array<profile, 2>{};
		const auto profiles = bar_items(shown);
		check(profiles.size() == 3 && profiles[0].keys == "{r2}" && profiles[1].keys == "{square}" && profiles[1].badge == 3 && profiles[2].keys == "{circle}");
		check(preview_opacity(1000, 1000) == 1.f && preview_opacity(1000, 1000 + preview_ms - fade_ms) == 1.f);
		check(preview_opacity(1000, 1000 + preview_ms - fade_ms / 2) == .5f && preview_opacity(1000, 1000 + preview_ms) == 0.f);
		check(preview_opacity(5000, 1000) == 1.f);
	}

	void view_publication_checks()
	{
		// Publication: an equal view does not move the revision; another emulation's view is never loaded.
		view first;
		first.present = true;
		first.game = 1;
		const auto before = revision();
		publish(7, first);
		check(revision() == before + 1 && load(7).present && !load(8).present);
		publish(7, first);
		check(revision() == before + 1);
	}
} // namespace

PBD_TEST(PbdSpectatorUi, Checks)
{
	minimal_state_checks();
	panel_state_checks();
	fight_visibility_checks();
	local_button_checks();
	held_start_checks();
	chat_navigation_checks();
	menu_capture_checks();
	score_and_bar_checks();
	view_publication_checks();
}
