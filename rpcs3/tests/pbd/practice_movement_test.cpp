#include "pbd_test.h"

#include "Emu/PBD/pbd_json_reader.h"
#include "Emu/PBD/pbd_practice_presentation.h"
#include "Emu/Cell/pbd_practice_controls.h"
#include "Emu/Cell/pbd_practice_mouse.h"
#include "Emu/PBD/pbd_movement_observation.h"
#include "Emu/Cell/pbd_game_overlay.h"

#include <limits>
#include <vector>

namespace
{
	using pbd::test::check;

	std::vector<std::uint16_t> run_movement_gap(const pbd::practice::movement::input_sequence& movement, std::uint32_t& position, std::uint32_t interval, pbd::practice::movement::screen_side side)
	{
		using namespace pbd::practice::movement;
		using words = std::vector<std::uint16_t>;
		words out;
		std::uint32_t overrun = 0;
		for (std::uint32_t guard = 0; guard < 10000; ++guard)
		{
			const auto frame = gap_word(movement, position, interval, overrun, side);
			if (frame.done)
				break;
			out.push_back(frame.word);
		}
		return out;
	}

	void orientation_checks()
	{
		using namespace pbd::practice::movement;
		constexpr std::uint16_t L = 0x8000;
		constexpr std::uint16_t R = 0x2000;
		constexpr std::uint16_t D = 0x4000;
		constexpr std::uint16_t U = 0x1000;
		// Side conversion: a dummy on the right faces left, so toward the opponent is Left on screen.
		check(oriented(forward, screen_side::right) == L && oriented(back, screen_side::right) == R &&
				  oriented(static_cast<std::uint16_t>(down | forward), screen_side::right) == (D | L) &&
				  oriented(static_cast<std::uint16_t>(down | back), screen_side::right) == (D | R),
			"a dummy on the right must move toward the opponent with Left and away with Right");
		check(oriented(forward, screen_side::left) == R && oriented(back, screen_side::left) == L, "a dummy on the left keeps the relative bits");
		check(oriented(static_cast<std::uint16_t>(U | 0x0080 | 0x0100), screen_side::right) == (U | 0x0080 | 0x0100),
			"vertical directions, attacks and system bits must never be mirrored");
		check(oriented(static_cast<std::uint16_t>(L | R), screen_side::right) == (L | R), "both horizontal bits held must stay both held");
		for (std::uint32_t word = 0; word <= 0xffff; word += 0x0101)
			check(oriented(oriented(static_cast<std::uint16_t>(word), screen_side::right), screen_side::right) == static_cast<std::uint16_t>(word),
				"side conversion must be its own inverse");
	}

	void whole_movement_intervals(const pbd::practice::movement::input_sequence& fast)
	{
		using namespace pbd::practice::movement;
		using words = std::vector<std::uint16_t>;
		constexpr std::uint16_t L = 0x8000;
		constexpr std::uint16_t R = 0x2000;
		constexpr std::uint16_t D = 0x4000;
		// One crouch dash at 4 a second in screen words: f 4, n 2, d 3, df 1, f 2, n 3 (15 frames).
		const auto crouch_dash = [&](std::uint16_t toward)
		{
			words out;
			const auto add = [&](std::uint32_t count, std::uint16_t word)
			{
				out.insert(out.end(), count, word);
			};
			add(4, toward);
			add(2, 0);
			add(3, D);
			add(1, static_cast<std::uint16_t>(D | toward));
			add(2, toward);
			add(3, 0);
			return out;
		};
		std::uint32_t position = 0;
		{
			words wanted = crouch_dash(L);
			const auto second = crouch_dash(L);
			wanted.insert(wanted.end(), second.begin(), second.end());
			check(run_movement_gap(fast, position, 20, screen_side::right) == wanted && position == 0,
				"a 20-frame interval at 4 a second on the right plays two whole crouch dashes (30 frames), never half of one");
			check(run_movement_gap(fast, position, 15, screen_side::right) == crouch_dash(L), "an interval of one whole cycle ends at the wrap");
			check(run_movement_gap(fast, position, 1, screen_side::left) == crouch_dash(R), "from the left only the horizontal bits mirror, and a pass is never cut");
		}
		check(run_movement_gap(input_sequence{}, position, 120, screen_side::right) == words(120, 0), "with no movement the whole interval stays neutral");
		{
			std::uint32_t none = 0, overrun = 0;
			const auto frame = gap_word(input_sequence{}, position, none, overrun, screen_side::right);
			check(frame.done && frame.word == 0, "a finished interval with no movement ends the gap at once");
		}
		{
			// A long custom pass past the interval is cut at the overrun limit; the ready gate's neutral lead-in follows it.
			const input_sequence long_loop{words(200, R), 0};
			std::uint32_t cursor = 0;
			check(run_movement_gap(long_loop, cursor, 30, screen_side::left).size() == 30 + gap_overrun_limit_frames && cursor == 0,
				"a pass longer than the overrun limit is cut there");
		}
		{
			const auto dash = generate(preset::forward_dash);
			// A 37-frame interval runs out inside the fifth 8-frame tap, which is finished: 40 frames.
			const words played = run_movement_gap(dash, position, 37, screen_side::right);
			check(played.size() == 40 && played[0] == L && played[3] == L && played[4] == 0 && played[7] == 0 && played[8] == L &&
					  played[39] == 0 && position == 0,
				"a forward dash gap on the right plays whole taps toward the player and ends on a tap's neutral");
			std::uint32_t mid = 0;
			const auto cancel = generate(preset::backdash_cancel);
			// The lead-in (2) leaves 28 frames of the interval, which run out inside the third loop; that loop is finished.
			const words looped = run_movement_gap(cancel, mid, 30, screen_side::right);
			words wanted;
			const auto add = [&](std::uint32_t count, std::uint16_t word)
			{
				wanted.insert(wanted.end(), count, word);
			};
			add(1, R);
			add(1, 0);
			for (int loop = 0; loop < 3; ++loop)
			{
				add(10, R);
				add(1, D | R);
				add(1, R);
				add(1, 0);
			}
			check(looped == wanted && mid == 0, "a backdash cancel plays its lead-in once, wraps to the long back and ends on a whole loop");
		}
	}

