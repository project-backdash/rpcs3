#include "pbd_test.h"

#include "Emu/PBD/pbd_session_start.h"
#include "Emu/PBD/pbd_json_reader.h"

#include <array>

namespace start = pbd::session_start;
using pbd::test::check;

PBD_TEST(PbdSessionStart, OnlineEntryCounterIsOnlyAWitnessAfterTheVerifiedStart)
{
	using namespace pbd::session_start;
	::pbd::test::check(entry_witness_recovery(true, false, true));
	::pbd::test::check(!entry_witness_recovery(true, false, false));
	::pbd::test::check(!entry_witness_recovery(false, false, true));
	::pbd::test::check(entry_witness_recovery(false, true, true));
	for (const std::uint32_t actual : {0u, 7u, 0x7fffffffu, 0xffffffffu})
	{
		const auto offset = counter_offset(actual, 122, 56443);
		::pbd::test::check(122u + 56443u + static_cast<std::uint32_t>(offset) == actual);
	}
}

PBD_TEST(PbdSessionStart, InputCounterCanRestartTwiceInOneBoundary)
{
	using pbd::session_start::input_counter_restarted;
	::pbd::test::check(input_counter_restarted(0, 0, true));
	::pbd::test::check(input_counter_restarted(10, 0, true));
	::pbd::test::check(input_counter_restarted(10, 1, true));
	::pbd::test::check(input_counter_restarted(10, 0, false));
	::pbd::test::check(!input_counter_restarted(0, 1, true));
	::pbd::test::check(!input_counter_restarted(10, 11, true));
	::pbd::test::check(!input_counter_restarted(10, 10, false));
	::pbd::test::check(!input_counter_restarted(10, 7, true));
}

PBD_TEST(PbdSessionStart, PresentationButtons)
{
	for (const auto scene : {16u, 17u})
	{
		for (const auto pending : {0u, 1u, 2u, 9u, 10u, 11u, 12u})
		{
			const auto input = start::filter(0x0840, {scene, pending, true}, false);
			check(input.word == 0x0840 && !input.suppressed, "A fresh presentation press retains Start and Cross");
		}
		for (const auto pending : {3u, 4u, 5u, 6u, 7u, 8u, 13u, 32u, 33u, 34u, 35u, 37u, ~0u})
		{
			const auto input = start::filter(0x0840, {scene, pending, true}, false);
			check(input.word == 0x0040 && input.suppressed, "Fight, KO, round calls and unknown phases remove only Start");
		}
	}
	check(start::filter(0x0800, {17, 5, true}, false).suppressed,
		"The first upcoming fight update blocks Start even before its battle counter advances");
	check(!start::filter(0x0800, {17, 1, true}, false).suppressed,
		"An upcoming intro accepts Start independently of the completed update's counter");
}

PBD_TEST(PbdSessionStart, RoundReplayButtons)
{
	bool suppressed = start::filter(0x0800, {17, 8, true}, false).suppressed;
	check(start::filter(0x0800, {17, 9, true}, suppressed).word == 0,
		"A Start held through KO setup stays withheld during its replay");
	suppressed = start::filter(0, {17, 9, true}, suppressed).suppressed;
	check(start::filter(0x0840, {17, 9, true}, suppressed).word == 0x0840,
		"Either port can send a fresh Start to the stock round replay without altering attacks");
	check(start::filter(0x0800, {17, 4, true}, false).word == 0,
		"Replay Start is removed when the upcoming dispatcher returns to the round call");
	check(start::filter(0x0800, {17, 5, true}, false).word == 0,
		"Replay Start is removed when the upcoming dispatcher returns to fighting");
	check(start::filter(0x0800, {17, 9, false}, false).word == 0,
		"An unreadable round replay phase cannot permit Start");
}

