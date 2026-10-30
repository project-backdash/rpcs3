// Checks of the skip guard's history class.
#include "pbd_test.h"

#include "Emu/Cell/pbd_skip_guard.h"

using pbd::test::check;

using history = pbd::fn_hook::skip_guard_history;
constexpr history::key identity{0x260d44, 0x25b86c, 0x10010000};
constexpr unsigned sp = 0xd0010000;
const history::tls_state tls{};

static void live_call(history& h, unsigned tick, bool core = false, bool changed = false, bool matched = true, bool readable = true)
{
	h.boundary(tick, false);
	h.observe(identity[0], identity[1], identity[2], sp, true, 0, readable, tls);
	if (core)
		h.observe(0x66950, 0x1234, 0, sp - 32, false, 4, true, tls);
	auto end = tls;
	if (changed)
		end[50] = 1;
	if (matched)
		h.observe(identity[1], 0, 0, sp, false, 0, true, end);
	h.boundary(tick + 1, false);
}

static void history_checks_and_stack()
{
	{
		history h;
		live_call(h, 10);
		h.boundary(10, true);
		check(h.allow(10, identity, sp) == history::allowed);
		check(h.allow(10, identity, sp) == history::extra_call);
		h.boundary(11, false);
		h.boundary(10, true);
		check(h.allow(10, identity, sp) == history::allowed); // fresh correction resets replay-use bookkeeping
	}
	{
		history h;
		h.boundary(1, true);
		check(h.allow(1, identity, sp) == history::missing_tick);
		live_call(h, 10);
		h.boundary(10, true);
		check(h.allow(10, {identity[0], identity[1] + 4, identity[2]}, sp) == history::missing_call);
		check(h.allow(10, {identity[0], identity[1], identity[2] + 4}, sp) == history::missing_call);
		check(h.allow(10, identity, sp + 16) == history::changed_stack);
		check(h.allow(10, identity, sp, true) == history::missing_tick);
	}
}

PBD_TEST(PbdSkipGuard, Checks)
{
	history_checks_and_stack();
	for (bool draw : {false, true})
	{
		history h;
		live_call(h, 10, draw, !draw);
		h.boundary(10, true);
		check(h.allow(10, identity, sp) == history::generator_draw); // core call OR any of the 137 bytes changed
	}
	{
		history h;
		live_call(h, 10, false, false, false);
		h.boundary(10, true);
		check(h.allow(10, identity, sp) == history::unmatched_return);
		check(h.abandoned == 1);
	}
	{
		history h;
		live_call(h, 10, false, false, true, false);
		h.boundary(10, true);
		check(h.allow(10, identity, sp) == history::unreadable_state);
	}
	{
		history h;
		h.boundary(10, false);
		for (int i = 0; i < 2; i++)
		{
			h.observe(identity[0], identity[1], identity[2], sp, true, 0, true, tls);
			h.observe(identity[1], 0, 0, sp, false, 0, true, tls);
		}
		h.boundary(11, false);
		h.boundary(10, true);
		check(h.allow(10, identity, sp) == history::ambiguous_call); // no ordinal matching
	}
	{
		history h;
		live_call(h, 10);
		live_call(h, 266);
		h.boundary(10, true);
		check(h.allow(10, identity, sp) == history::missing_tick); // ring reuse never reads stale evidence
	}
	{
		history h;
		live_call(h, 10);
		h.boundary(10, true);
		h.observe(identity[0], identity[1], identity[2], sp, true, 4, true, tls);
		check(h.allow(10, identity, sp) == history::allowed); // replay cannot replace original live evidence
	}
	{
		history h;
		h.boundary(10, false);
		h.observe(identity[0], identity[1], identity[2], sp, true, 0, true, tls);
		h.observe(identity[1], 0, 0, sp, false, 0, true, tls);
		h.boundary(10, true);
		check(h.allow(10, identity, sp) == history::unfinished_tick); // balanced call does not seal the whole frame
	}
	{
		history h;
		h.boundary(10, false);
		for (unsigned i = 0; i < 65; i++)
			h.observe(identity[0], identity[1] + 4 * i, identity[2] + 4 * i, sp - 32 * i, true, 0, true, tls);
		h.boundary(11, false);
		h.boundary(10, true);
		check(h.overflows == 1);
		check(h.allow(10, identity, sp) == history::missing_tick); // overflow poisons the whole tick
	}
}
