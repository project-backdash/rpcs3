#include "pbd_test.h"

#include "Emu/PBD/pbd_set_notices.h"
#include "Emu/PBD/pbd_game_overlay_parser.h"
#include "Emu/Cell/pbd_notice_art.h"
#include "Emu/Cell/pbd_live_stats.h"

#include <chrono>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <set>
#include <string>

PBD_TEST(PbdSetNotices, MatchConnectionKeepsDelayAndRollbackOnGoodAndPoorConnections)
{
	pbd::live_stats::snapshot stats;
	stats.input_delay = 2;
	stats.depth_recent = 3;
	for (const auto rtt : {40000u, 140000u})
	{
		stats.rtt_samples = 1;
		stats.rtt_avg_us = rtt;
		const auto line = pbd::live_stats::net_text(stats);
		pbd::test::check(line.find("Delay 2f") != std::string::npos && line.find("Rollback 3f") != std::string::npos);
		pbd::test::check(line.find(std::to_string(rtt / 1000) + " ms") != std::string::npos);
	}
	stats.rtt_samples = 0;
	stats.input_delay = stats.depth_recent = 0;
	pbd::test::check(pbd::live_stats::net_text(stats).find("-- ms") != std::string::npos);
	pbd::test::check(pbd::live_stats::net_text(stats).find("Rollback 0f") != std::string::npos);
	pbd::test::check(!pbd::live_stats::decide({"0"}).show());
}

PBD_TEST(PbdSetNotices, MatchConnectionUsesFallbackOnlyWhenNoticeFontIsMissing)
{
	int notice = 1, fallback = 2;
	using pbd::live_stats::connection_face;
	pbd::test::check(connection_face(&notice, &fallback) == &notice);
	pbd::test::check(connection_face(static_cast<int*>(nullptr), &fallback) == &fallback);
	pbd::test::check(connection_face(static_cast<int*>(nullptr), static_cast<int*>(nullptr)) == nullptr);
}
#include <vector>

using pbd::test::check;

namespace
{
	namespace notices = pbd::set_notices;
	using notices::plate;
	using notices::screen;
	using notices::set_mode;

	// A Ranked set: Ana (Lili) placing 2 of 5 on the left, Kai (Dragunov, 3rd Dan) on the right.
	notices::set_view ranked_view()
	{
		notices::set_view view;
		view.mode = set_mode::ranked;
		view.names = {"Ana", "Kai"};
		view.characters = {"LILI", "SERGEI DRAGUNOV"};
		view.ranks = {0, 12};
		view.placements = {pbd::rank_stakes::placement{2, 5}, std::nullopt};
		view.game_number = 1;
		view.first_to = 2;
		return view;
	}

	pbd::game_overlay::snapshot panel_of(const std::string& lines)
	{
		auto parsed = pbd::game_overlay::parse("PBD-MENU-1\ngeneration=4\nvisible=1\n" + lines);
		check(parsed.value, parsed.error);
		return *parsed.value;
	}
} // namespace

