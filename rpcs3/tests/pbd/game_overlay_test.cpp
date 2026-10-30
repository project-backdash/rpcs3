#include "pbd_test.h"

#include "Emu/Cell/pbd_game_overlay.h"
#include "Emu/Cell/pbd_session_wait.h"
#include "Emu/PBD/pbd_practice_match_prompt.h"

#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

using pbd::test::check;

namespace
{
	// Sets a variable for this process, or removes it when value is null.
	void put(const char* name, const char* value)
	{
#ifdef _WIN32
		_putenv_s(name, value ? value : "");
#else
		if (value)
			setenv(name, value, 1);
		else
			unsetenv(name);
#endif
	}

	// Puts the named switches back as they were, even when a failed check ends the test.
	struct saved_switches
	{
		std::vector<std::pair<const char*, std::optional<std::string>>> values;
		saved_switches(std::initializer_list<const char*> names)
		{
			for (const char* name : names)
			{
				const char* value = pbd::env::find(name);
				values.emplace_back(name, value ? std::optional<std::string>(value) : std::nullopt);
			}
		}
		~saved_switches()
		{
			for (const auto& [name, value] : values)
				put(name, value ? value->c_str() : nullptr);
		}
	};
} // namespace

// The renderer makes the wait notices for a two-PC session configured at launch and for a game held ready for a match, whose
// role arrives with the match bind after the overlays exist.
PBD_TEST(PbdGameOverlay, SessionWaitRequestedInWarmGame)
{
	const saved_switches restore{"PBD_PEER_ROLE", "PBD_MATCH_STANDBY_CHANNEL"};
	put("PBD_PEER_ROLE", nullptr);
	put("PBD_MATCH_STANDBY_CHANNEL", nullptr);
	check(!pbd::session_wait::requested(), "a game with no session makes no wait notices");
	put("PBD_PEER_ROLE", "");
	put("PBD_MATCH_STANDBY_CHANNEL", "");
	check(!pbd::session_wait::requested(), "empty switches are off");
	put("PBD_PEER_ROLE", "0");
	check(pbd::session_wait::requested(), "a two-PC session configured at launch");
	put("PBD_PEER_ROLE", nullptr);
	const std::string channel = (std::filesystem::temp_directory_path() / "pbd-held-match").string();
	put("PBD_MATCH_STANDBY_CHANNEL", channel.c_str());
	check(pbd::session_wait::requested(), "a game held ready for a match");
}

PBD_TEST(PbdGameOverlay, RecoveryWaitDoesNotTreatFightButtonsAsLeave)
{
	namespace modal = pbd::session_wait;
	for (const auto kind : {modal::leave_kind::set, modal::leave_kind::session})
	{
		modal::leave_read read;
		for (unsigned ms = 0; ms <= 90250; ms += 125)
		{
			const auto pad = ms % 250 ? std::uint16_t{0x00f0} : std::uint16_t{0};
			read = modal::read_leave(std::chrono::milliseconds(ms), kind, pad, read);
			check(!read.chosen && read.held_ms < 2000, "an attack press ended the protected recovery wait");
		}
	}
	// A held fight input cannot arm Leave, even after the whole recovery window.
	for (const auto kind : {modal::leave_kind::set, modal::leave_kind::session})
	{
		modal::leave_read read;
		for (unsigned ms = 0; ms <= 90250; ms += 125)
		{
			read = modal::read_leave(std::chrono::milliseconds(ms), kind, modal::leave_button, read);
			check(!read.chosen && !read.released && read.held_ms == 0);
		}
	}
}

PBD_TEST(PbdGameOverlay, WaitLeaveRequiresAnUninterruptedTwoSecondHoldAndResetsItsFill)
{
	namespace modal = pbd::session_wait;
	using std::chrono::milliseconds;
	for (const auto kind : {modal::leave_kind::set, modal::leave_kind::session})
	{
		auto read = modal::read_leave(milliseconds(30000), kind, 0, {});
		check(read.offered && read.released && !read.chosen);
		read = modal::read_leave(milliseconds(30005), kind, modal::leave_button, read);
		read = modal::read_leave(milliseconds(31005), kind, modal::leave_button, read);
		check(!read.chosen && read.held_ms == 1000);
		read = modal::read_leave(milliseconds(31010), kind, 0, read);
		check(!read.chosen && read.held_ms == 0 && !read.held_from);
		read = modal::read_leave(milliseconds(31015), kind, modal::leave_button, read);
		read = modal::read_leave(milliseconds(33014), kind, modal::leave_button, read);
		check(!read.chosen && read.held_ms == 1999);
		read = modal::read_leave(milliseconds(33015), kind, modal::leave_button, read);
		check(read.chosen && read.held_ms == 2000);
		check(!modal::read_leave(milliseconds(33020), modal::leave_kind::none, modal::leave_button, read).chosen);
		read = modal::read_leave(milliseconds(0), kind, modal::leave_button, read);
		check(!read.offered && !read.chosen && !read.released && !read.held_from && read.held_ms == 0);
	}
}

