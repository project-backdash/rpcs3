#include "pbd_test.h"

#include "Emu/PBD/pbd_practice_presentation.h"
#include "Emu/Cell/pbd_practice_controls.h"
#include "Emu/Cell/pbd_practice_mouse.h"
#include "Emu/PBD/pbd_movement_observation.h"
#include "Emu/Cell/pbd_game_overlay.h"

#include <limits>
#include <string_view>
#include <vector>

namespace
{
	// One controller through Practice's chord path a boundary at a time, as the latch runs it: the device report becomes the pad
	// word (the touchpad click through touchpad_click, in the same report as every other button), chord_window reads the
	// restart chord (SELECT with Cross, R3 or a direction: the latch's mask 0xf440), and the game's word is what select_modifier
	// lets through of the device word, which never carries SELECT.
	struct chord_bench
	{
		pbd::practice::controls::touchpad_click touch;
		pbd::practice::controls::chord_window window;
		pbd::practice::controls::select_modifier modifier;
		std::uint16_t previous = 0, game = 0;
		bool restart = false;

		void step(std::uint16_t buttons, bool click, std::uint32_t boundary)
		{
			const auto pad = static_cast<std::uint16_t>(buttons | touch.update(click, std::nullopt, true));
			const auto pressed = static_cast<std::uint16_t>(pad & ~previous);
			previous = pad;
			const auto chord = window.update(pad, pressed, boundary);
			const bool select = (pad & 0x0100) != 0;
			restart = select && (chord & 0xf440);
			game = modifier.filter(static_cast<std::uint16_t>(pad & ~0x0100), select, pad);
		}
	};

	void simulate_logged_chord(const auto& tries, std::int64_t offset, int release)
	{
		using namespace pbd::practice::controls;
		constexpr std::uint16_t cross = 0x0040, circle = 0x0020, square = 0x0080, select = 0x0100, start = 0x0800;
		const auto click_from = [&](const auto& p)
		{
			return static_cast<std::int64_t>(p.at) + offset;
		};
		const auto click_to = [&](const auto& p)
		{
			const std::int64_t at = p.at;
			return release == 0 ? at + (std::max)(offset, std::int64_t{0}) + 2 : release == 1 ? at + p.held :
			                                                                                    at + p.held + 4;
		};
		const auto clicked = [&](std::uint32_t b)
		{
			return std::any_of(tries.begin(), tries.end(), [&](const auto& p)
				{
					return b >= click_from(p) && b < click_to(p);
				});
		};
		const auto crossed = [&](std::uint32_t b)
		{
			return std::any_of(tries.begin(), tries.end(), [&](const auto& p)
				{
					return b >= p.at && b < p.at + p.held;
				});
		};
		chord_bench bench;
		std::vector<std::uint32_t> restarts;
		std::vector<bool> reached;
		const std::uint32_t first = tries.front().at - 10, last = tries.back().at + tries.back().held + 10;
		for (std::uint32_t b = first; b <= last; ++b)
		{
			bench.step(crossed(b) ? cross : 0, clicked(b), b);
			if (bench.restart)
				restarts.push_back(b);
			reached.push_back((bench.game & cross) != 0);
		}
		pbd::test::check(restarts.size() == tries.size(), "every touchpad SELECT+Cross from the logs restarts exactly once");
		for (std::size_t i = 0; i < tries.size(); ++i)
		{
			const auto& p = tries[i];
			std::uint32_t chord_at = p.at;
			while (!clicked(chord_at))
				++chord_at;
			pbd::test::check(restarts[i] == chord_at, "the restart comes on the boundary SELECT and Cross are first both down");
			pbd::test::check(chord_at - p.at <= late_select_frames, "only a Cross at most two boundaries ahead of the click reached the game");
			for (std::uint32_t b = p.at; b < p.at + p.held; ++b)
				pbd::test::check(reached[b - first] == (b < chord_at), "from the chord on, Cross stays out of the game until it is released");
		}
		for (std::uint32_t b = first; b <= last; ++b)
			pbd::test::check(crossed(b) || !reached[b - first], "nothing else reaches the game");
	}

	void logged_touchpad_chords()
	{
		using namespace pbd::practice::controls;
		constexpr std::uint16_t cross = 0x0040, circle = 0x0020, square = 0x0080, select = 0x0100, start = 0x0800;
		// Cross and the click form one restart when they overlap within the chord window.
		// Exercise clicks before, with and after Cross, and releases before, with and after its release.
		struct press
		{
			std::uint32_t at, held;
		};
		const std::vector<std::vector<press>> logged{
			{{40332, 7}, {40366, 8}, {40386, 7}, {40405, 7}, {40424, 6}},
			{{40719, 10}, {40741, 5}, {40751, 5}, {40760, 6}, {40770, 7}, {40780, 6}, {40791, 7}},
			{{43778, 6}},
			{{45863, 12}},
		};
		for (const auto& tries : logged)
			for (std::int64_t offset = -3; offset <= 2; ++offset)
				for (int release = 0; release < 3; ++release)
					simulate_logged_chord(tries, offset, release);
	}

