#pragma once

#include "pbd_movement_recovery.h"

namespace pbd::practice::movement
{
	// How a wavedash ends when it flows into an action (the Wavedash End row): after the last pass's df the stick goes
	// through the speed's own neutral and then, for Forward or Back, holds that way for the length of the speed's release
	// forward; then the action. Crouch Dash: f,n,d,df,n then the take (a take of f+4 is a crouch dash f+4, a take of f,n,d,df+4 one more
	// crouch dash into a hellsweep). Forward: f,n,d,df,n,f then the take (f+3 makes f,f+3; the f rolls into a take that starts
	// with d/f). Back: f,n,d,df,n,b then the take.
	enum class wavedash_end : std::uint8_t
	{
		crouch_dash,
		forward,
		back
	};
	inline constexpr std::uint32_t wavedash_end_count = 3;
	inline constexpr const char* wavedash_end_name(wavedash_end end) noexcept
	{
		return end == wavedash_end::forward ? "Forward" : end == wavedash_end::back ? "Back" :
		                                                                              "Crouch Dash";
	}

	inline constexpr std::uint16_t direction_bits = pad_up | pad_right | pad_down | pad_left;
	// Whether `next` presses a direction `held` already holds. On consecutive frames the two are one held input, not two
	// presses: a sidestep's u into a take's u+4 is a jump, the Wavedash End's f into f+3 a single f.
	inline constexpr bool joins(std::uint16_t held, std::uint16_t next) noexcept
	{
		return (held & next & direction_bits) != 0;
	}
	// Whether a take's first input `next` holds down together with the movement's direction `way` (d/f after forward, d/b
	// after back). Forward, neutral, d/f is a crouch dash, so a hand-over never puts a neutral between the two: a recorded
	// d/f+2 would come out as an electric. The forward rolls into the d/f instead, which the game reads as d/f+2.
	inline constexpr bool rolls_into(std::uint16_t way, std::uint16_t next) noexcept
	{
		return (next & pad_down) && (next & way & (forward | back));
	}

	// How a dash loop (Continuous Forward Dash, Continuous Backdash) ends when it flows into an action (the Dash End row).
	// Into Move: right after the loop's own neutral, so the action's own forward or back is the next tap (a recorded ff+2 or
	// ff+1+2 comes out as recorded); before an action that starts with d/f (d/b) it hands over before that neutral, so the
	// loop's forward rolls into it (rolls_into). Stop First: then dash_stop_neutral_frames more of neutral, long enough that
	// the game reads no double tap, so a recorded f+4 comes out as f+4.
	enum class dash_end : std::uint8_t
	{
		into_move,
		stop_first
	};
	inline constexpr std::uint32_t dash_end_count = 2;
	inline constexpr std::uint32_t dash_stop_neutral_frames = action_lead_neutral_frames;
	inline constexpr const char* dash_end_name(dash_end end) noexcept
	{
		return end == dash_end::stop_first ? "Stop First" : "Into Move";
	}

	// How the action starts after a hand-over. The movement hands over at its tape position `at`, then a lead-in plays:
	// a wavedash's Wavedash End (`neutral_frames` of neutral, then `lead_frames` of `lead_word`, opponent-relative forward or
	// back), a dash loop's Dash End (Stop First's neutral), and `release_frames` of neutral where the last input before the
	// take would join the take's first (joins). The take then plays from `first`, its first input (its leading neutral frames
	// are skipped, Trim Neutral or not).
	struct flow_plan
	{
		std::uint32_t at = 0;
		std::uint32_t neutral_frames = 0;
		std::uint16_t lead_word = 0;
		std::uint32_t lead_frames = 0;
		std::uint32_t release_frames = 0;
		std::uint32_t first = 0;

		std::uint32_t frames() const noexcept
		{
			return neutral_frames + lead_frames + release_frames;
		}
		// Frame `index` of the lead-in, opponent-relative.
		std::uint16_t word(std::uint32_t index) const noexcept
		{
			return index >= neutral_frames && index < neutral_frames + lead_frames ? lead_word : std::uint16_t{0};
		}
	};
	// A release takes the last frame of the tap or of the Wavedash End's forward / back, so the take still starts on the
	// frame it would have; a one-frame End has none to give up, so its release is added and the take starts a frame later.
	inline flow_plan plan_flow(const input_sequence& movement, std::span<const std::uint16_t> take, screen_side side,
		wavedash_end end = wavedash_end::crouch_dash, dash_end dash = dash_end::into_move) noexcept
	{
		flow_plan plan;
		plan.at = movement.flow_at;
		while (plan.first < take.size() && take[plan.first] == 0)
			++plan.first;
		const std::uint16_t first = plan.first < take.size() ? take[plan.first] : std::uint16_t{0};
		if (movement.tap && plan.at > 1 && joins(sequence_word(movement, movement.frames[plan.at - 1], side), play_word(first, side)))
		{
			--plan.at;
			plan.release_frames = 1;
		}
		if (movement.dash_way)
		{
			if (dash == dash_end::stop_first)
				plan.neutral_frames = dash_stop_neutral_frames;
			else if (rolls_into(movement.dash_way, first))
				while (plan.at > movement.loop_start + 1 && movement.frames[plan.at - 1] == 0)
					--plan.at;
			return plan;
		}
		plan.neutral_frames = movement.flow_neutral_frames;
		if (!plan.neutral_frames || end == wavedash_end::crouch_dash)
			return plan;
		plan.lead_word = end == wavedash_end::forward ? forward : back;
		plan.lead_frames = movement.flow_return_frames;
		if (joins(plan.lead_word, first) && !rolls_into(plan.lead_word, first))
		{
			if (plan.lead_frames > 1)
				--plan.lead_frames;
			plan.release_frames = 1;
		}
		return plan;
	}