PBD_TEST(PbdGameOverlay, DeliberateMenuWaitRequiresContinuingPolls)
{
	using pbd::game_overlay::hold_live;
	constexpr std::uint64_t minute = 60'000'000'000ull;
	check(hold_live(3 * minute, 3 * minute - 5'000'000));
	check(hold_live(3 * minute, 3 * minute - 1'000'000'000));
	check(!hold_live(3 * minute, 3 * minute - 1'000'000'001));
	check(!hold_live(3 * minute, minute));
	check(!hold_live(3 * minute, 0));
	check(!hold_live(minute, minute + 1));
}

PBD_TEST(PbdGameOverlay, ChallengerOnlyInterruptsLocalPracticeForBoundedOffers)
{
	using namespace pbd::game_overlay;
	using pbd::practice::match_prompt::interrupts;
	const auto offer = parse("PBD-MENU-1\ngeneration=1\nvisible=1\nkind=challenger\ntitle=A%20NEW%20CHALLENGER\n"
							 "row=1|Join%20Battle|\nrow=1|Decline|\ntimer=25000|24000|Join%20Battle||0\n");
	check(offer.value && offer.value->kind == panel_kind::challenger);
	auto panel = *offer.value;
	for (const auto duration : {15000u, 20000u, 24000u})
	{
		panel.timer.total_ms = duration;
		check(interrupts(panel, true, false));
		check(!interrupts(panel, false, false) && !interrupts(panel, true, true));
	}
	panel.timer.total_ms = 24001;
	check(!interrupts(panel, true, false));
	panel.row_count = 0;
	panel.timer.total_ms = 1000;
	check(interrupts(panel, true, false));
	check(!pbd::practice::match_prompt::expired(panel, 24999));
	check(pbd::practice::match_prompt::expired(panel, 25000));
	panel.practice = true;
	check(!interrupts(panel, true, false));
	panel.practice = false;
	panel.timer.present = false;
	check(!interrupts(panel, true, false));
}

PBD_TEST(PbdGameOverlay, RightClickBackBelongsToItsPanel)
{
	using namespace pbd::game_overlay;
	snapshot panel;
	panel.visible = true;
	panel.generation = 41;
	panel.back = "Back";
	publish(panel);
	check(request_back() && take_back(41));
	check(!take_back(41));
	check(request_back());
	++panel.generation;
	publish(panel);
	check(!take_back(42) && !take_back(41));
	panel.practice = true;
	publish(panel);
	check(!request_back());
	panel.practice = false;
	panel.back.clear();
	publish(panel);
	check(!request_back());
	publish(snapshot{});
	check(!request_back());
}

// rank=: the settled rank change on Set complete, its words and its place in the panel.
PBD_TEST(PbdGameOverlay, RankChangeOnSetComplete)
{
	using namespace pbd::game_overlay;
	const std::string over = "PBD-MENU-1\ngeneration=4\nvisible=1\ntitle=Set%20complete\nscore=2|LRL|A|B\nsubtitle=A%20wins\n";
	const auto parse_rank = [&](const std::string& value)
	{
		return parse(over + "rank=" + value + "\n");
	};
	const auto up = parse_rank("up|11|1216|+11");
	check(up.value && up.value->rank == rank_result{rank_change::up, 11, 1216, 11});
	const auto down = parse_rank("down|10|1190|-9");
	check(down.value && down.value->rank == rank_result{rank_change::down, 10, 1190, -9});
	const auto placed = parse_rank("attained|0|650|0");
	check(placed.value && placed.value->rank == rank_result{rank_change::attained, 0, 650, 0});
	check(parse_rank("up|34|99999|9999").value && parse_rank("up|34|2400|11").value->rank.change == 11);
	// A panel without the key has no rank change.
	check(parse(over).value->rank.kind == rank_change::none);
	for (const auto bad : {"promoted|11|1216|11", "up|35|1216|11", "up|11|100000|11", "up|11|1216|10000", "up|11|1216|--1", "up|11|1216|",
			 "up|11|1216", "up|11|1216|1|x", "up|x|1216|1", "UP|11|1216|1", "up|11|-5|1", "up|11|1216|+"})
		check(parse_rank(bad).error == "bad_rank");
	check(parse(over + "rank=up|11|1216|11\nrank=up|11|1216|11\n").error == "bad_rank");

	// The selection card replaces the panel and draws none of it.
	const auto card = selection_card(*parse(over + "rank=up|11|1216|11\n").value);
	check(card.rank.kind == rank_change::none);
}

// The latch reads a rank's badge from the launcher's folder: rank_NN.png, a PNG of at most 2 MiB, or nothing.
PBD_TEST(PbdGameOverlay, RankBadgeFromTheLaunchersFolder)
{
	using namespace pbd::game_overlay;
	const auto parent = std::filesystem::temp_directory_path();
	const auto directory = parent / ("pbd-rank-badges-test-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
	check(std::filesystem::create_directory(directory));
	const auto write = [&](const std::string& name, const std::string& bytes)
	{
		std::ofstream file(directory / name, std::ios::binary);
		file << bytes;
	};
	const std::string png = std::string("\x89PNG\r\n\x1a\n") + std::string(25, 'x');
	write("rank_11.png", png);
	write("rank_03.png", png);
	write("rank_12.png", "GIF89a" + std::string(40, 'x'));
	write("rank_13.png", png.substr(0, 8));
	{
		const auto badge = read_rank_badge(directory, 11);
		check(badge && badge->size() == 33 && (*badge)[1] == 'P');
		check(read_rank_badge(directory, 3) && !read_rank_badge(directory, 30));
		check(!read_rank_badge(directory, 12) && !read_rank_badge(directory, 13) && !read_rank_badge(directory, 35));
		check(!read_rank_badge({}, 11) && !read_rank_badge(std::filesystem::path("rank-badges"), 11));
	}
	const auto cleanup = std::filesystem::canonical(directory);
	check(std::filesystem::equivalent(cleanup.parent_path(), std::filesystem::canonical(parent)) &&
		  cleanup.filename().string().starts_with("pbd-rank-badges-test-"));
	std::filesystem::remove_all(cleanup);
}

// The Ranked notices' golden files (ranked-notices-1): the launcher writes these bytes from the same panels, and the game reads
// them back as these values.
namespace
{
	// __FILE__ is relative to the repository when the build compiles relative paths, and the tests run from the build output
	// folder, so the fixture is looked up from the working folder and each folder above it.
	std::string notice_fixture(std::string_view name)
	{
		const auto relative = std::filesystem::path(__FILE__).parent_path() / "fixtures" / "notices" / name;
		for (auto folder = std::filesystem::current_path();; folder = folder.parent_path())
		{
			if (std::ifstream file(folder / relative, std::ios::binary); file)
				return {std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
			if (folder == folder.parent_path())
				return {};
		}
	}
} // namespace

PBD_TEST(PbdGameOverlay, RankedNoticesGoldenFiles)
{
	using namespace pbd::game_overlay;
	{
		const auto parsed = parse(notice_fixture("set-complete-placing-flip.pbd"));
		check(parsed.value, parsed.error);
		const auto& panel = *parsed.value;
		check(panel.kind == panel_kind::post_set && panel.command_serial == 7 && panel.you == 'L' && panel.title == "YOU WIN");
		check(panel.placement == placing{true, 2, 3, 5});
		check(panel.rival == "Kai wants a rematch" && panel.link == link_line{true, 4, "Smooth \xC2\xB7 38 ms"});
		check(panel.row_count == 2 && panel.rows[1].label == "NEXT OPPONENT" && panel.score.left == "Lili P" && panel.score.games == "LRL");
		check(panel.rank.kind == rank_change::none && !panel.countdown_row && !panel.card.present && !panel.placement_pill);
	}
	{
		const auto parsed = parse(notice_fixture("between-games-ranked.pbd"));
		check(parsed.value, parsed.error);
		const auto& panel = *parsed.value;
		check(panel.kind == panel_kind::menu && panel.you == 'R' && panel.row_count == 3 && panel.rows[1].label == "REMATCH \xC2\xB7 RANDOM STAGE");
		check(panel.timer.present && panel.timer.total_ms == 15000 && panel.timer.detail == "Kai is choosing");
		check(panel.rival == "Kai is choosing" && panel.countdown_row == 0u && !panel.placement.present && !panel.link.present);
	}
	{
		const auto parsed = parse(notice_fixture("challenger-ranked-placing.pbd"));
		check(parsed.value, parsed.error);
		const auto& panel = *parsed.value;
		check(panel.kind == panel_kind::challenger && panel.timer.present && panel.timer.total_ms == 15000 && panel.countdown_row == 0u);
		check(panel.card.present && panel.card.character == "SERGEI DRAGUNOV" && panel.card.name == "Kai");
		check(panel.card.shown == plate{plate::kind::rank, 15, 0, 0});
		check(panel.ping == ping_line{true, 2, 142} && panel.placement_pill);
		check(panel.rows[1].help == "Two declines in 24 h pause Ranked." && panel.subtitle == "Kai \xC2\xB7 Sergei Dragunov");
	}
	{
		const auto parsed = parse(notice_fixture("set-complete-same.pbd"));
		check(parsed.value, parsed.error);
		check(parsed.value->rank == rank_result{rank_change::same, 15, 1301, 9} && !parsed.value->placement.present);
	}
	// The same panels with CRLF line ends read the same.
	auto crlf = notice_fixture("set-complete-placing-flip.pbd");
	for (std::size_t at = crlf.find('\n'); at != std::string::npos; at = crlf.find('\n', at + 2))
		crlf.insert(at, 1, '\r');
	check(parse(crlf).value && parse(crlf).value->placement == placing{true, 2, 3, 5});
}

// Each notice key: its limits, the kind and keys it needs, and that it is accepted once.
PBD_TEST(PbdGameOverlay, RankedNoticeKeys)
{
	using namespace pbd::game_overlay;
	const std::string set = "PBD-MENU-1\ngeneration=4\nvisible=1\nkind=post_set\ntitle=YOU%20WIN\nrow=1|REMATCH|\nrow=1|NEXT%20OPPONENT|\nscore=2|LL|A|B\n";
	const auto error = [&](const std::string& lines, const std::string& head = {})
	{
		return parse((head.empty() ? set : head) + lines).error;
	};
	// placing=<before>|<after>|<total>: 1..10 sets, before <= after <= total, one set at most between them; post_set and you=.
	for (const auto good : {"0|0|5", "2|3|5", "4|4|4", "9|10|10", "0|1|1"})
		check(error("you=L\nplacing=" + std::string(good) + "\n").empty(), good);
	for (const auto bad : {"0|0|0", "0|0|11", "3|2|5", "2|4|5", "5|6|5", "1|1", "1|1|1|1", "x|1|5", "-1|0|5", "1|2|"})
		check(error("you=L\nplacing=" + std::string(bad) + "\n") == "bad_placing", bad);
	check(error("placing=2|3|5\n") == "bad_placing" && error("you=L\nplacing=2|3|5\nplacing=2|3|5\n") == "bad_placing");
	const std::string menu = "PBD-MENU-1\ngeneration=4\nvisible=1\nkind=menu\ntitle=NEXT%20GAME\nrow=1|REMATCH|\nscore=2|L|A|B\nyou=L\n";
	check(error("placing=2|3|5\n", menu) == "bad_placing");
	// rival=: 1..80 bytes, with you= (or on a challenger).
	check(error("you=R\nrival=Kai%20left\n").empty() && error("rival=Kai%20left\n") == "bad_rival");
	check(error("you=R\nrival=\n") == "bad_rival" && error("you=R\nrival=" + std::string(81, 'x') + "\n") == "bad_rival");
	check(error("you=R\nrival=" + std::string(80, 'x') + "\n").empty() && error("you=R\nrival=a\nrival=b\n") == "bad_rival");
	// link=<bars 0..4>|<words>: post_set only.
	check(error("link=0|Rough%20%C2%B7%20try%20a%20wired%20connection\n").empty() && error("link=4|Smooth\n", menu) == "bad_link");
	for (const auto bad : {"5|Smooth", "4|", "4", "x|Smooth", "4|Smooth|x"})
		check(error("link=" + std::string(bad) + "\n") == "bad_link", bad);
	check(error("link=1|" + std::string(49, 'x') + "\n") == "bad_link" && error("link=1|" + std::string(48, 'x') + "\n").empty());
	// countdown_row=: needs timer= and a row it names.
	const std::string timer = "timer=5|15000|Rematch||0\n";
	check(error(timer + "countdown_row=1\n").empty() && error(timer + "countdown_row=2\n") == "bad_countdown_row");
	check(error("countdown_row=0\n") == "bad_countdown_row" && error(timer + "countdown_row=x\n") == "bad_countdown_row");
	// card=, ping= and pill=: a challenger's only.
	const std::string challenger = "PBD-MENU-1\ngeneration=4\nvisible=1\nkind=challenger\ntitle=A%20NEW%20CHALLENGER\nrow=1|JOIN%20BATTLE|\n";
	const auto card_of = [&](const std::string& value)
	{
		return parse(challenger + "card=" + value + "\n");
	};
	check(card_of("LILI|Ana|p2/5").value->card.shown == plate{plate::kind::placing, 0, 2, 5});
	check(card_of("|Ana|-").value->card.shown == plate{} && card_of("|Ana|-").value->card.character.empty());
	check(card_of("JIN|Ana|r0").value->card.shown == plate{plate::kind::rank, 0, 0, 0});
	for (const auto bad : {"LILI||r1", "LILI|Ana", "LILI|Ana|r35", "LILI|Ana|p6/5", "LILI|Ana|p1/11", "LILI|Ana|p1/0", "LILI|Ana|x", "LILI|Ana|r",
			 "LILI|Ana|p1", "LILI|Ana|", "LILI%C3%A9|Ana|r1"})
		check(card_of(bad).error == "bad_card", bad);
	check(card_of(std::string(25, 'X') + "|Ana|r1").error == "bad_card" && card_of(std::string(24, 'X') + "|Ana|r1").value);
	check(card_of("LILI|" + std::string(49, 'x') + "|r1").error == "bad_card");
	check(error("card=LILI|Ana|r1\n") == "bad_card");
	check(parse(challenger + "ping=3|96\n").value->ping == ping_line{true, 3, 96});
	for (const auto bad : {"0|96", "5|96", "3|10000", "3", "3|x"})
		check(parse(challenger + "ping=" + std::string(bad) + "\n").error == "bad_ping", bad);
	check(error("ping=2|142\n") == "bad_ping");
	check(parse(challenger + "pill=placement\n").value->placement_pill && parse(challenger + "pill=ranked\n").error == "bad_pill");
	check(error("pill=placement\n") == "bad_pill" && parse(challenger + "rival=Waiting%20for%20Kai\n").value->rival == "Waiting for Kai");
	// rank=same, and the selection card draws none of the notices.
	check(error("rank=same|15|1301|9\n").empty());
	auto full = *parse(notice_fixture("set-complete-placing-flip.pbd")).value;
	const auto card = selection_card(full);
	check(!card.placement.present && card.rival.empty() && !card.link.present && !card.countdown_row && !card.card.present && !card.placement_pill);
}

PBD_TEST(PbdGameOverlay, Checks)
{
	using namespace pbd::game_overlay;
	const auto good = parse(
		"PBD-MENU-1\n"
		"generation=12\nvisible=1\nkind=drawer\nselected=1\nclose=1\n"
		"title=Change%20Fighter\nsubtitle=Next%20game\nconfirm=Choose\nback=Back\n"
		"row=1|Stay|Keep%20Armor%20King\nrow=1|Choose|Open%20character%20select\n");
	check(good.value);
	check(good.value->generation == 12);
	check(good.value->kind == panel_kind::drawer);
	check(good.value->selected == 1);
	check(good.value->rows[1].detail == "Open character select");

	for (const auto kind : {"menu", "drawer", "prompt", "text", "challenger"})
		check(parse("PBD-MENU-1\ngeneration=1\nvisible=1\nkind=" + std::string(kind) + "\ntitle=Panel\n").value);
	// The prompt bar uses the Practice glyph family; a launcher's glyphs= key is rejected.
	check(parse("PBD-MENU-1\ngeneration=1\nvisible=1\nglyphs=xbox\ntitle=Panel\n").error == "unknown_key");

	std::string full = "PBD-MENU-1\ngeneration=2\nvisible=1\ntitle=Full\nselected=7\n";
	for (std::size_t i = 0; i < max_file_rows; i++)
		full += "row=1|Row|Detail\n";
	check(parse(full).value && parse(full).value->row_count == max_file_rows);
	full += "row=1|Extra|Rejected\n";
	check(!parse(full).value);

	check(!parse("PBD-MENU-1\ngeneration=1\nvisible=1\ntitle=x\nselected=1\nrow=1|Only|\n").value);
	check(!parse("PBD-MENU-1\ngeneration=1\nvisible=1\ntitle=bad%0Atext\n").value);
	check(!parse("PBD-MENU-1\ngeneration=1\nvisible=1\ntitle=bad%ZZtext\n").value);
	check(!parse("PBD-MENU-1\ngeneration=1\ngeneration=2\nvisible=0\n").value);
	check(!parse("PBD-MENU-1\ngeneration=1\nvisible=0\nunknown=1\n").value);
	check(!parse(std::string(max_file_bytes + 1, 'x')).value);
	const auto hidden = parse("PBD-MENU-1\ngeneration=9\nvisible=0\nkind=text\n");
	check(hidden.value && !hidden.value->visible && hidden.value->title.empty());

	// score=<first to>|<games>|<left>|<right>: one letter per decided game in order; never a game after the set was decided.
	{
		const auto with_score = [](std::string_view value)
		{
			return parse("PBD-MENU-1\ngeneration=3\nvisible=1\ntitle=You%20win%20game%201\nrow=1|Rematch|07\nscore=" + std::string(value) + "\n");
		};
		const auto between = with_score("2|L|Ana|Kai");
		check(between.value && between.value->score.present && between.value->score.first_to == 2 && between.value->score.games == "L");
		check(between.value->score.left == "Ana" && between.value->score.right == "Kai");
		check(between.value->score.wins(false) == 1 && between.value->score.wins(true) == 0);
		const auto over = with_score("2|LRL|Ana|Kai%20II");
		check(over.value && over.value->score.wins(false) == 2 && over.value->score.wins(true) == 1 && over.value->score.right == "Kai II");
		check(with_score("1||A|B").value && with_score("10|LLLLLLLLLRRRRRRRRR|A|B").value);
		check(with_score("10|LLLLLLLLLRRRRRRRRRL|A|B").value);
		for (const auto bad : {"0||A|B", "11||A|B", "2|LLR|A|B", "2|RRL|A|B", "2|LX|A|B", "2|l|A|B", "2|L|A", "2|L||B", "2|L|A|",
				 "2|L|A%0A|B", "x|L|A|B", "2|L|A|B|C"})
			check(!with_score(bad).value);
		check(!parse("PBD-MENU-1\ngeneration=3\nvisible=1\ntitle=T\nscore=1||A|B\nscore=1||A|B\n").value);
		check(!parse("PBD-MENU-1\ngeneration=3\nvisible=1\ntitle=T\n").value->score.present);
		// The automatic pre-selection's card never carries the panel's score.
		check(!selection_card(*over.value).score.present);
	}
	// A countdown republished every second continues the shown panel: same kind and rows (labels and enabled states), any
	// detail. The player's highlight stays while the file repeats its selected=; a new selected= or new rows start over.
	{
		const auto panel = [](std::string_view rematch_detail, std::string_view selected, std::string_view second_label = "Return%20to%20Lobby")
		{
			return *parse("PBD-MENU-1\ngeneration=5\nvisible=1\ntitle=You%20win%20game%201\nselected=" + std::string(selected) +
						  "\nrow=1|Rematch|" + std::string(rematch_detail) + "\nrow=1|" + std::string(second_label) + "|\nscore=2|L|Ana|Kai\n")
			            .value;
		};
		auto shown = panel("07", "0");
		shown.selected = 1; // the player moved to Return to Lobby
		const auto tick = panel("06", "0");
		check(continues(shown, tick) && republished_highlight(shown, 0, tick) == 1);
		// The launcher moves the highlight on purpose by writing a different selected=.
		check(republished_highlight(shown, 0, panel("06", "1")) == 1 && republished_highlight(shown, 1, panel("06", "0")) == 0);
		// A changed label, a changed row count or kind, a hidden panel on either side: a new panel at its own selected=.
		const auto relabelled = panel("06", "0", "Leave");
		check(!continues(shown, relabelled) && republished_highlight(shown, 0, relabelled) == 0);
		auto one_row = tick;
		one_row.row_count = 1;
		check(!continues(shown, one_row));
		auto drawer_kind = tick;
		drawer_kind.kind = panel_kind::drawer;
		check(!continues(shown, drawer_kind));
		auto hidden_panel = shown;
		hidden_panel.visible = false;
		check(!continues(hidden_panel, tick) && republished_highlight(hidden_panel, 0, tick) == 0);
		// A row that became unavailable is not kept highlighted (its enabled state changed, so this is a new panel anyway).
		auto disabled = tick;
		disabled.rows[1].enabled = false;
		check(!continues(shown, disabled) && republished_highlight(shown, 0, disabled) == 0);
		// The pre-selection card never continues a launcher panel.
		check(!continues(selection_card(shown), tick) && !continues(shown, selection_card(tick)));
	}
	// The lobby panel keys: each accepted, with its limits and the keys it needs.
	{
		const std::string head = "PBD-MENU-1\ngeneration=4\nvisible=1\ntitle=You%20lose%20game%202\n";
		const std::string score = "score=2|LR|Kai|Ana\n";
		const auto panel = parse(head + score +
								 "row=1|Rematch|Same%20stage|Game%203%20with%20the%20same%20fighters%20on%20the%20same%20stage.\n" +
								 "row=1|Rematch%20with%20random%20stage|Random%20stage\n" +
								 "fighters=Hwoarang|Jin%20Kazama\nyou=R\ntimer=1800000010000|10000|Rematch%20in|Kai%20is%20choosing|0\n" +
								 "status=Waiting%20for%20%7Bcross%7D\nnext=Stays%20on%20for%20the%20next%20set|Back%20of%20the%20line\n");
		check(panel.value);
		const auto& value = *panel.value;
		check(value.rows[0].detail == "Same stage" && value.rows[0].help == "Game 3 with the same fighters on the same stage.");
		check(value.rows[1].detail == "Random stage" && value.rows[1].help.empty());
		check(value.fighters[0] == "Hwoarang" && value.fighters[1] == "Jin Kazama" && value.you == 'R');
		check(value.timer.present && value.timer.deadline_ms == 1800000010000ull && value.timer.total_ms == 10000);
		check(value.timer.title == "Rematch in" && value.timer.detail == "Kai is choosing" && !value.timer.ready);
		check(value.status == "Waiting for {cross}" && value.next[0] == "Stays on for the next set" && value.next[1] == "Back of the line");
		check(!value.chosen && !value.question.present && !value.game_count);
		// Refusals.
		const auto error = [&](const std::string& lines, bool with_score = true)
		{
			return parse(head + (with_score ? score : "") + lines).error;
		};
		check(error("row=1|A|B|C|D\n") == "bad_row" && error("row=1|A\n") == "bad_row" && error("row=2|A|B\n") == "bad_row");
		check(error("row=1|A|B|" + std::string(161, 'x') + "\n") == "bad_row_text" && error("row=1||B\n") == "bad_row_text");
		check(error("fighters=" + std::string(33, 'x') + "|B\n") == "bad_fighters" && error("fighters=A\n") == "bad_fighters");
		check(error("fighters=A|B\nfighters=A|B\n") == "bad_fighters" && error("fighters=|\n").empty());
		check(error("you=X\n") == "bad_you" && error("you=L\n").empty());
		check(error("timer=0|10000|T||0\n") == "bad_timer" && error("timer=5|0|T||0\n") == "bad_timer");
		check(error("timer=5|600001|T||0\n") == "bad_timer" && error("timer=5|600000|T||0\n").empty());
		check(error("timer=5|10|||0\n") == "bad_timer" && error("timer=5|10|T||2\n") == "bad_timer" && error("timer=5|10|T|\n") == "bad_timer");
		check(error("timer=5|10|" + std::string(41, 'x') + "||1\n") == "bad_timer" && error("timer=5|10|T|" + std::string(65, 'x') + "|1\n") == "bad_timer");
		check(error("status=" + std::string(161, 'x') + "\n") == "bad_status");
		check(error("next=" + std::string(49, 'x') + "|B\n") == "bad_next");
		// Keys that describe the scoreboard's set need it.
		for (const auto key : {"fighters=A|B\n", "you=L\n", "next=A|B\n", "game=1|L|3|0|Stage\n"})
			check(error(key, false) == "missing_score");
		// chosen= marks a row of a panel on which nothing can be chosen any more.
		check(error("row=0|Rematch|Your%20choice\nrow=0|Exit|\nchosen=0\n").empty());
		check(error("row=0|Rematch|Your%20choice\nrow=1|Exit|\nchosen=0\n").empty());
		check(error("row=1|Rematch|Your%20choice\nrow=0|Exit|\nchosen=0\n") == "bad_chosen");
		check(error("row=0|Rematch|\nchosen=1\n") == "bad_chosen" && error("chosen=x\n") == "bad_chosen");
		// dialog= with its answers as rows and the panel's own rows as base= lines.
		const auto question = parse(head + "dialog=Return%20to%20Lobby%3F|You%20forfeit%20the%20set.\nrow=1|Return%20to%20Lobby|\nrow=1|Back|\n" +
									"base=1|Rematch|Same%20stage\nbase=0|Exit|Forfeits\nselected=1\n");
		check(question.value && question.value->question.present && question.value->question.message == "Return to Lobby?");
		check(question.value->base_count == 2 && question.value->base[1].label == "Exit" && !question.value->base[1].enabled);
		check(error("base=1|Rematch|\n") == "bad_base" && error("dialog=|x\n") == "bad_dialog" && error("dialog=x\n") == "bad_dialog");
		std::string nine_bases = "dialog=Q|\nrow=1|A|\n";
		for (int i = 0; i < 9; i++)
			nine_bases += "base=1|B|\n";
		check(error(nine_bases) == "bad_base");
		// game= lines in play order: n is the decided games before it plus one, a draw keeps the number, the letters are score='s.
		const std::string over = "PBD-MENU-1\ngeneration=4\nvisible=1\ntitle=Set%20complete\nscore=2|LRR|A|B\n";
		const auto games = parse(over + "game=1|L|2|1|Hokkaido%20Shrine\ngame=2|D|3|3|Waterfall\ngame=2|R|2|0|Waterfall\ngame=3|R|2|1|Waterfall\n");
		check(games.value && games.value->game_count == 4 && games.value->games[1].winner == 'D' && games.value->games[3].stage == "Waterfall");
		check(parse(over + "game=1|L|2|1|S\ngame=3|R|2|0|S\ngame=3|R|2|1|S\n").error == "bad_game");
		check(parse(over + "game=1|L|2|1|S\ngame=2|L|2|0|S\ngame=3|R|2|1|S\n").error == "bad_game");
		check(parse(over + "game=1|L|2|1|S\ngame=2|R|2|0|S\n").error == "bad_game");
		check(parse(over + "game=1|L|1|2|S\ngame=2|R|2|0|S\ngame=3|R|2|1|S\n").error == "bad_game");
		check(parse(over + "game=1|X|2|1|S\n").error == "bad_game" && parse(over + "game=1|L|10|1|S\n").error == "bad_game");
		check(parse(over + "game=1|L|2|1|" + std::string(41, 'x') + "\n").error == "bad_game");
	}
	// The band drains continuously toward its deadline: whole seconds rounded up (at most 99) and the share left.
	{
		countdown band;
		band.present = true;
		band.deadline_ms = 100000;
		band.total_ms = 10000;
		check(countdown_at(band, 90000).seconds == 10 && countdown_at(band, 90000).share == 1.f);
		check(countdown_at(band, 93001).seconds == 7 && countdown_at(band, 93000).seconds == 7 && countdown_at(band, 92999).seconds == 8);
		check(countdown_at(band, 95000).share == .5f && countdown_at(band, 100000).seconds == 0 && countdown_at(band, 100000).share == 0.f);
		check(countdown_at(band, 200000).seconds == 0 && countdown_at(band, 80000).share == 1.f);
		band.total_ms = 600000;
		band.deadline_ms = 700000;
		check(countdown_at(band, 100000).seconds == 99);
	}
	// A republish that changes only the timer or the status keeps the panel (and the player's highlight); the menu becoming its
	// waiting panel is a new one, since every row's enabled state changes.
	{
		const auto panel_of = [](const std::string& lines)
		{
			return *parse("PBD-MENU-1\ngeneration=4\nvisible=1\ntitle=T\n" + lines).value;
		};
		const std::string rows = "row=1|Rematch|Same%20stage\nrow=1|Exit|\n";
		auto shown = panel_of(rows + "timer=5000|10000|Rematch%20in|O%20is%20choosing|0\n");
		shown.selected = 1;
		const auto ready = panel_of(rows + "timer=5000|10000|Rematch%20in|O%20is%20ready|1\n");
		check(continues(shown, ready) && republished_highlight(shown, 0, ready) == 1);
		const auto waiting = panel_of("row=0|Rematch|Your%20choice\nrow=0|Exit|\nchosen=0\nstatus=Waiting\n");
		check(!continues(shown, waiting));
		const auto status = panel_of("row=0|Rematch|Your%20choice\nrow=0|Exit|\nchosen=0\nstatus=O%20is%20ready\n");
		check(continues(waiting, status));
	}
	// The selection card clears everything the panel it replaces drew.
	{
		const auto everything = parse("PBD-MENU-1\ngeneration=4\nvisible=1\ntitle=Set%20complete\nscore=1|L|A|B\nfighters=X|Y\nyou=L\n"
									  "timer=5|10|T||0\nstatus=S\nnext=N|M\ngame=1|L|3|0|S\ndialog=Q|D\nrow=1|Yes|\nbase=1|B|\n");
		check(everything.value);
		const auto card = selection_card(*everything.value);
		check(card.selection_transition);
		check(!card.score.present && card.fighters[0].empty() && !card.you && !card.timer.present && card.status.empty());
		check(!card.question.present && !card.base_count && !card.game_count && card.next[0].empty() && !card.row_count && !card.chosen);
	}
	publish(*good.value);
	const auto copy = load();
	check(copy.generation == 12 && copy.title == "Change Fighter");
	check(event_text(12, 3, "confirm", 1) ==
		  "PBD-EVENT-1\ngeneration=12\nserial=3\naction=confirm\nselected=1\n");

	// The waiting modal's name (PBD_PEER_NAME): trimmed, control characters dropped, at most 32 characters, and the
	// generic line when it is empty or not UTF-8.
	{
		namespace modal = pbd::session_wait;
		check(modal::clean_name("").empty() && modal::clean_name("   ").empty() && modal::clean_name("  Kai  ") == "Kai");
		check(modal::clean_name("A\tB\x7f"
								"C") == "ABC");
		check(modal::clean_name(std::string(40, 'x')) == std::string(32, 'x'));
		std::string accents;
		for (int i = 0; i < 33; i++)
			accents += "\xC3\xA9";
		check(modal::clean_name(accents) == accents.substr(0, 64) && modal::clean_name("Jos\xC3\xA9") == "Jos\xC3\xA9");
		check(modal::clean_name("bad\xC3").empty() && modal::clean_name("\xC0\xAF").empty() && modal::clean_name("\xED\xA0\x80").empty());
		check(modal::utf16_to_utf8(u"Jos\u00e9") == "Jos\xC3\xA9" && modal::utf16_to_utf8(u"\U0001F600") == "\xF0\x9F\x98\x80");
		const char16_t lone[] = {0xd800, 0x41, 0};
		check(modal::utf16_to_utf8(lone).empty());
		check(modal::show_after.count() == 500 && modal::leave_after.count() == 30);

		// How long this PC has waited, and the way out: Leave after 30 s, which ends a lobby set with no result or a friend
		// session; no Leave outside a two-PC session.
		check(modal::elapsed(0) == "0:00" && modal::elapsed(12) == "0:12" && modal::elapsed(65) == "1:05" && modal::elapsed(720) == "12:00");
		check(!modal::leave_open(29, modal::leave_kind::set) && modal::leave_open(30, modal::leave_kind::set));
		check(!modal::leave_open(300, modal::leave_kind::none));

		// Circle held into the offer must be released, then held for two seconds; a tap never leaves.
		using std::chrono::milliseconds;
		constexpr std::uint16_t circle = modal::leave_button, cross_button = 0x0040;
		auto read = modal::read_leave(milliseconds(29999), modal::leave_kind::set, circle, {});
		check(!read.offered && !read.chosen);
		read = modal::read_leave(milliseconds(30000), modal::leave_kind::set, circle, read);
		check(read.offered && !read.chosen && !read.released);
		read = modal::read_leave(milliseconds(30005), modal::leave_kind::set, circle, read);
		check(read.offered && !read.chosen);
		read = modal::read_leave(milliseconds(30010), modal::leave_kind::set, 0, read);
		check(read.offered && !read.chosen && read.released);
		read = modal::read_leave(milliseconds(30015), modal::leave_kind::set, circle | cross_button, read);
		check(!read.chosen && read.held_ms == 0);
		read = modal::read_leave(milliseconds(32015), modal::leave_kind::set, circle | cross_button, read);
		check(read.chosen);
		check(!modal::read_leave(milliseconds(32020), modal::leave_kind::set, cross_button, read).chosen);
		check(!modal::read_leave(milliseconds(90000), modal::leave_kind::none, circle, read).chosen);
	}
	// The panel's claim on this PC's controller, one read at a time.
	{
		constexpr std::uint16_t cross = 0x0040, square = 0x0080, up = 0x1000;
		// A press still held keeps the claim and navigates nothing; the read that finds every button up ends the hold and still
		// navigates nothing; only a free controller over a visible panel navigates it.
		auto seen = read_claim(true, cross, true, false);
		check(seen.ignore_until_release && seen.claimed && !seen.navigate);
		seen = read_claim(true, 0, true, false);
		check(!seen.ignore_until_release && seen.claimed && !seen.navigate);
		seen = read_claim(true, cross, false, false);
		check(seen.ignore_until_release && seen.claimed && !seen.navigate);
		seen = read_claim(true, 0, false, false);
		check(!seen.ignore_until_release && !seen.claimed && !seen.navigate);
		seen = read_claim(false, cross, true, false);
		check(!seen.ignore_until_release && seen.claimed && seen.navigate);
		seen = read_claim(false, cross, false, false);
		check(!seen.ignore_until_release && !seen.claimed && !seen.navigate);
		// A hidden selection owns the input: never claimed, never navigating, and a release is still seen.
		seen = read_claim(true, cross, true, true);
		check(seen.ignore_until_release && !seen.claimed && !seen.navigate);
		seen = read_claim(true, 0, false, true);
		check(!seen.ignore_until_release && !seen.claimed && !seen.navigate);

		// A shown panel takes the controller; with a peer, so does the hidden panel that replaces a shown one (the launcher hides
		// its panel after a choice, sometimes before any waiting card). Practice keeps its own rule.
		check(takes_controller(false, true, false) && takes_controller(true, true, true) && takes_controller(true, false, true));
		check(!takes_controller(true, false, false) && !takes_controller(false, false, true));

		// The claim's buttons: every button seen while claimed or still held joins them; the read that ends the hold keeps them;
		// a free read adds nothing.
		check(hold_buttons(0, cross, read_claim(true, cross, false, false)) == cross);
		check(hold_buttons(cross, 0, read_claim(true, 0, false, false)) == cross);
		check(hold_buttons(0, up, read_claim(false, up, true, false)) == up);
		check(hold_buttons(0, up, read_claim(false, up, false, false)) == 0);
		check(hold_buttons(0, cross, read_claim(true, cross, false, true)) == cross);

		// Admission: neutral while claimed, the selector's own word while it owns the sample, and after the hold a button the claim
		// saw stays out until a sample shows it up.
		const auto admitted = [](std::uint16_t sample, std::uint16_t physical, bool claimed, bool holding, bool selector_owns,
								  std::uint16_t buttons, std::uint16_t word, std::uint16_t left)
		{
			const auto out = admit_local_sample(sample, physical, claimed, holding, selector_owns, buttons);
			return out.word == word && out.buttons == left;
		};
		check(admitted(cross, cross, true, true, false, cross, 0, cross) && admitted(cross, 0, true, false, false, cross, 0, cross));
		check(admitted(up, cross, false, true, true, cross, up, cross) && admitted(up, 0, false, false, true, cross, up, 0));
		check(admitted(cross, cross, false, false, false, cross, 0, cross) && admitted(0, 0, false, false, false, cross, 0, 0));
		check(admitted(cross | up, cross | up, false, false, false, cross, up, cross));
		check(admitted(square, square, false, false, false, 0, square, 0));
		// The in-game Settings menu's hold: neutral while it holds, every button then seen down joins the claim's buttons and stays
		// out after it until a sample shows it up; the selector still owns its own sample.
		const auto in_menu = [](std::uint16_t sample, bool menu, std::uint16_t buttons, std::uint16_t word, std::uint16_t left)
		{
			const auto out = admit_local_sample(sample, sample, false, false, false, buttons, menu);
			return out.word == word && out.buttons == left;
		};
		check(in_menu(square, true, 0, 0, square) && in_menu(square | up, false, square, up, square) && in_menu(up, false, square, up, 0));
		check(in_menu(cross, true, square, 0, square | cross) && in_menu(0, false, square | cross, 0, 0) && in_menu(cross, false, 0, cross, 0));
		check(admit_local_sample(up, cross, false, false, true, 0, true).word == up);
		// Only Practice blanks executed ports; with a peer the owned port executes its frozen sample.
		check(blanks_executed_port(true, false, true) && !blanks_executed_port(false, false, true));
		check(!blanks_executed_port(true, true, false) && !blanks_executed_port(false, true, false));
		check(!blanks_executed_port(true, false, false));

		{
			constexpr std::uint16_t cross = 0x0040, square = 0x0080, up = 0x1000;
			// Local filtering after a panel closes: `pad` is the current physical sample, while `stale` can supply an older
			// game read. A hidden selection owns the sample while `selecting` and supplies `selector`.
			struct step
			{
				std::uint16_t pad;
				bool selecting;
				std::uint16_t selector;
				int stale = -1;
			};
			const auto run = [](const std::vector<step>& steps)
			{
				bool ignore_until_release = takes_controller(true, false, true);
				std::uint16_t buttons = 0;
				std::vector<std::uint16_t> filtered;
				for (const auto& at : steps)
				{
					const auto claim = read_claim(ignore_until_release, at.pad, false, at.selecting);
					ignore_until_release = claim.ignore_until_release;
					buttons = hold_buttons(buttons, at.pad, claim);
					const auto physical = at.stale < 0 ? at.pad : static_cast<std::uint16_t>(at.stale);
					const auto admit = admit_local_sample(at.selecting ? at.selector : physical, physical, claim.claimed,
						ignore_until_release, at.selecting, buttons);
					buttons = admit.buttons;
					filtered.push_back(admit.word);
				}
				return std::make_pair(filtered, ignore_until_release);
			};

			// An in-game set after Rematch: the held Cross stays out until released; the next press is the player's own.
			auto result = run({{cross, false, 0}, {cross, false, 0}, {cross | up, false, 0}, {0, false, 0}, {cross, false, 0}});
			check((result.first == std::vector<std::uint16_t>{0, 0, 0, 0, cross}) && !result.second);

			// The same, released just before the read that ends the hold: the game's older read still has Cross down. It stays out
			// until a sample shows it up. Up, which the claim never saw, passes at once; a Cross
			// pressed again before any sample showed it up counts as still held.
			result = run({{cross, false, 0}, {0, false, 0, cross}, {0, false, 0, cross}, {up, false, 0}, {cross, false, 0}});
			check((result.first == std::vector<std::uint16_t>{0, 0, 0, up, cross}) && !result.second);
			result = run({{cross, false, 0}, {0, false, 0, cross | up}, {cross, false, 0}});
			check((result.first == std::vector<std::uint16_t>{0, up, 0}) && !result.second);

			// Cross held from Next game into the hidden selection and released there. The selector's words pass the filter,
			// the release ends the hold during selection, and a later physical press passes once selection ends.
			result = run({{cross, true, up}, {cross, true, 0}, {0, true, up}, {square, true, cross}, {square, true, 0},
				{square, false, 0}});
			check((result.first == std::vector<std::uint16_t>{up, 0, up, cross, 0, square}) && !result.second);

			// A fixed set: Cross held through the whole selection into the first battle frames. The selector still steers; after
			// it the held Cross stays filtered until released, then a new press passes.
			result = run({{cross, true, up}, {cross, true, 0}, {cross, true, cross}, {cross, false, 0}, {cross | square, false, 0},
				{0, false, 0}, {square, false, 0}});
			check((result.first == std::vector<std::uint16_t>{up, 0, cross, 0, 0, 0, square}) && !result.second);

			// The same, released at the first battle frame while the game's read still has Cross and Square down: no first attack.
			result = run({{cross, true, up}, {cross | square, false, 0}, {0, false, 0, cross | square}, {0, false, 0, square},
				{0, false, 0}, {square, false, 0}});
			check((result.first == std::vector<std::uint16_t>{up, 0, 0, 0, 0, square}) && !result.second);
		}
	}
	// The layout drawn for a published panel (choose_draw). Practice's panel only ever draws in Practice's layout.
	{
		snapshot practice_panel;
		practice_panel.practice = true;
		practice_panel.visible = true;
		practice_panel.generation = 7;
		practice_panel.title = "PRACTICE";
		check(choose_draw(practice_panel, true, 7) == menu_draw::practice);
		// The game ran on under a menu still marked open: the view no longer shows it, and the panel left behind draws nothing,
		// never the generic panel with the Practice rows.
		check(choose_draw(practice_panel, false, 7) == menu_draw::hidden);
		// The panel published a moment before its view (a new generation the view has not reached yet).
		check(choose_draw(practice_panel, true, 6) == menu_draw::hidden);
		practice_panel.visible = false;
		check(choose_draw(practice_panel, true, 7) == menu_draw::hidden);

		snapshot launcher_panel = parse("PBD-MENU-1\ngeneration=7\nvisible=1\ntitle=Rematch\nrow=1|Yes|Play%20again\n").value.value();
		check(!launcher_panel.practice);
		check(choose_draw(launcher_panel, false, 0) == menu_draw::lobby);
		check(choose_draw(launcher_panel, true, 7) == menu_draw::lobby); // a matching generation does not make it Practice's
		launcher_panel.generation = 0;
		check(choose_draw(launcher_panel, false, 0) == menu_draw::hidden);
		check(choose_draw(snapshot{}, true, 0) == menu_draw::hidden);
	}
}