// Which plate each side shows: a placing side's count in Ranked, its rank otherwise; Quick Match never a placement plate.
PBD_TEST(PbdSetNotices, OpponentResultUsesTheSettledStandingOnEitherSide)
{
	using namespace pbd::game_overlay;
	const std::string result = "kind=post_set\ntitle=YOU%20WIN\nscore=2|LRL|Ana|Kai\n";
	for (const char side : {'L', 'R'})
	{
		const auto placing = panel_of(result + "you=" + side + "\nopponent=p4/5|-\nrank=attained|22|1792|162\n");
		check(notices::opponent_result_shown(placing));
		check(placing.opponent.shown == plate{plate::kind::placing, 0, 4, 5});
		check(notices::opponent_caption(placing.opponent) == "1 set to rank");
		const auto ranked = panel_of(result + "you=" + side + "\nopponent=r15|1430\n");
		check(ranked.opponent.shown == plate{plate::kind::rank, 15, 0, 0});
		check(notices::opponent_caption(ranked.opponent) == "MMR 1430");
		check(!notices::opponent_result_shown(panel_of(result + "you=" + side + "\n")));
	}
	check(notices::opponent_caption({plate{plate::kind::placing, 0, 2, 4}, 0}) == "2 sets to rank");
	const std::string header = "PBD-MENU-1\ngeneration=1\nvisible=1\n";
	for (const auto bad : {"-|-", "r35|1430", "r15|-", "r15|100000", "p6/5|-", "p4/5|1430", "p4/5", "p4/5|-|extra"})
		check(parse(header + result + "you=L\nopponent=" + bad + "\n").error == "bad_opponent");
	check(parse(header + result + "you=L\nopponent=r15|1430\nopponent=p4/5|-\n").error == "bad_opponent");
	check(parse(header + result + "opponent=r15|1430\n").error == "bad_opponent");
	check(parse(header + "kind=menu\ntitle=Menu\nopponent=r15|1430\n").error == "bad_opponent");
}

PBD_TEST(PbdSetNotices, PlatesFollowTheModeAndTheCount)
{
	const auto placing = std::optional<pbd::rank_stakes::placement>(pbd::rank_stakes::placement{2, 5});
	check(notices::plate_for(set_mode::ranked, placing, 0) == plate{plate::kind::placing, 0, 2, 5});
	check(notices::plate_for(set_mode::quick, placing, 0) == plate{plate::kind::rank, 0, 0, 0});
	check(notices::plate_for(set_mode::lobby, placing, 7) == plate{} && notices::plate_for(set_mode::quick, std::nullopt, 7).rank == 7);
	check(notices::plate_for(set_mode::ranked, std::nullopt, 12) == plate{plate::kind::rank, 12, 0, 0});
	// A rank the game cannot draw is Beginner, as the game treats it.
	check(notices::plate_for(set_mode::ranked, std::nullopt, 99).rank == 0);

	const auto set = notices::compose(ranked_view());
	check(set.active && set.sides[0].shown.shown == plate::kind::placing && set.sides[1].shown == plate{plate::kind::rank, 12, 0, 0});
	check(set.sides[1].character == "SERGEI DRAGUNOV" && set.sides[0].name == "Ana");
	auto quick = ranked_view();
	quick.mode = set_mode::quick;
	quick.stakes = pbd::rank_stakes::stakes{};
	const auto quick_set = notices::compose(quick);
	check(quick_set.sides[0].shown.shown == plate::kind::rank && !quick_set.stakes, "Quick Match shows no placement and no stakes");
}

// The screen at a boundary from the scene and the fight's pending phase.
PBD_TEST(PbdSetNotices, ScreensFromSceneAndPhase)
{
	check(notices::screen_of(10, 0) == screen::vs && notices::screen_of(10, 99) == screen::vs);
	for (const auto scene : {8u, 9u, 11u, 12u})
		check(notices::screen_of(scene, 0) == screen::other);
	check(notices::screen_of(3, 0) == screen::other && notices::screen_of(1, 5) == screen::other);
	check(notices::screen_of(17, 0) == screen::character_intro && notices::screen_of(16, 3) == screen::character_intro);
	check(notices::screen_of(17, 4) == screen::round_intro && notices::screen_of(17, 5) == screen::fight);
	// The K.O. and its slow motion keep the HUD up; the replay, the win pose and the reload after it do not.
	for (const auto pending : {6u, 7u, 8u})
		check(notices::screen_of(17, pending) == screen::knockout && notices::screen_of(16, pending) == screen::knockout);
	for (const auto pending : {9u, 10u, 12u, 37u})
		check(notices::screen_of(17, pending) == screen::round_result);
	check(notices::screen_of(17, 13) == screen::other && notices::screen_of(17, 36) == screen::other);
	// Whose intro plays: the left fighter's in pending 1, the right one's in 2, nobody's otherwise.
	check(notices::intro_side_of(17, 1) == 0 && notices::intro_side_of(17, 2) == 1 && notices::intro_side_of(16, 2) == 1);
	for (const auto pending : {0u, 3u, 4u, 5u})
		check(notices::intro_side_of(17, pending) == notices::no_side);
	check(notices::intro_side_of(12, 1) == notices::no_side && notices::intro_side_of(8, 2) == notices::no_side);
}