PBD_TEST(PbdSessionStart, ReleaseAndPorts)
{
	std::array<bool, 2> suppressed{};
	const auto apply = [&](unsigned port, std::uint16_t word, const start::phase& next)
	{
		const auto input = start::filter(word, next, suppressed[port]);
		suppressed[port] = input.suppressed;
		return input.word;
	};
	check(apply(0, 0x0800, {17, 5, true}) == 0, "Left fight Start is blocked");
	check(apply(0, 0x0800, {17, 10, true}) == 0, "A held fight Start cannot become an unrequested win skip");
	check(apply(1, 0x0800, {17, 10, true}) == 0x0800, "Right fresh Start skips independently");
	check(apply(0, 0x0040, {17, 10, true}) == 0x0040, "Releasing Start preserves other buttons");
	check(apply(0, 0x0800, {17, 10, true}) == 0x0800, "A fresh win Start after release reaches the game");
	check(apply(0, 0x0800, {17, 5, true}) == 0, "Held intro Start is removed at the first fight dispatch");
	check(apply(0, 0, {17, 5, true}) == 0 && !suppressed[0], "A neutral fight input clears the latch");
	check(apply(0, 0x0800, {17, 2, true}) == 0x0800, "A fresh later intro Start is permitted");
}

PBD_TEST(PbdSessionStart, UnknownAndReexecution)
{
	for (const auto scene : {0u, 3u, 8u, 12u, 44u, ~0u})
		check(start::filter(0x0800, {scene, 1, true}, false).suppressed, "An unrelated scene cannot open Start");
	check(start::filter(0x0800, {17, 1, false}, false).suppressed, "An unreadable phase blocks Start");
	for (unsigned word = 0; word <= 0xffff; ++word)
	{
		const auto first = start::filter(static_cast<std::uint16_t>(word), {17, 5, true}, false);
		check(first.word == (word & ~start::start_button), "Fight filtering preserves every unrelated input bit");
	}
}

PBD_TEST(PbdSessionStart, ConfirmedDecisions)
{
	using pbd::json::detail::boolean;
	using pbd::json::detail::json_parser;
	using pbd::json::detail::number32;
	std::uint32_t boundary = 0, port = 0, pending = 0;
	bool masked = false, verified = false, confirmed = false;
	start::confirmed_input value{42, 1, 0x0800, 0x0800, {17, 10, true}};
	auto decision = json_parser(start::describe(value)).parse();
	check(decision.value, "The Start decision is valid JSON");
	const auto* type = decision.value->find("type");
	check(type && type->kind == pbd::json::json_kind::string && type->text == "start_skip");
	check(number32(decision.value->find("boundary"), 0, ~0u, boundary) && boundary == 42,
		"The journal binds the actual win input to its confirmed boundary");
	check(number32(decision.value->find("port"), 0, 1, port) && port == 1);
	check(boolean(decision.value->find("masked"), masked) && !masked);
	check(number32(decision.value->find("phase"), 0, ~0u, pending) && pending == 10);
	check(boolean(decision.value->find("phase_read"), verified) && verified);
	check(boolean(decision.value->find("confirmed"), confirmed) && confirmed);
	value.applied = 0;
	decision = json_parser(start::describe(value)).parse();
	check(decision.value);
	check(boolean(decision.value->find("masked"), masked) && masked,
		"A held Start removed in a permitted phase is reported from the game-seen word");
	value.next.pending = 9;
	value.applied = 0x0800;
	decision = json_parser(start::describe(value)).parse();
	check(decision.value);
	check(boolean(decision.value->find("masked"), masked) && !masked &&
			  number32(decision.value->find("phase"), 0, ~0u, pending) && pending == 9,
		"The confirmed journal reports the actual stock round replay press on the same input path");
	value.raw = value.applied = 0;
	check(start::describe(value).empty(), "A predicted press withdrawn by confirmed input produces no decision");
	value.raw = value.applied = 0x0040;
	check(start::describe(value).empty(), "An unrelated button produces no Start decision");
	value = {~0u, 0, 0x0800, 0, {17, ~0u, false}};
	decision = json_parser(start::describe(value)).parse();
	check(decision.value);
	check(number32(decision.value->find("boundary"), 0, ~0u, boundary) && boundary == ~0u,
		"The journal retains the full unsigned boundary identity");
	check(number32(decision.value->find("phase"), 0, ~0u, pending) && pending == ~0u &&
			  boolean(decision.value->find("phase_read"), verified) && !verified,
		"An unreadable native phase remains explicitly unverified");
}
