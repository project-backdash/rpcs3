#pragma once

#include "pbd_movement_presets.h"

namespace pbd::practice::movement
{
	// Recovery gates. After an action's last input the dummy gets neutral until it can act again, and before an action it
	// gets neutral until it is back in its resting stance, so a recorded command never starts inside a recovery, a dash or a
	// crouch, and the movement never runs into the action's follow-ups. Both gates are capped, so a missing or wrong reading
	// can never stop the cycle.
	inline constexpr std::uint32_t settle_cap_frames = 180;
	// Without a usable resting-stance reading the recovery gate waits this long instead, about a typical move's recovery.
	inline constexpr std::uint32_t settle_fallback_frames = 40;
	// A fresh input reaches the fighter's move a few frames late, so a recovery is never judged over before this.
	inline constexpr std::uint32_t recovery_min_frames = 4;
	// Neutral frames given before an action the ready gate starts, so a movement's last motion cannot join the action's first
	// inputs into one command (a backdash's b,n followed by a take's b reads as another backdash). A movement that hands
	// straight over to its action (gap_word's flow) skips the gate: there the joined input is the point.
	inline constexpr std::uint32_t action_lead_neutral_frames = 10;
	// Without a usable reading, a run of neutral frames this long before an action stands in for the dummy settling.
	inline constexpr std::uint32_t ready_fallback_neutral_frames = 30;

	enum class settle : std::uint8_t
	{
		waiting,
		actionable,
		unconfirmed,
		fallback,
		capped,
		skipped
	};

	inline const char* settle_text(settle value) noexcept
	{
		return value == settle::actionable ? "actionable" : value == settle::unconfirmed ? "unconfirmed" :
		                                                value == settle::fallback        ? "fallback" :
		                                                value == settle::capped          ? "capped" :
		                                                value == settle::skipped         ? "skipped_backturned" :
		                                                                                   "waiting";
	}

	// `rest`: whether the dummy is in a resting stance now (nullopt: no usable reading). `waited`: neutral frames already
	// given in this gate. `moved`: the dummy was seen out of its resting stance since the action started.
	inline settle recovery_verdict(std::optional<bool> rest, std::uint32_t waited, bool moved) noexcept
	{
		if (waited >= settle_cap_frames)
			return settle::capped;
		if (!rest)
			return waited >= settle_fallback_frames ? settle::fallback : settle::waiting;
		if (!*rest || waited < recovery_min_frames)
			return settle::waiting;
		// Back at rest. An action that never showed a move (its inputs did nothing, or the reading does not follow moves)
		// cannot prove its recovery is over, so the fixed wait applies.
		return moved ? settle::actionable : waited >= settle_fallback_frames ? settle::unconfirmed :
		                                                                       settle::waiting;
	}

	// `neutral`: the consecutive neutral frames the dummy has had, the end of the gap before this gate included.
	inline settle ready_verdict(std::optional<bool> rest, std::uint32_t waited, std::uint32_t neutral) noexcept
	{
		if (waited >= settle_cap_frames)
			return settle::capped;
		if (!rest)
			return neutral >= ready_fallback_neutral_frames ? settle::fallback : settle::waiting;
		return *rest && neutral >= action_lead_neutral_frames ? settle::actionable : settle::waiting;
	}

	// Facing. A take's directions follow DR's input side (record_word / play_word), which does not depend on where the
	// dummy looks; but a dummy with its back to the opponent (often left so after a throw break) performs its back-turned
	// moves instead of the recorded ones. So the dummy must face its opponent before an action or a movement starts. DR
	// places a fighter facing the horizontal direction `forward` with yaw atan2(forward.x, forward.z):
	// yaw 0 faces +Z, and the 16-bit facing spans a full turn the same way. A dummy at rest that faces away is turned with
	// forward taps (turn_around), as a standing fighter never turns by itself.
	inline constexpr float facing_limit_degrees = 30.f;
	// From this on the game gives the back-turned version of a move (a broken throw leaves the dummy at about 103 degrees).
	inline constexpr float backturned_degrees = 90.f;
	enum class facing : std::uint8_t
	{
		unknown,
		toward,
		turned,
		backturned
	};

	inline const char* facing_text(facing value) noexcept
	{
		return value == facing::toward ? "toward" : value == facing::turned ? "turned" :
		                                        value == facing::backturned ? "backturned" :
		                                                                      "unknown";
	}

	// The 16-bit facing (+0x7a) as a yaw in radians.
	inline float facing16_yaw(std::int16_t value) noexcept
	{
		return static_cast<float>(value) * (std::numbers::pi_v<float> / 32768.f);
	}