// The Placement Match pill and the life bar's placement plate: when they show and when they never do.
PBD_TEST(PbdSetNotices, PillAndLifeBarPlate)
{
	auto view = ranked_view();
	view.now = screen::character_intro;
	view.first_intro = true;
	view.intro_side = 0;
	check(notices::intro_pill(notices::compose(view)));
	// Only on the placing player's own intro: never over the other player's, nor between the two intros.
	view.intro_side = 1;
	check(!notices::intro_pill(notices::compose(view)), "not over the opponent's intro");
	view.intro_side = notices::no_side;
	check(!notices::intro_pill(notices::compose(view)));
	auto both = view;
	both.placements = {pbd::rank_stakes::placement{2, 5}, pbd::rank_stakes::placement{1, 4}};
	for (const std::uint32_t side : {0u, 1u})
	{
		both.intro_side = side;
		check(notices::intro_pill(notices::compose(both)), "both placing: each intro shows its own player's pill");
	}
	view.intro_side = 0;
	// Every game of the set, but never where the game draws its own "Ranking Match".
	view.game_stakes = {0, 2};
	check(!notices::intro_pill(notices::compose(view)));
	view.game_stakes = {};
	// Only the game's first intro: a later round's reload looks the same to the phase.
	view.first_intro = false;
	check(!notices::intro_pill(notices::compose(view)));
	view.first_intro = true;
	for (const auto other : {screen::vs, screen::round_intro, screen::fight, screen::knockout, screen::round_result, screen::other})
	{
		view.now = other;
		check(!notices::intro_pill(notices::compose(view)));
	}
	view.now = screen::character_intro;
	auto quick = view;
	quick.mode = set_mode::quick;
	check(!notices::intro_pill(notices::compose(quick)), "Quick Match never shows the pill");
	auto placed = view;
	placed.placements = {};
	check(!notices::intro_pill(notices::compose(placed)), "no pill without a placing player");

	// The life bar's plate: a placing side whose plate the game draws, from the round call through the knockout, while the
	// HUD is up.
	view.plate_drawn = {true, true};
	for (const auto now : {screen::round_intro, screen::fight, screen::knockout})
	{
		view.now = now;
		const auto set = notices::compose(view);
		check(notices::life_plate(set, 0) && !notices::life_plate(set, 1) && !notices::life_plate(set, 2));
	}
	for (const auto now : {screen::character_intro, screen::round_result, screen::vs, screen::other})
	{
		view.now = now;
		check(!notices::life_plate(notices::compose(view), 0), "the plate hides with the HUD: before the round call and from the replay on");
	}
	// It stops at the knockout where the game writes this side's change word and flips its own plate; the other side's
	// change leaves it.
	view.now = screen::knockout;
	view.promoted = {true, false};
	check(!notices::life_plate(notices::compose(view), 0), "gone where the game flips the plate");
	view.promoted = {false, true};
	check(notices::life_plate(notices::compose(view), 0));
	view.promoted = {};
	view.now = screen::fight;
	view.plate_drawn = {false, true};
	check(!notices::life_plate(notices::compose(view), 0), "nothing where the game draws no plate");
	quick.now = screen::fight;
	quick.plate_drawn = {true, true};
	check(!notices::life_plate(notices::compose(quick), 0));
}