	void restored_movement_cursor(const pbd::practice::movement::input_sequence& fast)
	{
		using namespace pbd::practice::movement;
		constexpr std::uint16_t L = 0x8000;
		// A discarded preparation restores the cursor and counters, and the replayed frame produces the same word.
		{
			std::uint32_t cursor = 2, remaining = 9, overrun = 0;
			const std::uint32_t saved = cursor, saved_remaining = remaining;
			const auto first = gap_word(fast, cursor, remaining, overrun, screen_side::right);
			cursor = saved;
			remaining = saved_remaining;
			const auto again = gap_word(fast, cursor, remaining, overrun, screen_side::right);
			check(again.word == first.word && !again.done && cursor == 3 && remaining == 8, "restoring the saved cursor must replay the same frame");
			cursor = 99;
			check(gap_word(fast, cursor, remaining, overrun, screen_side::right).word == L && cursor == 1,
				"a stale cursor must resume at the loop start, never past the tape");
		}
	}

	void recovery_and_ready_gates()
	{
		using namespace pbd::practice::movement;
		// Recovery gates: bounded, and judged on the dummy's resting stance when it can be read.
		{
			static_assert(settle_cap_frames == 180 && settle_fallback_frames < settle_cap_frames && recovery_min_frames < settle_fallback_frames &&
							  action_lead_neutral_frames < ready_fallback_neutral_frames,
				"gate tuning stays ordered");
			using settle_t = pbd::practice::movement::settle;
			check(recovery_verdict(false, 179, true) == settle_t::waiting && recovery_verdict(false, 180, true) == settle_t::capped,
				"a dummy that never returns to rest is released at the cap");
			check(recovery_verdict(true, 180, false) == settle_t::capped && recovery_verdict(std::nullopt, 500, false) == settle_t::capped,
				"the cap bounds every reading");
			check(recovery_verdict(true, 0, true) == settle_t::waiting && recovery_verdict(true, recovery_min_frames, true) == settle_t::actionable,
				"back at rest after a move: actionable once the input delay has passed");
			check(recovery_verdict(false, 60, true) == settle_t::waiting, "still in the move or its recovery: keep waiting");
			check(recovery_verdict(true, settle_fallback_frames - 1, false) == settle_t::waiting &&
					  recovery_verdict(true, settle_fallback_frames, false) == settle_t::unconfirmed,
				"a move that never showed cannot prove its recovery is over, so the fixed wait applies");
			check(recovery_verdict(std::nullopt, settle_fallback_frames - 1, true) == settle_t::waiting &&
					  recovery_verdict(std::nullopt, settle_fallback_frames, true) == settle_t::fallback,
				"without a reading the recovery gate waits the fixed recovery");
			check(ready_verdict(true, 0, action_lead_neutral_frames) == settle_t::actionable && ready_verdict(true, 0, action_lead_neutral_frames - 1) == settle_t::waiting,
				"an action needs the neutral lead-in even from rest");
			check(ready_verdict(false, 0, 500) == settle_t::waiting && ready_verdict(false, 180, 500) == settle_t::capped,
				"an action waits for rest, up to the cap");
			check(ready_verdict(std::nullopt, 0, ready_fallback_neutral_frames) == settle_t::fallback &&
					  ready_verdict(std::nullopt, 0, ready_fallback_neutral_frames - 1) == settle_t::waiting,
				"without a reading a longer neutral run stands in for settling");
			// Every gate ends by the cap whatever the reading does.
			for (const std::optional<bool> reading : {std::optional<bool>{}, std::optional<bool>{true}, std::optional<bool>{false}})
			{
				std::uint32_t waited = 0;
				while (recovery_verdict(reading, waited, false) == settle_t::waiting)
					++waited;
				check(waited <= settle_cap_frames, "the recovery gate always ends by the cap");
				waited = 0;
				while (ready_verdict(reading, waited, 0) == settle_t::waiting)
					++waited;
				check(waited <= settle_cap_frames, "the ready gate always ends by the cap");
			}
		}
	}

