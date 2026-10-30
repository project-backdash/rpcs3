#include "pbd_test.h"

#include "Emu/Cell/pbd_practice_streak.h"

#include <cstdint>
#include <initializer_list>
#include <optional>
#include <string>
#include <vector>

namespace
{
	namespace movement = pbd::practice::movement;
	namespace streaks = pbd::practice::streak;
	using words = std::vector<std::uint16_t>;

	using pbd::test::check;

	struct run
	{
		std::uint16_t word;
		std::uint32_t frames;
	};

	words expand(std::initializer_list<run> runs)
	{
		words out;
		for (const auto& each : runs)
			out.insert(out.end(), each.frames, each.word);
		return out;
	}

	words repeat(const words& part, std::size_t times)
	{
		words out;
		for (std::size_t i = 0; i < times; ++i)
			out.insert(out.end(), part.begin(), part.end());
		return out;
	}

	constexpr std::uint16_t n = 0, f = movement::forward, b = movement::back, d = movement::down, u = movement::up;
	constexpr std::uint16_t df = d | f, db = d | b;
	// An attack button (right punch), outside the direction bits.
	constexpr std::uint16_t two = 0x0010;

	// P1's screen words for `relative` on `side`, read back as the latch reads them (oriented with P1's side), then followed
	// by `rest` neutral frames; every run that ends along the way, or with `singles` every single backdash cancel instead.
	std::vector<streaks::streak> feed(const words& relative, movement::screen_side side, std::uint32_t rest = 200, bool singles = false)
	{
		streaks::tracker tracker;
		std::vector<streaks::streak> out;
		words all = relative;
		all.insert(all.end(), rest, n);
		for (const auto word : all)
		{
			const auto screen = movement::oriented(word, side);
			if (const auto ended = tracker.frame(movement::oriented(screen, side)); ended && (ended->what == streaks::kind::single_cancel) == singles)
				out.push_back(*ended);
		}
		return out;
	}
	std::vector<streaks::streak> singles(const words& relative, movement::screen_side side)
	{
		return feed(relative, side, 200, true);
	}

} // namespace

PBD_TEST(PbdPracticeStreak, Wavedash)
{
	for (const auto side : {movement::screen_side::left, movement::screen_side::right})
	{
		// The player's relaxed capture, 31 passes of f 5, n 3, d 4, d/f 3, f 2, n 6 (23 frames): 2.61 a second.
		const words relaxed = expand({{f, 5}, {n, 3}, {d, 4}, {df, 3}, {f, 2}, {n, 6}});
		const auto seen = feed(repeat(relaxed, 31), side);
		check(seen.size() == 1 && seen[0].what == streaks::kind::wavedash && seen[0].count == 31, "31 crouch dashes are one streak of 31");
		check(streaks::text(seen[0]) == "WAVEDASH x31 - 2.61/s", "with its pace in crouch dashes a second");
		// Every Practice speed reads back as itself, from the dummy's own tapes; 8 a second (a lightdash) too.
		for (std::uint32_t speed = movement::min_wavedash_speed; speed <= movement::max_wavedash_speed; ++speed)
		{
			const auto input_sequence = movement::generate(movement::preset::wavedash, speed);
			const auto passes = movement::wavedash_passes(speed).count;
			const auto read = feed(repeat(input_sequence.frames, 14 / passes), side);
			check(read.size() == 1 && read[0].what == streaks::kind::wavedash, "each speed is one wavedash streak");
			const double expected = static_cast<double>(speed);
			check(read[0].per_second > expected - .15 && read[0].per_second < expected + .15, "each speed's pace reads back");
		}
		// f, n, d/f without the down step is a crouch dash too; one without the neutral (f, d, d/f) is not.
		check(feed(repeat(expand({{f, 3}, {n, 2}, {df, 2}, {n, 6}}), 5), side).at(0).count == 5, "f, n, d/f counts");
		check(feed(repeat(expand({{f, 3}, {d, 2}, {df, 2}, {n, 6}}), 5), side).empty(), "f, d, d/f is not a crouch dash");
		// An electric (f, n, d, d/f+2) is an attack: three in a row are no wavedash, and one ends a run as its last move.
		const words electric = expand({{f, 2}, {n, 1}, {d, 1}, {static_cast<std::uint16_t>(df | two), 1}, {n, 20}});
		check(feed(repeat(electric, 3), side).empty(), "electrics are no wavedash");
		words into_electric = repeat(relaxed, 3);
		into_electric.insert(into_electric.end(), electric.begin(), electric.end());
		const auto ended = feed(into_electric, side);
		check(ended.size() == 1 && ended[0].count == 3, "a wavedash into an electric counts the crouch dashes only");
		// Two crouch dashes are not a streak; a long pause splits a run in two.
		check(feed(repeat(relaxed, 2), side).empty(), "two in a row are not shown");
		words paused = repeat(relaxed, 4);
		paused.insert(paused.end(), 60, n);
		const auto after = repeat(relaxed, 3);
		paused.insert(paused.end(), after.begin(), after.end());
		const auto split = feed(paused, side);
		check(split.size() == 2 && split[0].count == 4 && split[1].count == 3, "a pause longer than the link ends the run");
	}
}