	// The dummy back after a hit or a block: the rest reading shows it back on a resting stance, having shown it away from
	// rest since (`left`). The movement's next input goes to the frame right after the one the reading saw, with no wait on
	// top. A reading that never left rest proves nothing about the hit, so then the input delay (recovery_min_frames) stands
	// in. A block or hit reaction that ends standing is back on the stance only at the end of its end command's window,
	// though the game takes the dummy's input from the window's first frame; the contact watch takes the game's own reading
	// of that frame when it has one (contact_tracker's `free_next`).
	inline bool back_at_rest(std::optional<bool> rest, bool left, std::uint32_t waited) noexcept
	{
		return rest.value_or(false) && (left || waited >= recovery_min_frames);
	}

	// The game's Dummy Action values (DR's practice object +0x6C): 0 Standing ... 9 Quick Roll Back, 10 Controller, which takes
	// the dummy's input from its port.
	inline constexpr std::uint32_t dummy_action_standing = 0, dummy_action_controller = 10;
	// The Dummy Action that drives the dummy while playback hands it to the game to get up (movement_pause): the player's
	// own, or Standing in place of Controller, whose neutral port word would leave the dummy lying.
	inline constexpr std::uint32_t getup_action(std::uint32_t chosen) noexcept
	{
		return chosen == dummy_action_controller ? dummy_action_standing : chosen;
	}

	// Playback hands the dummy back to the game's Dummy Action during recovery: Controller's neutral input cannot get it up.
	// Resume on the first actionable frame; another hit restarts the wait, bounded by settle_cap_frames after the last hit.
	// Without a rest reading, the game keeps control until that cap; facing is checked when the next pass starts.
	struct movement_pause
	{
		bool active = false;
		// The reading showed the dummy away from rest since the last hit.
		bool left = false;
		// The pause ended at its cap on the frame just observed, not on a rest reading. A recovery gate the pause held to the
		// gate's own cap is then no sign of a wrong reading.
		bool capped = false;
		std::uint32_t waited = 0;

		// One frame that really ran: whether the dummy's health dropped on it and the rest reading after it.
		void observe(bool hit, std::optional<bool> rest) noexcept
		{
			capped = false;
			if (hit)
			{
				active = true;
				left = rest == false;
				waited = 0;
				return;
			}
			if (!active)
				return;
			++waited;
			left = left || rest == false;
			if (back_at_rest(rest, left, waited))
				active = false;
			else if (waited >= settle_cap_frames)
			{
				active = false;
				capped = true;
			}
		}
		void reset() noexcept
		{
			*this = {};
		}
	};

	// Offense's Action After a Hit or Block = Movement: how long the movement plays after each contact before the dummy
	// goes back to the game's Dummy Action (the loop still finishes the pass it is in).
	inline constexpr std::uint32_t after_contact_frames = 60;
	// The interval of that gap: a one-shot plays its motion once, at once.
	inline std::uint32_t after_contact_wait(const input_sequence& movement) noexcept
	{
		return movement.once ? static_cast<std::uint32_t>(movement.frames.size()) : after_contact_frames;
	}

	// Watches the dummy while the game's own Dummy Action drives it. A hit (its health drops) or a block (it leaves its
	// resting stance, which nothing of ours caused: none of our input reaches it then) is a contact. The movement may start
	// on the first frame the dummy can act again while it does not face away from P1: the frame the game's own reading
	// names (`free_next`), or else the first one back at rest (back_at_rest). Without a usable rest reading only hits are
	// seen, and the fixed recovery wait stands in for rest. A contact the dummy has not recovered from within
	// settle_cap_frames (a long combo, a knockdown) is dropped rather than queued.
	// A series (the contacts before the dummy recovers) is a hit when its health dropped on any of its frames, otherwise a
	// block: series_hit holds that answer from the series' first contact until the next series starts.
	struct contact_tracker
	{
		enum class event : std::uint8_t
		{
			none,
			contact,
			ready,
			dropped
		};
		bool pending = false;
		bool series_hit = false;
		// The reading showed the dummy away from rest during the series.
		bool left = false;
		std::uint32_t waited = 0;
		std::optional<bool> previous_rest;

		// One frame that really ran: whether the dummy's health dropped on it, the rest reading after it and its facing.
		// `free_next`: the game reads the dummy able to act on the next frame, the one the word chosen now plays on, even
		// though its block or hit reaction is still playing out (pbd::practice::dummy_move::contact_batch::free_next).
		// Returns contact for the first contact of a series (a later one only restarts the wait), then ready or dropped.
		event observe(bool hit, std::optional<bool> rest, facing now, bool contacted = false, bool free_next = false) noexcept
		{
			const bool left_rest = previous_rest == true && rest == false;
			previous_rest = rest;
			if (hit || left_rest || contacted)
			{
				const bool first = !pending;
				series_hit = (!first && series_hit) || hit;
				left = (!first && left) || rest == false;
				pending = true;
				waited = 0;
				return first ? event::contact : event::none;
			}
			if (!pending)
				return event::none;
			++waited;
			left = left || rest == false;
			const bool recovered = free_next || (rest ? back_at_rest(rest, left, waited) : waited >= settle_fallback_frames);
			if (recovered && !faces_away(now))
			{
				pending = false;
				return event::ready;
			}
			if (waited >= settle_cap_frames)
			{
				pending = false;
				return event::dropped;
			}
			return event::none;
		}
	};

} // namespace pbd::practice::movement
