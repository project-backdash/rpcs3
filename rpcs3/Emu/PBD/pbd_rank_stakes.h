#pragma once

#include "pbd_json_reader.h"
#include "../Cell/pbd_game_profile.h"

#include <array>
#include <charconv>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>

// Rank stakes supplied by the launcher. Only a game that can decide a promotion or demotion displays that stake.
// Both players and recorded viewers use the same game_values for the game's banners and promotion screen.
namespace pbd::rank_stakes
{
	using u32 = std::uint32_t;
	using u8 = std::uint8_t;
	using json_value = pbd::json::json_value;

	// What one set result does to a player's rank. place: the set places a player who had no rank yet.
	enum class event : std::uint8_t
	{
		none,
		promote,
		demote,
		place
	};

	// A stake names a PBD rank: the game's rank index from Beginner (0) to Dark Lord (34) (pbd::game_profile::rank_names).
	inline constexpr u32 max_rank = 34;

	// One player's two outcomes: the event and the rank a set win gives, and those a set loss gives.
	struct side
	{
		event win = event::none, loss = event::none;
		u32 win_rank = 0, loss_rank = 0;
		bool operator==(const side&) const = default;
	};
	using stakes = std::array<side, 2>; // left, right
	using score = std::array<u32, 2>;   // the games each side has won before this game, left and right

	inline std::optional<event> read_event(const json_value* value)
	{
		if (!value || value->kind != pbd::json::json_kind::string)
			return {};
		const auto& text = value->text;
		if (text == "none")
			return event::none;
		if (text == "promote")
			return event::promote;
		if (text == "demote")
			return event::demote;
		if (text == "place")
			return event::place;
		return {};
	}

	// One side of rank_stakes: {"win":<event>,"winRank":<0..34>,"loss":<event>,"lossRank":<0..34>}, each event one of none,
	// promote, demote and place. The rule below reads only a promoting win and a demoting loss.
	inline std::optional<side> read_side(const json_value* value)
	{
		side out;
		const auto win = read_event(value ? value->find("win") : nullptr);
		const auto loss = read_event(value ? value->find("loss") : nullptr);
		if (!win || !loss || !pbd::json::detail::number32(value->find("winRank"), 0, max_rank, out.win_rank) ||
			!pbd::json::detail::number32(value->find("lossRank"), 0, max_rank, out.loss_rank))
			return {};
		out.win = *win;
		out.loss = *loss;
		return out;
	}

	// A placing player's count, sent beside a side of rank_stakes ("placement":{"done":2,"total":5}): the rated sets the
	// player's character had before this set, of the total that places it (1..10). Display only: it is not part of the stakes
	// word the two PCs agree on and changes nothing the game executes.
	inline constexpr u32 max_placement_total = 10;
	struct placement
	{
		u32 done = 0, total = 0;
		bool operator==(const placement&) const = default;
	};

	// The side's optional "placement"; none when it is left out, null or not a valid count (the notices then show the rank).
	inline std::optional<placement> read_placement(const json_value* side_value)
	{
		const auto* value = side_value ? side_value->find("placement") : nullptr;
		placement out;
		if (!value || value->kind != pbd::json::json_kind::object ||
			!pbd::json::detail::number32(value->find("total"), 1, max_placement_total, out.total) ||
			!pbd::json::detail::number32(value->find("done"), 0, out.total, out.done))
			return {};
		return out;
	}

	// The stakes one player plays a game under, from the set's score before that game.
	struct game_stake
	{
		bool promotion = false, demotion = false;
		u32 promotion_rank = 0, demotion_rank = 0;
		bool operator==(const game_stake&) const = default;
	};

	inline game_stake at(const stakes& set, u32 player, const score& games, u32 first_to) noexcept
	{
		game_stake out;
		if (player > 1 || !first_to)
			return out;
		const auto& own = set[player];
		out.promotion = games[player] == first_to - 1 && own.win == event::promote;
		out.demotion = games[player ^ 1] == first_to - 1 && own.loss == event::demote;
		out.promotion_rank = out.promotion ? own.win_rank : 0;
		out.demotion_rank = out.demotion ? own.loss_rank : 0;
		return out;
	}

	// Each side's stake word and promoted rank if it wins this game (-1 for no promotion).
	// Recorded with the game so spectators and replay viewers write the same values at the same hooks.
	struct game_values
	{
		std::array<u32, 2> stake{};
		std::array<int, 2> promote{-1, -1};
		bool operator==(const game_values&) const = default;
		bool any() const noexcept
		{
			return stake[0] || stake[1] || promote[0] >= 0 || promote[1] >= 0;
		}
	};

