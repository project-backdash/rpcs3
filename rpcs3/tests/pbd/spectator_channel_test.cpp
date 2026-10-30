// Mirror the native latch's include order: both headers add names to namespace pbd.
#include "pbd_test.h"

#include "Emu/Cell/pbd_update_script.h"
#include "Emu/PBD/pbd_json_reader.h"
#include "Emu/PBD/pbd_spectator_channel_input.h"
#include <iterator>
#include <optional>

#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#include <share.h>
#endif

using pbd::test::check;
using namespace pbd::json::detail;

#include "spectator_channel_model.h"
// namespace

PBD_TEST(PbdSpectatorChannel, InitialResourceLoadingStaysHidden)
{
	auto m = identity();
	m.direct_start = true;
	sample_document f(310, false, m);
	f.prepare();
	f.begin();
	check(load_presentation(9).hidden);
}

// A game's start may name the values the players' games wrote for the game's own Ranked screens; the watcher writes the same.
// Display only: a malformed value shows none rather than ending the watch.
PBD_TEST(PbdSpectatorChannel, BeginGameCarriesTheRankScreens)
{
	sample_document f(311);
	f.prepare();
	check(!f.input.rank_screens());
	f.begin(1, 0, nullptr, true, fixed_picks + R"("rankScreens":{"stakeLeft":1,"stakeRight":2,"promoteLeft":11,"promoteRight":-1},)");
	pbd::rank_stakes::game_values written;
	written.stake = {1, 2};
	written.promote = {11, -1};
	check(f.input.rank_screens() == written);
	sample_document odd(312);
	odd.prepare();
	odd.begin(1, 0, nullptr, true, fixed_picks + R"("rankScreens":{"stakeLeft":7,"stakeRight":0,"promoteLeft":-1,"promoteRight":-1},)");
	check(odd.input.healthy() && !odd.input.rank_screens());
}

PBD_TEST(PbdSpectatorChannel, CapsuleStartKeepsOriginalCoordinatesAndChecksEveryExecutedInput)
{
	for (const u32 game : {2u, 19u})
		for (const u32 origin : {50u, 70000u, 0xffffff80u})
		{
			auto m = identity();
			m.pbd_capsule = "fixture";
			m.pbd_capsule_game = game;
			m.pbd_source_game = game;
			m.pbd_first_frame = 7;
			m.pbd_source_origin = origin + 56;
			m.pbd_game_origin = origin;
			m.pbd_source_segment = 1;
			sample_document f(300 + game, false, m);
			f.prepare();
			check(f.input.pbd_set_native_anchor(5));
			check(f.input.pbd_set_native_anchor(5));
			check(f.input.pbd_recorded_boundary(5) == origin + 57);
			check(f.input.pbd_native_boundary(origin + 57) == 5);
			f.control("\"type\":\"begin_game\",\"gameNumber\":" + std::to_string(game) + ",\"originBoundary\":" + std::to_string(origin) + ",\"originCounter\":2147483825,"
																																		   "\"kind\":\"capsule_start\"," +
					  fixed_picks + "\"roundsPerGame\":3");
			check(load_presentation(9).native_through == 0);
			f.packet(1, game, 7, origin + 57);
			check(f.input.poll_input(test_hash));
			for (u32 i = 0; i < 2; ++i)
			{
				const auto frame = f.input.reserve();
				check(frame && frame->boundary == origin + 57 + i);
				check(f.input.verify_state(5 + i, 1, 17, 0x8000004d + origin + 57 + i, 7, {11, 22, 33}));
				check(f.input.commit(0x01000000, 0x12efc, 5 + i, f.completed(), {0x40, 0x20}));
			}
			const auto report = json_parser(f.report(4)).parse();
			u32 boundary = 0;
			check(f.input.consumed() == 9 && report.value && number32(report.value->find("boundaryThrough"), origin + 59, origin + 59, boundary));
		}
}

PBD_TEST(PbdSpectatorChannel, CapsuleBoundaryMappingRefusesOverflow)
{
	auto m = identity();
	m.pbd_capsule = "fixture";
	m.pbd_source_origin = 106;
	sample_document f(303, false, m);
	f.prepare();
	check(f.input.pbd_set_native_anchor(70000));
	check(f.input.pbd_recorded_boundary(70000) == 107);
	check(f.input.pbd_native_boundary(107) == 70000);
	check(f.input.pbd_recorded_boundary(69999) == ~0u);
	check(f.input.pbd_native_boundary(106) == ~0u);
	check(f.input.pbd_native_boundary(maximum_boundary) == ~0u);
	check(f.input.pbd_native_boundary(~0u) == ~0u);
	m.pbd_source_origin = maximum_boundary - 1;
	sample_document near_end(304, false, m);
	near_end.prepare();
	check(near_end.input.pbd_set_native_anchor(5));
	check(near_end.input.pbd_recorded_boundary(5) == maximum_boundary);
	check(near_end.input.pbd_recorded_boundary(6) == ~0u);
	check(near_end.input.pbd_native_boundary(maximum_boundary) == 5);
}

