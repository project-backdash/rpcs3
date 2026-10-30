#include "pbd_test.h"

#include "Emu/PBD/pbd_practice_presentation.h"
#include "Emu/PBD/pbd_movement_observation.h"

#include <array>
#include <tuple>
#include <vector>

namespace
{
	using pbd::test::check;
	namespace movement = pbd::practice::movement;
	using event = movement::contact_tracker::event;
	using facing = movement::facing;

	auto state(const movement::movement_pause& value)
	{
		return std::tuple{value.active, value.left, value.capped, value.waited};
	}
	auto state(const movement::contact_tracker& value)
	{
		return std::tuple{value.pending, value.series_hit, value.left, value.waited, value.previous_rest};
	}
} // namespace

PBD_TEST(PbdPracticeDummyState, ARecordedPauseReplaysWithoutSharingItsLaterState)
{
	movement::movement_pause original;
	original.observe(true, false);
	original.observe(false, false);
	const auto saved = original;
	struct reading
	{
		bool hit;
		std::optional<bool> rest;
	};
	const std::array<reading, 7> readings{{{false, false}, {true, std::nullopt}, {false, std::nullopt}, {false, false},
		{false, true}, {false, true}, {false, false}}};
	std::vector<decltype(state(original))> expected;
	for (const auto& next : readings)
	{
		original.observe(next.hit, next.rest);
		expected.push_back(state(original));
	}
	check(!original.active && !original.capped);
	auto restored = saved;
	for (std::size_t i = 0; i < readings.size(); ++i)
	{
		restored.observe(readings[i].hit, readings[i].rest);
		check(state(restored) == expected[i]);
	}
	check(saved.active && saved.waited == 1 && saved.left);
	check(state(restored) == state(original));
	restored.reset();
	check(state(restored) == state(movement::movement_pause{}));
	check(saved.active);
}

PBD_TEST(PbdPracticeDummyState, ARecordedContactKeepsItsRecoveryAndFacingGate)
{
	movement::contact_tracker original;
	original.previous_rest = true;
	check(original.observe(false, false, facing::toward) == event::contact);
	check(original.observe(true, false, facing::toward) == event::none);
	const auto saved = original;
	struct reading
	{
		std::optional<bool> rest;
		facing direction;
		event expected;
	};
	const std::array<reading, 4> readings{{{false, facing::backturned, event::none}, {true, facing::backturned, event::none},
		{true, facing::toward, event::ready}, {true, facing::toward, event::none}}};
	auto restored = saved;
	for (const auto& next : readings)
	{
		check(original.observe(false, next.rest, next.direction) == next.expected);
		check(restored.observe(false, next.rest, next.direction) == next.expected);
		check(state(original) == state(restored));
	}
	check(!restored.pending && restored.series_hit);
	check(saved.pending && saved.series_hit && saved.waited == 0);
	restored = {};
	restored.previous_rest = true;
	check(restored.observe(false, true, facing::toward) == event::none);
	check(restored.observe(false, false, facing::toward) == event::contact);
	check(!restored.series_hit);
}

PBD_TEST(PbdPracticeDummyState, MissingObservationsDoNotLeaveAnUnboundedPendingContact)
{
	movement::contact_tracker fallback;
	check(fallback.observe(true, std::nullopt, facing::unknown) == event::contact);
	for (std::uint32_t i = 1; i < movement::settle_fallback_frames; ++i)
		check(fallback.observe(false, std::nullopt, facing::unknown) == event::none);
	check(fallback.observe(false, std::nullopt, facing::unknown) == event::ready);
	check(!fallback.pending);
	check(fallback.observe(false, std::nullopt, facing::unknown) == event::none);

	movement::contact_tracker away;
	check(away.observe(true, false, facing::backturned) == event::contact);
	for (std::uint32_t i = 1; i < movement::settle_cap_frames; ++i)
		check(away.observe(false, true, facing::backturned) == event::none);
	check(away.observe(false, true, facing::backturned) == event::dropped);
	check(!away.pending);
	check(away.observe(false, true, facing::toward) == event::none);
	check(away.observe(true, false, facing::toward) == event::contact);
	check(away.waited == 0);
}

PBD_TEST(PbdPracticeDummyState, AStartedTurnCompletesItsReleaseBeforeAnotherTap)
{
	movement::turn_around original;
	check(!original.active(facing::toward, true));
	check(!original.active(facing::backturned, false));
	check(original.active(facing::backturned, true) && original.tap_starts());
	check(original.next() == movement::forward);
	const auto saved = original;
	auto restored = saved;
	for (std::uint32_t i = 1; i < movement::turn_cycle_frames; ++i)
	{
		check(original.active(facing::toward, false));
		check(restored.active(facing::toward, false));
		const auto expected = i < movement::turn_tap_frames ? movement::forward : std::uint16_t{0};
		check(original.next() == expected && restored.next() == expected);
		check(original.frames == restored.frames);
	}
	check(!original.active(facing::toward, true));
	check(original.tap_starts() && original.taps() == 1);
	check(original.active(facing::backturned, true));
	check(original.next() == movement::forward && original.taps() == 2);
	check(saved.frames == 1 && restored.frames == movement::turn_cycle_frames);
	original.reset();
	check(original.frames == 0 && original.taps() == 0);
}

PBD_TEST(PbdPracticeDummyState, RecoveredEventCountsRestoreAndResetIndependently)
{
	using namespace pbd::practice;
	reaction_counter blocks;
	blocks.applied(false, contact_counted::blocks, 2);
	check(!blocks.ready());
	const auto saved = blocks;
	blocks.applied(true, contact_counted::blocks, 2);
	check(!blocks.ready());
	check(blocks.seen == 1);
	blocks.applied(false, contact_counted::blocks, 2);
	check(blocks.ready() && blocks.seen == 0);
	auto restored = saved;
	restored.applied(false, contact_counted::blocks, 2);
	check(restored.ready() && restored.seen == 0);
	check(saved.seen == 1);
	restored = saved;
	restored.reset();
	restored.applied(false, contact_counted::blocks, 2);
	check(!restored.ready() && restored.seen == 1);
	restored.applied(false, contact_counted::blocks, 2);
	check(restored.ready());
	restored.applied(true, contact_counted::hits, 0);
	check(restored.ready());
	restored.applied(false, contact_counted::hits, 1);
	check(!restored.ready());
	restored.applied(true, contact_counted::either, max_reaction_times + 1);
	check(!restored.ready());
	for (std::uint32_t i = 1; i < max_reaction_times - 1; ++i)
	{
		restored.applied(false, contact_counted::either, max_reaction_times + 1);
		check(!restored.ready());
	}
	restored.applied(true, contact_counted::either, max_reaction_times + 1);
	check(restored.ready());
}
