#pragma once

#include "pbd_game_profile.h"
#include "../PBD/pbd_rank_stakes.h"
#include "../PBD/pbd_set_notices.h"

#include <array>
#include <atomic>
#include <cstdint>
#include <mutex>
#include <optional>
#include <string>
#include <utility>

// The VS cards over the game's own VS screen in a lobby set: the set's score and its length between the game's portraits, and
// over each player's side the game's own stake word ("Promotion Chance", "Demotion Risk") when this game can end the set that
// way for that player. Over a watcher's cover, which hides that screen, each player's card shows too, with the fighter's
// caption, the name and the plate. The launcher's vs_cards command shows them (and names the captions and the mode); the
// names, the game and the score come from the set manifest, the plates and stakes from the set's ranks and rank_stakes. The
// update latch composes one snapshot at each fresh boundary and the RSX overlay only draws it. Nothing here reads the pad,
// writes input or touches game state, so the cards can never change what either game executes. They show on the game's VS
// screen only: never in the select, a fight, between games or on any other screen. Standard-library only, so the rules are
// testable.
namespace pbd::vs_cards
{
	using u32 = std::uint32_t;
	using u64 = std::uint64_t;
	using plate = pbd::game_overlay::plate;
	using stake_word = pbd::set_notices::stake_word;

	// The VS screen. The pre-battle stretch runs 8 -> 9 -> 10 -> 11 -> 12 -> 16/17: 10 is the VS screen (VS BATTLE n, GET READY,
	// then the portraits with the game's own name bar, plate and PLAYER DATA); 8/9 and 11/12 are black loads. The select is
	// scene 3 and a fight 16/17, so neither ever shows a card.
	inline constexpr bool vs_scene(u32 scene)
	{
		return scene == 10;
	}

	struct card
	{
		std::string name;      // the set manifest's display name
		std::string character; // the fighter's caption ("SERGEI DRAGUNOV"); empty when the launcher named none
		plate shown;
		stake_word stake = stake_word::none;
		bool operator==(const card&) const = default;
	};

	struct snapshot
	{
		u64 generation = 0; // the emulation that published it; another emulation draws nothing
		bool visible = false;
		std::array<card, 2> cards{}; // left, right
		u32 local = 0;               // this PC's card: 0 left, 1 right
		// The set's score before this game, when the composer knows it (a watcher does not), and its length.
		std::optional<std::array<u32, 2>> games;
		u32 first_to = 0;
		bool over_cover = false; // drawn over a cover that hides the game's VS screen (a watcher's), not over the screen itself
		bool operator==(const snapshot&) const = default;
	};

	struct set_view
	{
		std::array<std::string, 2> names, characters;
		std::array<plate, 2> plates{};
		u32 local = 0, game_number = 0, first_to = 0;
		std::optional<pbd::rank_stakes::score> games;
		std::optional<pbd::rank_stakes::stakes> stakes;
		// A watcher's cover hides the game's VS screen, so its cards carry the names and plates; over the game's own VS screen
		// the game's portraits already show them.
		bool over_cover = false;
	};

	// Whether a watcher's cards draw over its cover. Only a game launched to watch (PBD_SPECTATOR_CHANNEL) has the
	// spectator overlay that draws the cover, and it covers the game exactly while the watch is not visible: the presentation
	// it draws is hidden whenever local_channel::visible() is false. A watch shown from the select (a lobby room, a repick
	// game) or taken by a game held ready for a match leaves the game's own VS screen in view, as a fighter's game does.
	inline constexpr bool watch_over_cover(bool cover_overlay, bool watch_visible)
	{
		return cover_overlay && !watch_visible;
	}

	// The stake word over one player's card in this game (pbd::rank_stakes::at): none before the game that can end the set; a
	// player with both shows the promotion chance, as the game does.
	inline stake_word stake_of(const set_view& set, u32 player)
	{
		if (!set.stakes || !set.games)
			return stake_word::none;
		const auto stake = pbd::rank_stakes::at(*set.stakes, player, *set.games, set.first_to);
		return stake.promotion ? stake_word::promotion : stake.demotion ? stake_word::demotion :
		                                                                  stake_word::none;
	}

	// The cards for one fresh boundary: shown while the game is on a VS screen, hidden on every other scene.
	inline snapshot compose(const set_view& set, u32 scene)
	{
		snapshot out;
		if (!vs_scene(scene) || set.local > 1 || !set.game_number)
			return out;
		for (u32 side = 0; side < 2; side++)
			out.cards[side] = {set.names[side], set.characters[side], set.plates[side], stake_of(set, side)};
		out.visible = true;
		out.local = set.local;
		out.games = set.games;
		out.first_to = set.first_to;
		out.over_cover = set.over_cover;
		return out;
	}

	// The renderer rebuilds its draw list only when a published value or generation changes.
	inline std::mutex published_mutex;
	inline snapshot published;
	inline std::atomic<u64> published_revision{0};

	inline void publish(u64 generation, snapshot value)
	{
		value.generation = generation;
		std::lock_guard lock(published_mutex);
		if (published == value)
			return;
		published = std::move(value);
		published_revision.fetch_add(1, std::memory_order_release);
	}
	inline void hide(u64 generation)
	{
		publish(generation, snapshot{});
	}
	inline u64 revision()
	{
		return published_revision.load(std::memory_order_acquire);
	}
	inline snapshot load()
	{
		std::lock_guard lock(published_mutex);
		return published;
	}
} // namespace pbd::vs_cards