	void facing_readings_and_gates()
	{
		using namespace pbd::practice::movement;
		// Facing: the dummy's yaw against the direction to P1 on the floor plane, yaw 0 facing +Z.
		{
			using settle_t = pbd::practice::movement::settle;
			constexpr float pi = std::numbers::pi_v<float>;
			const auto about = [](std::optional<float> value, float wanted, float within = .2f)
			{
				return value && std::abs(*value - wanted) < within;
			};
			// The Centre restart: P1 at x -1000 facing +X (yaw pi/2), the dummy at +1000 facing -X.
			check(about(facing_error_degrees(-pi / 2, 1000.f, 0.f, -1000.f, 0.f), 0.f) && about(facing_error_degrees(pi / 2, -1000.f, 0.f, 1000.f, 0.f), 0.f),
				"both fighters of the Centre restart face each other");
			check(about(facing_error_degrees(pi / 2, 1000.f, 0.f, -1000.f, 0.f), 180.f), "a dummy with its back to P1 is 180 degrees off");
			check(about(facing_error_degrees(-pi / 2 + pi / 4, 1000.f, 0.f, -1000.f, 0.f), 45.f) &&
					  about(facing_error_degrees(-pi / 2 - pi / 4, 1000.f, 0.f, -1000.f, 0.f), 45.f),
				"turned either way gives the same angle");
			check(about(facing_error_degrees(-pi / 2 + 2 * pi, 1000.f, 0.f, -1000.f, 0.f), 0.f), "a yaw a whole turn on is the same facing");
			// A Practice capture: the dummy at (8530, -96) with yaw -90.5 degrees, P1 at (7570, -88).
			check(about(facing_error_degrees(-90.5f * pi / 180.f, 8530.f, -96.f, 7570.f, -88.f), 1.f, .5f),
				"a captured standing dummy faces P1 within a degree");
			check(!facing_error_degrees(0.f, 5.f, 5.f, 5.f, 5.f) && !facing_error_degrees(std::numeric_limits<float>::quiet_NaN(), 0.f, 0.f, 1000.f, 0.f),
				"fighters on one spot or an unreadable yaw give no facing");
			check(about(std::optional<float>(facing16_yaw(-0x4000) * 180.f / pi), -90.f) && about(std::optional<float>(facing16_yaw(0x4000) * 180.f / pi), 90.f),
				"the 16-bit facing spans a full turn");
			check(facing_verdict(0.f) == facing::toward && facing_verdict(facing_limit_degrees) == facing::toward &&
					  facing_verdict(facing_limit_degrees + 1.f) == facing::turned && facing_verdict(backturned_degrees) == facing::backturned &&
					  facing_verdict(180.f) == facing::backturned && facing_verdict(std::nullopt) == facing::unknown,
				"facing within 30 degrees faces P1");
			check(facing_verdict(89.9f) == facing::turned && facing_verdict(90.f) == facing::backturned && facing_verdict(103.f) == facing::backturned &&
					  facing_verdict(87.f) == facing::turned && facing_verdict(39.f) == facing::turned,
				"from 90 degrees on the game gives back-turned moves (a broken throw leaves the dummy at about 103)");
			check(with_facing(settle_t::actionable, facing::backturned, 0) == settle_t::waiting &&
					  with_facing(settle_t::fallback, facing::turned, 10) == settle_t::waiting,
				"a ready dummy facing away waits");
			check(with_facing(settle_t::actionable, facing::toward, 0) == settle_t::actionable &&
					  with_facing(settle_t::actionable, facing::unknown, 0) == settle_t::actionable,
				"facing P1, or an unknown facing, does not hold it");
			check(with_facing(settle_t::waiting, facing::toward, 5) == settle_t::waiting && with_facing(settle_t::capped, facing::backturned, 180) == settle_t::skipped &&
					  with_facing(settle_t::capped, facing::turned, 180) == settle_t::capped && with_facing(settle_t::capped, facing::toward, 180) == settle_t::capped &&
					  with_facing(settle_t::capped, facing::unknown, 180) == settle_t::capped && with_facing(settle_t::actionable, facing::turned, 180) == settle_t::capped,
				"the facing never shortens a wait or outlasts the cap; backturned at the cap the action is skipped");
			std::uint32_t waited = 0;
			while (with_facing(ready_verdict(true, waited, 500), facing::backturned, waited) == settle_t::waiting)
				++waited;
			check(waited == settle_cap_frames && with_facing(ready_verdict(true, waited, 500), facing::backturned, waited) == settle_t::skipped,
				"a dummy that stays backturned ends the gate at the cap with no action, so the cycle never stops");
			std::uint32_t partly = 0;
			while (with_facing(ready_verdict(true, partly, 500), facing::turned, partly) == settle_t::waiting)
				++partly;
			check(partly == settle_cap_frames && with_facing(ready_verdict(true, partly, 500), facing::turned, partly) == settle_t::capped,
				"a dummy turned part of the way still acts at the cap");
		}
	}

	void held_movement_passes()
	{
		using namespace pbd::practice::movement;
		constexpr std::uint16_t L = 0x8000;
		constexpr std::uint16_t R = 0x2000;
		constexpr std::uint16_t D = 0x4000;
		// Holding the movement at a pass start: neutral, the interval still running, never in the middle of a pass.
		{
			const auto dash = generate(preset::forward_dash);
			check(at_pass_start(dash, 0) && !at_pass_start(dash, 1) && at_pass_start(dash, 99) && !at_pass_start(input_sequence{}, 0),
				"a pass starts at the tape's start (or a stale cursor's wrap); an empty tape has none");
			const auto cancel = generate(preset::backdash_cancel);
			check(at_pass_start(cancel, 0) && at_pass_start(cancel, 15) && !at_pass_start(cancel, cancel.loop_start) && !at_pass_start(cancel, 3),
				"the tape's start and each loop's end are pass starts, never inside a b, n, b");
			std::uint32_t cursor = 0, remaining = 5, overrun = 0;
			const auto held = gap_word(dash, cursor, remaining, overrun, screen_side::right, true);
			check(held.word == 0 && !held.done && cursor == 0 && remaining == 4, "a held pass start gives neutral and keeps the cursor");
			const auto moved = gap_word(dash, cursor, remaining, overrun, screen_side::right, false);
			check(moved.word == L && cursor == 1, "released, the pass starts from its first frame");
			const auto inside = gap_word(dash, cursor, remaining, overrun, screen_side::right, true);
			const auto further = gap_word(dash, cursor, remaining, overrun, screen_side::right, true);
			check(inside.word == L && further.word == L && cursor == 3, "a hold never stops a pass in the middle");
			std::uint32_t start = 0, none = 0, over = 0;
			check(gap_word(dash, start, none, over, screen_side::right, true).done, "a finished interval still ends the gap at a held pass start");
			// Continuous Backdash wraps between the b, n and the long b of one backdash, so a hold there goes back to the lead-in.
			std::uint32_t wrap = 15, wrap_frames = 3, wrap_over = 0;
			const auto held_wrap = gap_word(cancel, wrap, wrap_frames, wrap_over, screen_side::right, true);
			check(held_wrap.word == 0 && !held_wrap.done && wrap == 0 && wrap_frames == 2, "a backdash cancel held at its wrap goes back to the lead-in");
			check(gap_word(cancel, wrap, wrap_frames, wrap_over, screen_side::right, false).word == R && wrap == 1,
				"released, it starts again with a whole backdash");
			std::uint32_t late = 15, late_frames = 1, late_over = 0;
			gap_word(cancel, late, late_frames, late_over, screen_side::right, true);
			check(gap_word(cancel, late, late_frames, late_over, screen_side::right, true).done && late_over == 0,
				"an interval that runs out while the lead-in is held ends the gap at once");
			// A held crouch has no pass start inside it: a hold never releases it.
			const auto crouch = generate(preset::crouch);
			std::uint32_t low = 0, low_frames = 10, low_over = 0;
			check(gap_word(crouch, low, low_frames, low_over, screen_side::right, false).word == D, "the crouch starts");
			for (int frame = 0; frame < 5; ++frame)
				check(gap_word(crouch, low, low_frames, low_over, screen_side::right, true).word == D, "a hold never stands a crouch up");
		}
	}