// The game's stake words over the VS cards and between games: for the game the score leads to, Ranked only.
PBD_TEST(PbdSetNotices, StakeWords)
{
	using pbd::rank_stakes::event;
	auto view = ranked_view();
	view.stakes = pbd::rank_stakes::stakes{{{event::place, event::place, 0, 0}, {event::none, event::demote, 12, 11}}};
	const auto set = notices::compose(view);
	check(notices::stake_for(set, 1, {0, 0}) == notices::stake_word::none, "nothing before the game that can end the set");
	check(notices::stake_for(set, 1, {1, 0}) == notices::stake_word::demotion && notices::stake_for(set, 0, {1, 0}) == notices::stake_word::none);
	check(notices::stake_words(notices::stake_word::none).empty());
	view.stakes = pbd::rank_stakes::stakes{{{event::promote, event::demote, 13, 11}, {}}};
	check(notices::stake_for(notices::compose(view), 0, {1, 1}) == notices::stake_word::promotion, "both: the promotion chance");
	view.mode = set_mode::quick;
	check(notices::stake_for(notices::compose(view), 0, {1, 1}) == notices::stake_word::none);
}

// Numeric formatting and the panel fields that supply its headline, rival status and countdown row.
PBD_TEST(PbdSetNotices, FormattingAndPanelFields)
{
	check(notices::game_headline(3).find("3") != std::string::npos && notices::first_to_caption(2).find("2") != std::string::npos);
	check(notices::placing_words(2, 5).find("2/5") != std::string::npos);
	check(notices::sets_to_go(3, 5).find("2") != std::string::npos && notices::sets_to_go(3, 4).find("1") != std::string::npos);
	check(notices::search_words(true, std::pair<std::uint32_t, std::uint32_t>{2, 5}).count == "2/5");
	check(notices::search_words(true, std::nullopt).count.empty());
	check(notices::search_words(false, std::pair<std::uint32_t, std::uint32_t>{2, 5}).count.empty());
	check(notices::signal_of(4) == notices::signal::good && notices::signal_of(3) == notices::signal::fair && notices::signal_of(2) == notices::signal::fair);
	check(notices::signal_of(1) == notices::signal::poor && notices::signal_of(0) == notices::signal::poor);

	// Set complete's one headline, the caption under the score, the rival's line and the countdown's row.
	const std::string set_complete = "kind=post_set\ntitle=YOU%20WIN\nrow=1|REMATCH|\nrow=1|NEXT%20OPPONENT|\nscore=2|LRL|Ana|Kai\nyou=L\n";
	check(notices::headline(panel_of(set_complete)) == "YOU WIN");
	check(notices::headline(panel_of(set_complete + "rank=down|10|1262|-14\n")) == "YOU WIN");
	check(notices::headline(panel_of(set_complete + "rank=same|12|1301|9\n")) == "YOU WIN");
	const auto between = panel_of("title=Next%20game\nrow=1|REMATCH|\nrow=1|FORFEIT%20SET|\nscore=2|L|Ana|Kai\nyou=L\n"
								  "timer=5|15000|Rematch|Kai%20is%20choosing|0\n");
	check(notices::headline(between) == "NEXT GAME" && notices::score_caption(between).find("2") != std::string::npos);
	check(notices::rival_line(between) == "Kai is choosing" && notices::countdown_row(between) == 0u);
	const auto told = panel_of("title=Next%20game\nrow=1|REMATCH|\nrow=1|FORFEIT%20SET|\nscore=2|L|Ana|Kai\nyou=L\n"
							   "timer=5|15000|Rematch||0\nrival=Kai%20is%20ready\ncountdown_row=1\n");
	check(notices::rival_line(told) == "Kai is ready" && notices::countdown_row(told) == 1u);
	check(!notices::countdown_row(panel_of(set_complete)) && notices::rival_line(panel_of(set_complete)).empty());

	using pbd::game_overlay::rank_change;
	using pbd::game_overlay::rank_result;
	check(notices::rating_caption(rank_result{rank_change::attained, 11, 1216, 40}).find("1216") != std::string::npos &&
		  notices::rating_change(rank_result{rank_change::attained, 11, 1216, 40}).empty());
	check(notices::rating_change(rank_result{rank_change::up, 12, 1301, 13}) == "+13");
	check(notices::rating_change(rank_result{rank_change::down, 10, 1262, -14}) == "\xE2\x88\x92"
																				   "14");
	check(notices::rating_change(rank_result{rank_change::same, 12, 1301, 9}) == "+9");
}

