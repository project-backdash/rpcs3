#pragma once

#include "pbd_movement_sequence.h"

namespace pbd::practice::movement
{
	inline input_sequence generate(preset kind, std::uint32_t wavedash_speed = default_wavedash_speed)
	{
		input_sequence out;
		const auto size = [&]
		{
			return static_cast<std::uint32_t>(out.frames.size());
		};
		if (kind == preset::backdash_cancel)
		{
			// b, n once, then b (held), db, b, n repeated: the lead-in b, n and the loop's long b make the first backdash; the
			// down-back cancels it, and its b, n and the next long b make the next one.
			append(out.frames, {{back, dash_tap_frames}, {0, dash_gap_frames}});
			out.loop_start = size();
			append(out.frames, {{back, dash_hold_frames}, {static_cast<std::uint16_t>(down | back), dash_cancel_frames}, {back, dash_release_frames},
								   {0, dash_neutral_frames}});
			out.dash_way = back;
		}
		else if (kind == preset::forward_dash)
		{
			// f, n repeated: every tap after the first is the second f of a dash.
			append(out.frames, {{forward, forward_dash_tap_frames}, {0, forward_dash_gap_frames}});
			out.dash_way = forward;
		}
		else if (is_wavedash(kind))
		{
			// Consecutive crouch dashes: f, n, d, df, the release forward, n, and the whole crouch dash again; the rest makes the
			// next forward a fresh press.
			const auto rhythm = wavedash_passes(wavedash_speed);
			for (std::size_t i = 0; i < rhythm.count; ++i)
			{
				const auto& pass = rhythm.passes[i];
				append(out.frames, {{forward, pass.forward_frames}, {0, pass.neutral_frames}, {down, pass.down_frames},
									   {static_cast<std::uint16_t>(down | forward), pass.down_forward_frames}, {forward, pass.release_frames}, {0, pass.rest_frames}});
				if (rhythm.count > 1)
					out.pass_ends.push_back(size());
			}
			out.flow_at = rhythm.passes[0].entry();
			out.flow_neutral_frames = rhythm.passes[0].neutral_frames;
			out.flow_return_frames = rhythm.passes[0].release_frames;
		}
		else if (kind == preset::backdash)
			append(out.frames, {{back, dash_tap_frames}, {0, dash_gap_frames}, {back, dash_hold_frames}});
		else if (kind == preset::sidestep_left || kind == preset::sidestep_right)
		{
			out.sideways = true;
			out.tap = true;
			append(out.frames, {{kind == preset::sidestep_left ? own_left : own_right, sideways_tap_frames}});
		}
		else if (kind == preset::sidewalk_left || kind == preset::sidewalk_right)
		{
			out.sideways = true;
			out.held = true;
			const std::uint16_t way = kind == preset::sidewalk_left ? own_left : own_right;
			append(out.frames, {{way, sideways_tap_frames}, {0, sidewalk_gap_frames}});
			out.loop_start = size();
			append(out.frames, {{way, 1}});
		}
		else if (kind == preset::crouch)
		{
			out.held = true;
			append(out.frames, {{down, 1}});
		}
		else if (kind == preset::jump_back)
		{
			append(out.frames, {{static_cast<std::uint16_t>(up | back), jump_back_hold_frames}, {0, jump_back_cycle_frames - jump_back_hold_frames}});
			out.rest_each_pass = true;
		}
		out.once = is_one_shot(kind);
		if (!out.empty() && !out.flow_at && !out.rest_each_pass)
			out.flow_at = size();
		return out;
	}