	void touchpad_recording()
	{
		using namespace pbd::practice::controls;
		constexpr std::uint16_t cross = 0x0040, circle = 0x0020, square = 0x0080, select = 0x0100, start = 0x0800;
		// SELECT withholds the live controller's buttons from P2's recording; SELECT+Circle or a fresh START ends it.
		// A click without a contact supplies SELECT, so it records nothing alone and ends the take with Circle.
		{
			touchpad_click touch;
			record_stop stop;
			select_modifier take_filter;
			menu_start menu;
			std::vector<std::uint16_t> take;
			std::uint16_t previous = 0;
			std::size_t tail = 0;
			struct outcome
			{
				bool chord, start;
			};
			const auto boundary = [&](std::uint16_t buttons, bool click, std::uint32_t index)
			{
				const auto pad = static_cast<std::uint16_t>(buttons | touch.update(click, std::nullopt, true));
				const auto pressed = static_cast<std::uint16_t>(pad & ~previous);
				previous = pad;
				const bool chord = stop.update(pad, pressed, index, take.size(), tail);
				// idle: recording in the match with no menu open; ready: no restart pending and DR's pause readable.
				const bool start_press = menu.update((pad & start) != 0, (pressed & start) != 0, (pad & select) != 0, false, true, true);
				take.push_back(take_filter.filter(static_cast<std::uint16_t>(pad & ~(select | start)), (pad & select) != 0, pad));
				return outcome{chord, start_press};
			};
			std::uint32_t index = 30319;
			for (int i = 0; i < 9; ++i)
				pbd::test::check(!boundary(0, false, index++).chord, "neutral before the throw");
			for (int i = 0; i < 8; ++i)
				pbd::test::check(!boundary(square | cross, false, index++).chord, "the 1+3 throw is recorded");
			for (int tap = 0; tap < 3; ++tap)
			{
				for (int i = 0; i < 6; ++i)
				{
					const auto result = boundary(0, true, index++);
					pbd::test::check(!result.chord && !result.start, "a click alone neither ends the take nor opens anything");
				}
				for (int i = 0; i < 10; ++i)
					boundary(0, false, index++);
			}
			pbd::test::check(std::count(take.begin(), take.end(), static_cast<std::uint16_t>(square | cross)) == 8 &&
								 std::count(take.begin(), take.end(), std::uint16_t{0}) == static_cast<std::ptrdiff_t>(take.size() - 8),
				"the take holds the throw and nothing of the clicks");
			const auto result = boundary(start, false, index++);
			pbd::test::check(result.start && !result.chord, "the first START press during the take ends it");
			pbd::test::check(!boundary(start, false, index++).start, "a START still held does not act twice");
		}
	}

	void touchpad_record_stop()
	{
		using namespace pbd::practice::controls;
		constexpr std::uint16_t cross = 0x0040, circle = 0x0020, square = 0x0080, select = 0x0100, start = 0x0800;
		for (std::uint32_t early = 0; early <= late_select_frames; ++early)
		{
			touchpad_click touch;
			record_stop stop;
			std::uint16_t previous = 0;
			std::size_t tail = 99, recorded = 50;
			bool stopped = false;
			for (std::uint32_t b = 200; b < 210 && !stopped; ++b)
			{
				const auto pad = static_cast<std::uint16_t>(circle | touch.update(b >= 200 + early, std::nullopt, true));
				const auto pressed = static_cast<std::uint16_t>(pad & ~previous);
				previous = pad;
				stopped = stop.update(pad, pressed, b, recorded++, tail);
				pbd::test::check(stopped == (b == 200 + early), "touchpad SELECT and Circle end the take on the boundary both are down");
			}
			pbd::test::check(stopped && tail == early, "a Circle up to two boundaries ahead of the click is the chord's tail");
		}
		{
			touchpad_click touch;
			record_stop stop;
			std::size_t tail = 99;
			const auto pad = [&](bool circle_down, bool click)
			{
				return static_cast<std::uint16_t>((circle_down ? circle : 0) | touch.update(click, std::nullopt, true));
			};
			pbd::test::check(!stop.update(pad(false, true), select, 300, 10, tail), "the click alone is not the stop");
			pbd::test::check(stop.update(pad(true, true), circle, 301, 11, tail) && tail == 0, "Circle under the click ends the take");
		}
		{
			menu_start menu;
			pbd::test::check(!menu.update(true, true, true, false, true, true), "touchpad SELECT held with START is the Exit hold, as with the SELECT button");
		}
	}

} // namespace

