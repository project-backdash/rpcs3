#include "pbd_test.h"

#include "Emu/Cell/pbd_select_timer.h"
#include <string_view>

using pbd::test::check;

namespace
{
	using namespace pbd::select_timer;
	constexpr u16 left = 0x8000, right = 0x2000, up = 0x1000, square = 0x0080, circle = 0x0020;
	view in_select(u32 left_state, u32 right_state)
	{
		view out;
		out.verified = true;
		out.vs_mode = true;
		out.scene = scene_select;
		out.state = {left_state, right_state};
		if (left_state == state_stage || right_state == state_stage)
		{
			out.stage_open = true;
			out.stage_owner = left_state == state_stage ? 0u : 1u;
		}
		return out;
	}
	view in_scene(u32 value)
	{
		view out;
		out.verified = true;
		out.vs_mode = true;
		out.scene = value;
		return out;
	}
	// One timer per PC, each fed the same native view at the same boundary, as the two games are in phase lockstep.
	struct two_pcs
	{
		std::array<timer, 2> pcs;
		two_pcs(u32 character = 30, u32 stage = 15)
		{
			check(pcs[0].begin(character, stage, 0) && pcs[1].begin(character, stage, 1));
		}
		std::array<u16, 2> sample(u32 source, const view& now, std::array<u16, 2> physical = {})
		{
			return {pcs[0].sample(source, now, physical[0]), pcs[1].sample(source, now, physical[1])};
		}
		// The band both screens draw: identical by construction.
		band status(u32 source, const view& now) const
		{
			const auto a = pcs[0].status(source, now), b = pcs[1].status(source, now);
			check(a.current == b.current && a.seconds == b.seconds && a.frames == b.frames && a.total == b.total && a.stage_owner == b.stage_owner &&
				  a.choosing == b.choosing);
			return a;
		}
	};
} // namespace