	// Offense's Movement Technique as the dummy plays it on its own. Backdash is a technique there: one whole backdash, then
	// neutral, again each time the dummy is back at rest, so it keeps backdashing without cancelling (Continuous Backdash
	// cancels every one). As a one-shot it would never play, with no action to come before it. The other techniques play
	// their own tapes.
	inline constexpr std::uint32_t repeated_backdash_frames = 30;
	static_assert(repeated_backdash_frames > backdash_frames && repeated_backdash_frames <= gap_overrun_limit_frames,
		"a repeated backdash ends on neutral, so each one starts with a fresh back");
	inline input_sequence technique_sequence(preset kind, std::uint32_t wavedash_speed = default_wavedash_speed)
	{
		if (kind != preset::backdash)
			return generate(is_technique(kind) ? kind : preset::none, wavedash_speed);
		input_sequence out;
		append(out.frames, {{back, dash_tap_frames}, {0, dash_gap_frames}, {back, dash_hold_frames}, {0, repeated_backdash_frames - backdash_frames}});
		out.rest_each_pass = true;
		return out;
	}

	// Offense's plain movement inputs for Action After a Hit or Block, in menu order, each played once after the counted
	// hit or block. A held one (a sidewalk, Crouch) holds for after_contact_frames, one second. Fuzzy Duck is a quick human
	// tap of down, fuzzy_duck_frames long, then neutral: the dummy ducks and stands straight back up.
	enum class reaction_input : std::uint8_t
	{
		backdash,
		sidestep_left,
		sidestep_right,
		sidewalk_left,
		sidewalk_right,
		crouch,
		fuzzy_duck
	};
	inline constexpr std::uint32_t reaction_input_count = 7;
	inline constexpr std::uint32_t fuzzy_duck_frames = 8;
	inline constexpr const char* reaction_input_name(reaction_input input) noexcept
	{
		switch (input)
		{
		case reaction_input::backdash: return "Backdash";
		case reaction_input::sidestep_left: return "Sidestep Left";
		case reaction_input::sidestep_right: return "Sidestep Right";
		case reaction_input::sidewalk_left: return "Sidewalk Left";
		case reaction_input::sidewalk_right: return "Sidewalk Right";
		case reaction_input::crouch: return "Crouch";
		case reaction_input::fuzzy_duck: return "Fuzzy Duck";
		}
		return "Backdash";
	}
	inline input_sequence reaction_sequence(reaction_input input)
	{
		switch (input)
		{
		case reaction_input::backdash: return generate(preset::backdash);
		case reaction_input::sidestep_left: return generate(preset::sidestep_left);
		case reaction_input::sidestep_right: return generate(preset::sidestep_right);
		case reaction_input::sidewalk_left: return generate(preset::sidewalk_left);
		case reaction_input::sidewalk_right: return generate(preset::sidewalk_right);
		case reaction_input::crouch: return generate(preset::crouch);
		case reaction_input::fuzzy_duck: break;
		}
		input_sequence out;
		append(out.frames, {{down, fuzzy_duck_frames}});
		out.once = true;
		out.flow_at = static_cast<std::uint32_t>(out.frames.size());
		return out;
	}

	// The loop cursor runs from 0 to the tape's size: 0 is the start of a run, and the size the end of a pass (a one-shot's
	// motion, a dash loop, a wavedash pass or one frame of a held input), which wraps to loop_start when the loop goes on. On a
	// tape of several passes each pass end inside it is a pass boundary too. A cursor past the size is stale and counts as a
	// pass end.
	inline bool at_pass_boundary(const input_sequence& movement, std::uint32_t position) noexcept
	{
		return position == 0 || position >= movement.frames.size() ||
		       std::find(movement.pass_ends.begin(), movement.pass_ends.end(), position) != movement.pass_ends.end();
	}

	// The pass the frame at `position` belongs to: where it starts and ends on the tape (a cursor at the end belongs to the last).
	struct pass_span
	{
		std::uint32_t start = 0, end = 0;
	};
	inline pass_span pass_at(const input_sequence& movement, std::uint32_t position) noexcept
	{
		const auto size = static_cast<std::uint32_t>(movement.frames.size());
		std::uint32_t start = 0;
		for (const auto end : movement.pass_ends)
		{
			if (position < end || end >= size)
				return {start, end};
			start = end;
		}
		return {start, size};
	}