	void turnaround_taps_and_settle()
	{
		using namespace pbd::practice::movement;
		using words = std::vector<std::uint16_t>;
		constexpr std::uint16_t L = 0x8000;
		constexpr std::uint16_t R = 0x2000;
		// A backturned dummy is turned around with forward taps: turn_tap_frames of forward, then turn_settle_frames of neutral,
		// the facing checked again after each. A tap in progress always settles; no tap starts at the cap or away from rest.
		{
			turn_around turn;
			check(!turn.active(facing::toward, true) && turn.active(facing::turned, true) && turn.active(facing::backturned, true) &&
					  !turn.active(facing::unknown, true) && !turn.active(facing::backturned, false) && !turn.active(facing::turned, false),
				"a dummy facing away (turned or backturned) with the gate open starts a turn");
			words given;
			while (turn.active(facing::backturned, true) && given.size() < turn_cycle_frames)
				given.push_back(turn.next());
			check(given.size() == turn_cycle_frames && given[0] == forward && given[1] == forward &&
					  std::all_of(given.begin() + turn_tap_frames, given.end(), [](std::uint16_t word)
						  {
							  return word == 0;
						  }),
				"a tap is two frames of forward, then neutral");
			check(!turn.active(facing::toward, true) && turn.taps() == 1, "a dummy facing P1 after the settle needs no more taps");
			turn.reset();
			words cut;
			cut.push_back(turn.next());
			while (turn.active(facing::toward, false))
				cut.push_back(turn.next());
			check(cut.size() == turn_cycle_frames && cut[0] == forward && cut[1] == forward && cut.back() == 0,
				"a tap started before the dummy turned or the gate capped still settles in full");
			check(play_word(forward, screen_side::right) == L && play_word(forward, screen_side::left) == R, "the tap is toward P1 on either side");
		}
	}

	void contact_recovery_movement()
	{
		using namespace pbd::practice::movement;
		// Offense's Action After a Hit or Block: a contact, then the movement once the dummy has recovered.
		{
			using event = contact_tracker::event;
			const auto at_rest = std::optional<bool>{true}, moving = std::optional<bool>{false};
			{
				contact_tracker watch;
				watch.previous_rest = at_rest;
				check(watch.observe(false, at_rest, facing::toward) == event::none, "a dummy standing still is no contact");
				check(watch.observe(false, moving, facing::toward) == event::contact && watch.pending, "leaving the resting stance is a block (or a hit)");
				for (int frame = 0; frame < 20; ++frame)
					check(watch.observe(false, moving, facing::toward) == event::none, "still in block stun");
				check(watch.observe(false, at_rest, facing::toward) == event::ready && !watch.pending, "back at rest and facing P1: the movement may start");
			}
			{
				// The first frame back at rest is the first frame the dummy can act: no input-delay wait on top of the reading.
				contact_tracker watch;
				watch.previous_rest = at_rest;
				check(watch.observe(false, moving, facing::toward) == event::contact && watch.left, "a short block");
				check(watch.observe(false, moving, facing::toward) == event::none, "one frame of block stun");
				check(watch.observe(false, at_rest, facing::toward) == event::ready && watch.waited < recovery_min_frames,
					"ready on the first frame back at rest");
				check(watch.observe(true, at_rest, facing::toward) == event::contact && !watch.left, "a hit the reading has not shown yet");
				check(watch.observe(false, moving, facing::toward) == event::none && watch.left, "then shows it away from rest");
				check(watch.observe(false, at_rest, facing::toward) == event::ready && watch.waited == 1, "ready on the first frame back at rest");
			}
			{
				contact_tracker watch;
				watch.previous_rest = at_rest;
				check(watch.observe(true, at_rest, facing::toward) == event::contact, "a hit is a contact even before the stance changes");
				check(watch.observe(false, at_rest, facing::toward) == event::none, "a recovery is never judged before the input delay");
				for (std::uint32_t frame = 2; frame < recovery_min_frames; ++frame)
					watch.observe(false, at_rest, facing::toward);
				check(watch.observe(false, at_rest, facing::toward) == event::ready, "then a dummy at rest has recovered");
			}
			{
				contact_tracker watch;
				watch.previous_rest = at_rest;
				watch.observe(false, moving, facing::toward);
				check(watch.observe(true, moving, facing::toward) == event::none && watch.waited == 0, "a later hit of the same combo restarts the wait, not the series");
				check(watch.observe(false, at_rest, facing::backturned) == event::none, "a dummy back at rest but turned away waits");
				std::uint32_t frames = 1;
				while (watch.observe(false, at_rest, facing::backturned) == event::none)
					++frames;
				check(!watch.pending && frames + 1 == settle_cap_frames, "a contact the dummy never recovers from is dropped at the cap");
			}
			{
				contact_tracker watch;
				check(watch.observe(true, std::nullopt, facing::unknown) == event::contact, "without a rest reading a hit is still a contact");
				std::uint32_t frames = 1;
				while (watch.observe(false, std::nullopt, facing::unknown) == event::none)
					++frames;
				check(frames == settle_fallback_frames, "and the fixed recovery wait stands in for rest");
				check(watch.observe(false, std::nullopt, facing::unknown) == event::none, "with no reading, only hits are contacts");
			}
			check(after_contact_frames == 60, "the movement after a contact plays for a second");
		}
	}