PBD_TEST(PbdSelectTimer, Checks)
{
	{
		timer t;
		check(!t.begin(0, 15, 0) && !t.begin(30, 0, 0) && !t.begin(30, 15, 2) && !t.begin(max_seconds + 1, 15, 0));
		check(!t.selecting() && t.sample(5, in_select(3, 3), left) == left);
	}
	{
		// Nothing counts before the select. The loading into it runs neutral input, so its samples are neutral too; an unverified
		// or non-VS view passes the word through.
		two_pcs p;
		check(p.status(0, in_scene(2)).current == phase::waiting);
		check((p.sample(0, in_scene(2), {left, right}) == std::array<u16, 2>{0, 0}));
		view unread = in_select(3, 3);
		unread.verified = false;
		check((p.sample(1, unread, {left, right}) == std::array<u16, 2>{left, right}));
		view other = in_select(9, 9);
		other.vs_mode = false;
		check((p.sample(2, other, {left, right}) == std::array<u16, 2>{left, right}));
		check(!p.pcs[0].character_deadline() && p.status(2, in_scene(2)).current == phase::waiting);
		// The first character-select sample is the origin on both PCs; the count is whole seconds of boundaries. What is held
		// there (still the loading's Left and Right) stays out until it is let go: no press arrives on the highlighted fighter.
		check((p.sample(10, in_select(3, 3), {left, right}) == std::array<u16, 2>{0, 0}));
		check(p.pcs[0].character_deadline() == 10u + 1800u && p.pcs[1].character_deadline() == 10u + 1800u);
		const auto at = [&](u32 source)
		{
			return p.status(source, in_select(3, 3));
		};
		check(at(10).current == phase::character && at(10).seconds == 30 && at(69).seconds == 30 && at(70).seconds == 29);
		check(at(1809).seconds == 1 && at(1810).seconds == 0 && at(4000).seconds == 0);
		check(at(10).choosing[0] && at(10).choosing[1]);
		// Once let go, the players' own input reaches the select before zero, directions included.
		check((p.sample(11, in_select(3, 3)) == std::array<u16, 2>{0, 0}));
		check((p.sample(12, in_select(3, 3), {up, square}) == std::array<u16, 2>{up, square}));
		// At zero each PC presses for its own player only: Cross six samples, nothing for eighteen, again, whatever is held.
		for (u32 source = 1810; source < 1810 + 48; ++source)
		{
			const u16 expected = (source - 1810) % 24 < 6 ? cross : u16{0};
			check((p.sample(source, in_select(3, 3), {left, circle}) == std::array<u16, 2>{expected, expected}));
		}
		// A player who already chose is not pressed (and a waiting record takes no direction); the other is pressed on with the
		// same cadence from the same deadline.
		const auto mixed = p.sample(1858, in_select(12, 3), {square, left});
		check(mixed[0] == square && mixed[1] == cross);
		check(p.sample(1859, in_select(12, 3), {square | left, 0})[0] == square);
		const auto drawn = p.status(1858, in_select(12, 3));
		check(!drawn.choosing[0] && drawn.choosing[1]);
		// The handicap state is confirmed for each player from its first sample, so the even handicap is taken.
		check((p.sample(1882, in_select(9, 9), {right, left}) == std::array<u16, 2>{cross, cross}));
		// A repeated source returns the same word and never moves an origin.
		check((p.sample(1882, in_select(9, 9), {right, left}) == std::array<u16, 2>{cross, cross}));
		check(p.pcs[0].character_deadline() == 1810u);
		// The first battle frame ends the selection: no press after it.
		check((p.sample(1900, in_scene(17), {left, cross}) == std::array<u16, 2>{left, cross}));
		check(!p.pcs[0].selecting() && p.status(1900, in_scene(17)).current == phase::finished);
		check((p.sample(1906, in_select(3, 3), {left, left}) == std::array<u16, 2>{left, left}));
		// A natural continuation arms both countdowns again; the next character select is the new origin.
		check(p.pcs[0].begin(30, 15, 0) && p.pcs[1].begin(30, 15, 1));
		check((p.sample(4990, in_scene(17), {left, left}) == std::array<u16, 2>{left, left}) && p.pcs[0].selecting());
		// The end of the previous game is neutral, and a press held through it into the select (the Rematch Cross) stays out
		// until it is let go; a new press then reaches the select.
		check((p.sample(5000, in_scene(1), {cross, cross}) == std::array<u16, 2>{0, 0}));
		check((p.sample(5100, in_select(3, 3), {cross, cross}) == std::array<u16, 2>{0, 0}) && p.pcs[1].character_deadline() == 5100u + 1800u);
		check((p.sample(5101, in_select(3, 3), {cross, 0}) == std::array<u16, 2>{0, 0}));
		check((p.sample(5102, in_select(3, 3), {cross, cross}) == std::array<u16, 2>{0, cross}));
	}
	{
		// The handicap state never takes the local player's buttons: each PC confirms it for its own player with Cross at the
		// press cadence from its first sample there, so the life bar stays full and the screen passes at once.
		two_pcs p;
		check(p.sample(0, in_select(3, 3))[0] == 0);
		check((p.sample(1, in_select(9, 3), {left | circle, left}) == std::array<u16, 2>{cross, left}));
		check((p.sample(2, in_select(9, 9), {up | right, 0x4000}) == std::array<u16, 2>{cross, cross}));
		for (u32 source = 3; source < 2 + cadence_samples; ++source)
		{
			const u16 left_word = (source - 1) % cadence_samples < press_samples ? cross : u16{0};
			const u16 right_word = (source - 2) % cadence_samples < press_samples ? cross : u16{0};
			check((p.sample(source, in_select(9, 9), {circle | up, square | right}) == std::array<u16, 2>{left_word, right_word}));
		}
		// Back in the handicap state after leaving it, the cadence starts again from that first sample.
		check(p.sample(40, in_select(10, 9), {0, 0})[0] == 0);
		check(p.sample(41, in_select(9, 9), {0, 0})[0] == cross && p.sample(47, in_select(9, 9))[0] == 0);
		check(p.sample(65, in_select(9, 9))[0] == cross);
		// Another handicap than 100 in either record refuses the selection on both PCs.
		view changed = in_select(9, 9);
		changed.handicap[1] = 90;
		p.sample(66, changed, {});
		check(p.pcs[0].refusal() && std::string_view(p.pcs[0].refusal()) == "selection_handicap_changed" && !p.pcs[0].selecting());
		check(p.pcs[1].refusal() && !p.pcs[1].selecting());
	}
	{
		// The fighter's costume press may still be in flight when the handicap state shows: the confirmation waits for the next
		// cadence press after it, so it never merges into that press.
		two_pcs p;
		check(p.sample(0, in_select(3, 3))[1] == 0);
		check(p.sample(100, in_select(3, 3), {0, square})[1] == square);
		check(p.sample(105, in_select(3, 9), {0, square})[1] == 0);
		for (u32 source = 106; source < 105 + cadence_samples; ++source)
			check(p.sample(source, in_select(3, 9))[1] == 0);
		for (u32 source = 105 + cadence_samples; source < 105 + cadence_samples + press_samples; ++source)
			check(p.sample(source, in_select(3, 9))[1] == cross);
		// No press of the player's own starts a window in the handicap state.
		two_pcs q;
		check(q.sample(0, in_select(3, 3))[1] == 0);
		check(q.sample(200, in_select(3, 9), {0, square})[1] == cross);
		check(q.sample(206, in_select(3, 9), {0, cross})[1] == 0 && q.sample(224, in_select(3, 9), {0, 0})[1] == cross);
	}
	{
		// After a costume press in the character state, directions stay out while that press may still be in flight, and one
		// held through that stretch stays out until it is let go.
		two_pcs p;
		check(p.sample(0, in_select(3, 3))[0] == 0);
		check(p.sample(1, in_select(3, 3), {left, 0})[0] == left);
		check(p.sample(2, in_select(3, 3), {cross | left, 0})[0] == cross);
		check(p.sample(3, in_select(3, 3), {right, 0})[0] == 0);
		check(p.sample(2 + confirm_flight_samples - 1, in_select(3, 3), {right, 0})[0] == 0);
		check(p.sample(2 + confirm_flight_samples, in_select(3, 3), {right, 0})[0] == 0);
		check(p.sample(2 + confirm_flight_samples + 1, in_select(3, 3), {0, 0})[0] == 0);
		check(p.sample(2 + confirm_flight_samples + 2, in_select(3, 3), {right, 0})[0] == right);
		// A held button is no new press: it opens no second window.
		check(p.sample(20, in_select(3, 3), {square, 0})[0] == square);
		check(p.sample(20 + confirm_flight_samples, in_select(3, 3), {square | right, 0})[0] == (square | right));
	}
	{
		// The stage countdown starts when a record reaches the stage state; only its owner (here the Right side) is pressed. The
		// waiting Left record takes no direction.
		two_pcs p;
		check(p.sample(0, in_select(3, 3))[0] == 0);
		check((p.sample(600, in_select(12, 13), {left, up}) == std::array<u16, 2>{0, up}));
		check(p.pcs[0].stage_deadline() == 600u + 900u && p.pcs[1].stage_deadline() == 600u + 900u);
		const auto shown = p.status(600, in_select(12, 13));
		check(shown.current == phase::stage && shown.seconds == 15 && shown.stage_owner == 1);
		check(p.status(1499, in_select(12, 13)).seconds == 1 && p.status(1500, in_select(12, 13)).seconds == 0);
		// Before zero the picker's own presses work the roulette and the lists.
		check(p.sample(1499, in_select(12, 13), {0, right})[1] == right);
		for (u32 source = 1500; source < 1500 + 30; ++source)
		{
			const auto words = p.sample(source, in_select(12, 13), {cross, left});
			check(words[0] == cross && words[1] == ((source - 1500) % 24 < 6 ? cross : u16{0}));
		}
		// Past the character deadline too, a record outside the character states is never pressed.
		check(p.sample(1900, in_select(12, 13), {square, 0})[0] == square);
	}
	{
		// Rematch with random stage: the stage countdown starts at zero. Its zero is the first boundary with a record in the stage
		// state, the same on both PCs; the stage owner's PC presses from there with the usual cadence and nothing the player holds
		// reaches the roulette; the other PC never presses. The band shows the random stage on both screens.
		two_pcs p;
		check(p.pcs[0].begin(30, 15, 0, true) && p.pcs[1].begin(30, 15, 1, true));
		check(p.pcs[0].random_stage() && p.pcs[1].random_stage());
		check(p.sample(0, in_select(3, 3))[0] == 0);
		// The character countdown is untouched.
		check(p.pcs[0].character_deadline() == 1800u && p.status(10, in_select(3, 3)).seconds == 30 && p.status(10, in_select(3, 3)).random_stage);
		check((p.sample(600, in_select(12, 13), {left, up}) == std::array<u16, 2>{0, cross}));
		check(p.pcs[0].stage_deadline() == 600u && p.pcs[1].stage_deadline() == 600u);
		const auto shown = p.status(600, in_select(12, 13));
		check(shown.current == phase::stage && shown.seconds == 0 && shown.frames == 0 && shown.total == 0 && shown.random_stage && shown.stage_owner == 1);
		for (u32 source = 600; source < 600 + 2 * cadence_samples; ++source)
		{
			const auto words = p.sample(source, in_select(12, 13), {0, static_cast<u16>(source % 3 ? up | square : 0)});
			check(words[0] == 0 && words[1] == ((source - 600) % cadence_samples < press_samples ? cross : u16{0}));
		}
		// A plain continuation arms the stage countdown with its full time again.
		check(p.pcs[1].begin(30, 15, 1) && !p.pcs[1].random_stage());
	}
	{
		// A handicap confirmation still in flight holds the random stage's zero press back as it holds any time-out's.
		two_pcs p;
		check(p.pcs[0].begin(30, 15, 0, true) && p.pcs[1].begin(30, 15, 1, true));
		check(p.sample(0, in_select(3, 3))[1] == 0);
		check(p.sample(500, in_select(9, 9), {0, cross})[1] == cross);
		check(p.sample(503, in_select(10, 13))[1] == 0 && p.pcs[1].stage_deadline() == 503u);
		for (u32 source = 504; source < 500 + confirm_flight_samples; ++source)
			check(p.sample(source, in_select(10, 13))[1] == 0);
		for (u32 source = 503 + cadence_samples; source < 503 + cadence_samples + press_samples; ++source)
			check(p.sample(source, in_select(10, 13))[1] == cross);
	}
	{
		// The random stop: the stage owner's PC first presses `draw % span` boundaries after the zero. Until then the roulette spins on
		// its own (nothing the stage owner presses reaches it, and no press of theirs moves the stop); the other PC's draw is never
		// applied; both screens show the same zero and no delay.
		two_pcs p;
		check(p.pcs[0].begin(30, 15, 0, true, 7) && p.pcs[1].begin(30, 15, 1, true, random_stop_span + 100));
		check(p.sample(0, in_select(3, 3))[1] == 0);
		check((p.sample(600, in_select(12, 13), {square, cross}) == std::array<u16, 2>{square, 0}));
		check(p.pcs[0].stage_deadline() == 600u && p.pcs[1].stage_deadline() == 600u);
		check(p.pcs[0].stage_stop() == 600u && p.pcs[1].stage_stop() == 700u);
		for (u32 source = 601; source < 700; ++source)
		{
			// Cross and Up pressed fresh every other sample, right up to the stop.
			const u16 mash = source % 2 ? static_cast<u16>(cross | up) : u16{0};
			check(p.sample(source, in_select(12, 13), {0, mash})[1] == 0);
			const auto shown = p.status(source, in_select(12, 13));
			check(shown.current == phase::stage && shown.seconds == 0 && shown.frames == 0 && shown.total == 0 && shown.random_stage);
		}
		for (u32 source = 700; source < 700 + 2 * cadence_samples; ++source)
			check(p.sample(source, in_select(12, 13), {0, cross})[1] == ((source - 700) % cadence_samples < press_samples ? cross : u16{0}));
		// The random stop stays fixed when the stage state is entered again.
		check(p.sample(800, in_select(12, 12))[1] == 0);
		p.sample(900, in_select(12, 13));
		check(p.pcs[1].stage_stop() == 700u);
	}
	{
		// The delay is bounded by the stage countdown the set names, and any draw stays below the bound; a plain continuation
		// ignores a draw.
		timer short_stage;
		check(short_stage.begin(30, 1, 1, true, 0xffffffffu));
		short_stage.sample(0, in_select(3, 3), 0);
		short_stage.sample(100, in_select(12, 13), 0);
		check(short_stage.stage_deadline() == 100u && short_stage.stage_stop() == 100u + 0xffffffffu % boundaries_per_second);
		check(*short_stage.stage_stop() < 100u + boundaries_per_second);
		timer long_stage;
		check(long_stage.begin(30, max_seconds, 1, true, 0xffffffffu));
		long_stage.sample(0, in_select(3, 3), 0);
		long_stage.sample(100, in_select(12, 13), 0);
		check(long_stage.stage_stop() == 100u + 0xffffffffu % random_stop_span);
		timer plain;
		check(plain.begin(30, 15, 1, false, 100));
		plain.sample(0, in_select(3, 3), 0);
		plain.sample(100, in_select(12, 13), 0);
		check(plain.stage_stop() == plain.stage_deadline() && plain.stage_deadline() == 100u + 15 * boundaries_per_second);
		// A Left owner draws on the Left PC; the Right PC's draw is never applied.
		timer left_owner, right_watch;
		check(left_owner.begin(30, 15, 0, true, 30) && right_watch.begin(30, 15, 1, true, 50));
		for (auto* t : {&left_owner, &right_watch})
		{
			t->sample(0, in_select(3, 3), 0);
			t->sample(100, in_select(13, 12), 0);
		}
		check(left_owner.stage_stop() == 130u && right_watch.stage_stop() == 100u);
		check(left_owner.sample(129, in_select(13, 12), cross) == 0 && left_owner.sample(130, in_select(13, 12), 0) == cross);
	}
	{
		// The stage owner is read from the game every game: a Left owner is pressed on the Left PC.
		two_pcs p(30, 5);
		check(p.sample(0, in_select(3, 3))[0] == 0);
		check(p.sample(100, in_select(13, 12))[0] == 0 && p.status(100, in_select(13, 12)).stage_owner == 0);
		const auto words = p.sample(400, in_select(13, 12), {0, 0});
		check(words[0] == cross && words[1] == 0);
	}
	{
		// A direction held through the handicap state does not arrive in the character state as a fresh press.
		two_pcs p;
		check(p.sample(0, in_select(3, 3))[0] == 0);
		check(p.sample(1, in_select(9, 3), {up, 0})[0] == cross);
		check(p.sample(2, in_select(3, 3), {up, 0})[0] == 0);
		check(p.sample(3, in_select(3, 3), {0, 0})[0] == 0);
		check(p.sample(4, in_select(3, 3), {up, 0})[0] == up);
	}
	{
		// Cross held through the handicap confirmation does not come back as a fresh press when the stage select opens: the
		// roulette is stopped only by a new press, or by the stage countdown's own zero.
		two_pcs p;
		check(p.sample(0, in_select(3, 3))[1] == 0);
		for (u32 source = 1800; source < 1800 + cadence_samples; ++source)
			check(p.sample(source, in_select(9, 9), {0, cross})[1] == ((source - 1800) % cadence_samples < press_samples ? cross : u16{0}));
		check(p.sample(1824, in_select(10, 13), {0, cross})[1] == 0 && p.pcs[1].stage_deadline() == 1824u + 900u);
		check(p.sample(1825, in_select(10, 13), {0, 0})[1] == 0);
		check(p.sample(1826, in_select(10, 13), {0, cross})[1] == cross);
	}
	{
		// The player's own fighter confirmation just before zero is still in flight: the zero press waits for it, so no
		// countdown press lands in the handicap state that confirmation opens.
		two_pcs p;
		check(p.sample(0, in_select(3, 3))[1] == 0);
		check(p.sample(1798, in_select(9, 3), {0, cross})[1] == cross);
		check(p.sample(1799, in_select(9, 3))[1] == 0);
		for (u32 source = 1800; source < 1798 + confirm_flight_samples; ++source)
			check(p.sample(source, in_select(9, 3))[1] == 0);
		check(p.sample(1810, in_select(9, 3))[1] == 0 && p.sample(1824, in_select(9, 3))[1] == cross);
		// Presses after zero never reach the game, so they never hold the zero press back.
		two_pcs r;
		check(r.sample(0, in_select(3, 3))[0] == 0);
		for (u32 source = 1800; source < 1800 + press_samples; ++source)
			check(r.sample(source, in_select(3, 3), {static_cast<u16>(source % 2 ? cross : 0), 0})[0] == cross);
	}
	{
		// Change character, Right side chooses. The Left PC's word is the selection driver's (its player's own buttons never
		// reach the select) and passes unchanged; the Right PC runs the character countdown, its zero press and the handicap.
		timer t;
		check(!t.begin_repick(0, 0, 1) && !t.begin_repick(max_seconds + 1, 0, 1) && !t.begin_repick(30, 2, 1) && !t.begin_repick(30, 0, 2));
		two_pcs p;
		check(p.pcs[0].begin_repick(30, 0, 1) && p.pcs[1].begin_repick(30, 1, 1));
		check(p.pcs[0].chooser() == 1u && p.pcs[1].chooser() == 1u && p.pcs[0].selecting() && !p.pcs[0].random_stage());
		check((p.sample(0, in_scene(2), {square, left}) == std::array<u16, 2>{0, 0}));
		check((p.sample(10, in_select(3, 3), {square, left}) == std::array<u16, 2>{square, 0}));
		check(p.pcs[0].character_deadline() == 10u + 1800u && p.pcs[1].character_deadline() == 10u + 1800u);
		const auto shown = p.status(10, in_select(3, 3));
		check(shown.current == phase::character && shown.seconds == 30 && shown.chooser == 1u);
		check((p.sample(11, in_select(9, 3), {cross | up, 0}) == std::array<u16, 2>{cross | up, 0}));
		check((p.sample(12, in_select(12, 3), {0, up}) == std::array<u16, 2>{0, up}));
		// At zero only the chooser's PC presses; the kept PC still passes the driver's word.
		for (u32 source = 1810; source < 1810 + 48; ++source)
		{
			const u16 expected = (source - 1810) % 24 < 6 ? cross : u16{0};
			check((p.sample(source, in_select(12, 3), {0, left}) == std::array<u16, 2>{0, expected}));
		}
		check((p.sample(1858, in_select(12, 9), {0, left}) == std::array<u16, 2>{0, cross}));
		// No word of the chooser's reaches a waiting record: the stage select opens next, and it is steered.
		check(p.sample(1882, in_select(12, 12), {0, 0})[1] == 0);
		check(p.sample(1883, in_select(12, 12), {0, cross})[1] == 0 && p.sample(1884, in_select(12, 12), {0, circle})[1] == 0);
		// In the stage state the driver's word passes unchanged on the stage owner's PC; nothing presses at a zero.
		check((p.sample(1900, in_select(12, 13), {0, up}) == std::array<u16, 2>{0, up}));
		check(p.pcs[1].stage_deadline() == 1900u);
		for (u32 source = 1901; source < 1901 + 2 * cadence_samples; ++source)
			check(p.sample(source, in_select(12, 13), {0, 0})[1] == 0);
		const auto stage = p.status(1950, in_select(12, 13));
		check(stage.current == phase::stage && stage.seconds == 0 && stage.total == 0 && stage.chooser == 1u && !stage.random_stage);
		check((p.sample(2000, in_scene(17), {left, cross}) == std::array<u16, 2>{left, cross}) && !p.pcs[1].selecting());
		// Another selection clears the repick.
		check(p.pcs[1].begin(30, 15, 1) && !p.pcs[1].chooser());
	}
	{
		// A costume press still in flight: the chooser's next press stays out, as it could land in a later state of the select;
		// the held press goes on. In any other selection that press reaches the game.
		two_pcs p;
		check(p.pcs[0].begin_repick(30, 0, 0) && p.pcs[1].begin_repick(30, 1, 0));
		check(p.sample(0, in_select(3, 3))[0] == 0);
		check(p.sample(1, in_select(3, 3), {square, 0})[0] == square);
		check(p.sample(2, in_select(3, 3), {square, 0})[0] == square);
		check(p.sample(3, in_select(3, 3), {square | cross, 0})[0] == square);
		check(p.sample(4, in_select(3, 3), {cross, 0})[0] == 0);
		check(p.sample(1 + confirm_flight_samples, in_select(3, 3), {0, 0})[0] == 0);
		check(p.sample(2 + confirm_flight_samples, in_select(3, 3), {circle, 0})[0] == circle);
		two_pcs q;
		check(q.sample(0, in_select(3, 3))[0] == 0);
		check(q.sample(1, in_select(3, 3), {square, 0})[0] == square && q.sample(3, in_select(3, 3), {cross, 0})[0] == cross);
		// The even handicap still binds the kept player's record too.
		view changed = in_select(3, 9);
		changed.handicap[1] = 90;
		p.sample(40, changed, {});
		check(p.pcs[0].refusal() && p.pcs[1].refusal() && std::string_view(p.pcs[1].refusal()) == "selection_handicap_changed");
	}
}