PBD_TEST(PbdSpectatorChannel, CapsuleStartCannotChangeItsAnchorOrSkipARequiredHash)
{
	auto m = identity();
	m.pbd_capsule = "fixture";
	m.pbd_capsule_game = 3;
	m.pbd_first_frame = 7;
	m.pbd_source_origin = 106;
	m.pbd_game_origin = 100;
	{
		sample_document f(301, false, m);
		f.prepare();
		check(f.input.pbd_set_native_anchor(5));
		check(!f.input.pbd_set_native_anchor(6) && f.input.reason() == "capsule_native_anchor_changed");
	}
	{
		sample_document f(302, false, m);
		f.prepare();
		check(f.input.pbd_set_native_anchor(5));
		f.control("\"type\":\"begin_game\",\"gameNumber\":3,\"originBoundary\":100,\"originCounter\":2147483825,"
				  "\"kind\":\"capsule_start\"," +
				  fixed_picks + "\"roundsPerGame\":3");
		f.packet(1, 3, 7, 107);
		check(f.input.poll_input(test_hash) && f.input.reserve());
		check(!f.input.verify_state(5, 1, 17, 0x8000004d + 107, 7, {11, 99, 33}));
		check(f.input.consumed() == 7 && load_presentation(9).native_through == 0);
	}
}

#ifdef _WIN32
namespace
{
	class pbd_packet_lock
	{
		int m_handle = -1;

	public:
		explicit pbd_packet_lock(const std::filesystem::path& path)
		{
			check(_wsopen_s(&m_handle, path.c_str(), _O_RDWR | _O_BINARY, _SH_DENYRW, 0) == 0);
		}
		~pbd_packet_lock()
		{
			if (m_handle != -1)
				_close(m_handle);
		}
		pbd_packet_lock(const pbd_packet_lock&) = delete;
		pbd_packet_lock& operator=(const pbd_packet_lock&) = delete;
	};
} // namespace

PBD_TEST(PbdSpectatorChannel, ABriefInputLockKeepsTheReceiptUntilThePacketCanBeChecked)
{
	sample_document f(240);
	f.prepare();
	f.begin();
	f.packet(1, 1, 0, 0);
	const std::chrono::steady_clock::time_point start{};
	{
		pbd_packet_lock locked(f.directory / "spectator-input-1.bin");
		check(f.input.poll_input(test_hash, start));
		check(f.input.poll_input(test_hash, start + std::chrono::milliseconds(999)));
		check(f.input.healthy() && f.input.consumed() == 0 && !f.input.reserve());
	}
	check(f.input.poll_input(test_hash, start + std::chrono::seconds(1)));
	f.play(0);
	f.play(1);
	check(f.input.healthy() && f.input.consumed() == 2);
}

PBD_TEST(PbdSpectatorChannel, AnInputLockThatPersistsForASecondRefusesWithoutConsuming)
{
	sample_document f(241);
	f.prepare();
	f.begin();
	f.packet(1, 1, 0, 0);
	const std::chrono::steady_clock::time_point start{};
	pbd_packet_lock locked(f.directory / "spectator-input-1.bin");
	check(f.input.poll_input(test_hash, start));
	check(!f.input.poll_input(test_hash, start + std::chrono::seconds(1)));
	check(f.input.reason() == "input_read_failed" && f.input.consumed() == 0 && !f.input.reserve());
}
#endif

static void countdown_packets_and_sync_facts()
{
	{
		// Two 99 s countdowns and a 3,600-frame margin: bounded packets, not one giant start record.
		constexpr u32 intro = 15480;
		sample_document f(101);
		f.prepare();
		f.begin();
		u32 serial = 0;
		for (u32 first = 0; first <= intro;)
		{
			const u32 count = std::min<u32>(static_cast<u32>(packet_frames), intro + 1 - first);
			f.packet(++serial, 1, first, first, count);
			check(f.input.poll_input(test_hash));
			for (u32 i = 0; i < count; ++i)
			{
				if (first + i < intro)
					check(!f.input.visible());
				f.play(first + i, first + i == intro);
			}
			first += count;
			check(f.input.healthy() && f.input.consumed() == first);
		}
		f.control("\"type\":\"end_game\",\"gameNumber\":1,\"finalFrame\":" + std::to_string(intro + 1));
		check(f.input.end(intro + 1, true));
	}
	{
		sample_document f(102);
		f.prepare();
		f.begin();
		f.packet(1, 1, 0, ~0u, 1);
		check(!f.input.poll_input(test_hash) && f.input.reason() == "sync_facts_invalid");
	}
	{
		sample_document f(100);
		const auto session = [&](const std::string& keyed, const std::string& ban = "", const std::string& direct = "")
		{
			const std::string policy = "\"sessionStartSkip\":true,\"exitGate\":true,\"phaseDelay\":false,\"battleClockVersion\":2,\"menuHashMask\":0" +
			                           (keyed.empty() ? "" : ",\"keyedDraws\":" + keyed) +
			                           (ban.empty() ? "" : ",\"banJinpachi\":" + ban) +
			                           (direct.empty() ? "" : ",\"directStart\":" + direct);
			f.write("spectator-session.json", "{\"schema\":\"pbd-spectator-session-1\",\"generation\":\"123\",\"bindingDigest\":\"" + hex(identity().binding) +
												  "\",\"build\":\"test-build\",\"compat\":\"test-compat\",\"titleId\":\"NPUB30009\",\"emulatorSha256\":\"" + identity().emulator +
												  "\",\"checkpointSha256\":\"" + identity().checkpoint + "\",\"recipeSha256\":\"" + identity().recipe +
												  "\",\"seed\":77,\"stageId\":17,\"leftCharacterRow\":12,\"rightCharacterRow\":32,\"roundsPerGame\":3,"
												  "\"syncEncoding\":\"pbd-sync-facts-v1\",\"syncDefinition\":\"pbd-peer-sync-v3\",\"appliedEncoding\":\"pbd-applied-pad-v1\",\"inputPolicy\":{" +
												  policy + "}}");
			return read_manifest(f.directory);
		};
		for (const auto& keyed : {std::string("false"), std::string("true")})
		{
			const auto parsed = session(keyed);
			check(parsed.value && parsed.value->policy_present);
			const bool actual = keyed == "true";
			check(parsed.value->keyed_draws == actual && keyed_policy_matches(*parsed.value, actual));
			check(!keyed_policy_matches(*parsed.value, !actual));
		}
		for (const auto& keyed : {std::string(), std::string("0"), std::string("\"false\"")})
			check(session(keyed).error == "spectator_input_policy_invalid");
		check(session("false", "true").value->ban_jinpachi);
		check(!session("false", "false").value->ban_jinpachi);
		for (const auto& ban : {std::string("null"), std::string("1"), std::string("\"true\"")})
			check(session("false", ban).error == "spectator_input_policy_invalid");
		check(session("false", "false", "true").value->direct_start);
		check(!session("false", "false", "false").value->direct_start);
		for (const auto& direct : {std::string("null"), std::string("1"), std::string("\"true\"")})
			check(session("false", "false", direct).error == "spectator_input_policy_invalid");
	}
}

