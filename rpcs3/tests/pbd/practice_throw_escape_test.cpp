#include "pbd_test.h"

#include "Emu/PBD/pbd_practice_throw_escape.h"
#include "Emu/Cell/pbd_practice_controls.h"

#include <vector>

namespace
{
	using namespace pbd::practice;
	using pbd::test::check;
	using view = std::optional<throw_escape_view>;
	constexpr std::uint16_t neutral = 0, button_1 = 0x0080, button_2 = 0x0010, buttons_1_2 = 0x0090;
}

PBD_TEST(PbdPracticeThrowEscape, OptionsAndBreakWords)
{
	check(static_cast<std::uint32_t>(throw_escape_option::none) == 0 && static_cast<std::uint32_t>(throw_escape_option::random) == 2,
		"the options are numbered in menu order");
	// The game's +0x192 numbers: 1, 2, and 3 for 1+2; the port words are the stock buttons 1 (0x80) and 2 (0x10).
	check(throw_escape_word(1) == pbd::practice::controls::defaults[0] && throw_escape_word(2) == pbd::practice::controls::defaults[1],
		"a 1 or 2 break presses that stock button");
	check(throw_escape_word(3) == (pbd::practice::controls::defaults[0] | pbd::practice::controls::defaults[1]),
		"a 1+2 break presses both on one frame");
	check(throw_escape_word(0) == 0 && throw_escape_word(4) == 0 && throw_escape_word(~0u) == 0, "nothing else presses a button");
}

PBD_TEST(PbdPracticeThrowEscape, NoneAllRandom)
{
	for (std::uint32_t word = 0; word < 64; ++word)
	{
		check(!throw_escape_breaks(throw_escape_option::none, word), "None never breaks");
		check(throw_escape_breaks(throw_escape_option::all, word), "All always breaks");
		check(throw_escape_breaks(throw_escape_option::random, word) == ((word & 1) != 0), "Random follows its word");
	}
}

PBD_TEST(PbdPracticeThrowEscape, OnePressThenLetsGo)
{
	throw_escape_control control;
	const throw_escape_view open{0x2166e000, 1, 0}, taken{0x2166e000, 1, 1};
	check(!control.fresh(std::nullopt) && control.fresh(open), "a window seen open is a throw to decide on");
	control.begin(open, true);
	check(control.owns() && !control.fresh(open), "the decided throw is not new");
	check(control.next(open) == button_1, "the break is pressed on the next frame");
	check(!control.next(taken) && !control.owns() && control.done, "once the game has taken it the control lets go");
	check(!control.next(taken) && !control.fresh(taken), "and stays out for the rest of the window");
	check(!control.next(std::nullopt) && !control.active, "the move change ends the throw");
	check(control.fresh(open), "the next throw is decided again");
}

PBD_TEST(PbdPracticeThrowEscape, MissedPressIsFreshAgain)
{
	throw_escape_control control;
	const throw_escape_view open{0x2166e04c, 3, 0};
	control.begin(open, true);
	// The game takes a press only when the buttons were up the frame before, so presses alternate with neutral frames.
	for (std::uint32_t at = 0; at < 2 * throw_escape_presses; ++at)
		check(control.next(open) == (at % 2 ? neutral : buttons_1_2), "press, release, press");
	check(!control.next(open) && !control.owns(), "a break the game never takes stops after a few presses");
	check(!control.next(open) && !control.fresh(open), "and does not start again in the same throw");
}

PBD_TEST(PbdPracticeThrowEscape, KeptThrowsAndNewThrows)
{
	throw_escape_control control;
	const throw_escape_view first{0x2166e000, 2, 0}, second{0x2166e098, 2, 0};
	control.begin(first, false);
	check(!control.owns() && !control.next(first) && !control.fresh(first), "a throw the dummy keeps leaves its port alone");
	check(control.fresh(second), "another throw's window (a chain throw's next part) is a new decision");
	control.begin(second, true);
	check(control.next(second) == button_2, "and can break");
	check(!control.next(first) && control.owns(), "a view of another move gives no word");
	control.reset();
	check(!control.active && !control.owns() && control.fresh(first), "Restart and menus start over");
}
