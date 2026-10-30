#include "pbd_test.h"

#include "Emu/Cell/pbd_controller_takeover.h"

#include <string>
#include <vector>

namespace
{
	using pbd::test::check;

	using namespace pbd::controller_takeover;

	const device_id xinput_0{1, "XInput Pad #0"}, ds4_2{2, "DS4 Pad #2"}, sdl_twin{3, "PS4 Controller 1"};

	std::vector<reading> pads(bool xinput_pressed, bool ds4_pressed)
	{
		return {{xinput_0, xinput_pressed}, {ds4_2, ds4_pressed}};
	}
} // namespace

PBD_TEST(PbdControllerTakeover, TakeoverChecks)
{
	detector d;
	// Watching starts with both pads released and Player 1 quiet.
	check(!d.observe(0, false, pads(false, false)), "nothing at the start");
	check(!d.observe(400, false, pads(true, false)), "a press must last before it counts");
	check(!d.observe(416, false, pads(true, false)), "still too short");
	const auto chosen = d.observe(448, false, pads(true, false));
	check(chosen && *chosen == xinput_0, "a press held 40 ms while Player 1 is quiet takes Player 1 over");

	// After a takeover every pad must be released again, and nothing switches within the cooldown.
	check(!d.observe(500, false, pads(true, true)), "pads held through the takeover do not count");
	check(!d.observe(600, false, pads(false, false)), "released");
	check(!d.observe(1000, false, pads(false, true)), "a new press inside the cooldown");
	check(!d.observe(1100, false, pads(false, true)), "still inside the cooldown");
	const auto next = d.observe(1500, false, pads(false, true));
	check(next && *next == ds4_2, "a press that began inside the cooldown counts once it is over");
}

PBD_TEST(PbdControllerTakeover, QuietChecks)
{
	detector d;
	d.observe(0, false, pads(false, false));
	// Player 1 plays: another pad's press while Player 1 is active never takes over, however long it is held.
	check(!d.observe(1000, true, pads(true, false)), "Player 1 active");
	check(!d.observe(1100, false, pads(true, false)), "the press began while Player 1 was active");
	check(!d.observe(2000, false, pads(true, false)), "held on, it still never counts");
	check(!d.observe(2100, false, pads(false, false)), "released");
	// Player 1 was active at 1000: a press 300 ms after that may count.
	check(!d.observe(2200, false, pads(true, false)), "a new press");
	check(d.observe(2250, false, pads(true, false)) == xinput_0, "it counts once Player 1 has been quiet long enough");

	// Player 1's own pad listed a second time: its press shows there a moment before Player 1 reports it.
	detector twin;
	const std::vector<reading> released{{sdl_twin, false}}, pressed{{sdl_twin, true}};
	twin.observe(0, false, released);
	check(!twin.observe(1000, false, pressed), "the second listing shows the press first");
	check(!twin.observe(1016, true, pressed), "Player 1 reports it within a poll");
	check(!twin.observe(1060, true, pressed), "never a takeover by Player 1's own pad");
	check(!twin.observe(1500, false, pressed), "nor once Player 1 goes quiet with the press still held");
}

PBD_TEST(PbdControllerTakeover, StartChecks)
{
	detector d;
	// A pad held when watching starts, or one stuck down, never takes over until it has been seen released.
	check(!d.observe(0, false, pads(true, false)), "held at the start");
	check(!d.observe(500, false, pads(true, false)), "still held");
	check(!d.observe(1000, false, pads(false, false)), "released");
	check(!d.observe(1400, false, pads(true, false)), "pressed again");
	check(d.observe(1450, false, pads(true, false)) == xinput_0, "now it counts");

	// A pad that leaves the list is forgotten: listed again, it must be seen released first.
	detector e;
	e.observe(0, false, pads(false, false));
	e.observe(100, false, {{ds4_2, false}});
	check(!e.observe(500, false, pads(true, false)), "listed again with a button down");
	check(!e.observe(600, false, pads(true, false)), "not seen released since it came back");

	// Player 1 counts as active at the first poll, so a press then needs quiet_ms more.
	detector f;
	check(!f.observe(0, false, {{xinput_0, false}}), "first poll");
	check(!f.observe(100, false, {{xinput_0, true}}), "pressed 100 ms in");
	check(!f.observe(200, false, {{xinput_0, true}}), "the press began within quiet_ms of the first poll");
}

PBD_TEST(PbdControllerTakeover, ReleaseChecks)
{
	// The takeover press (Cross at character select) stays out of the game until the new Player 1 releases it.
	release_hold hold;
	check(!hold.active() && !hold.observe(0, true), "no hold before a takeover");
	hold.start(1000);
	check(hold.active() && hold.observe(1000, true) && hold.observe(1016, true) && hold.observe(1500, true),
		"the held takeover press stays out for as long as it is held");
	check(!hold.observe(1516, false) && !hold.active(), "released, the pad plays");
	check(!hold.observe(1532, true), "the next press is the player's own");

	// The new pad's handler may not have reported the press yet after the rebind: an empty first poll still holds.
	hold.start(2000);
	check(hold.observe(2000, false) && hold.observe(2016, false), "a pad not read yet stays held");
	check(hold.observe(2048, true) && hold.observe(2200, true), "its press, once reported, stays out");
	check(!hold.observe(2216, false), "and its release ends the hold");

	// A press released before the pad was first read: nothing shows, and the hold ends after first_poll_ms.
	hold.start(3000);
	check(hold.observe(3000 + first_poll_ms - 1, false), "held until first_poll_ms");
	check(!hold.observe(3000 + first_poll_ms, false) && !hold.active(), "a pad that never showed the press plays after first_poll_ms");

	// A new takeover starts the hold afresh.
	hold.start(4000);
	check(hold.active() && hold.observe(4000, true), "every takeover holds its own press");
}

PBD_TEST(PbdControllerTakeover, NoticeChecks)
{
	std::uint64_t seen = 0;
	check(!take_new(seen), "no notice before a takeover");
	announce(xinput_0.device);
	const auto text = take_new(seen);
	check(text && text->find(xinput_0.device) != std::string::npos, "the notice names the pad");
	check(!take_new(seen), "each notice is taken once");
	check(current().until_ms > steady_ms() && current().until_ms <= steady_ms() + notice_ms, "it shows for notice_ms");
}