static void source_coordinates_and_input_faults()
{
	{
		sample_document f(1);
		check(f.input.healthy() && !f.input.reserve());
		f.prepare();
		f.begin();
		check(!f.input.reserve());
		f.packet(1, 1, 0, 0);
		check(f.input.poll_input(test_hash));
		check(f.input.consumed() == 0); // Receiving does not establish execution.
		f.play(0, true);
		f.play(1);
		check(f.input.consumed() == 2);
		f.control("\"type\":\"end_game\",\"gameNumber\":1,\"finalFrame\":2");
		check(f.input.awaiting_end() && f.input.end(2, true));
		check(!f.input.awaiting_end()); // The next owner poll waits for game 2 instead of proving game 1's end twice.
		f.begin(2, 2);
		f.packet(2, 2, 0, 2);
		check(f.input.poll_input(test_hash));
		f.play(2, true);
		f.play(3);
		check(f.input.consumed() == 2 && f.input.healthy()); // Same process/token, monotonically increasing packet serial.
	}
	{
		sample_document f(2);
		f.prepare();
		f.begin();
		f.packet(1, 1, 0, 0, 2, true);
		check(!f.input.poll_input(test_hash) && f.input.consumed() == 0 && !f.input.healthy());
	}
	{
		sample_document f(3);
		f.prepare();
		f.begin();
		f.packet(1, 1, 0, 0);
		check(f.input.poll_input(test_hash));
		check(f.input.reserve());
		check(!f.input.verify_state(0, 1, 17, 0x8000004d, 7, {11, 99, 33}));
		check(f.input.reason() == "peer_sync_hash_mismatch" && f.input.consumed() == 0);
	}
	{
		sample_document f(4);
		f.prepare();
		f.begin();
		f.packet(1, 1, 0, 0);
		check(f.input.poll_input(test_hash));
		check(f.input.reserve());
		check(f.input.verify_state(0, 1, 17, 0x8000004d, 7, {11, 22, 33}));
		auto row = f.completed();
		row.ports[1].posted = false;
		check(!f.input.commit(0x01000000, 0x12efc, 0, row, {0x40, 0x20}) && f.input.consumed() == 0);
	}
}