PBD_TEST(PbdSelectTimer, SelectAfterPlainRematch)
{
	// A game ended by a plain Rematch load drops the timer (lobby_select_timer.reset(): its resolved load never enters the select).
	// The next set's stock select (Join Battle) arms a fresh one with both countdowns, and its origin is that select's first sample.
	std::optional<timer> clock;
	clock.emplace();
	check(clock->begin(30, 15, 0));
	clock->sample(10, in_select(3, 3), 0);
	clock->sample(20, in_scene(17), 0);
	check(!clock->selecting());
	clock.reset();
	check(arm_select(clock, 30, 15, 1) && clock && clock->selecting() && !clock->chooser() && !clock->character_deadline());
	clock->sample(500, in_select(3, 3), 0);
	check(clock->character_deadline() == 500u + 1800u);
	// A repick's timer (Change character) is replaced by a full selection for the stock select after it.
	check(clock->begin_repick(30, 1, 1) && clock->chooser() == 1u);
	check(arm_select(clock, 30, 15, 1) && !clock->chooser() && clock->selecting() && !clock->character_deadline());
	// Invalid rules still refuse, with or without a timer.
	clock.reset();
	check(!arm_select(clock, 0, 15, 0) && !clock->selecting());
	check(!arm_select(clock, 30, 15, 2) && !clock->selecting());
}
