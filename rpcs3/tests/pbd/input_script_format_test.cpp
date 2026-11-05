#include "Emu/PBD/pbd_input_script_format.h"
#include "Emu/Cell/Modules/pbd_input_script.h"
#include "pbd_test.h"
#include "Emu/PBD/pbd_fight_tape.h"

PBD_TEST(InputScriptFormat, StoredHeaders)
{
	using pbd::input_script::accepts_header;
	pbd::test::check(accepts_header("pbd-tape-1"));
	pbd::test::check(accepts_header("DR2026_SESSION_REPLAY 1"));
	for (const auto value : {"", "pbd-tape-2", "PBD-TAPE-1", "pbd-tape-1 ", " pbd-tape-1",
			 "DR2026_SESSION_REPLAY 2", "DR2026_UPDATE_TAPE 1", "DR2026-PAD-TAPE 1", "pbd-tape-1\n"})
		pbd::test::check(!accepts_header(value));
}

PBD_TEST(InputScriptFormat, PadScriptSuppliesOnlyItsOwnedPeerPort)
{
	using pbd::test::check;
	check(!pbd::pad_input_script{}.supplies_only_port(0));
	for (const unsigned port : {0u, 1u})
	{
		const auto id = std::to_string(port);
		auto input_sequence = pbd::pad_input_script::parse("DR2026-PAD-TAPE 1\n" + id + " 0 0 64 128 128 128 128\n" + id + " 2 0 0 128 128 128 128\n");
		check(input_sequence.supplies_only_port(port) && !input_sequence.supplies_only_port(port ^ 1) && !input_sequence.supplies_only_port(2));
		const auto pressed = input_sequence.next(port);
		check(pressed && pressed->poll == 0 && pressed->sample.digital2 == 64 && !pressed->final);
		check(input_sequence.next(port)->sample.digital2 == 64);
		const auto released = input_sequence.next(port);
		check(released && released->poll == 2 && released->sample.neutral() && released->final);
		check(!input_sequence.next(port) && input_sequence.supplies_only_port(port));
	}
	const auto both = pbd::pad_input_script::parse("DR2026-PAD-TAPE 1\n0 0 0 0 128 128 128 128\n1 0 0 0 128 128 128 128\n");
	check(!both.supplies_only_port(0) && !both.supplies_only_port(1));
}

PBD_TEST(InputScriptFormat, LegacyReplayReadsCompleteOrderedInputs)
{
	using pbd::test::check;
	for (const auto header : pbd::input_script::headers)
	{
		const auto parsed = pbd::input_script::parse_legacy(std::string(header) +
															"\r\n# recorded changes\r\nsync 1 4294967295\r\nseg 1 0 65535 0\r\nseg 1 3 0 65535\r\nseg 4294967295 0 1 2\r\n");
		check(!parsed.error && parsed.sync.at(1) == 0xffffffffu && parsed.inputs.size() == 2);
		check(parsed.inputs.at(1) == std::vector<std::array<std::uint32_t, 3>>{{0, 65535, 0}, {3, 0, 65535}});
	}
}

PBD_TEST(InputScriptFormat, LegacyReplayRejectsMalformedOrUnorderedRows)
{
	using pbd::test::check;
	for (const auto row : {"seg 1", "seg 1 0 0", "sync 1", "sync 1 0 1", "unknown 1 0", "seg 4294967297 0 0 0",
			 "seg 1 4294967296 0 0", "seg 1 0 65536 0", "seg 1 0 0 65536", "seg 1 0 0 0 junk", "seg 1 0 0 0 5 6",
			 "seg -1 0 0 0", "seg 1 0 0 1junk", "sync 1 4294967296", "sync 1 -1", "sync 1 0\nsync 1 1",
			 "seg 1 10 0 0\nseg 1 9 0 0", "seg 1 10 0 0\nseg 1 10 0 0"})
	{
		const auto parsed = pbd::input_script::parse_legacy(std::string("pbd-tape-1\n") + row + '\n');
		check(parsed.error && parsed.line >= 2);
	}
	check(pbd::input_script::parse_legacy("pbd-tape-1\nsync 1 0\n").error);
}

PBD_TEST(InputScriptFormat, FightFramesRepeatWithoutConsumingPolls)
{
	using pbd::test::check;
	auto tape = pbd::pad_input_script::parse("DR2026-PAD-TAPE 1\r\nanchor fight\r\n# frame changes\r\n"
											 "1 0 32 64 128 128 128 128\n1 10 32 0 128 128 128 128\n1 880 0 0 128 128 128 128\n");
	check(tape.fight_anchored() && tape.supplies_only_port(1));
	check(!tape.next(1) && !tape.at_frame(0, 0) && !tape.at_frame(2, 0));
	for (unsigned poll = 0; poll < 100; ++poll)
	{
		const auto first = tape.at_frame(1, 0);
		check(first && first->poll == 0 && first->sample.digital1 == 32 && first->sample.digital2 == 64 && !first->final);
	}
	check(tape.at_frame(1, 9)->sample.digital2 == 64);
	check(tape.at_frame(1, 10)->sample.digital2 == 0);
	check(tape.at_frame(1, 879)->sample.digital1 == 32);
	for (unsigned poll = 0; poll < 3; ++poll)
		check(tape.at_frame(1, 880)->final && tape.at_frame(1, 880)->sample.neutral());
	check(!tape.at_frame(1, 881));
	// Looking up an earlier rollback frame never depends on the newest live lookup.
	check(tape.at_frame(1, 0)->sample.digital2 == 64);
}