PBD_TEST(PbdPracticeStreak, BackdashCancel)
{
	for (const auto side : {movement::screen_side::left, movement::screen_side::right})
	{
		// The Practice tape: b 1, n 1, then b 10, d/b 1, b 1, n 1 twelve times: 12 backdashes, one every 13 frames.
		const auto input_sequence = movement::generate(movement::preset::backdash_cancel);
		words stream(input_sequence.frames.begin(), input_sequence.frames.begin() + input_sequence.loop_start);
		const words loop(input_sequence.frames.begin() + input_sequence.loop_start, input_sequence.frames.end());
		const auto loops = repeat(loop, 12);
		stream.insert(stream.end(), loops.begin(), loops.end());
		const auto seen = feed(stream, side);
		check(seen.size() == 1 && seen[0].what == streaks::kind::backdash_cancel && seen[0].count == 12, "12 cancelled backdashes");
		check(streaks::text(seen[0]) == "BACKDASH CANCEL x12 - 4.62/s	Backdash 10f, cancel 3f (perfect 3f), 11 of 11 perfect",
			"with the pace of one every 13 frames, each back held 10 frames and every cancel perfect");
		check(seen[0].cancels == 11 && seen[0].perfect_cancels == 11, "the last cancel leads into no backdash and is not measured");
		// A slower player's loop, b 9, d/b 6, b 3, n 7 (25 frames): 2.4 a second.
		words slower = expand({{b, 2}, {n, 2}});
		const auto slow_loops = repeat(expand({{b, 9}, {db, 6}, {b, 3}, {n, 7}}), 10);
		slower.insert(slower.end(), slow_loops.begin(), slow_loops.end());
		const auto slow = feed(slower, side);
		check(slow.size() == 1 && slow[0].count == 10 &&
				  streaks::text(slow[0]) == "BACKDASH CANCEL x10 - 2.4/s	Backdash 9f, cancel 16f (perfect 3f), 0 of 9 perfect",
			"a slower loop reads 2.4/s with 16-frame cancels (d/b 6, b 3, n 7)");
		// Mixed cancels: two perfect (d/b 1, b 1, n 1) and two a frame slow (d/b 2): 3.5 frames on average.
		words mixed = expand({{b, 1}, {n, 1}});
		const auto cycle = [&](std::uint32_t cancel_db)
		{
			const auto part = expand({{b, 8}, {db, cancel_db}, {b, 1}, {n, 1}});
			mixed.insert(mixed.end(), part.begin(), part.end());
		};
		cycle(1);
		cycle(2);
		cycle(1);
		cycle(2);
		const auto tail = expand({{b, 8}, {db, 1}});
		mixed.insert(mixed.end(), tail.begin(), tail.end());
		const auto blend = feed(mixed, side);
		check(blend.size() == 1 && blend[0].count == 5 && blend[0].cancels == 4 && blend[0].perfect_cancels == 2 &&
				  blend[0].cancel_frames > 3.49 && blend[0].cancel_frames < 3.51 && blend[0].backdash_frames > 7.99 && blend[0].backdash_frames < 8.01,
			"two perfect and two four-frame cancels average 3.5 frames, each back held 8");
		// Backdashes that are not cancelled, and crouching after one backdash, are not a streak.
		check(feed(repeat(expand({{b, 2}, {n, 2}, {b, 30}, {n, 10}}), 5), side).empty(), "plain backdashes are not cancels");
		check(feed(expand({{b, 2}, {n, 2}, {b, 8}, {db, 40}}), side).empty(), "one backdash into a crouch is not a streak");

		// Each cancel of a run also shows on its own: every one of the Practice tape's is frame-perfect, the slower loop's
		// d/b 6 is five frames late, and the mixed run alternates Perfect and Great; its last d/b leads into no b.
		const auto script_singles = singles(stream, side);
		check(script_singles.size() == 12 && streaks::text(script_singles[0]) == "BACKDASH CANCEL - PERFECT" &&
				  streaks::text(script_singles[11]) == "BACKDASH CANCEL - PERFECT",
			"each of the tape's 12 cancels is frame-perfect");
		const auto slow_singles = singles(slower, side);
		check(slow_singles.size() == 10 && streaks::text(slow_singles[0]) == "BACKDASH CANCEL - SLOW	5 frames late", "d/b held 6 frames is slow");
		const auto mixed_singles = singles(mixed, side);
		check(mixed_singles.size() == 4 && streaks::text(mixed_singles[0]) == "BACKDASH CANCEL - PERFECT" &&
				  streaks::text(mixed_singles[1]) == "BACKDASH CANCEL - GREAT	1 frame late",
			"one frame late is great, and a d/b with no b after it is no cancel");
		check(singles(repeat(expand({{b, 2}, {n, 2}, {b, 30}, {n, 10}}), 5), side).empty(), "plain backdashes are no single cancels");
	}
}