static void end_watermark_and_integrity()
{
	{
		sample_document f(5);
		f.prepare();
		f.begin();
		f.control("\"type\":\"presentation\",\"hidden\":false,\"catchUp\":false,\"mode\":\"watch\",\"leftName\":\"Left\",\"rightName\":\"Right\",\"score\":{\"left\":0,\"right\":0},\"delayFrames\":180");
		check(load_presentation(9).hidden); // Host presentation cannot expose a game before actual native intro readback.
		f.packet(1, 1, 0, 0);
		check(f.input.poll_input(test_hash));
		f.play(0, true);
		check(!load_presentation(9).hidden);
		f.control("\"type\":\"chat\",\"visible\":true,\"additions\":[{\"messageId\":\"one\",\"authorName\":\"Player\",\"text\":\"GGs\"}]");
		check(pbd::corner_chat::load().count == 1);
		f.control("\"type\":\"chat\",\"visible\":false,\"additions\":[]");
		check(!pbd::corner_chat::load().count);
		f.control("\"type\":\"stop\"");
		check(f.input.stopped() && load_presentation(9).hidden);
	}
	{
		sample_document f(6);
		f.prepare();
		f.begin();
		f.packet(1, 1, 0, 0);
		check(f.input.poll_input(test_hash));
		check(f.input.reserve());
		check(f.input.verify_state(0, 1, 17, 0x8000004d, 7, {11, 22, 33}));
		auto row = f.completed();
		row.ports[0].injected_r3 = 0x80;
		row.ports[0].raw = row.ports[0].effective = 0x80;
		check(!f.input.commit(0x01000000, 0x12efc, 0, row, {0x80, 0x20}));
		check(f.input.reason() == "source_applied_input_mismatch" && f.input.consumed() == 0);
	}
	{
		sample_document f(7);
		f.prepare();
		f.begin();
		f.control("\"type\":\"presentation\",\"hidden\":false,\"catchUp\":false,\"mode\":\"analyze\",\"leftName\":\"Left\",\"rightName\":\"Right\",\"score\":{\"left\":0,\"right\":0},\"delayFrames\":180");
		f.packet(1, 1, 0, 0, 2, false, 0x840);
		check(f.input.poll_input(test_hash));
		check(load_presentation(9).native_through == 0 && load_presentation(9).history_count[0] == 0);
		f.play(0, true);
		const auto first = load_presentation(9);
		check(first.analyze && !first.hidden && first.native_through == 1 && first.history[0][0].held == 0x840 && first.history[0][0].applied == 0x40);
		f.play(1);
		check(load_presentation(9).history[0][0].frames == 2 && load_presentation(9).native_through == 2);
		f.control("\"type\":\"end_game\",\"gameNumber\":1,\"finalFrame\":2");
		check(f.input.end(2, true));
		f.begin(2, 2);
		check(load_presentation(9).hidden && load_presentation(9).native_game == 2 && load_presentation(9).history_count[0] == 0);
	}
}

static void native_round_state_checks()
{
	{
		const native_round round{4, 1, {0x1234, 0xabcd}};
		const auto fields = fixed_picks + "\"roundWitnessRequired\":true,";
		sample_document f(401);
		f.prepare();
		f.begin(1, 0, nullptr, true, fields);
		f.control("\"type\":\"presentation\",\"hidden\":false,\"catchUp\":false,\"mode\":\"watch\",\"delayFrames\":120,\"score\":{\"left\":0,\"right\":0},\"leftName\":\"Left\",\"rightName\":\"Right\"");
		check(f.input.needs_round_state() && !f.input.visible());
		f.packet(1, 1, 0, 0);
		check(f.input.poll_input(test_hash));
		check(!f.input.visible());
		f.play(0);
		check(f.input.visible() && !load_presentation(9).hidden);
		const auto round_state = "\"type\":\"game_witness\",\"gameNumber\":1,\"nativeRound\":" + pbd::set_channel::detail::serialize(round.json());
		f.control(round_state);
		f.control(round_state);
		check(f.input.reserve());
		check(f.input.verify_state(1, 1, 17, 0x8000004e, 7, {11, 22, 33}));
		check(f.input.expose(0x01000000, 1, 17, 12, 32, 3, true, &round));
		check(f.input.commit(0x01000000, 0x12efc, 1, f.completed(), {0x40, 0x20}));
		f.control("\"type\":\"end_game\",\"gameNumber\":1,\"finalFrame\":2");
		check(f.input.end(2, true));
		f.begin(2, 2, nullptr, true, fields);
		check(f.input.needs_round_state() && !f.input.visible());
	}
	{
		sample_document f(402);
		f.prepare();
		f.begin(1, 0, nullptr, true, fixed_picks + "\"roundWitnessRequired\":true,");
		f.packet(1, 1, 0, 0);
		check(f.input.poll_input(test_hash));
		check(f.input.reserve());
		const native_round round{4, 1, {0x1234, 0xabcd}};
		check(!f.input.expose(0x01000000, 0, 17, 12, 32, 3, true, &round));
		check(f.input.reason() == "native_round_start_missing" && load_presentation(9).hidden);
	}
	{
		sample_document f(403);
		f.prepare();
		f.begin(1, 0, nullptr, true, fixed_picks + "\"roundWitnessRequired\":true,");
		const native_round round{4, 1, {0x1234, 0xabcd}}, wrong{4, 1, {0x1234, 0xabce}};
		f.control("\"type\":\"game_witness\",\"gameNumber\":1,\"nativeRound\":" + pbd::set_channel::detail::serialize(round.json()));
		f.control("\"type\":\"game_witness\",\"gameNumber\":1,\"nativeRound\":" + pbd::set_channel::detail::serialize(wrong.json()), false);
		check(f.input.reason() == "native_round_start_mismatch");
	}
	{
		sample_document f(404);
		f.prepare();
		f.begin(1, 0, nullptr, false, fixed_picks + "\"roundWitnessRequired\":false,");
		check(f.input.reason() == "native_round_control_invalid");
	}
}