PBD_TEST(PbdPractice, TouchpadChecks)
{
	using pbd::practice::controls::touchpad_click;
	using pbd::practice::controls::touchpad_mode;
	constexpr std::uint16_t select = touchpad_click::select, start = touchpad_click::start;
	// PBD_TOUCHPAD_CLICK: select, start, split (the default, and anything unknown); any case.
	using pbd::practice::controls::parse_touchpad_mode;
	pbd::test::check(parse_touchpad_mode("") == touchpad_mode::split && parse_touchpad_mode("select") == touchpad_mode::select &&
						 parse_touchpad_mode("start") == touchpad_mode::start && parse_touchpad_mode("split") == touchpad_mode::split &&
						 parse_touchpad_mode("Split") == touchpad_mode::split && parse_touchpad_mode("START") == touchpad_mode::start &&
						 parse_touchpad_mode("starts") == touchpad_mode::split && parse_touchpad_mode("left") == touchpad_mode::split,
		"the touchpad setting reads select, start or split, and falls back to split");
	pbd::test::check(touchpad_click{}.mode == pbd::practice::controls::configured_touchpad_mode(), "a new click takes the configured mode");
	// select: every click is SELECT, wherever the finger is or whether the pad reports one.
	{
		touchpad_click every;
		every.mode = touchpad_mode::select;
		for (const std::optional<float> x : {std::optional<float>(.2f), std::optional<float>(.8f), std::optional<float>()})
		{
			pbd::test::check(every.update(true, x, true) == select, "select: every click is SELECT");
			every.update(false, {}, true);
		}
		pbd::test::check(every.update(true, .9f, false) == 0, "outside Practice the click presses nothing");
	}
	// start: every click is START, held until release.
	{
		touchpad_click every;
		every.mode = touchpad_mode::start;
		for (const std::optional<float> x : {std::optional<float>(.2f), std::optional<float>(.8f), std::optional<float>()})
		{
			pbd::test::check(every.update(true, x, true) == start && every.update(true, .1f, true) == start,
				"start: every click is START until it is released");
			pbd::test::check(every.update(false, {}, true) == 0, "released, it presses nothing");
		}
		pbd::test::check(every.update(true, .9f, false) == 0, "outside Practice the click presses nothing");
	}
	// split: left half SELECT, right half START, SELECT without a finger position.
	touchpad_click touch;
	touch.mode = touchpad_mode::split;
	pbd::test::check(touch.update(false, .2f, true) == 0, "touch contact without click must not press a system button");
	pbd::test::check(touch.update(true, .2f, true) == select, "left click must supply SELECT");
	pbd::test::check(touch.update(true, .8f, true) == select && touch.update(true, std::nullopt, true) == select,
		"dragging or losing contact during a click must retain the original modifier");
	pbd::test::check(touch.update(false, .8f, true) == 0 && touch.update(true, .8f, true) == start,
		"release and a new right click must supply START");
	pbd::test::check(touch.update(true, .2f, false) == 0, "touchpad system gestures must remain scoped to Practice");
	touch.update(false, {}, true);
	pbd::test::check(touch.update(true, .5f, true) == start, "the midpoint must belong to the right half");

	// A touchpad that is only a button (an arcade stick's on SDL) clicks without ever reporting a contact. That click
	// is SELECT from its first report until release; a contact that appears later in the same click changes nothing.
	touch.update(false, {}, true);
	pbd::test::check(touch.update(true, std::nullopt, true) == select,
		"a click without a contact is SELECT on its first report");
	pbd::test::check(touch.update(true, .8f, true) == select && touch.update(true, std::nullopt, true) == select,
		"a contact appearing later in the click does not turn it into START");
	pbd::test::check(touch.update(false, std::nullopt, true) == 0 && touch.update(true, std::nullopt, true) == select,
		"every new click without a contact is SELECT again");
	touch.update(false, {}, true);
	pbd::test::check(touch.update(true, std::numeric_limits<float>::quiet_NaN(), true) == select, "an invalid coordinate counts as no contact");
	touch.update(false, {}, true);
	pbd::test::check(touch.update(true, -.1f, true) == select && touch.update(false, {}, true) == 0 && touch.update(true, 1.5f, true) == select,
		"an out-of-range contact counts as no contact");
	touch.update(false, {}, true);
	pbd::test::check(touch.update(true, std::nullopt, false) == 0, "outside Practice a click without a contact presses nothing either");
	touch.update(false, {}, true);
	pbd::test::check(touch.update(true, std::nullopt, true) == select && touch.update(false, {}, true) == 0 && touch.update(true, .7f, true) == start,
		"split on a pad that reports a finger: a position-less click is SELECT, a right-half click START");
}