PBD_TEST(PbdPracticeStreak, SingleCancel)
{
	for (const auto side : {movement::screen_side::left, movement::screen_side::right})
	{
		// One backdash cancel, b, n, b, d/b, b: shown right away, graded from its d/b to the b after it.
		const auto once = [&](std::initializer_list<run> after)
		{
			words stream = expand({{b, 1}, {n, 1}, {b, 10}});
			const auto rest = expand(after);
			stream.insert(stream.end(), rest.begin(), rest.end());
			return stream;
		};
		const auto perfect = singles(once({{db, 1}, {b, 1}}), side);
		check(perfect.size() == 1 && perfect[0].what == streaks::kind::single_cancel && perfect[0].cancel_frames == 1.0 &&
				  streaks::text(perfect[0]) == "BACKDASH CANCEL - PERFECT",
			"a single frame-perfect cancel shows with no detail");
		check(feed(once({{db, 1}, {b, 1}}), side).empty(), "a single cancel is not a run");
		check(streaks::text(singles(once({{db, 3}, {b, 1}}), side).at(0)) == "BACKDASH CANCEL - GOOD	2 frames late", "d/b 3 frames is good");
		check(streaks::text(singles(once({{db, 1}, {n, 3}, {b, 1}}), side).at(0)) == "BACKDASH CANCEL - FAIR	3 frames late",
			"neutral between the d/b and the b counts");
		check(singles(once({{db, 1}, {d, 1}, {b, 1}}), side).empty(), "anything but neutral between them drops it");
		check(singles(once({{db, 17}, {b, 1}}), side).empty(), "a b too long after the d/b is no cancel");
	}
	using streaks::grade;
	check(streaks::grade_of(1) == grade::perfect && streaks::grade_of(2) == grade::great && streaks::grade_of(3) == grade::good &&
			  streaks::grade_of(4) == grade::fair && streaks::grade_of(5) == grade::slow && streaks::grade_of(16) == grade::slow,
		"one grade a frame: Perfect, Great, Good, Fair, then Slow");
}

PBD_TEST(PbdPracticeStreak, NothingElse)
{
	// Walking, jumping and the dummy's other movements are no streak.
	const auto side = movement::screen_side::left;
	check(feed(expand({{f, 300}}), side).empty() && feed(expand({{b, 300}}), side).empty() && feed(repeat(expand({{u, 10}, {n, 45}}), 6), side).empty(),
		"walking and jumping are no streak");
	check(feed(repeat(expand({{f, 4}, {n, 4}}), 30), side).empty(), "forward dashes are no wavedash");
	streaks::tracker tracker;
	for (int i = 0; i < 5; ++i)
		for (const auto w : expand({{f, 4}, {n, 2}, {d, 3}, {df, 1}, {f, 2}, {n, 3}}))
			tracker.frame(w);
	tracker.reset();
	bool any = false;
	for (int i = 0; i < 200; ++i)
		any |= tracker.frame(n).has_value();
	check(!any, "a restart drops the run in progress");
	check(streaks::text({streaks::kind::wavedash, 7, 3.0}) == "WAVEDASH x7 - 3/s", "a whole pace shows no decimals");
}