// Which look each launcher panel takes, whether your own plate steps aside, and the prompts that act now.
PBD_TEST(PbdSetNotices, PanelLooksAndPrompts)
{
	using look = notices::panel_look;
	const std::string set_complete = "kind=post_set\ntitle=YOU%20WIN\nrow=1|REMATCH|\nrow=1|NEXT%20OPPONENT|\nscore=2|LRL|Ana|Kai\nyou=L\n";
	check(notices::look_of(panel_of(set_complete)) == look::result);
	check(notices::look_of(panel_of(set_complete + "placing=2|3|5\n")) == look::placement);
	check(notices::look_of(panel_of(set_complete + "rank=attained|11|1216|0\n")) == look::rank_headline);
	check(notices::look_of(panel_of(set_complete + "rank=up|12|1301|13\nplacing=4|5|5\n")) == look::rank_headline, "rank news is the one headline");
	check(notices::look_of(panel_of(set_complete + "rank=same|12|1301|9\n")) == look::rank_line);
	check(notices::look_of(panel_of(set_complete + "rank=down|10|1262|-14\n")) == look::rank_line);
	const auto between = panel_of("title=NEXT%20GAME\nrow=1|REMATCH|\nrow=1|FORFEIT%20SET|\nscore=2|L|Ana|Kai\nyou=L\n");
	check(notices::look_of(between) == look::between);
	check(notices::look_of(panel_of("title=FORFEITED\n")) == look::prompt);
	check(notices::look_of(panel_of("title=NEXT%20GAME\ndialog=Forfeit%20set%3F|Counts%20as%20a%20loss.\nrow=1|BACK|\nrow=1|FORFEIT|\n")) == look::question);
	check(notices::look_of(pbd::game_overlay::selection_card(between)) == look::cover);
	for (const auto shown : {look::placement, look::rank_headline, look::rank_line})
		check(notices::own_plate_aside(shown));
	for (const auto shown : {look::result, look::between, look::question, look::prompt, look::cover})
		check(!notices::own_plate_aside(shown));
	// The held game's shade under each look.
	check(notices::held_shade(look::between, true) == .28f && notices::held_shade(look::question, true) == .34f);
	check(notices::held_shade(look::placement, true) == .3f && notices::held_shade(look::result, true) == .34f);
	check(notices::held_shade(look::rank_line, true) == .34f && notices::held_shade(look::rank_headline, true) == .55f);
	check(notices::held_shade(look::prompt, false) == .45f && notices::held_shade(look::rank_headline, false) == .55f);
	// Move first with two open rows, then Choose and Back; nothing at all with no row open.
	using keys = std::vector<std::pair<std::string, std::string>>;
	const auto prompts = notices::prompts(between);
	check(prompts.size() == 3 && prompts[0].first == "{dpad:ud}" && prompts[1] == keys::value_type{"{cross}", between.confirm} &&
		  prompts[2] == keys::value_type{"{circle}", between.back});
	check(notices::prompts(panel_of("title=X\nconfirm=Choose\nback=\nrow=1|REMATCH|\n")) == keys{{"{cross}", "Choose"}});
	check(notices::prompts(panel_of("title=X\nrow=0|REMATCH|\nrow=0|FORFEIT%20SET|\n")).empty());
	// A chosen row with the other still open (Rematch chosen, Next opponent live).
	check(notices::prompts(panel_of("title=X\nconfirm=Choose\nback=\nrow=0|REMATCH|YOUR%20CHOICE\nrow=1|NEXT%20OPPONENT|\nchosen=0\n")) == keys{{"{cross}", "Choose"}});
}

