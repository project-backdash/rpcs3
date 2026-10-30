#include "pbd_test.h"

#include "Emu/Cell/pbd_vs_cards.h"
#include "Emu/Cell/pbd_stage_names.h"

using pbd::test::check;

PBD_TEST(PbdVsCards, Checks)
{
	using namespace pbd::vs_cards;
	set_view set;
	set.names = {"Ana", "Kai"};
	set.characters = {"LILI", "SERGEI DRAGUNOV"};
	set.plates = {plate{plate::kind::placing, 0, 2, 5}, plate{plate::kind::rank, 12, 0, 0}};
	set.local = 1;
	set.game_number = 2;
	set.first_to = 2;
	set.games = pbd::rank_stakes::score{1, 0};

	// The VS screen shows the cards; the loads around it, the select, a fight and every other scene hide them.
	check(vs_scene(10));
	for (const u32 scene : {0u, 1u, 2u, 3u, 8u, 9u, 11u, 12u, 16u, 17u})
		check(!vs_scene(scene) && !compose(set, scene).visible);

	const auto shown = compose(set, 10);
	check(shown.visible && shown.local == 1 && shown.first_to == 2 && shown.games == std::array<u32, 2>{1, 0});
	check(shown.cards[0].name == "Ana" && shown.cards[0].character == "LILI" && shown.cards[0].shown == set.plates[0]);
	check(shown.cards[1].name == "Kai" && shown.cards[1].character == "SERGEI DRAGUNOV" && shown.cards[1].shown.rank == 12);
	// A watcher knows no score: the cards carry none.
	auto watched = set;
	watched.games.reset();
	check(compose(watched, 10).visible && !compose(watched, 10).games);
	// No game number (no manifest yet) or a role past the two sides draws nothing.
	auto unset = set;
	unset.game_number = 0;
	check(!compose(unset, 10).visible);
	auto stray = set;
	stray.local = 2;
	check(!compose(stray, 10).visible);

	// A fighter's cards draw over the game's own VS screen; a watcher's over its cover only while the cover is up. A watch
	// in view shows the game's VS screen, and a game held ready for a match has no cover overlay at all.
	check(!shown.over_cover);
	check(watch_over_cover(true, false));
	check(!watch_over_cover(true, true));
	check(!watch_over_cover(false, false) && !watch_over_cover(false, true));
	set_view watcher;
	watcher.names = {"Ana", "Kai"};
	watcher.game_number = 2;
	watcher.first_to = 3;
	watcher.over_cover = watch_over_cover(true, false);
	const auto covered = compose(watcher, 10);
	check(covered.visible && covered.over_cover && !covered.games && covered.first_to == 3 && covered.cards[0].name == "Ana" &&
		  covered.cards[1].name == "Kai");
	check(!compose(watcher, 12).visible);
	// Over the game's VS screen nothing reaches the game's art: the stake word ends above the game's name bar (its top at 438)
	// and the set's length sits below the game's W/L bars (their foot about 590).
	namespace at = pbd::set_notices::layout;
	static_assert(at::vs_game_stake_top + at::stake_h <= 438.f);
	static_assert(at::vs_caption_y > 590.f);

	// The generated table names exactly the launcher's stage pool.
	check(pbd::stage_names::names.size() == 34 && pbd::stage_names::name(17) == "Western Gate" && pbd::stage_names::name(16).empty());
	check(pbd::stage_names::name(0) == "Dragon's Nest" && pbd::stage_names::name(15) == "Final Stage" &&
		  pbd::stage_names::name(14).empty());
	check(pbd::stage_names::source_sha256.size() == 64);

	// No stakes (outside Ranked, or before rank_stakes arrives): no stake word.
	check(shown.cards[0].stake == stake_word::none && shown.cards[1].stake == stake_word::none);

	// Publication: equal cards do not move the revision; hiding does.
	const u64 before = revision();
	publish(7, shown);
	const u64 first = revision();
	check(first == before + 1);
	publish(7, shown);
	check(revision() == first);
	check(load().visible && load().generation == 7 && load().cards[1].character == "SERGEI DRAGUNOV");
	hide(7);
	check(revision() == first + 1 && !load().visible);
}

// A Ranked set's stake words: only a game that can end the set shows one, and each player's card shows its own.
PBD_TEST(PbdVsCards, StakeWordsFollowTheScore)
{
	using namespace pbd::vs_cards;
	using pbd::rank_stakes::event;
	set_view set;
	set.names = {"Ana", "Kai"};
	set.local = 0;
	set.first_to = 2;
	// Ana ranks up to 2nd Dan with a set win; Kai drops to 1st Dan with a set loss.
	set.stakes = pbd::rank_stakes::stakes{{{event::promote, event::none, 11, 10}, {event::none, event::demote, 11, 10}}};
	const auto at = [&](u32 game, u32 left, u32 right)
	{
		auto view = set;
		view.game_number = game;
		view.games = pbd::rank_stakes::score{left, right};
		return compose(view, 10);
	};
	const auto words = [](const snapshot& cards)
	{
		return std::array<stake_word, 2>{cards.cards[0].stake, cards.cards[1].stake};
	};
	using pair = std::array<stake_word, 2>;
	// Game 1 at 0-0 shows nothing.
	const auto first = at(1, 0, 0);
	check(first.visible && words(first) == pair{stake_word::none, stake_word::none});
	// Game 2 at 1-0: Ana's win ends the set, Kai's loss ends it.
	const auto ahead = at(2, 1, 0);
	check(words(ahead) == pair{stake_word::promotion, stake_word::demotion});
	// Game 2 at 0-1: neither stake can be decided in this game.
	check(words(at(2, 0, 1)) == pair{stake_word::none, stake_word::none});
	// Game 3 at 1-1: both, each on its own card.
	check(words(at(3, 1, 1)) == pair{stake_word::promotion, stake_word::demotion});
	// A drawn game is played again under its number at the same score, and shows the same.
	check(at(2, 1, 0) == ahead);
	// The stakes are each player's own: at 0-1 a demoting loss for Ana is at stake.
	auto risky = set;
	risky.stakes = pbd::rank_stakes::stakes{{{event::none, event::demote, 11, 10}, {}}};
	risky.game_number = 2;
	risky.games = pbd::rank_stakes::score{0, 1};
	check(words(compose(risky, 10)) == pair{stake_word::demotion, stake_word::none});
	// One player with both at 1-1 (a high-swing player): the promotion chance, as the game shows it.
	auto both = set;
	both.stakes = pbd::rank_stakes::stakes{{{event::promote, event::demote, 12, 10}, {}}};
	both.game_number = 3;
	both.games = pbd::rank_stakes::score{1, 1};
	check(compose(both, 10).cards[0].stake == stake_word::promotion);
	// Placement names no chance and no risk, nor do a win with no change or a demotion on a win.
	auto placing = set;
	placing.stakes = pbd::rank_stakes::stakes{{{event::place, event::place, 12, 0}, {event::none, event::none, 3, 3}}};
	placing.game_number = 3;
	placing.games = pbd::rank_stakes::score{1, 1};
	check(words(compose(placing, 10)) == pair{stake_word::none, stake_word::none});
	// Without a known score nothing is at stake; outside the VS screens nothing is drawn.
	auto unknown = set;
	unknown.game_number = 2;
	check(words(compose(unknown, 10)) == pair{stake_word::none, stake_word::none});
	auto fight = set;
	fight.game_number = 2;
	fight.games = pbd::rank_stakes::score{1, 0};
	check(!compose(fight, 16).visible && compose(fight, 16).cards[0].stake == stake_word::none);
}