static void round_state_mismatch_refusals()
{
	{
		const native_round round{4, 1, {0x1234, 0xabcd}};
		sample_document f(8);
		f.prepare();
		f.begin(1, 0, &round);
		check(f.input.needs_round_state());
		f.packet(1, 1, 0, 0);
		check(f.input.poll_input(test_hash));
		check(f.input.reserve());
		check(f.input.verify_state(0, 1, 17, 0x8000004d, 7, {11, 22, 33}));
		check(f.input.expose(0x01000000, 0, 17, 12, 32, 3, true, &round));
		check(f.input.commit(0x01000000, 0x12efc, 0, f.completed(), {0x40, 0x20}));
		f.play(1);
		f.control("\"type\":\"end_game\",\"gameNumber\":1,\"finalFrame\":2");
		check(f.input.end(2, true));
		f.begin(2, 2, nullptr, false);
		check(f.input.reason() == "native_round_contract_changed");
	}
	{
		const native_round round{4, 1, {0x1234, 0xabcd}}, wrong{4, 1, {0x1234, 0xabce}};
		sample_document f(9);
		f.prepare();
		f.begin(1, 0, &round);
		f.packet(1, 1, 0, 0);
		check(f.input.poll_input(test_hash));
		check(f.input.reserve());
		check(!f.input.expose(0x01000000, 0, 17, 12, 32, 3, true, &wrong));
		check(f.input.reason() == "native_round_start_mismatch" && load_presentation(9).hidden && f.input.consumed() == 0);
	}
	{
		const native_round wrong{5, 1, {0x1234, 0xabcd}};
		sample_document f(10);
		f.prepare();
		f.begin(1, 0, &wrong, false);
		check(f.input.reason() == "native_round_control_invalid");
	}
}

static void in_game_pick_binding()
{
	{
		// Pick fields in the manifest and the controls: all numbers within the native bounds, or all null or left out.
		const auto all = parse_picks("{\"stageId\":5,\"leftCharacterRow\":40,\"rightCharacterRow\":84}");
		check(all && *all && **all == (picks{5, 40, 84}));
		const auto none = parse_picks("{\"stageId\":null,\"leftCharacterRow\":null,\"rightCharacterRow\":null}");
		check(none && !*none);
		const auto left_out = parse_picks("{\"roundsPerGame\":3}");
		check(left_out && !*left_out);
		check(!parse_picks("{\"stageId\":5,\"leftCharacterRow\":null,\"rightCharacterRow\":84}"));
		check(!parse_picks("{\"stageId\":38,\"leftCharacterRow\":1,\"rightCharacterRow\":2}"));
		check(!parse_picks("{\"stageId\":1,\"leftCharacterRow\":85,\"rightCharacterRow\":2}"));
	}
	{
		// An in-game set: each game binds the picks its own intro shows, reports them as ready, and the next game may change them.
		sample_document f(12, true);
		f.prepare();
		f.begin(1, 0, nullptr, true, "");
		f.packet(1, 1, 0, 0);
		check(f.input.poll_input(test_hash));
		f.play(0, true, {5, 40, 41});
		f.play(1);
		const auto ready = json_parser(f.report(3)).parse();
		u32 pick = 0;
		check(ready.value && is(ready.value->find("type"), "ready") && number32(ready.value->find("stageId"), 5, 5, pick) &&
			  number32(ready.value->find("leftCharacterRow"), 40, 40, pick) && number32(ready.value->find("rightCharacterRow"), 41, 41, pick));
		f.control("\"type\":\"end_game\",\"gameNumber\":1,\"finalFrame\":2");
		check(f.input.end(2, true));
		f.begin(2, 2, nullptr, true, "\"stageId\":null,\"leftCharacterRow\":null,\"rightCharacterRow\":null,");
		f.packet(2, 2, 0, 2);
		check(f.input.poll_input(test_hash));
		f.play(2, true, {0x25, 84, 0});
		f.play(3);
		check(f.input.healthy() && f.input.consumed() == 2);
		// The lobby's agreed picks after the intro only confirm what the game applied.
		check(f.game_picks(2, 0x25, 84, 0) && f.input.healthy());
	}
	{
		// The lobby's picks may come before the intro (in begin_game or a game_picks marker); the readback must then match them.
		sample_document f(13, true);
		f.prepare();
		f.begin(1, 0, nullptr, true, "");
		check(f.game_picks(1, 5, 40, 41));
		f.packet(1, 1, 0, 0);
		check(f.input.poll_input(test_hash));
		f.play(0, true, {5, 40, 41});
		check(f.input.healthy());
	}
}

static void pick_report_mismatch_refusals()
{
	{
		sample_document f(14, true);
		f.prepare();
		f.begin(1, 0, nullptr, true, "\"stageId\":5,\"leftCharacterRow\":40,\"rightCharacterRow\":41,");
		f.packet(1, 1, 0, 0);
		check(f.input.poll_input(test_hash));
		f.refused_intro(0, {5, 41, 40}, "game_picks_mismatch");
	}
	{
		// After the intro a disagreeing report refuses, and so does a report for another game or with only some picks.
		sample_document f(15, true);
		f.prepare();
		f.begin(1, 0, nullptr, true, "");
		f.packet(1, 1, 0, 0);
		check(f.input.poll_input(test_hash));
		f.play(0, true, {5, 40, 41});
		f.game_picks(1, 6, 40, 41, false);
		check(f.input.reason() == "game_picks_mismatch");
	}
	{
		sample_document f(16, true);
		f.prepare();
		f.begin(1, 0, nullptr, true, "");
		f.game_picks(2, 5, 40, 41, false);
		check(f.input.reason() == "game_picks_invalid");
	}
	{
		sample_document f(17, true);
		f.prepare();
		f.begin(1, 0, nullptr, false, "\"stageId\":5,\"leftCharacterRow\":null,\"rightCharacterRow\":41,");
		check(f.input.reason() == "game_control_unverified");
	}
	{
		// An intro outside the native bounds is not a game's picks.
		sample_document f(18, true);
		f.prepare();
		f.begin(1, 0, nullptr, true, "");
		f.packet(1, 1, 0, 0);
		check(f.input.poll_input(test_hash));
		f.refused_intro(0, {0x26, 1, 2}, "applied_intro_unverified");
	}
	{
		// A fixed set keeps its one binding: a game without its picks, a marker or an intro with other picks are refused.
		sample_document f(19);
		f.prepare();
		f.begin(1, 0, nullptr, false, "");
		check(f.input.reason() == "game_control_unverified");
	}
	{
		sample_document f(20);
		f.prepare();
		f.begin();
		f.game_picks(1, 5, 12, 32, false);
		check(f.input.reason() == "game_picks_mismatch");
	}
}