// Set complete's counter flip: the plate as it stood, then the next lamp with the flash, then the number and what is left.
// It plays once per panel command and count; an unrated set moves nothing.
PBD_TEST(PbdSetNotices, CounterFlip)
{
	const pbd::game_overlay::placing rated{true, 2, 3, 5}, unrated{true, 2, 2, 5};
	const auto at = [&](std::uint64_t ms)
	{
		return notices::flip_at(rated, ms);
	};
	check(at(0).lamps == 2 && !at(0).flash && at(0).count == 2 && at(0).to_go == 0.f);
	check(at(notices::flip_lamp_ms).lamps == 3 && at(notices::flip_lamp_ms).flash && at(notices::flip_lamp_ms).count == 2);
	check(at(notices::flip_number_ms).count == 3 && at(notices::flip_number_ms).to_go == 0.f && at(notices::flip_number_ms).flash);
	check(at(800).to_go > 0.f && at(800).to_go < 1.f);
	const auto settled = at(notices::flip_settle_ms);
	check(settled.lamps == 3 && !settled.flash && settled.count == 3 && settled.to_go == 1.f);
	check(at(60000).lamps == 3 && !at(60000).flash && at(60000).count == 3);
	for (const std::uint64_t ms : {0ull, 300ull, 700ull, 5000ull})
	{
		const auto still = notices::flip_at(unrated, ms);
		check(still.lamps == 2 && still.count == 2 && !still.flash && still.to_go == 0.f, "an unrated set moves nothing");
	}
	auto panel = panel_of("kind=post_set\ncommand=7\ntitle=YOU%20WIN\nscore=2|LRL|Ana|Kai\nyou=L\nplacing=2|3|5\n");
	const auto key = notices::flip_key(panel);
	check(key != 0);
	panel.generation = 99;
	check(notices::flip_key(panel) == key, "a republished panel does not play it again");
	check(notices::flip_key(panel_of("kind=post_set\ncommand=7\ntitle=YOU%20WIN\nscore=2|LRL|Ana|Kai\nyou=L\nplacing=2|2|5\n")) == 0);
	check(notices::flip_key(panel_of("kind=post_set\ncommand=8\ntitle=YOU%20WIN\nscore=2|LRL|Ana|Kai\nyou=L\nplacing=2|3|5\n")) != key);
}

// The layout on the 1280x720 canvas (the game's art 1:1 at 1080p is 2/3 of its pixels here).
PBD_TEST(PbdSetNotices, LayoutOnTheCanvas)
{
	namespace at = notices::layout;
	const auto near = [](float a, float b)
	{
		return std::abs(a - b) < .5f;
	};
	// The intro pill: the game's 1000 x 400 texture with its 855 x 201 body centred at (960, 541) on a 1080p frame.
	check(near(at::pill_w, 1000.f * 2.f / 3.f) && near(at::pill_h, 400.f * 2.f / 3.f));
	check(near(at::pill_x + at::pill_w / 2.f, 960.f * 2.f / 3.f) && near(at::pill_y + at::pill_h / 2.f, 541.f * 2.f / 3.f));
	// The placement plate is 1.28x a rank plate's width at the same height, here and at Set complete's size.
	check(std::abs(at::placing_w / at::rank_w - 1.28f) < .02f && at::placing_h == at::rank_h && near(at::big_placing_w / at::big_placing_h, at::placing_w / at::placing_h));
	// The cards sit where the VS screen shows them: the same edges on the cover and the VS screen, mirrored.
	check(at::margin == 56.f && at::right_edge == 1224.f && at::life_plate_right == 1280.f - at::life_plate_left);
	// The word art at 2/3 of its 1240 x 168, centred.
	check(near(at::challenger_art_w, 1240.f * 2.f / 3.f) && near(at::challenger_art_h, 168.f * 2.f / 3.f) && near(at::challenger_art_x + at::challenger_art_w / 2.f, 640.f));
	check(near(at::challenger_pill_x + at::challenger_pill_w / 2.f, 640.f) && near(at::challenger_rows_x + at::challenger_rows_w / 2.f, 640.f));
	// Rows stack on the row pitch under each headline, never into the prompts.
	for (const float rows_top : {at::between_rows_top, at::confirm_rows_top, at::result_rows_top, at::placing_rows_top, at::rank_rows_top})
		check(rows_top + 3.f * at::row_h < at::prompt_y - 20.f);
}