PBD_TEST(PbdPractice, TouchpadChordChecks)
{
	using namespace pbd::practice::controls;
	constexpr std::uint16_t cross = 0x0040, circle = 0x0020, square = 0x0080, select = 0x0100, start = 0x0800;

	logged_touchpad_chords();
	// Cross held three boundaries before the click is an ordinary 3: no restart, and from the click on it is withheld until
	// released, after the click as well.
	{
		chord_bench bench;
		bool restarted = false;
		for (std::uint32_t b = 100; b < 120; ++b)
		{
			bench.step(b >= 100 && b < 112 ? cross : 0, b >= 103 && b < 106, b);
			restarted |= bench.restart;
			pbd::test::check(((bench.game & cross) != 0) == (b < 103), "a 3 already under way is withheld from the click until released");
		}
		pbd::test::check(!restarted, "a Cross held longer before the click is not the restart chord");
	}
	// A press after the chord's Cross was released is a real 3 again.
	{
		chord_bench bench;
		for (std::uint32_t b = 100; b < 108; ++b)
			bench.step(b < 106 ? cross : 0, b < 104, b);
		bench.step(cross, false, 108);
		pbd::test::check(bench.game == cross, "a fresh Cross after the chord was released reaches the game");
	}

	touchpad_recording();
	touchpad_record_stop();
}

// Ending a take, boundary by boundary: START (each press up to twice), SELECT from the button or a touchpad click without a
// contact (up to twice), Circle, and a 1+3 held throughout or not. The take ends at the first boundary where START is
// newly pressed without SELECT (SELECT+START is the Exit hold, also when SELECT lifts first) or SELECT+Circle completes,
// whatever else is held; the touchpad ends it exactly as the button does.
PBD_TEST(PbdPractice, RecordStopTimingChecks)
{
	using namespace pbd::practice::controls;
	using intervals = std::vector<std::pair<std::uint32_t, std::uint32_t>>;
	constexpr std::uint16_t select = 0x0100, start = 0x0800, circle = 0x0020, throw_13 = 0x00c0;
	constexpr std::uint32_t frames = 6;
	std::vector<intervals> sets{{}};
	for (std::uint32_t a = 0; a < frames; ++a)
		for (std::uint32_t b = a + 1; b <= frames; ++b)
		{
			sets.push_back({{a, b}});
			for (std::uint32_t c = b + 1; c < frames; ++c)
				for (std::uint32_t d = c + 1; d <= frames; ++d)
					sets.push_back({{a, b}, {c, d}});
		}
	const auto in = [](const intervals& v, std::uint32_t t)
	{
		return std::any_of(v.begin(), v.end(), [t](const auto& i)
			{
				return t >= i.first && t < i.second;
			});
	};
	const auto stop_at = [&](const intervals& st, const intervals& s, const intervals& ci, bool held, bool touchpad)
	{
		touchpad_click touch;
		menu_start menu;
		record_stop stop;
		std::uint16_t previous = 0;
		std::size_t tail = 0;
		for (std::uint32_t n = 0; n < frames + 2; ++n)
		{
			const bool click = in(s, n);
			const std::uint16_t system = touchpad ? touch.update(click, std::nullopt, true) : click ? select :
			                                                                                          std::uint16_t{0};
			const auto pad = static_cast<std::uint16_t>((in(st, n) ? start : 0) | (in(ci, n) ? circle : 0) | (held ? throw_13 : 0) | system);
			const auto pressed = static_cast<std::uint16_t>(pad & ~previous);
			previous = pad;
			// idle: the take is recording with no menu open; ready: no restart pending (none can be, while recording) and DR's
			// pause readable.
			const bool by_start = menu.update((pad & start) != 0, (pressed & start) != 0, (pad & select) != 0, false, true, true);
			const bool by_chord = stop.update(pad, pressed, n, n, tail);
			if (by_start || by_chord)
				return static_cast<std::int64_t>(n);
		}
		return std::int64_t{-1};
	};
	for (const auto& st : sets)
		for (const auto& s : sets)
			for (const auto& ci : {intervals{}, intervals{{1, 3}}, intervals{{2, 6}}})
			{
				std::int64_t expected = -1;
				for (std::uint32_t n = 0; n < frames + 2 && expected < 0; ++n)
				{
					const bool start_edge = in(st, n) && !(n > 0 && in(st, n - 1));
					const bool chord = in(s, n) && in(ci, n) && ((!(n > 0 && in(s, n - 1))) || !(n > 0 && in(ci, n - 1)));
					if ((start_edge && !in(s, n)) || chord)
						expected = n;
				}
				for (const bool held : {false, true})
				{
					pbd::test::check(stop_at(st, s, ci, held, false) == expected, "the take ends at the first START press without SELECT, or SELECT+Circle");
					pbd::test::check(stop_at(st, s, ci, held, true) == expected, "a touchpad click without a contact ends the take as the SELECT button does");
				}
			}
}
