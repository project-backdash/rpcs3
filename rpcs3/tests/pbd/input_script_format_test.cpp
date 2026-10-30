#include "Emu/PBD/pbd_input_script_format.h"
#include "Emu/Cell/Modules/pbd_input_script.h"
#include "pbd_test.h"

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