	void move_entries_and_rest_learning()
	{
		using namespace pbd::practice::movement;
		// The move-entry reading and the resting-stance learner.
		{
			check(move_index(0x21716474, 1065, 0x21716474 + 546 * 0x4c) == 546u && move_index(0x21716474, 1065, 0x21716474) == 0u,
				"an exact entry of the table gives its index");
			check(!move_index(0x21716474, 1065, 0x21716474 + 1065 * 0x4c) && !move_index(0x21716474, 1065, 0x21716474 + 3) &&
					  !move_index(0x21716474, 1065, 0x21716470) && !move_index(0, 1065, 0x4c) && !move_index(0x21716474, 0, 0x21716474) &&
					  !move_index(0x21716474, max_move_entries + 1, 0x21716474),
				"anything but an exact entry inside the table is unreadable");
			rest_learner learner;
			learner.reset(7);
			for (std::uint32_t frame = 1; frame < rest_learn_frames; ++frame)
				check(!learner.observe(220u, true), "a stance is not learned early");
			check(!learner.learned() && learner.observe(220u, true) && learner.learned() && learner.at_rest(220) && !learner.at_rest(221),
				"an entry held with a neutral word for rest_learn_frames becomes a resting stance");
			for (std::uint32_t frame = 0; frame < 400; ++frame)
				learner.observe(220u, true);
			for (std::uint32_t frame = 0; frame < 300; ++frame)
				learner.observe(231u, frame % 2 == 0);
			check(!learner.at_rest(231), "an entry that never stays still with a neutral word is never a stance");
			for (std::uint32_t frame = 0; frame < 80; ++frame)
				learner.observe(500u, true);
			check(learner.at_rest(220) && !learner.at_rest(500), "a briefly held entry (a knockdown) stays out while standing dominates");
			check(learner.settled(500) && !learner.settled(220), "an entry held still with a neutral word right now counts as settled");
			learner.observe(500u, false);
			check(!learner.settled(500), "a non-neutral word ends the settled run");
			for (std::uint32_t frame = 0; frame < 300; ++frame)
				learner.observe(std::nullopt, true);
			check(learner.at_rest(220) && !learner.settled(220), "unreadable frames teach nothing, forget nothing and settle nothing");
			// Frames the movement is paused on (the dummy may be lying on the floor) settle a still dummy but teach no stance.
			const std::size_t known = learner.count;
			for (std::uint32_t frame = 0; frame < 600; ++frame)
				check(!learner.observe(38u, true, teacher::none), "a paused frame learns nothing");
			check(learner.settled(38) && !learner.at_rest(38) && learner.count == known, "lying still while paused settles, never becomes a stance");
			// The rest reading: while the dummy is hurt a still entry is not rest (lying on the floor is still too); a learned
			// stance, or the stance the dummy rested on before, is.
			check(!learner.resting(38, true) && learner.resting(220, true), "hurt, a still entry is not rest, a learned stance is");
			for (std::uint32_t frame = 0; frame < rest_learn_frames; ++frame)
				learner.observe(77u, true);
			check(learner.settled(77) && !learner.at_rest(77) && learner.resting(77, false) && learner.stance == 77u,
				"not hurt, a still entry is rest, and becomes the stance the dummy rests on");
			check(learner.resting(77, true) && !learner.resting(38, true), "hurt, the stance it rested on before still counts");
			// The game's own Dummy Action: the stance it holds the dummy still on counts while the dummy is hurt (the game gets
			// it up into that stance), joins the stances without outweighing standing however long the game holds it, and
			// never displaces the stance our neutral rests the dummy in.
			for (std::uint32_t frame = 0; frame < rest_learn_frames - 1; ++frame)
				check(!learner.observe(91u, true, teacher::game), "not yet");
			check(learner.observe(91u, true, teacher::game) && learner.held == 91u && learner.resting(91, true), "the game's crouch is learned and held");
			for (std::uint32_t frame = 0; frame < 5000; ++frame)
				learner.observe(91u, true, teacher::game);
			check(learner.at_rest(220) && !learner.at_rest(91) && learner.stance == 77u, "however long the game holds it, standing still dominates");
			for (std::uint32_t frame = 0; frame < 30; ++frame)
				learner.observe(220u, true, teacher::game);
			check(learner.held == 91u && learner.resting(91, true), "an entry not yet held still is not the game's stance");
			learner.reset(8);
			check(!learner.learned() && learner.identity == 8 && !learner.stance && !learner.held, "a new moveset starts the learning again");
			// With nothing learned from our own neutral, the game's standing alone is a usable reading.
			for (std::uint32_t frame = 0; frame < rest_learn_frames; ++frame)
				learner.observe(220u, true, teacher::game);
			check(learner.learned() && learner.at_rest(220) && !learner.at_rest(221), "the game's standing is the reading until ours is learned");
		}
	}

	void throw_reaction_rest_learning()
	{
		using namespace pbd::practice::movement;
		// Throw and break animations are moves, however long they hold the dummy still with a neutral word. A recorded
		// sequence: standing 236, the 1+3 (630), the throw (885), the break (888 for 60 frames), standing again.
		{
			rest_learner learner;
			learner.reset(3);
			const auto run = [&](std::uint32_t move, std::uint32_t frames, bool neutral = true)
			{
				for (std::uint32_t frame = 0; frame < frames; ++frame)
					learner.observe(move, neutral);
			};
			run(236, 300);
			check(learner.main_stance() == 236u, "standing is the main stance");
			const auto broken_throw = [&](bool first)
			{
				run(630, 8, false);
				run(885, 15);
				bool settled_888 = false, rest_888 = false, resting_888 = false;
				for (std::uint32_t frame = 0; frame < 60; ++frame)
				{
					learner.observe(888u, true);
					settled_888 = settled_888 || learner.settled(888);
					rest_888 = rest_888 || learner.at_rest(888);
					resting_888 = resting_888 || learner.resting(888, false);
				}
				if (!first)
					check(!settled_888 && !rest_888 && !resting_888, "a second break is never read as rest");
				run(236, 1);
				check(learner.is_move(888) && !learner.at_rest(888) && !learner.settled(888) && learner.at_rest(236) && learner.resting(236, false),
					"the break leads into standing, so it is a move");
			};
			broken_throw(true);
			broken_throw(false);
			run(236, 100);
			run(885, 128);
			run(236, 1);
			check(!learner.at_rest(885) && !learner.settled(885) && learner.is_move(885), "an unbroken throw held 128 frames is a move too");
			run(885, 128);
			check(!learner.settled(885) && !learner.resting(885, false) && !learner.resting(885, true), "and is never rest again");
			check(!learner.is_move(236) && learner.at_rest(236), "the main stance is never marked");
			// The stance the game holds the dummy on (a crouch under Crouching) stays a stance when it stands up from it.
			run(236, 10);
			for (std::uint32_t frame = 0; frame < 60; ++frame)
				learner.observe(91u, true, teacher::game);
			run(236, 10);
			check(!learner.is_move(91) && learner.held == 91u && learner.resting(91, true), "the game's crouch is not a move");
			learner.reset(4);
			check(!learner.is_move(888) && !learner.is_move(885), "a new moveset forgets the moves");
			// More than 16 moves: the oldest is forgotten.
			run(236, 300);
			for (std::uint32_t move = 1000; move < 1000 + rest_learner::move_capacity + 1; ++move)
			{
				run(move, 5);
				run(236, 1);
			}
			check(!learner.is_move(1000) && learner.is_move(1001) && learner.is_move(1000 + rest_learner::move_capacity), "16 moves are kept");
			// Offense under Crouching: the game's crouch is learned first, our neutral then stands the dummy up after a reaction
			// (standing learned, tied with the crouch), and the game crouches it straight away. Standing stays rest.
			learner.reset(5);
			for (std::uint32_t frame = 0; frame < rest_learn_frames; ++frame)
				learner.observe(91u, true, teacher::game);
			run(236, rest_learn_frames);
			learner.observe(91u, true, teacher::game);
			check(!learner.is_move(236) && learner.at_rest(236) && learner.at_rest(91), "the game's own transitions mark nothing");
			run(236, rest_learn_frames);
			check(learner.resting(236, false) && learner.resting(236, true), "standing is still rest after the game's crouch");
			// A move that ends while the dummy is hurt teaches nothing either way.
			run(236, 300);
			for (std::uint32_t frame = 0; frame < 60; ++frame)
				learner.observe(700u, true, teacher::none);
			learner.observe(236u, true, teacher::none);
			check(!learner.is_move(700) && !learner.at_rest(700), "a hurt frame neither marks nor learns");
		}
	}