	// The word the game reads as a side's stake: 1 shows the promotion chance and 2 the demotion risk. The game draws nothing for
	// its "both" value (4), so a side with both shows its promotion chance.
	inline u32 stake_word(const game_stake& stake) noexcept
	{
		if (stake.promotion)
			return pbd::game_profile::stake_promotion;
		return stake.demotion ? pbd::game_profile::stake_demotion : pbd::game_profile::stake_none;
	}

	// The rank PROMOTED TO names if `player` wins this game: only in a game whose win ends the set, when that set win promotes the
	// player or places them above Beginner.
	inline int promoted_rank(const stakes& set, u32 player, const score& games, u32 first_to) noexcept
	{
		if (player > 1 || !first_to || games[player] != first_to - 1)
			return -1;
		const auto& own = set[player];
		const bool shown = own.win == event::promote || (own.win == event::place && own.win_rank > 0);
		return shown ? static_cast<int>(own.win_rank) : -1;
	}

	inline game_values plan(const stakes& set, const score& games, u32 first_to) noexcept
	{
		game_values out;
		for (u32 player = 0; player < 2; ++player)
		{
			out.stake[player] = stake_word(at(set, player, games, first_to));
			out.promote[player] = promoted_rank(set, player, games, first_to);
		}
		return out;
	}

	// Viewers change values between games, outside VS (8/12) and battle (16/17), so setup and match end use the recorded start.
	// Players instead take values from the game's command.
	inline constexpr bool values_may_change(u32 scene) noexcept
	{
		return scene != 8 && scene != 12 && scene != 16 && scene != 17;
	}

	// A write into the game's memory: a big-endian word or one byte.
	struct guest_write
	{
		u32 address = 0, bytes = 0, value = 0;
		bool operator==(const guest_write&) const = default;
	};
	struct write_plan
	{
		std::array<guest_write, 4> writes{};
		u32 count = 0;
		void add(u32 address, u32 bytes, u32 value) noexcept
		{
			if (count < writes.size())
				writes[count++] = {address, bytes, value};
		}
	};

	// Right after the battle's own setup (0x2a2aac), which has just reset both stake words: both sides' stakes. Nothing when
	// neither side has one, so a game without stakes runs as the game itself left it.
	inline write_plan stake_writes(const game_values& game) noexcept
	{
		write_plan out;
		if (game.stake[0] || game.stake[1])
			for (u32 side = 0; side < 2; ++side)
				out.add(pbd::game_profile::stake_address(side), 4, game.stake[side]);
		return out;
	}

	// The side the game recorded as the match's winner (pbd::game_profile::winner_bytes); none for a draw or anything else.
	inline std::optional<u32> winner_side(const std::array<u8, 3>& bytes) noexcept
	{
		if (bytes == std::array<u8, 3>{1, 0, 1})
			return 0u;
		if (bytes == std::array<u8, 3>{2, 1, 0})
			return 1u;
		return std::nullopt;
	}

	// After 0x2a2488 clears both VS change words, write a promoting winner's rank and change word 1 for PROMOTED TO.
	// Restore the other side's held rank so re-execution with a corrected winner removes the wrong side's promotion.
	inline write_plan promotion_writes(const game_values& game, std::optional<u32> winner, const std::optional<std::array<u32, 2>>& held) noexcept
	{
		write_plan out;
		if (game.promote[0] < 0 && game.promote[1] < 0)
			return out;
		const bool promoted = winner && *winner < 2 && game.promote[*winner] >= 0;
		for (u32 side = 0; side < 2; ++side)
		{
			if (promoted && side == *winner)
				out.add(pbd::game_profile::rank_address(side), 1, static_cast<u32>(game.promote[side]));
			else if (held)
				out.add(pbd::game_profile::rank_address(side), 1, pbd::game_profile::rank_byte((*held)[side]));
		}
		if (promoted)
			out.add(pbd::game_profile::change_address(*winner), 4, pbd::game_profile::change_promoted);
		return out;
	}

	// Preserve a promotion across scene changes only after the guest's change word and rank byte confirm it.
	inline std::optional<u32> kept_promotion(const game_values& game, u32 player, u32 change_word, u8 rank_byte) noexcept
	{
		if (player > 1 || game.promote[player] < 0 || change_word != pbd::game_profile::change_promoted ||
			rank_byte != static_cast<u8>(game.promote[player]))
			return std::nullopt;
		return static_cast<u32>(game.promote[player]);
	}