static void draw_end_and_replay_markers()
{
	{
		sample_document f(21);
		f.prepare();
		f.begin();
		check(f.game_picks(1, 17, 12, 32));
		f.packet(1, 1, 0, 0);
		check(f.input.poll_input(test_hash));
		f.refused_intro(0, {17, 12, 33}, "applied_intro_unverified");
	}
	{
		// A drawn game ends with "outcome":"draw" and the draw's notice on the cover. The lobby replays it under its own number;
		// that begin_game ends the watch as a stop with its reason, not a fault.
		sample_document f(22, true);
		f.prepare();
		f.begin(1, 0, nullptr, true, "");
		f.packet(1, 1, 0, 0);
		check(f.input.poll_input(test_hash));
		f.play(0, true, {5, 40, 41});
		f.play(1);
		f.control("\"type\":\"end_game\",\"gameNumber\":1,\"finalFrame\":2");
		check(f.input.awaiting_end() && f.input.end(2, true, true));
		const auto ended = json_parser(f.report(5)).parse();
		check(ended.value && is(ended.value->find("type"), "ended") && is(ended.value->find("outcome"), "draw"));
		check(f.input.draw_closed() && f.input.healthy() && load_presentation(9).hidden && load_presentation(9).notice == draw_notice);
		f.begin(1, 2, nullptr, false, "");
		check(f.input.stopped() && !f.input.healthy() && f.input.reason() == "draw_replay" && load_presentation(9).notice == draw_notice);
		const auto stopped = json_parser(f.report(6)).parse();
		check(stopped.value && is(stopped.value->find("type"), "stopped") && is(stopped.value->find("reason"), "draw_replay"));
	}
	{
		// A decided game names no outcome, and a draw still needs the native end proof.
		sample_document f(23, true);
		f.prepare();
		f.begin(1, 0, nullptr, true, "");
		f.packet(1, 1, 0, 0);
		check(f.input.poll_input(test_hash));
		f.play(0, true, {5, 40, 41});
		f.play(1);
		f.control("\"type\":\"end_game\",\"gameNumber\":1,\"finalFrame\":2");
		check(!f.input.end(2, false, true));
		check(f.input.reason() == "natural_end_unverified" && !f.input.draw_closed());
	}
	{
		// The drawn game's replay goes through the select again: its picks marker carries the same game number with other
		// fighters. It ends the watch as the replay's begin_game does, and is never checked against the drawn game.
		sample_document f(24, true);
		f.prepare();
		f.begin(1, 0, nullptr, true, "");
		f.packet(1, 1, 0, 0);
		check(f.input.poll_input(test_hash));
		f.play(0, true, {5, 40, 41});
		f.play(1);
		f.control("\"type\":\"end_game\",\"gameNumber\":1,\"finalFrame\":2");
		check(f.input.end(2, true, true));
		f.game_picks(1, 6, 50, 51, false);
		check(f.input.stopped() && f.input.reason() == "draw_replay");
		const auto stopped = json_parser(f.report(6)).parse();
		check(stopped.value && is(stopped.value->find("type"), "stopped") && is(stopped.value->find("reason"), "draw_replay"));
	}
}

static void draw_replay_end_watermark()
{
	{
		// So does the replay's end watermark, which no longer belongs to a game this watch is playing.
		sample_document f(25, true);
		f.prepare();
		f.begin(1, 0, nullptr, true, "");
		f.packet(1, 1, 0, 0);
		check(f.input.poll_input(test_hash));
		f.play(0, true, {5, 40, 41});
		f.play(1);
		f.control("\"type\":\"end_game\",\"gameNumber\":1,\"finalFrame\":2");
		check(f.input.end(2, true, true));
		f.control("\"type\":\"end_game\",\"gameNumber\":1,\"finalFrame\":7", false);
		check(f.input.stopped() && f.input.reason() == "draw_replay");
	}
}

#include "spectator_presentation_checks.h"

