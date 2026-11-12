#pragma once

#include <array>
#include <cstdint>
#include <optional>

#include "pbd_stage_rule.h"

namespace pbd::rematch
{
	using u32 = std::uint32_t;
	using u64 = std::uint64_t;
	inline constexpr u64 max_serial = 9007199254740991ull;

	struct selection
	{
		std::array<u32, 2> rows{};
		u32 stage = 0;
		bool operator==(const selection&) const = default;
	};
	struct options
	{
		u32 rounds = 3, time_index = 0;
		std::array<u32, 2> handicap{100, 100};
		bool operator==(const options&) const = default;
	};
	struct sealed_state
	{
		u32 game = 0, end_frame = 0;
		u64 epoch = 0;
		selection selected;
		options rules;
		bool operator==(const sealed_state&) const = default;
	};
	struct load
	{
		u32 previous_game = 0, next_game = 0, end_frame = 0;
		u64 epoch = 0;
		selection selected;
		options rules;
		bool random_stage = false;
		// Change character: the side (0 left, 1 right) that lost the previous game picks again in the stock select. The load still
		// freezes both previous rows, but only the other row and the stage bind the next game. A drawn game has no loser and the
		// stage stays, so a repick never comes with a draw or a random stage.
		std::optional<u32> repick;
		bool operator==(const load&) const = default;
	};
	enum class error
	{
		none,
		invalid_state,
		invalid_load,
		stale_game,
		stale_epoch,
		changed_end,
		changed_fighters,
		changed_options,
		changed_stage,
		repick_after_draw
	};

	struct readiness_gate
	{
		bool announce = false, hold = false;
	};
	// A valid resource handle may be held, but an absent or stale handle must retain the stock predicate's cleanup.
	inline readiness_gate gate_resource(bool peer_rematch, bool spectator_hold, bool valid_handle, bool ready, bool peer_hold)
	{
		return {peer_rematch && valid_handle && ready,
			valid_handle && (spectator_hold || (peer_rematch && ready && peer_hold))};
	}

	inline bool valid(const selection& value)
	{
		return value.rows[0] <= 84 && value.rows[1] <= 84 && stage_rule::loadable(value.stage);
	}
	inline bool valid(const options& value)
	{
		return value.rounds == 3 && value.time_index <= 4 && value.handicap == std::array<u32, 2>{100, 100};
	}
	inline bool valid(const sealed_state& value)
	{
		return value.game >= 1 && value.game <= 19 && value.end_frame && value.epoch < max_serial &&
		       valid(value.selected) && valid(value.rules);
	}
	inline bool valid(const load& value)
	{
		return value.previous_game >= 1 && value.previous_game <= 19 && value.next_game >= 1 && value.next_game <= 19 &&
		       value.end_frame && value.epoch && value.epoch <= max_serial && valid(value.selected) && valid(value.rules) &&
		       (!value.repick || (*value.repick <= 1 && !value.random_stage));
	}
	// What a game started from this load must apply: the frozen rows and stage, except the row of the side that picks again.
	inline bool matches(const load& value, const selection& applied)
	{
		for (u32 side = 0; side < 2; ++side)
			if (value.repick != side && applied.rows[side] != value.selected.rows[side])
				return false;
		return applied.stage == value.selected.stage;
	}
	inline error validate(const load& requested, const sealed_state& previous, bool draw, bool next_set = false)
	{
		if (!valid(previous))
			return error::invalid_state;
		if (!valid(requested))
			return error::invalid_load;
		if (requested.previous_game != previous.game || requested.next_game != (next_set ? 1u : previous.game + (draw ? 0u : 1u)) ||
			(next_set && (draw || requested.repick)))
			return error::stale_game;
		// Draws repeat a game number, so only a separate epoch can reject a previously consumed load.
		if (requested.epoch != previous.epoch + 1)
			return error::stale_epoch;
		if (requested.end_frame != previous.end_frame)
			return error::changed_end;
		if (requested.selected.rows != previous.selected.rows)
			return error::changed_fighters;
		if (requested.rules != previous.rules)
			return error::changed_options;
		if (requested.random_stage ? draw || requested.selected.stage == previous.selected.stage :
									 requested.selected.stage != previous.selected.stage)
			return error::changed_stage;
		if (requested.repick && draw)
			return error::repick_after_draw;
		return error::none;
	}
} // namespace pbd::rematch
