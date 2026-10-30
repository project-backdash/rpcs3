#pragma once

#include <string_view>

// Capabilities printed by `rpcs3 --pbd-features` without booting and discoverable as text in the executable.
// The launcher checks them before sending commands, keys or fields that an older emulator would refuse.
//
// rank-stakes-1: the set command rank_stakes; next_set's left.rank and right.rank; the PBD-MENU-1 key rank= and its
// PBD_RANK_BADGES folder; and, with PBD_RANK_SCREENS=1, the game's rank screen values as start.rankScreens in game_preparing
// and game_started, as capsule.rankScreens in the replay's game start, and read back from a watcher's begin_game rankScreens.
//
// ranked-notices-1: the PBD-MENU-1 keys placing=, rival=, link=, countdown_row=, card=, ping= and pill=, rank= kind same,
// and chosen= with other rows still enabled; the PBD-SEARCH-1 key placing=; the set command fields vs_cards.mode,
// vs_cards.left.character and vs_cards.right.character, and rank_stakes.left.placement and rank_stakes.right.placement; and
// the PBD_NOTICE_ART folder of pictures the launcher makes from the player's own game files.
// opponent-results-1: PBD-MENU-1 opponent=, the opponent's settled rank/MMR or placement on Set complete.
namespace pbd::features
{
	inline constexpr std::string_view answer = R"({"schema":"pbd-features-1","features":["rank-stakes-1","ranked-notices-1","opponent-results-1"]})";
} // namespace pbd::features