	void recorded_direction_text()
	{
		using namespace pbd::practice::movement;
		using words = std::vector<std::uint16_t>;
		constexpr std::uint16_t L = 0x8000;
		constexpr std::uint16_t R = 0x2000;
		constexpr std::uint16_t D = 0x4000;
		constexpr std::uint16_t U = 0x1000;
		// Readable text of the capture files.
		{
			check(direction_text(0, screen_side::left) == "n" && direction_text(R, screen_side::left) == "f" && direction_text(R, screen_side::right) == "b" &&
					  direction_text(static_cast<std::uint16_t>(D | L), screen_side::right) == "df" && direction_text(static_cast<std::uint16_t>(U | L), screen_side::left) == "ub",
				"directions are Tekken notation for the fighter's facing");
			check(button_text(0) == "" && button_text(0x0080 | 0x0010) == "1+2" && button_text(0x0040 | 0x0020 | 0x0100) == "3+4+SELECT",
				"buttons are the game's attack buttons in order");
			using namespace pbd::json::detail;
			const auto text = input_row(12, 345, 1, static_cast<std::uint16_t>(D | R | 0x0080), screen_side::left, screen_side::left);
			check(text.ends_with('\n') && text.find('\n') == text.size() - 1, "an input row is one complete JSONL line");
			const auto row = json_parser(text).parse();
			check(row.value);
			std::uint32_t number = 0;
			check(is(row.value->find("type"), "input") && number32(row.value->find("frame"), 12, 12, number) &&
					  number32(row.value->find("boundary"), 345, 345, number) && number32(row.value->find("port"), 1, 1, number) &&
					  is(row.value->find("word"), "0x6080") && is(row.value->find("dirs"), "df") &&
					  is(row.value->find("buttons"), "1") && is(row.value->find("side"), "left"),
				"an input row names the frame, the word and its readable form");
			const auto unknown = json_parser(input_row(1, 2, 2, L, std::nullopt, screen_side::right)).parse();
			check(unknown.value && is(unknown.value->find("dirs"), "f") && is(unknown.value->find("buttons"), "") &&
					  is(unknown.value->find("side"), "unknown"),
				"an unknown side is written as such and decoded for the assumed side");
			const words take{0, 0, D, D, static_cast<std::uint16_t>(D | R), 0x0080};
			const auto steps = json_parser(steps_json(take, screen_side::left)).parse();
			check(steps.value && steps.value->kind == pbd::json::json_kind::array && steps.value->items.size() == 4);
			const std::uint32_t frames[]{2, 2, 1, 1};
			const char* encoded[]{"0x0000", "0x4000", "0x6000", "0x0080"};
			const char* directions[]{"n", "d", "df", "n"};
			for (std::size_t i = 0; i < steps.value->items.size(); ++i)
			{
				const auto& step = steps.value->items[i];
				check(number32(step.find("frames"), frames[i], frames[i], number) && is(step.find("word"), encoded[i]) &&
						  is(step.find("dirs"), directions[i]) && is(step.find("buttons"), i == 3 ? "1" : ""),
					"a saved tape's steps are its run lengths");
			}
			const auto empty = json_parser(steps_json(words{}, screen_side::left)).parse();
			check(empty.value && empty.value->kind == pbd::json::json_kind::array && empty.value->items.empty());
		}
	}