// The hand-off: once the lobby ends the set, this emulation's picture fades to black over 0.3 s; another emulation (the game
// the launcher starts or parks next) never inherits it.
PBD_TEST(PbdSetNotices, HandOffFadesToBlack)
{
	check(notices::handoff_fade(0) == 0.f && notices::handoff_fade(150) == .5f && notices::handoff_fade(notices::handoff_fade_ms) == 1.f);
	check(notices::handoff_fade(60000) == 1.f);
	check(notices::handoff_shade(7, 1400) == 0.f, "nothing before a hand-off");
	notices::hand_off(7, 1000);
	check(notices::handoff_shade(7, 1000) == 0.f && notices::handoff_shade(7, 1150) == .5f && notices::handoff_shade(7, 1400) == 1.f);
	check(notices::handoff_shade(8, 1400) == 0.f && notices::handoff_shade(0, 1400) == 0.f, "only the emulation that handed off");
	check(notices::handoff_shade(7, 900) == 0.f, "a clock read before the hand-off is no fade");
	notices::hand_off(0, 0);
}

// The snapshot is published for the overlays; the revision moves only when a drawn value changes.
PBD_TEST(PbdSetNotices, PublishesOnlyChanges)
{
	const auto set = notices::compose(ranked_view());
	notices::publish(5, set);
	const auto first = notices::revision();
	notices::publish(5, set);
	check(notices::revision() == first);
	auto later = set;
	later.now = screen::fight;
	notices::publish(5, later);
	check(notices::revision() == first + 1 && notices::load().now == screen::fight && notices::load().generation == 5);
	notices::publish(5, notices::snapshot{});
	check(!notices::load().active);
}

namespace
{
	// A PNG's first 33 bytes for the header checks, padded.
	std::string png_header(std::uint32_t width, std::uint32_t height, std::uint8_t colour)
	{
		std::string out("\x89PNG\r\n\x1a\n\0\0\0\x0dIHDR", 16);
		for (const auto value : {width, height})
			for (int shift = 24; shift >= 0; shift -= 8)
				out.push_back(static_cast<char>((value >> shift) & 0xff));
		out.push_back(8);
		out.push_back(static_cast<char>(colour));
		out += std::string(64, '\0');
		return out;
	}
} // namespace

// The notice pictures' names, and the lamps for 4 and 5 sets.
PBD_TEST(PbdSetNotices, NoticeArtNames)
{
	namespace art = pbd::notice_art;
	std::set<std::string> names;
	for (const auto& entry : art::entries())
		check(!entry.name.empty() && names.insert(entry.name).second, "every picture has its own name");
	check(names.size() == art::picture_count && art::picture_count == 24);
	check(art::entries()[*art::lamps(5, 3, true)].name == "lamps-5-3-flash.png" && art::entries()[*art::lamps(4, 0, false)].name == "lamps-4-0.png");
	check(art::entries()[*art::lamps(5, 5, false)].name == "lamps-5-5.png" && art::entries()[*art::lamps(4, 4, true)].name == "lamps-4-4-flash.png");
	check(!art::lamps(3, 1, false) && !art::lamps(6, 1, false) && !art::lamps(5, 6, false) && !art::lamps(5, 0, true) && !art::lamps(10, 2, false));
	check(art::entries()[static_cast<std::uint32_t>(art::picture::placement_stone)].width == 492 && art::entries()[static_cast<std::uint32_t>(art::picture::select_flames)].rgb);
}