	// Whether the dummy may be held for readiness here (gap_word's hold): between two passes, never inside a held input.
	inline bool at_pass_start(const input_sequence& movement, std::uint32_t position) noexcept
	{
		return !movement.frames.empty() && (position == 0 || (!movement.held && at_pass_boundary(movement, position)));
	}

	// As at_pass_start, and the pass is due now: a one-shot waits on neutral for the interval until its motion would end
	// with it, and only then does the dummy need to be ready.
	inline bool pass_due(const input_sequence& movement, std::uint32_t position, std::uint32_t remaining) noexcept
	{
		return at_pass_start(movement, position) && !(movement.once && position == 0 && remaining > movement.frames.size());
	}

	// With no action to play: whether the movement plays at all. A one-shot plays only before an action (or once after a
	// contact), so alone it does not play and the dummy follows the game's own Dummy Action.
	inline bool plays_alone(const input_sequence& movement) noexcept
	{
		return !movement.once;
	}

	// One frame of the gap between two actions, as a screen word for a dummy on `side`. The loop plays from `position`.
	// `remaining` counts the interval's frames still to play and is decremented; once it is zero the loop only finishes the
	// pass it is in (counted in `overrun`, cut at gap_overrun_limit_frames), so a gap ends between two passes with the cursor
	// back at 0. `done` marks the first frame after the gap: its word is neutral and the caller moves on to the ready gate in
	// that same frame. An empty tape (Neutral) keeps the interval neutral.
	// A one-shot gives neutral until its motion would end with the interval, then plays it; it never ends the gap before
	// its motion has played.
	// `hold` keeps the dummy neutral at a pass start (not ready to move yet); the interval still runs meanwhile, and a held
	// one-shot whose interval is over keeps waiting. A dash loop held at its wrap goes back to the lead-in, so the dummy starts
	// again with a whole b, n, b rather than a lone back.
	// `flow_at` (an action follows and may take over at once, may_flow; 0 when not): the hand-over's position in its pass
	// (plan_flow's `at`). There, when the interval is used up by the end of this pass, the gap ends with `flow` set on the
	// frame: the action starts in that same frame, so not one frame passes between the movement's last input and the
	// action's first.
	struct gap_frame
	{
		std::uint16_t word = 0;
		bool done = false;
		bool flow = false;
	};
	inline gap_frame gap_word(const input_sequence& movement, std::uint32_t& position, std::uint32_t& remaining, std::uint32_t& overrun,
		screen_side side, bool hold = false, std::uint32_t flow_at = 0) noexcept
	{
		const auto size = static_cast<std::uint32_t>(movement.frames.size());
		if (!size)
		{
			position = 0;
			if (!remaining)
				return {0, true};
			--remaining;
			return {0, false};
		}
		if (position > size)
			position = size;
		// A one-shot's motion is the whole of its run.
		if (movement.once && position == size)
			remaining = 0;
		const auto pass = pass_at(movement, position);
		if (flow_at && pass.start + flow_at <= pass.end && position == pass.start + flow_at && remaining <= pass.end - position)
		{
			position = 0;
			remaining = 0;
			return {0, true, true};
		}
		const bool waiting = movement.once && position == 0;
		if (!remaining && !waiting && (at_pass_boundary(movement, position) || overrun >= gap_overrun_limit_frames))
		{
			position = 0;
			return {0, true};
		}
		if (waiting && remaining > size)
		{
			--remaining;
			return {0, false};
		}
		if (hold && at_pass_start(movement, position))
		{
			if (remaining)
				--remaining;
			position = 0;
			return {0, false};
		}
		if (remaining)
			--remaining;
		else
			++overrun;
		if (position == size)
			position = movement.loop_start < size ? movement.loop_start : 0;
		const std::uint16_t word = movement.frames[position++];
		return {sequence_word(movement, word, side), false};
	}

} // namespace pbd::practice::movement