	// The values as the players record them with a game, for a spectator's start and the replay's game start:
	// {"stakeLeft":0..2,"stakeRight":0..2,"promoteLeft":-1..34,"promoteRight":-1..34}.
	inline json_value values_json(const game_values& game)
	{
		const auto rank = [](int value)
		{
			auto out = json_value::number(0);
			out.text = std::to_string(value);
			return out;
		};
		return json_value::object({{"stakeLeft", json_value::number(game.stake[0])}, {"stakeRight", json_value::number(game.stake[1])},
			{"promoteLeft", rank(game.promote[0])}, {"promoteRight", rank(game.promote[1])}});
	}
	inline std::optional<game_values> read_values(const json_value* value)
	{
		game_values out;
		if (!value || value->kind != pbd::json::json_kind::object)
			return {};
		const std::array<std::string_view, 2> stakes{"stakeLeft", "stakeRight"}, promotions{"promoteLeft", "promoteRight"};
		for (u32 side = 0; side < 2; ++side)
		{
			const auto* promote = value->find(promotions[side]);
			u32 rank = 0;
			if (!pbd::json::detail::number32(value->find(stakes[side]), 0, pbd::game_profile::stake_demotion, out.stake[side]) || !promote)
				return {};
			if (promote->kind == pbd::json::json_kind::number && promote->text == "-1")
				out.promote[side] = -1;
			else if (pbd::json::detail::number32(promote, 0, max_rank, rank))
				out.promote[side] = static_cast<int>(rank);
			else
				return {};
		}
		return out;
	}

	// Barrier announcements pack each side into 16 bits, left side low: win event in bits 0-1, loss event in 2-3,
	// win rank in 4-9 and loss rank in 10-15. Zero plans no stakes; a game uses only matching announcements.
	static_assert(static_cast<u32>(event::none) == 0 && static_cast<u32>(event::place) == 3, "an event fits two bits of a stakes word");
	inline u32 stakes_word(const stakes& set) noexcept
	{
		u32 word = 0;
		for (u32 player = 0; player < 2; ++player)
		{
			const auto& own = set[player];
			const u32 half = static_cast<u32>(own.win) | static_cast<u32>(own.loss) << 2 | (own.win_rank & 63u) << 4 | (own.loss_rank & 63u) << 10;
			word |= half << (16 * player);
		}
		return word;
	}
	// The stakes in `word`; none for no stakes or a word stakes_word never writes.
	inline std::optional<stakes> read_stakes_word(u32 word) noexcept
	{
		if (!word)
			return {};
		stakes out;
		for (u32 player = 0; player < 2; ++player)
		{
			const u32 half = word >> (16 * player) & 0xffffu;
			auto& own = out[player];
			own.win = static_cast<event>(half & 3u);
			own.loss = static_cast<event>(half >> 2 & 3u);
			own.win_rank = half >> 4 & 63u;
			own.loss_rank = half >> 10 & 63u;
			if (own.win_rank > max_rank || own.loss_rank > max_rank)
				return {};
		}
		return out;
	}
	// Both peers hold these words at the same barrier; use no stakes while their launcher updates disagree.
	inline std::optional<stakes> agreed_stakes(u32 own, u32 other) noexcept
	{
		return own == other ? read_stakes_word(own) : std::nullopt;
	}

	// Offline overrides: PBD_TEST_RANK_STAKE=left:1,right:2 (0..4) or PBD_TEST_RANK_PROMOTE=left:11 (0..36), each side once.
	// Invalid input returns false and leaves `out` unusable; read_switches restricts these values to offline VS games.
	inline bool read_test_values(std::string_view text, bool promotion, game_values& out)
	{
		std::array<bool, 2> named{};
		for (std::size_t from = 0, comma = 0; comma != std::string_view::npos; from = comma + 1)
		{
			comma = text.find(',', from);
			const auto item = text.substr(from, comma == std::string_view::npos ? std::string_view::npos : comma - from);
			const auto colon = item.find(':');
			if (colon == std::string_view::npos)
				return false;
			const auto name = item.substr(0, colon), digits = item.substr(colon + 1);
			const u32 side = name == "left" ? 0u : (name == "right" ? 1u : 2u);
			u32 number = 0;
			const auto [end, error] = std::from_chars(digits.data(), digits.data() + digits.size(), number);
			const u32 limit = promotion ? pbd::game_profile::rank_count - 1 : 4;
			if (side > 1 || named[side] || digits.empty() || error != std::errc{} || end != digits.data() + digits.size() || number > limit)
				return false;
			named[side] = true;
			if (promotion)
				out.promote[side] = static_cast<int>(number);
			else
				out.stake[side] = number;
		}
		return named[0] || named[1];
	}
} // namespace pbd::rank_stakes