// The latch reads the launcher's folder: only this recipe's version, regular files inside it, PNGs of each picture's size.
PBD_TEST(PbdSetNotices, NoticeArtFolder)
{
	namespace art = pbd::notice_art;
	const auto parent = std::filesystem::temp_directory_path();
	const auto directory = parent / ("pbd-notice-art-test-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
	check(std::filesystem::create_directory(directory));
	const auto write = [&](const std::string& name, const std::string& bytes)
	{
		std::ofstream file(directory / name, std::ios::binary);
		file << bytes;
	};
	write("placement-stone.png", png_header(492, 192, 6));
	write("placement-match.png", png_header(1000, 400, 2)); // no alpha: refused
	write("new-challenger.png", png_header(1240, 169, 6));  // the wrong size: refused
	write("select-flames.png", png_header(1280, 720, 2));
	write("lamps-5-2.png", png_header(696, 320, 6));
	write("lamps-5-3-flash.png", "GIF89a" + std::string(64, 'x'));
	write("lamps-4-1.png", std::string(5 * 1024 * 1024, '\0').replace(0, 33, png_header(696, 320, 6).substr(0, 33))); // too large
	// Without this recipe's version.txt nothing is read.
	check(!art::read_folder(directory).recipe && art::read_folder(directory).found == 0);
	write("version.txt", "1:other");
	check(!art::read_folder(directory).recipe);
	write("version.txt", std::string(art::recipe_version) + "\r\n");
	const auto read = art::read_folder(directory);
	check(read.recipe && read.found == 3);
	check(read.pictures[static_cast<std::uint32_t>(art::picture::placement_stone)] && read.pictures[static_cast<std::uint32_t>(art::picture::select_flames)]);
	check(read.pictures[*art::lamps(5, 2, false)] && !read.pictures[*art::lamps(5, 3, true)] && !read.pictures[*art::lamps(4, 1, false)]);
	check(!read.pictures[static_cast<std::uint32_t>(art::picture::placement_match)] && !read.pictures[static_cast<std::uint32_t>(art::picture::new_challenger)]);
	// A relative folder, or none, reads nothing.
	check(!art::read_folder(std::filesystem::path("notice-art")).recipe && !art::read_folder({}).recipe);
	check(!art::read_picture(directory, art::picture_count));
	// A later look takes only what the first read missed: the pill the launcher finished meanwhile, and nothing already held.
	auto held = art::read_folder(directory);
	write("placement-match.png", png_header(1000, 400, 6));
	const auto later = art::read_folder(directory);
	const auto stone = held.pictures[static_cast<std::uint32_t>(art::picture::placement_stone)];
	check(later.found == 4 && art::take_missing(held, later) && held.found == 4 && held.recipe);
	check(held.pictures[static_cast<std::uint32_t>(art::picture::placement_match)] && held.pictures[static_cast<std::uint32_t>(art::picture::placement_stone)] == stone);
	check(!art::take_missing(held, later) && !art::take_missing(held, art::folder{}) && held.found == 4);
	// A link in the folder reads as missing, even to a good picture (skipped where this PC cannot make links).
	std::error_code ec;
	std::filesystem::create_symlink(directory / "placement-stone.png", directory / "lamps-5-0.png", ec);
	if (!ec)
		check(!art::read_picture(directory, *art::lamps(5, 0, false)));
	const auto cleanup = std::filesystem::canonical(directory);
	check(std::filesystem::equivalent(cleanup.parent_path(), std::filesystem::canonical(parent)) &&
		  cleanup.filename().string().starts_with("pbd-notice-art-test-"));
	std::filesystem::remove_all(cleanup);
}