static void rematch_option_readback()
{
	for (unsigned changed = 0; changed < 6; ++changed)
	{
		sample_document f(200 + changed, true);
		f.prepare();
		f.control("\"type\":\"begin_game\",\"gameNumber\":1,\"originBoundary\":0,\"originCounter\":2147483725,"
				  "\"kind\":\"clean_character_select\",\"roundsPerGame\":3,\"roundWitnessRequired\":true");
		const native_round round{4, 0, {1, 2}};
		f.control("\"type\":\"game_witness\",\"gameNumber\":1,\"nativeRound\":" +
				  pbd::set_channel::detail::serialize(round.json()));
		f.packet(1, 1, 0, 0);
		check(f.input.poll_input(test_hash));
		check(f.input.reserve() && f.input.verify_state(0, 1, 17, 0x8000004d, 7, {11, 22, 33}));
		check(f.input.expose(0x01000000, 0, 17, 12, 32, 3, true, &round));
		check(f.input.commit(0x01000000, 0x12efc, 0, f.completed(), {0x40, 0x20}));
		f.play(1);
		f.control("\"type\":\"end_game\",\"gameNumber\":1,\"finalFrame\":2");
		check(f.input.end(2, true));
		pbd::rematch::options actual;
		pbd::rematch::load load{1, 2, 2, 1, {{12, 32}, 23}, actual, true};
		switch (changed)
		{
		case 1: load.end_frame++; break;
		case 2: load.selected.rows[1]++; break;
		case 3: load.epoch++; break;
		case 4: actual.time_index++; break;
		}
		const auto text = pbd::set_channel::detail::serialize(pbd::rematch::load_json<value>(load));
		f.control("\"type\":\"begin_game\",\"gameNumber\":2,\"originBoundary\":2,\"originCounter\":2147483727,"
				  "\"kind\":\"natural_vs\",\"roundsPerGame\":3,\"roundWitnessRequired\":true,\"load\":" +
					  text,
			changed == 0, changed == 5 ? nullptr : &actual);
		if (changed)
		{
			check(f.input.reason() == (changed == 5 ? "rematch_options_unverified" : "rematch_load_changed"));
			continue;
		}
		const auto queued = f.input.take_rematch();
		check(queued && queued->first == load);
		check(queued->second.end_frame == 2 && queued->second.selected.stage == 17 && !f.input.take_rematch());
		f.packet(2, 2, 0, 2);
		check(f.input.poll_input(test_hash));
		f.play(2);
		check(!f.input.visible());
	}
}

static void rematch_load_and_game_number()
{
	{
		sample_document f(210, true);
		f.prepare();
		const native_round round{4, 0, {1, 2}};
		pbd::rematch::options actual;
		const auto begin = [&](u32 game, u32 origin, const pbd::rematch::load* load)
		{
			f.control("\"type\":\"begin_game\",\"gameNumber\":" + std::to_string(game) +
						  ",\"originBoundary\":" + std::to_string(origin) + ",\"originCounter\":2147483725,\"kind\":\"" +
						  (game == 1 ? "clean_character_select" : "natural_vs") +
						  "\",\"roundsPerGame\":3,\"roundWitnessRequired\":true" +
						  (load ? ",\"load\":" + pbd::set_channel::detail::serialize(pbd::rematch::load_json<value>(*load)) : ""),
				true, &actual);
		};
		begin(1, 0, nullptr);
		for (u32 game = 1; game <= 2; ++game)
		{
			const u32 origin = (game - 1) * 2;
			f.control("\"type\":\"game_witness\",\"gameNumber\":" + std::to_string(game) +
					  ",\"nativeRound\":" + pbd::set_channel::detail::serialize(round.json()));
			f.packet(game, game, 0, origin);
			check(f.input.poll_input(test_hash));
			check(f.input.reserve() && f.input.verify_state(origin, 1, 17, 0x8000004d + origin, 7, {11, 22, 33}));
			check(f.input.expose(0x01000000, origin, 17, 12, 32, 3, true, &round));
			check(f.input.commit(0x01000000, 0x12efc, origin, f.completed(), {0x40, 0x20}));
			f.play(origin + 1);
			f.control("\"type\":\"end_game\",\"gameNumber\":" + std::to_string(game) + ",\"finalFrame\":2");
			check(f.input.end(origin + 2, true));
			const pbd::rematch::load load{game, game + 1, 2, game, {{12, 32}, game == 1 ? 17u : 23u}, actual, game == 2};
			begin(game + 1, origin + 2, &load);
			const auto queued = f.input.take_rematch();
			check(queued && queued->first == load && queued->second.game == game && queued->second.end_frame == 2);
			check(queued->second.selected.stage == 17 && !f.input.take_rematch());
		}
		check(f.input.game() == 3 && f.input.healthy());
	}
}

