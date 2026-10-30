#include "pbd_test.h"

#include "Emu/RSX/pbd_frame_tags.h"
#include "Emu/PBD/pbd_fight_timing.h"

using pbd::test::check;

PBD_TEST(PbdFightTiming, OnlyDeliveredForwardFightIntervalsCount)
{
	pbd::fight_timing::counter value;
	constexpr auto period = pbd::fight_timing::counter::period_ns;
	for (unsigned frame = 0; frame < 6; ++frame)
	{
		value.forward(7, frame, frame > 0 && frame < 5);
		value.produced(7, frame);
	}
	for (unsigned frame = 1; frame < 5; ++frame)
		value.delivered(7, frame, 1'000'000'000 + frame * period, true, true, true);
	const auto measured = value.take(7, 0, 6);
	check(measured && measured->fight_frames == 3 && measured->on_time_frames == 3);
	check(!value.take(7, 0, 6));
}

PBD_TEST(PbdFightTiming, CorrectionsCostTimeWithoutInventingForwardFrames)
{
	pbd::fight_timing::counter value;
	constexpr auto period = pbd::fight_timing::counter::period_ns;
	constexpr std::uint64_t start = 1'000'000'000;
	for (unsigned frame = 0; frame < 5; ++frame)
	{
		value.forward(7, frame, true);
		value.produced(7, frame);
	}
	value.forward(7, 0, true);
	value.produced(7, 0);
	value.delivered(7, 0, start, true, true, true);
	value.delivered(7, 0, start + period, false, true, true);
	value.delivered(7, 1, start + 2 * period, true, true, true);
	value.delivered(7, 2, start + 3 * period, true, true, true);
	value.delivered(7, 4, start + 5 * period, true, true, true);
	const auto measured = value.take(7, 0, 5);
	check(measured && measured->fight_frames == 4 && measured->on_time_frames == 2);
}

PBD_TEST(PbdFightTiming, ApplicationDispatchesWithoutARenderAreNotInventedFightFrames)
{
	pbd::fight_timing::counter value;
	constexpr auto period = pbd::fight_timing::counter::period_ns;
	for (unsigned boundary = 0; boundary < 10; ++boundary)
	{
		value.forward(7, boundary, true);
		if (boundary % 2 == 0)
		{
			value.produced(7, boundary);
			value.delivered(7, boundary, 1'000'000'000 + boundary / 2 * period, true, true, true);
		}
	}
	const auto measured = value.take(7, 0, 10);
	check(measured && measured->fight_frames == 4 && measured->on_time_frames == 4);
}

PBD_TEST(PbdFightTiming, BetweenRoundTimeIsOutsideTheFightMeasurement)
{
	pbd::fight_timing::counter value;
	constexpr auto period = pbd::fight_timing::counter::period_ns;
	for (unsigned boundary = 0; boundary < 5; ++boundary)
	{
		value.forward(7, boundary, boundary != 2);
		value.produced(7, boundary);
	}
	value.delivered(7, 0, 1'000'000'000, true, true, true);
	value.delivered(7, 1, 1'000'000'000 + period, true, true, true);
	value.delivered(7, 3, 20'000'000'000, true, true, true);
	value.delivered(7, 4, 20'000'000'000 + period, true, true, true);
	const auto measured = value.take(7, 0, 5);
	check(measured && measured->fight_frames == 2 && measured->on_time_frames == 2);
}

PBD_TEST(PbdFightTiming, MissingHiddenOrUnprovedDeliveryStaysUnknown)
{
	for (const unsigned failure : {0u, 1u, 2u, 3u})
	{
		pbd::fight_timing::counter value;
		value.forward(7, 0, true);
		value.produced(7, 0);
		value.forward(7, 1, true);
		value.produced(7, 1);
		if (failure)
		{
			value.delivered(7, 0, 100, true, true, true);
			value.delivered(7, 1, failure == 3 ? 99 : 200, true, failure != 1, failure != 2);
		}
		check(!value.take(7, 0, 2));
	}
}

PBD_TEST(PbdFightTiming, NewGameAndNewEmulationKeepTheirOwnClock)
{
	pbd::fight_timing::counter value;
	constexpr auto period = pbd::fight_timing::counter::period_ns;
	for (unsigned game = 0; game < 2; ++game)
	{
		for (unsigned frame = 0; frame < 4; ++frame)
		{
			const auto boundary = game * 10 + frame;
			value.forward(7, boundary, true);
			value.produced(7, boundary);
			value.delivered(7, boundary, 1'000'000'000ull * (game + 1) + frame * period, true, true, true);
		}
		const auto measured = value.take(7, game * 10, game * 10 + 4);
		check(measured && measured->fight_frames == 3 && measured->on_time_frames == 3);
	}
	for (unsigned frame = 0; frame < 3; ++frame)
	{
		value.forward(8, frame, true);
		value.produced(8, frame);
		value.delivered(7, frame, 1, true, true, true);
		value.delivered(8, frame, 2'000'000'000 + frame * period, true, true, true);
	}
	check(!value.take(7, 0, 3));
	const auto measured = value.take(8, 0, 3);
	check(measured && measured->fight_frames == 2 && measured->on_time_frames == 2);
}

PBD_TEST(PbdFightTiming, TimestampAllowanceDoesNotCertifyASlowerSteadyRate)
{
	for (const std::uint64_t interval : {16'683'333ull, 18'000'000ull})
	{
		pbd::fight_timing::counter value;
		for (unsigned frame = 0; frame < 600; ++frame)
		{
			value.forward(7, frame, true);
			value.produced(7, frame);
			value.delivered(7, frame, 1'000'000'000 + frame * interval, true, true, true);
		}
		const auto measured = value.take(7, 0, 600);
		check(measured && measured->fight_frames == 599);
		if (interval == 16'683'333)
			check(measured->on_time_frames * 100 >= measured->fight_frames * 97);
		else
			check(measured->on_time_frames * 100 < measured->fight_frames * 70);
	}
}

static void counter_epoch_selection()
{
	using queue = pbd::frame_presentation::frame_tag_queue;
	using decision = pbd::frame_presentation::counter_select_result;
	using state = pbd::frame_presentation::command_state;
	pbd::frame_presentation::counter_epoch_selector selector(11, 770);
	check(selector.observe((std::uint64_t{10} << 32) | 952) == decision::wait);
	check(selector.observe((std::uint64_t{11} << 32) | 953) == decision::wait && !selector.established());
	check(selector.observe((std::uint64_t{11} << 32) | 0) == decision::wait && selector.established());
	check(selector.observe((std::uint64_t{11} << 32) | 769) == decision::wait);
	check(selector.observe((std::uint64_t{11} << 32) | 770) == decision::select);
	pbd::frame_presentation::counter_epoch_selector missed(11, 770);
	check(missed.observe((std::uint64_t{11} << 32) | 0) == decision::wait);
	check(missed.observe((std::uint64_t{11} << 32) | 771) == decision::reject_counter);
	// After the first epoch, a counter restart before the target means the target missed its epoch.
	pbd::frame_presentation::counter_epoch_selector epoch(11, 770);
	check(epoch.observe((std::uint64_t{11} << 32) | 0) == decision::wait);
	check(epoch.observe((std::uint64_t{11} << 32) | 500) == decision::wait);
	check(epoch.observe((std::uint64_t{11} << 32) | 0) == decision::reject_epoch_ended);
	pbd::frame_presentation::counter_epoch_selector absent(11, 770);
	check(absent.observe(queue::invalid) == decision::wait);
	check(absent.observe((std::uint64_t{12} << 32)) == decision::reject_segment);
}

static void command_reuse_and_ambiguity(const auto& make, const auto& rchain)
{
	using queue = pbd::frame_presentation::frame_tag_queue;
	using decision = pbd::frame_presentation::counter_select_result;
	using state = pbd::frame_presentation::command_state;
	// Savestate resumed between a DRIVER_QUEUE and its FLIP_HEAD: the forced frame end
	// before alignment is counted, not a fault; a head that never aligns faults once.
	queue s;
	auto s1 = make(1, 0x10000200, 5);
	s.push_record(1, 5, 5, true, &s1);
	check(s.pop_command(0, queue::no_address, rchain).command == state::no_command && s.snapshot().faults == 0 && s.snapshot().unaligned == 1);
	for (int i = 0; i < 7; i++)
		s.pop_command(0, 0x10000100, rchain);
	check(s.snapshot().unaligned == 8 && s.snapshot().faults == 0);
	s.pop_command(0, 0x10000100, rchain);
	check(s.snapshot().unaligned == 9 && s.snapshot().faults == 1 && s.snapshot().pending == 1);
	// Writing over a still-pending command's address is detected at registration.
	queue o;
	auto w1 = make(0, 0x10000200, 1);
	auto w2 = make(1, 0x10000200, 2);
	o.push_record(0, 1, 1, true, &w1);
	o.push_record(1, 2, 2, true, &w2);
	check(o.snapshot().overwrites == 1 && o.snapshot().faults == 1);
	// Two pending registrations that could both have produced the executed command: ambiguous, fault.
	queue amb;
	auto a1 = make(0, 0x10020000, 1);
	auto a2 = make(1, 0x10020000, 2);
	amb.push_record(0, 1, 1, true, &a1);
	amb.push_record(1, 2, 2, true, &a2); // (also an overwrite fault at registration)
	const auto ambp = amb.pop_command(0, 0x10020000, rchain);
	check(ambp.command == state::mismatch && amb.snapshot().faults >= 2);
	// Ring reuse: the same address bound twice in order carries generations 1 then 2.
	queue g;
	auto g1 = make(0, 0x10000200, 1);
	g.push_record(0, 1, 1, true, &g1);
	check(g.pop_command(0, 0x10000200, rchain).fifo_generation == 1);
	auto g2 = make(1, 0x10000200, 2);
	g.push_record(1, 2, 2, true, &g2);
	const auto g2p = g.pop_command(1, 0x10000200, rchain);
	check(g2p.command == state::exact && g2p.fifo_generation == 2 && g.snapshot().faults == 0);
	// verified() requires the FLIP_HEAD proof too.
	auto v = g2p;
	check(!v.verified());
	v.flip_head_verified = true;
	check(v.verified());
}

static void command_address_binding()
{
	using queue = pbd::frame_presentation::frame_tag_queue;
	using decision = pbd::frame_presentation::counter_select_result;
	using state = pbd::frame_presentation::command_state;
	// Command-address binding. The chain callback models guest FIFO memory:
	// it returns the NOP/JUMP words walked from `from` to `to`, or -1.
	{
		using state = pbd::frame_presentation::command_state;
		auto chain = [](std::uint32_t from, std::uint32_t to) -> int
		{
			if (from == to)
				return 0;
			if (from == 0x1000fff8 && to == 0x10010000)
				return 1; // segment switch: JUMP at the old write pointer
			return -1;
		};
		auto rchain = [&](const queue::record& r, std::uint32_t to)
		{
			return chain(r.entry_current, to);
		};
		const auto make = [](std::uint32_t buffer, std::uint32_t entry, std::uint64_t context)
		{
			queue::record r{};
			r.entry_current = entry;
			r.context = context;
			r.live = context != queue::invalid;
			r.buffer = buffer;
			return r;
		};
		queue c;
		// A savestate-era DRIVER_QUEUE executes before any registration: counted, not a fault, nothing bound.
		auto early = c.pop_command(0, 0x10000100, rchain);
		check(early.command == state::mismatch && c.snapshot().faults == 0 && c.snapshot().unaligned == 1 && !c.snapshot().aligned);
		auto r1 = make(1, 0x10000200, 870);
		r1.scene = 3;
		c.push_record(1, 870, 1790, true, &r1);
		r1.scene = 8;
		// The savestate-era command may also execute after the first registration: the head stays queued.
		early = c.pop_command(0, 0x10000180, rchain);
		check(early.command == state::mismatch && c.snapshot().pending == 1 && c.snapshot().unaligned == 2 && c.snapshot().faults == 0);
		auto p1 = c.pop_command(1, 0x10000200, rchain);
		check(p1.command == state::exact && p1.fifo_address == 0x10000200 && p1.fifo_generation == 1 && p1.context == 870 && p1.live);
		check(p1.scene == 3, "A later VS boundary cannot relabel a queued character-select image");
		check(c.snapshot().aligned && c.snapshot().faults == 0);
		// Segment switch: the command lies after a JUMP from the registered write pointer.
		auto r2 = make(0, 0x1000fff8, 871);
		c.push_record(0, 871, 1791, true, &r2);
		auto p2 = c.pop_command(0, 0x10010000, rchain);
		check(p2.command == state::via_chain && p2.chain_words == 1 && p2.fifo_address == 0x10010000 && p2.fifo_generation == 1);
		// After alignment, a command the head did not register is a fault and invalidates the head's identity.
		auto r3 = make(1, 0x10010100, 872);
		c.push_record(1, 872, 1792, true, &r3);
		auto p3 = c.pop_command(1, 0x10010300, rchain);
		check(p3.command == state::mismatch && p3.context == queue::invalid && !p3.live && c.snapshot().faults == 1);
		// Buffer disagreement is a fault even at the right address.
		auto r4 = make(0, 0x10010400, 873);
		c.push_record(0, 873, 1793, true, &r4);
		check(c.pop_command(1, 0x10010400, rchain).command == state::mismatch && c.snapshot().faults == 2);
		// A forced/unqueued frame end carries no command and is a fault once armed.
		check(c.pop_command(0, queue::no_address, rchain).command == state::no_command && c.snapshot().faults == 3);
		// An underflow after alignment is a fault.
		check(c.pop_command(0, 0x10010500, rchain).command == state::mismatch && c.snapshot().faults == 4);
		command_reuse_and_ambiguity(make, rchain);
	}
}

PBD_TEST(PbdFrameTags, Checks)
{
	using queue = pbd::frame_presentation::frame_tag_queue;
	using decision = pbd::frame_presentation::counter_select_result;
	counter_epoch_selection();
	queue q;
	check(q.pop(0) == queue::invalid && q.snapshot().faults == 0);
	// The first accepted label is deliberately invalid: it aligns a newly
	// created host queue with any savestate-era flip already in the RSX FIFO.
	q.push(0, 769);
	check(q.pop(0) == queue::invalid && q.snapshot().faults == 0);
	// Identical buffer IDs retain distinct ordered frame identities; replay is consumed too.
	const auto live = q.push_record(1, 770, 1689, true);
	const auto replay = q.push_record(1, queue::invalid, 1689, false);
	q.push(1, 771);
	check(live.accepted && live.sequence == 2 && live.boundary == 1689 && live.live && live.context == 770);
	check(replay.accepted && replay.sequence == 3 && replay.boundary == 1689 && !replay.live && replay.context == queue::invalid);
	const auto live_pop = q.pop_record(1);
	const auto replay_pop = q.pop_record(1);
	check(live_pop.accepted && live_pop.sequence == live.sequence && live_pop.boundary == 1689 && live_pop.live && live_pop.context == 770);
	check(replay_pop.accepted && replay_pop.sequence == replay.sequence && replay_pop.boundary == 1689 && !replay_pop.live && replay_pop.context == queue::invalid);
	check(q.pop(1) == 771);
	check(q.snapshot().produced == 4 && q.snapshot().consumed == 4 && q.snapshot().faults == 0);
	check(q.pop(1) == queue::invalid && q.snapshot().faults == 1);
	// If a savestate-era command consumes the sentinel, the corresponding new
	// command has no tag and faults instead of silently shifting the stream.
	q.reset();
	q.push(0, 799);
	check(q.pop(0) == queue::invalid && q.snapshot().faults == 0);
	check(q.pop(0) == queue::invalid && q.snapshot().faults == 1);
	q.reset();
	q.push(0, 800);
	check(q.pop(0) == queue::invalid && q.snapshot().faults == 0);
	q.push(0, 801);
	check(q.pop(1) == queue::invalid && q.snapshot().faults == 1);
	q.reset();
	for (unsigned i = 0; i < 256; i++)
		q.push(i % 2, i);
	q.push(0, 256);
	check(q.snapshot().faults == 1 && q.snapshot().pending == 256);
	for (unsigned i = 0; i < 256; i++)
		check(q.pop(i % 2) == (i ? i : queue::invalid));
	q.reset();
	q.push(8, 100);
	check(q.snapshot().faults == 1 && q.snapshot().pending == 0);
	command_address_binding();
}