	void recording_side_conversion()
	{
		using namespace pbd::practice::movement;
		using words = std::vector<std::uint16_t>;
		constexpr std::uint16_t L = 0x8000;
		constexpr std::uint16_t R = 0x2000;
		constexpr std::uint16_t D = 0x4000;
		constexpr std::uint16_t U = 0x1000;
		// Recorded takes are opponent-relative frame by frame: recorded with the side DR read each word with, played with the
		// side DR will read each word with.
		{
			check(mirrored(L) == R && mirrored(R) == L && mirrored(static_cast<std::uint16_t>(D | L)) == (D | R) &&
					  mirrored(static_cast<std::uint16_t>(U | R | 0x0080)) == (U | L | 0x0080) && mirrored(D) == D,
				"mirroring must swap Left and Right, carry diagonals and leave other bits alone");
			const auto record = [](const words& screen, screen_side side)
			{
				words out;
				for (const auto word : screen)
					out.push_back(record_word(word, side));
				return out;
			};
			const auto play = [](const words& take, screen_side side)
			{
				words out;
				for (const auto word : take)
					out.push_back(play_word(word, side));
				return out;
			};
			const auto notation = [](const words& screen, screen_side side)
			{
				std::string text;
				for (const auto word : screen)
					text += (text.empty() ? "" : ",") + direction_text(word, side) + (button_text(word).empty() ? "" : "+" + button_text(word));
				return text;
			};
			// A throw-break drill: f,f into a throw recorded with the dummy on the 2P side, where DR reads
			// Left as forward. After the player breaks the throw the sides swap and the dummy stands on the 1P side.
			constexpr std::uint16_t throw_1_3 = 0x0080 | 0x0040, throw_2_4 = 0x0010 | 0x0020;
			for (const std::uint16_t throw_buttons : {throw_1_3, throw_2_4, static_cast<std::uint16_t>(0x0080 | 0x0010)})
			{
				const words on_2p{L, 0, L, static_cast<std::uint16_t>(L | throw_buttons)};
				const auto take = record(on_2p, screen_side::right);
				check(take == words{R, 0, R, static_cast<std::uint16_t>(R | throw_buttons)}, "a take is kept as f, n, f, f+throw");
				const auto on_1p = play(take, screen_side::left);
				check(on_1p == words{R, 0, R, static_cast<std::uint16_t>(R | throw_buttons)}, "played on the 1P side, f must be Right on screen");
				check(notation(on_1p, screen_side::left) == "f,n,f,f+" + button_text(throw_buttons),
					"f,f,throw recorded on 2P must play as f,f,throw on 1P, never b,b (a backdash) or b,b+2");
				check(play(take, screen_side::right) == on_2p, "played on its own side, the take is the recorded words");
				// The screen words played unchanged on the 1P side, as they would be without a side reading: b,b into the throw.
				check(notation(on_2p, screen_side::left) == "b,n,b,b+" + button_text(throw_buttons),
					"unturned words read as a backdash on the 1P side");
			}
			// A side swap in the middle of a take: each frame keeps the meaning it had when it was recorded.
			const words crossing_screen{L, L, R, R};
			const auto crossing = [&]
			{
				words out;
				const std::array<screen_side, 4> sides{screen_side::right, screen_side::right, screen_side::left, screen_side::left};
				for (std::size_t at = 0; at < crossing_screen.size(); ++at)
					out.push_back(record_word(crossing_screen[at], sides[at]));
				return out;
			}();
			check(crossing == words{R, R, R, R}, "holding toward the opponent through a crossing records forward on every frame");
			check(play(crossing, screen_side::left) == words{R, R, R, R} && play(crossing, screen_side::right) == words{L, L, L, L},
				"forward plays toward the opponent on either side");
			check(record_word(static_cast<std::uint16_t>(U | L | 0x0090), screen_side::right) == (U | R | 0x0090) &&
					  play_word(static_cast<std::uint16_t>(U | R | 0x0090), screen_side::left) == (U | R | 0x0090),
				"a recorded u/f+1+2 on the 2P side plays as u/f+1+2 on the 1P side");
		}
	}

} // namespace

// A contact series retains its hit/block classification until recovery.
static void recovered_contact_series()
{
	using namespace pbd::practice;
	using pbd::practice::movement::contact_tracker;
	using pbd::practice::movement::facing;
	using event = contact_tracker::event;
	// A series is a hit when the dummy's health dropped on any of its frames, otherwise a block.
	{
		const auto at_rest = std::optional<bool>{true}, moving = std::optional<bool>{false};
		const auto recover = [&](contact_tracker& watch)
		{
			event seen = event::none;
			for (int frame = 0; frame < 400 && seen == event::none; ++frame)
				seen = watch.observe(false, at_rest, facing::toward);
			return seen;
		};
		contact_tracker watch;
		watch.previous_rest = at_rest;
		check(watch.observe(false, moving, facing::toward) == event::contact && !watch.series_hit, "leaving rest without damage starts a block");
		check(recover(watch) == event::ready && !watch.series_hit, "and recovers as a block");
		check(watch.observe(true, at_rest, facing::toward) == event::contact && watch.series_hit, "a health drop starts a hit");
		check(recover(watch) == event::ready && watch.series_hit, "and recovers as a hit");
		check(watch.observe(false, moving, facing::toward) == event::contact && !watch.series_hit, "the next series starts as a block again");
		check(watch.observe(true, moving, facing::toward) == event::none && watch.series_hit, "a hit later in the same series makes it a hit");
		check(recover(watch) == event::ready && watch.series_hit, "which it stays until it recovers");
	}
}

PBD_TEST(PbdPractice, ExplicitBlockedContactsExtendTheRecoveryWait)
{
	using namespace pbd::practice::movement;
	contact_tracker watch;
	watch.previous_rest = true;
	check(watch.observe(false, false, facing::toward, true) == contact_tracker::event::contact,
		"a native blocked contact starts the recovery wait");
	for (unsigned frame = 0; frame < 10; ++frame)
		watch.observe(false, false, facing::toward);
	check(watch.waited == 10, "ten frames have passed since the first block");
	watch.observe(false, false, facing::toward, true);
	check(watch.waited == 0 && watch.pending, "another blocked hit of the string restarts the wait even without leaving rest again");
	watch = {};
	check(watch.observe(false, {}, facing::toward, true) == contact_tracker::event::contact,
		"blocks are also seen before a resting stance has been learned");
}

PBD_TEST(PbdPractice, GameReadingEndsTheRecoveryWait)
{
	using namespace pbd::practice::movement;
	using event = contact_tracker::event;
	// A blocked jab (block stun 19): the game takes the dummy's input again 19 frames after the block, while its block
	// reaction plays on to the end of the end command's window and the rest reading shows it away from rest until then.
	contact_tracker watch;
	watch.previous_rest = true;
	check(watch.observe(false, false, facing::toward, true) == event::contact, "the block starts the wait");
	for (unsigned frame = 1; frame < 18; ++frame)
		check(watch.observe(false, false, facing::toward) == event::none, "block stun");
	check(watch.observe(false, false, facing::toward, false, true) == event::ready && !watch.pending,
		"ready at the boundary whose word plays on the first frame the game lets the dummy act");
	watch = {};
	watch.previous_rest = true;
	watch.observe(false, false, facing::toward, true);
	check(watch.observe(false, false, facing::turned, false, true) == event::none && watch.pending,
		"a dummy turned away from P1 waits, as one back at rest does");
	check(watch.observe(false, false, facing::toward, true, true) == event::none && watch.pending && watch.waited == 0,
		"a further contact restarts the wait whatever the reading says");
	check(watch.observe(false, false, facing::toward, false, true) == event::ready, "then the reading ends it");
	watch = {};
	check(watch.observe(false, std::nullopt, facing::unknown, false, true) == event::none && !watch.pending,
		"without a contact the reading starts nothing");
}