PBD_TEST(InputScriptFormat, FightClockUsesRoundInitializerAndAppliedDelay)
{
	using pbd::test::check;
	for (std::uint32_t delay = 0; delay <= 6; ++delay)
	{
		pbd::fight_tape_clock clock;
		check(!clock.frame(0) && !clock.poll_frame());
		clock.observe(10, 3, 4, 0, delay);
		clock.observe(20, 17, 5, 0, delay);
		clock.observe(30, 16, 4, 1, delay);
		check(!clock.frame(100) && !clock.poll_frame());
		clock.observe(100, 17, 4, 0, delay);
		check(!clock.frame(100) && clock.frame(101) == 0);
		check(delay ? clock.poll_frame() == delay - 1 : !clock.poll_frame());
		for (unsigned poll = 0; poll < 10; ++poll)
			check(delay ? clock.poll_frame() == delay - 1 : !clock.poll_frame());
		clock.observe(120, 16, 5, 1, delay);
		check(clock.frame(120) == 19 && clock.poll_frame() == 19 + delay);
		clock.observe(140, 17, 4, 0, delay);
		check(clock.frame(140) == 39);
	}
}

PBD_TEST(InputScriptFormat, FightClockIgnoresDifferentLoadLengthsAndRoundCounterResets)
{
	using pbd::test::check;
	pbd::fight_tape_clock left, right;
	left.observe(100, 16, 4, 0, 2);
	right.observe(900, 16, 4, 0, 2);
	for (std::uint32_t frame = 0; frame < 1000; ++frame)
	{
		left.observe(101 + frame, 17, frame == 600 ? 5 : 0, frame >= 600 ? 1 : 0, 2);
		right.observe(901 + frame, 17, frame == 600 ? 5 : 0, frame >= 600 ? 1 : 0, 2);
		check(left.frame(101 + frame) == frame && right.frame(901 + frame) == frame);
		check(left.poll_frame() == right.poll_frame());
	}
}

PBD_TEST(InputScriptFormat, FightAnchorRetainsStrictFormatAndBounds)
{
	using pbd::test::check;
	const auto refuses = [](const std::string& text)
	{
		try
		{
			(void)pbd::pad_input_script::parse(text);
		}
		catch (const pbd::input_script_error&)
		{
			return true;
		}
		return false;
	};
	const std::string header = "DR2026-PAD-TAPE 1\nanchor fight\n";
	for (const auto row : {"", "0 1 0 0 128 128 128 128\n", "0 0 0 64 128 128 128 128\n",
			 "2 0 0 0 128 128 128 128\n", "0 0 256 0 128 128 128 128\n",
			 "0 0 0 0 128 128 128 128\n0 0 0 0 128 128 128 128\n",
			 "0 0 0 0 128 128 128 128\n0 10000001 0 0 128 128 128 128\n",
			 "0 0 0 0 128 128 128 128\n1 0 0 0 128 128 128 128\n"})
		check(refuses(header + row));
	check(refuses("DR2026-PAD-TAPE 1\n# misplaced anchor\nanchor fight\n0 0 0 0 128 128 128 128\n"));
	check(refuses("DR2026-PAD-TAPE 1\nanchor fight extra\n0 0 0 0 128 128 128 128\n"));
	check(refuses(header + std::string(pbd::pad_input_script::max_bytes, '#')));
	std::string many = header;
	for (unsigned i = 0; i <= pbd::pad_input_script::max_events; ++i)
		many += "0 " + std::to_string(i) + " 0 0 128 128 128 128\n";
	check(refuses(many));
	const auto limit = pbd::pad_input_script::parse(header + "0 0 0 64 128 128 128 128\n0 10000000 0 0 128 128 128 128\n");
	check(limit.at_frame(0, 10000000)->final && !limit.at_frame(0, 10000001));
}

PBD_TEST(InputScriptFormat, FightFrameZeroNamesTheRecordedRoundInputOffset)
{
	using pbd::test::check;
	constexpr std::uint32_t match_start = 20, round_initializer = 120;
	constexpr auto ready_input_offset = round_initializer - match_start;
	constexpr auto first_recorded_boundary = match_start + 1;
	pbd::fight_tape_clock clock;
	clock.observe(round_initializer, 16, 4, 0, 3);
	check(!clock.frame(round_initializer));
	check(clock.frame(first_recorded_boundary + ready_input_offset) == 0);
	const auto tape = pbd::pad_input_script::parse("DR2026-PAD-TAPE 1\nanchor fight\n"
												   "0 0 32 64 128 128 128 128\n0 2 0 0 128 128 128 128\n");
	for (std::uint32_t delay = 0; delay <= 6; ++delay)
	{
		clock.observe(round_initializer + 1, 17, 0, 0, delay);
		const auto applied = clock.frame(round_initializer + 1 + delay);
		check(applied == delay && clock.poll_frame() == applied);
		const auto sample = tape.at_frame(0, *applied);
		check(delay > 2 ? !sample : sample.has_value());
		if (delay < 2)
			check(sample->sample.digital2 == 64);
		if (delay == 2)
			check(sample->final && sample->sample.neutral());
	}
}