static void round_state_required_per_game()
{
	// Watching from the intro (no selection stream): the round call comes with each begin_game, and a Rematch load is queued
	// the same way. A watch that checks no round call refuses the load.
	for (const bool sampled : {true, false})
	{
		sample_document f(220 + (sampled ? 1 : 0), true);
		f.prepare();
		const native_round round{4, 0, {1, 2}};
		const std::string round_field = sampled ? ",\"nativeRound\":" + pbd::set_channel::detail::serialize(round.json()) : "";
		f.control("\"type\":\"begin_game\",\"gameNumber\":1,\"originBoundary\":0,\"originCounter\":2147483725,"
				  "\"kind\":\"clean_character_select\",\"roundsPerGame\":3" +
				  round_field);
		f.packet(1, 1, 0, 0);
		check(f.input.poll_input(test_hash));
		check(f.input.reserve() && f.input.verify_state(0, 1, 17, 0x8000004d, 7, {11, 22, 33}));
		check(f.input.expose(0x01000000, 0, 17, 12, 32, 3, true, sampled ? &round : nullptr));
		check(f.input.commit(0x01000000, 0x12efc, 0, f.completed(), {0x40, 0x20}));
		f.play(1);
		f.control("\"type\":\"end_game\",\"gameNumber\":1,\"finalFrame\":2");
		check(f.input.end(2, true));
		pbd::rematch::options actual;
		const pbd::rematch::load load{1, 2, 2, 1, {{12, 32}, 17}, actual, false};
		f.control("\"type\":\"begin_game\",\"gameNumber\":2,\"originBoundary\":2,\"originCounter\":2147483727,"
				  "\"kind\":\"natural_vs\",\"roundsPerGame\":3" +
					  round_field + ",\"load\":" +
					  pbd::set_channel::detail::serialize(pbd::rematch::load_json<value>(load)),
			sampled, &actual);
		if (!sampled)
		{
			check(f.input.reason() == "rematch_tail_unverified");
			continue;
		}
		const auto queued = f.input.take_rematch();
		check(queued && queued->first == load);
		check(queued->second.game == 1 && queued->second.end_frame == 2 && queued->second.selected.stage == 17);
		check(f.input.needs_round_state() && !f.input.take_rematch());
	}
}

static void repick_readback_matches_source()
{
	// Change character: the Right side picks again. Its select plays from the recorded input and shows like any in-game select,
	// and the intro may name any row for the chooser but must keep the other row and the stage.
	for (unsigned variant = 0; variant < 4; ++variant)
	{
		sample_document f(230 + variant, true);
		f.prepare();
		f.control("\"type\":\"presentation\",\"hidden\":false,\"catchUp\":false,\"mode\":\"watch\",\"leftName\":\"Left\",\"rightName\":\"Right\","
				  "\"score\":{\"left\":0,\"right\":0},\"delayFrames\":180");
		f.control("\"type\":\"begin_game\",\"gameNumber\":1,\"originBoundary\":0,\"originCounter\":2147483725,"
				  "\"kind\":\"clean_character_select\",\"roundsPerGame\":3,\"roundWitnessRequired\":true");
		const native_round round{4, 0, {1, 2}};
		const auto round_state = [&](u32 game)
		{
			f.control("\"type\":\"game_witness\",\"gameNumber\":" + std::to_string(game) + ",\"nativeRound\":" + pbd::set_channel::detail::serialize(round.json()));
		};
		round_state(1);
		f.packet(1, 1, 0, 0);
		check(f.input.poll_input(test_hash));
		check(f.input.reserve() && f.input.verify_state(0, 1, 17, 0x8000004d, 7, {11, 22, 33}));
		check(f.input.expose(0x01000000, 0, 17, 12, 32, 3, true, &round));
		check(f.input.commit(0x01000000, 0x12efc, 0, f.completed(), {0x40, 0x20}));
		f.play(1);
		f.control("\"type\":\"end_game\",\"gameNumber\":1,\"finalFrame\":2");
		check(f.input.end(2, true));
		pbd::rematch::options actual;
		pbd::rematch::load load{1, 2, 2, 1, {{12, 32}, 17}, actual, false};
		load.repick = 1;
		f.control("\"type\":\"begin_game\",\"gameNumber\":2,\"originBoundary\":2,\"originCounter\":2147483727,"
				  "\"kind\":\"natural_vs\",\"roundsPerGame\":3,\"roundWitnessRequired\":true,\"load\":" +
					  pbd::set_channel::detail::serialize(pbd::rematch::load_json<value>(load)),
			true, &actual);
		const auto queued = f.input.take_rematch();
		check(queued && queued->first == load && queued->first.repick == 1u);
		round_state(2);
		f.packet(2, 2, 0, 2);
		check(f.input.poll_input(test_hash));
		f.play(2);
		check(f.input.visible());
		// The lobby's report of the game is held to the same rule as the intro.
		if (variant == 3)
		{
			f.game_picks(2, 17, 13, 61, false);
			check(f.input.reason() == "game_picks_mismatch");
			continue;
		}
		if (variant == 0)
			check(f.game_picks(2, 17, 12, 61));
		const picks shown = variant == 1 ? picks{17, 13, 61} : variant == 2 ? picks{23, 12, 61} :
		                                                                      picks{17, 12, 61};
		check(f.input.reserve() && f.input.verify_state(3, 1, 17, 0x8000004d + 3, 7, {11, 22, 33}));
		const bool exposed = f.input.expose(0x01000000, 3, shown.stage, shown.left, shown.right, 3, true, &round);
		check(exposed == (variant == 0));
		if (variant)
			check(f.input.reason() == "rematch_picks_changed");
	}
}

PBD_TEST(PbdSpectatorChannel, Checks)
{
	countdown_packets_and_sync_facts();
	source_coordinates_and_input_faults();
	end_watermark_and_integrity();
	native_round_state_checks();
	round_state_mismatch_refusals();
	in_game_pick_binding();
	pick_report_mismatch_refusals();
	draw_end_and_replay_markers();
	draw_replay_end_watermark();
	watch_screen_and_game_visibility();
	input_deadline_and_wait_controls();
	publication_and_receipt_collisions();
	rematch_option_readback();
	rematch_load_and_game_number();
	round_state_required_per_game();
	repick_readback_matches_source();
}