PBD_TEST(PbdPractice, MovementChecks)
{
	using namespace pbd::practice::movement;
	static_assert(pad_up == 0x1000 && pad_right == 0x2000 && pad_down == 0x4000 && pad_left == 0x8000, "pad direction bits");

	orientation_checks();
	// The gap: the loop plays through the interval, then finishes the pass it is in, and the gap ends at the loop's wrap.
	const auto fast = generate(preset::wavedash, 4);

	whole_movement_intervals(fast);
	restored_movement_cursor(fast);
	recovery_and_ready_gates();
	facing_readings_and_gates();
	held_movement_passes();
	turnaround_taps_and_settle();
	contact_recovery_movement();
	move_entries_and_rest_learning();
	throw_reaction_rest_learning();
	recorded_direction_text();
	recording_side_conversion();
	check(wavedash_character(0x22) && wavedash_character(0x23) && wavedash_character(0x24) && wavedash_character(0x25) &&
			  wavedash_character(0x44) && wavedash_character(0x45),
		"Heihachi, Kazuya and Devil Jin rows must offer the wavedash");
	check(!wavedash_character(0x00) && !wavedash_character(0x01) && !wavedash_character(0x19) && !wavedash_character(0x26) &&
			  !wavedash_character(0x51) && !wavedash_character(~0u),
		"other characters, Jin and an unread row must not count as Mishima-style");
	check(is_wavedash(preset::wavedash) && !is_wavedash(preset::none) && !is_wavedash(preset::backdash_cancel) && !is_wavedash(preset::forward_dash) &&
			  !is_wavedash(preset::jump_back),
		"one Wavedash");
	check(static_cast<std::uint32_t>(preset::jump_back) + 1 == preset_count && preset_count <= pbd::practice::max_options,
		"the menu lists every preset");

	namespace rows = pbd::practice::dummy_rows;
	check(rows::interval == rows::slot_count && rows::movement == rows::interval + 1 && rows::wavedash_speed == rows::movement + 1 &&
			  rows::movement_end == rows::wavedash_speed + 1 && rows::trim == rows::movement_end + 1 && rows::save_recordings == rows::trim + 1 &&
			  rows::load_recordings == rows::save_recordings + 1 && rows::input_delay == rows::load_recordings + 1 &&
			  rows::restore == rows::input_delay + 1 && rows::count == rows::restore + 1,
		"Defense rows must follow the slots in menu order, Wavedash Speed and End under Movement Between Actions");
	const auto defense_rows = pbd::practice::practice_row_count(pbd::practice::training_mode::defense);
	const auto visible = pbd::practice::pbd_visible_rows(defense_rows, defense_rows - 1, pbd::game_overlay::max_rows);
	check(defense_rows == pbd::practice::practice_rows::first + rows::count && visible.first + visible.count == defense_rows &&
			  visible.count <= pbd::game_overlay::max_rows && visible.count <= pbd::practice::mouse::layout{}.rows.size(),
		"the last Defense row remains reachable within the panel and pointer viewport");
}

PBD_TEST(PbdPractice, ReactionChecks)
{
	using namespace pbd::practice;

	check(counts(contact_counted::either, true) && counts(contact_counted::either, false), "Hits or Blocks counts both");
	check(counts(contact_counted::blocks, false) && !counts(contact_counted::blocks, true), "Blocks counts only blocks");
	check(counts(contact_counted::hits, true) && !counts(contact_counted::hits, false), "Hits counts only hits");

	{
		reaction_counter counter;
		for (int contact = 0; contact < 6; ++contact)
		{
			counter.applied(contact % 2 == 0, contact_counted::either, 1);
			check(counter.ready() && counter.seen == 0, "the defaults react every time");
		}
	}
	{
		// "After blocking 3 times, counterattack": hits in between do not count, and the count starts again after it.
		reaction_counter counter;
		counter.applied(false, contact_counted::blocks, 3);
		check(!counter.ready() && counter.seen == 1, "first block");
		counter.applied(true, contact_counted::blocks, 3);
		check(!counter.ready() && counter.seen == 1, "a hit does not count toward Blocks");
		counter.applied(false, contact_counted::blocks, 3);
		check(!counter.ready() && counter.seen == 2, "second block");
		counter.applied(false, contact_counted::blocks, 3);
		check(counter.ready() && counter.seen == 0, "the third block brings the reaction and clears the count");
		counter.applied(false, contact_counted::blocks, 3);
		check(!counter.ready() && counter.seen == 1, "and the next block starts a new count");
	}
	{
		// "After being hit once, backdash away": blocks never bring it.
		reaction_counter counter;
		counter.applied(false, contact_counted::hits, 1);
		check(!counter.ready(), "a block does not trigger a hit reaction");
		counter.applied(false, contact_counted::hits, 1);
		check(!counter.ready() && counter.seen == 0, "blocks do not count toward Hits");
		counter.applied(true, contact_counted::hits, 1);
		check(counter.ready(), "the first hit brings it");
	}
	{
		reaction_counter counter;
		for (std::uint32_t times = min_reaction_times; times <= max_reaction_times; ++times)
		{
			counter.reset();
			for (std::uint32_t contact = 1; contact < times; ++contact)
			{
				counter.applied(true, contact_counted::either, times);
				check(!counter.ready(), "fewer contacts than After never react");
			}
			counter.applied(true, contact_counted::either, times);
			check(counter.ready() && counter.seen == 0, "exactly After contacts react");
		}
		counter.reset();
		counter.applied(true, contact_counted::either, 0);
		check(counter.ready(), "an After below 1 acts as 1 Time");
		for (int contact = 0; contact < 4; ++contact)
		{
			counter.applied(true, contact_counted::either, 99);
			check(!counter.ready(), "an After above 5 acts as 5 Times");
		}
		counter.applied(true, contact_counted::either, 99);
		check(counter.ready(), "on the fifth");
		counter.applied(true, contact_counted::either, 5);
		counter.applied(true, contact_counted::either, 5);
		counter.reset();
		check(counter.seen == 0, "a reset (Restart, round start, changed setting) clears the count");
	}

	recovered_contact_series();
}
