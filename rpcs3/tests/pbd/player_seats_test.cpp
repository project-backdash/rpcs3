#include "Emu/PBD/pbd_player_seats.h"
#include "pbd_test.h"

#include <vector>

namespace
{
	using namespace pbd::controllers;
	using pbd::test::check;

	struct bench
	{
		seats::table table;
		std::vector<seats::pad> pads;
		seats::outcome poll()
		{
			return table.update(pads);
		}
		// One press and its release on controller `i`.
		seats::outcome tap(unsigned i, std::uint32_t buttons)
		{
			pads[i].buttons = buttons;
			const auto result = poll();
			pads[i].buttons = 0;
			poll();
			return result;
		}
	};

} // namespace

PBD_TEST(PbdPlayerSeats, LocalPlayersKeepTheirControllers)
{
	bench b;
	b.pads = {{"pad-a", "Stick A"}, {"pad-b", "Pad B"}};
	b.poll();
	check(!b.table.snapshot().question.open, "nobody is asked anything at the start");
	b.table.claim(0, "pad-a");
	b.table.claim(1, "pad-a");
	check(b.table.player(0) == "pad-a" && b.table.player(1).empty(), "one controller never takes both players");
	b.table.claim(1, "pad-b");
	b.table.claim(1, "pad-c");
	check(b.table.player(1) == "pad-b", "a player keeps its controller whoever claims");

	// Both players have a controller: a third one connected and pressed is not asked and changes nothing.
	b.pads.push_back({"pad-c", "Stick C"});
	b.poll();
	b.tap(2, bit(cross));
	b.tap(2, bit(select));
	check(!b.table.snapshot().question.open && b.table.player(0) == "pad-a" && b.table.player(1) == "pad-b",
		"the players are locked to their controllers");

	// Select with Start held is the hold that leaves Versus: it asks nothing.
	b.pads[0].buttons = bit(start);
	b.poll();
	b.pads[0].buttons = bit(start) | bit(select);
	b.poll();
	check(!b.table.snapshot().question.open, "Start + Select asks nothing");
	b.pads[0].buttons = 0;
	b.poll();

	// Select on a player's controller asks; moving to the other player swaps the two controllers.
	b.tap(0, bit(select));
	auto q = b.table.snapshot().question;
	check(q.open && q.device == "pad-a" && q.name == "Stick A" && q.player == 0, "Select asks, on its own player");
	check(b.table.withheld("pad-a") && !b.table.withheld("pad-b"), "only the asked controller is kept from the game");
	b.tap(1, bit(cross));
	check(b.table.snapshot().question.open, "another controller cannot answer the question");
	b.tap(0, bit(right));
	check(b.table.snapshot().question.player == 1, "Right chooses player 2");
	b.pads[0].buttons = bit(cross);
	check(b.poll() == seats::outcome::seated, "Cross answers");
	check(b.table.player(1) == "pad-a" && b.table.player(0) == "pad-b", "changing player swaps the controllers");
	check(b.table.answered() == "pad-a" && b.table.answered_player() == 1, "the answer names the controller and its player");
	check(b.table.withheld("pad-a"), "the answering press stays out of the game until it is let go");
	b.pads[0].buttons = 0;
	b.poll();
	check(!b.table.withheld("pad-a"), "released, the controller plays");

	// Circle leaves everything as it was; Select held from the question never asks again until it is let go.
	b.pads[1].buttons = bit(select);
	b.poll();
	b.pads[1].buttons = bit(select) | bit(circle);
	b.poll();
	check(!b.table.snapshot().question.open && b.table.player(0) == "pad-b", "Circle cancels");
	b.pads[1].buttons = bit(select);
	b.poll();
	check(!b.table.snapshot().question.open && b.table.withheld("pad-b"), "a button held from the question asks nothing");
	b.pads[1].buttons = 0;
	b.poll();

	// Button setup: Down then Cross.
	b.tap(1, bit(select));
	b.tap(1, bit(down));
	check(b.table.snapshot().question.setup_focused, "Down reaches Button setup");
	b.tap(1, bit(right));
	check(b.table.snapshot().question.player == 0, "Left and Right wait while Button setup is focused");
	check(b.tap(1, bit(cross)) == seats::outcome::setup && b.table.answered() == "pad-b", "Button setup opens for its controller");
	check(b.table.player(0) == "pad-b", "Button setup changes no player");

	// Unplugged, a player's controller leaves it free; the next controller pressed takes that side, the other stays.
	b.pads.erase(b.pads.begin() + 1);
	b.poll();
	check(b.table.player(0).empty() && b.table.player(1) == "pad-a", "an unplugged controller frees its player");
	check(b.table.claim(0, "pad-c") && b.table.player(0) == "pad-c" && b.table.player(1) == "pad-a", "a new controller joins the free side");
	check(b.table.withheld("pad-c"), "the press that joins stays out of the game");
	b.poll();
	check(!b.table.withheld("pad-c"), "let go, the controller plays");
	b.pads.push_back({"pad-b", "Pad B"});
	b.poll();
	b.tap(2, bit(cross));
	check(b.table.player(0) == "pad-c" && !b.table.snapshot().question.open, "plugged back after its seat was taken, it waits");
	const auto names = b.table.snapshot().names;
	check(names[0] == "Stick C" && names[1] == "Stick A", "each player's controller is named");

	// A question whose controller is unplugged closes.
	b.tap(1, bit(select));
	check(b.table.snapshot().question.device == "pad-c", "asked");
	b.pads.erase(b.pads.begin() + 1);
	b.poll();
	check(!b.table.snapshot().question.open && b.table.player(0).empty(), "the question leaves with its controller");

	// The VS mode opens again: both players are free, and a Start held through it joins only once pressed again.
	b.table.claim(0, "pad-b");
	b.pads[1].buttons = bit(start);
	b.poll();
	b.table.reset();
	check(b.table.player(0).empty() && b.table.player(1).empty(), "each opening of the VS mode frees both players");
	check(!b.table.claim(0, "pad-b") && b.table.withheld("pad-b"), "a Start held through the opening does not join");
	b.pads[1].buttons = 0;
	b.poll();
	check(b.table.claim(0, "pad-b") && b.table.player(1).empty(), "pressed again, it joins");
}