	// The angle in degrees (0..180) between a fighter's yaw and the direction from it to its opponent, both on the floor
	// plane (world x, z). Nothing when a value is not finite or the two stand on the same spot.
	inline std::optional<float> facing_error_degrees(float yaw, float x, float z, float opponent_x, float opponent_z) noexcept
	{
		const float dx = opponent_x - x, dz = opponent_z - z;
		if (!std::isfinite(yaw) || !std::isfinite(dx) || !std::isfinite(dz) || std::hypot(dx, dz) < 1.f)
			return std::nullopt;
		const float off = std::remainder(std::atan2(dx, dz) - yaw, 2.f * std::numbers::pi_v<float>);
		return std::abs(off) * (180.f / std::numbers::pi_v<float>);
	}

	inline facing facing_verdict(std::optional<float> error_degrees) noexcept
	{
		if (!error_degrees || !std::isfinite(*error_degrees))
			return facing::unknown;
		return *error_degrees <= facing_limit_degrees ? facing::toward : *error_degrees >= backturned_degrees ? facing::backturned :
		                                                                                                        facing::turned;
	}

	inline bool faces_away(facing value) noexcept
	{
		return value == facing::turned || value == facing::backturned;
	}

	// A gate's verdict with the facing on top: a dummy that would be ready but faces away keeps waiting until it faces the
	// opponent or the gate reaches its cap, so the cycle never stops. At the cap a dummy turned part of the way still acts,
	// but a backturned one would play the back-turned move, so its action is skipped. An unknown facing never holds it.
	inline settle with_facing(settle verdict, facing value, std::uint32_t waited) noexcept
	{
		if (verdict == settle::waiting || !faces_away(value))
			return verdict;
		if (verdict == settle::capped || waited >= settle_cap_frames)
			return value == facing::backturned ? settle::skipped : settle::capped;
		return settle::waiting;
	}

	// Turning a dummy that faces away around before an action or a movement pass. Tapping forward (toward the opponent, DR's
	// input side) turns a fighter at rest toward the opponent, so a gate that finds the dummy facing away taps forward
	// instead of waiting: turn_tap_frames of forward, then turn_settle_frames of neutral, after which the facing is checked
	// again. The settle is longer than the action's neutral lead-in, so a tap is never read as the first half of a dash and
	// never joins the next input; a turn in progress always finishes its settle before the dummy moves on. New taps start
	// only below the gate's cap and never while the dummy is seen away from rest (forward there would be a move of its own).
	inline constexpr std::uint32_t turn_tap_frames = 2, turn_settle_frames = 16;
	inline constexpr std::uint32_t turn_cycle_frames = turn_tap_frames + turn_settle_frames;
	static_assert(turn_settle_frames > action_lead_neutral_frames, "a turn tap must stay apart from the action's first input");

	struct turn_around
	{
		// Frames given since this gate's first tap; 0 when no turn has started.
		std::uint32_t frames = 0;

		// Whether this frame belongs to the turn: a tap is settling, or the dummy faces away and `may_start` lets a new tap
		// begin.
		bool active(facing now, bool may_start) const noexcept
		{
			return frames % turn_cycle_frames != 0 || (may_start && faces_away(now));
		}
		// Whether this frame starts a tap (for the log).
		bool tap_starts() const noexcept
		{
			return frames % turn_cycle_frames == 0;
		}
		// This frame's opponent-relative word: forward during the tap, neutral while it settles.
		std::uint16_t next() noexcept
		{
			const std::uint32_t at = frames % turn_cycle_frames;
			++frames;
			return at < turn_tap_frames ? forward : std::uint16_t{0};
		}
		std::uint32_t taps() const noexcept
		{
			return (frames + turn_cycle_frames - 1) / turn_cycle_frames;
		}
		void reset() noexcept
		{
			frames = 0;
		}
	};

	// Whether a gap's movement may hand straight over to the action after it (gap_word's flow): a movement with a hand-over
	// point, an action that follows with an input to play, and a dummy not facing away. Facing away, the pass finishes and
	// the ready gate turns the dummy and gives its lead-in; an unknown facing does not stop the flow, as it holds no
	// gate either. `unread_hit`: the dummy was hit in this gap with no rest reading to say it has recovered, so it may still
	// be in hitstun; the ready gate then starts the action instead.
	inline bool may_flow(const input_sequence& movement, bool action_follows, facing now, bool unread_hit = false) noexcept
	{
		return movement.flow_at && action_follows && !faces_away(now) && !unread_hit;
	}

} // namespace pbd::practice::movement
