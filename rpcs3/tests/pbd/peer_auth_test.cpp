#include "pbd_test.h"

#include "Emu/PBD/pbd_peer_auth.h"
#include "Emu/PBD/pbd_peer_message.h"
#include "Emu/PBD/pbd_peer_auth_state.h"
#include "Emu/PBD/pbd_peer_key_pipe.h"
#include "Emu/PBD/pbd_lobby_set_model.h"
#include "Emu/PBD/pbd_peer_recovery.h"
#include "Emu/Cell/pbd_session_wait.h"

#include <limits>
#include <string_view>
#include <vector>

using pbd::test::check;

// Before current-key contact, loading silence stays under the start wait's 180 s budget without requesting recovery.
PBD_TEST(PbdPeerRecovery, AnOpponentNotYetHeardUnderTheKeyIsStillLoadingNotALostPath)
{
	using namespace pbd::peer_recovery;
	const endpoint direct{pbd::peer_endpoint::family::v4, {10, 0, 0, 2}, 0x1234};
	state link;
	link.configure({"session", "set", 42, 7}, direct, 1000);
	link.sent(6, 1500);
	link.received(6, 1600);
	constexpr std::uint64_t ready = 2000;
	// The start digest is resent every 10 ms while the other game loads for two minutes.
	for (std::uint64_t elapsed = 0; elapsed <= 120000000; elapsed += 10000)
	{
		link.sent(7, ready + elapsed);
		link.poll(ready + elapsed);
		check(!link.get().recovering && link.get().request_id == 0, "a still-loading opponent was counted as a lost path");
		check(!link.expired(ready + elapsed));
	}
	check(link.get().last_valid_us == 0, "a retired key's input counted as the new key's first contact");
	check(link.timeout(ready, ready + 90000000, 180000000, 0) == wait_result::pending, "the start wait was cut at 90 s");
	check(link.timeout(ready, ready + 180000000, 180000000, 0) == wait_result::pending);
	check(link.timeout(ready, ready + 180000001, 180000000, 0) == wait_result::deadline,
		"an opponent that never arrives must end the start with its own timeout, not as an exhausted recovery");
	// The opponent arrives after 125 s. Its first input neither exhausts the start wait nor arms anything by itself.
	constexpr std::uint64_t heard = ready + 125000000;
	link.received(7, heard);
	link.poll(heard);
	check(!link.get().recovering && link.timeout(ready, heard, 180000000, 0) == wait_result::pending,
		"the first contact after a slow load ended the start wait");
	// First contact starts the absolute lease; 500 ms of silence requests recovery.
	check(link.timeout(ready, heard + 89999999, 180000000, 0) == wait_result::pending);
	check(link.timeout(ready, heard + 90000000, 180000000, 0) == wait_result::recovery_exhausted);
	link.poll(heard + 499999);
	check(!link.get().recovering);
	link.poll(heard + 500000);
	check(link.get().recovering && link.get().request_id == 1, "a heard peer's silence must still ask for recovery");
	// A continuation's fresh key starts unheard again.
	link.configure({"session", "next-set", 42, 8}, direct, heard + 600000);
	link.sent(8, heard + 700000);
	link.poll(heard + 60000000);
	check(!link.get().recovering && link.get().request_id == 0, "a continuation inherited the old key's contact");
}

// The status distinguishes loading from recovery so an unheard peer's receive age cannot imply a restored route.
PBD_TEST(PbdPeerRecovery, TheStartWaitsForALoadingOpponentAndReportsItUnheard)
{
	using namespace pbd::peer_recovery;
	const endpoint direct{pbd::peer_endpoint::family::v4, {10, 0, 0, 2}, 0x1234};
	const auto document = [](const state& link, std::uint64_t now)
	{
		auto parsed = pbd::json::detail::json_parser(status_json(link.get(), now)).parse();
		check(parsed.value && parsed.value->find("peerHeard") && parsed.value->find("recovering"));
		return std::move(*parsed.value);
	};
	for (const std::uint64_t contact : {100000, 2000000})
	{
		state link;
		link.configure({"session", "set", 42, 7}, direct, 1000, 47001);
		constexpr std::uint64_t start = 2000;
		for (std::uint64_t now = start; now < start + contact; now += 10000)
		{
			link.sent(7, now);
			link.poll(now);
			check(!link.traffic().recovering, "the loading opponent was counted as a lost connection");
		}
		const auto waiting = document(link, start + contact - 1);
		check(!waiting.find("peerHeard")->truth && !waiting.find("recovering")->truth, "the status did not say the opponent is unheard");
		link.received(7, start + contact, start);
		link.poll(start + contact);
		const auto heard = document(link, start + contact);
		check(heard.find("peerHeard")->truth && !heard.find("recovering")->truth && heard.find("lastValidReceiveAgoMs")->text == "0");
		link.poll(start + contact + 499999);
		check(!link.traffic().recovering);
		link.poll(start + contact + 600000);
		check(link.traffic().recovering, "a heard opponent's silence must request recovery");
		check(document(link, start + contact + 600000).find("recovering")->truth);
	}
}

PBD_TEST(PbdPeerRecovery, RoundProgressIsFencedWithTheCurrentHealthIdentity)
{
	using namespace pbd::peer_recovery;
	const endpoint direct{pbd::peer_endpoint::family::v4, {10, 0, 0, 2}, 0x1234};
	const context identity{"session", "set", 42, 7};
	state link;
	link.configure(identity, direct, 1000);
	const pbd::session_health::progress behind{1, {0, 2}};
	link.observe_progress("session", "set", 42, 7, behind);
	const auto sequence = link.get().sequence;
	check(link.get().progress == behind);
	link.observe_progress("session", "set", 42, 7, behind);
	check(link.get().sequence == sequence, "unchanged scores caused an extra health publication");
	link.observe_progress("old-session", "set", 42, 7, {});
	link.observe_progress("session", "old-set", 42, 7, {});
	link.observe_progress("session", "set", 41, 7, {});
	link.observe_progress("session", "set", 42, 6, {});
	check(link.get().progress == behind && link.get().sequence == sequence, "retired progress changed the current health page");
	link.configure({"session", "next-set", 43, 8}, direct, 2000);
	check(!link.get().progress, "a new set inherited the previous set's round score");
	link.observe_progress("session", "set", 42, 7, behind);
	check(!link.get().progress, "a late old-set observation supplied evidence against the new opponent");
	link.observe_progress("session", "next-set", 43, 8, pbd::session_health::progress{1, {0, 0}});
	check(link.get().progress == pbd::session_health::progress{1, {0, 0}});
	const auto changed = link.get().sequence;
	link.observe_progress("session", "next-set", 43, 8, pbd::session_health::progress{1, {5, 0}});
	check(!link.get().progress && link.get().sequence == changed + 1, "invalid progress did not invalidate the current observation");
	link.observe_progress("session", "next-set", 43, 8, {});
	check(!link.get().progress && link.get().sequence == changed + 1, "repeated unknown progress caused an extra health publication");
}

PBD_TEST(PbdPeerRecovery, OutagesRetainTheSameContextAndPauseBothWaitBudgets)
{
	using namespace pbd::peer_recovery;
	const pbd::peer_endpoint::endpoint direct{pbd::peer_endpoint::family::v4, {10, 0, 0, 2}, 0x1234};
	for (const std::uint64_t seconds : {1, 5, 15, 30, 60})
	{
		state link;
		link.configure({"session", "set", 42, 7}, direct, 1000);
		link.received(7, 1000);
		const auto credit = link.paused(1000);
		link.poll(501000);
		check(link.get().recovering && link.get().request_id == 1);
		const auto resumed = 1000 + seconds * 1000000 + 250000;
		link.poll(resumed);
		check(!link.expired(resumed));
		// The first valid datagram needs transit time after the full sixty-second outage.
		link.received(7, resumed);
		check(!link.get().recovering && link.get().identity.epoch == 7 && link.get().identity.peer_session == 42);
		check(link.wait_elapsed(1000, resumed, credit) == 500000);
		check(link.wait_elapsed(1000, resumed, credit) < 60000000);
		check(link.timeout(1000, resumed, 60000000, credit) == wait_result::pending);
	}
}

PBD_TEST(PbdPeerRecovery, ReconnectUiOnlyRefreshesAtAQuiescentFrameHandshake)
{
	using pbd::peer_recovery::refresh_at_handshake;
	check(refresh_at_handshake(true, false, true, true));
	check(!refresh_at_handshake(false, false, true, true));
	check(!refresh_at_handshake(true, true, true, true));
	check(!refresh_at_handshake(true, false, false, true));
	check(!refresh_at_handshake(true, false, true, false));
}

PBD_TEST(PbdPeerRecovery, StageRequiresCurrentPeerProofBeforeCommit)
{
	using namespace pbd::peer_recovery;
	const pbd::peer_endpoint::endpoint direct{pbd::peer_endpoint::family::v4, {10, 0, 0, 2}, 0x1234};
	const pbd::peer_endpoint::endpoint relay{pbd::peer_endpoint::family::v4, {127, 0, 0, 1}, 0x2345};
	const context identity{"session", "set", 42, 7};
	state link;
	link.configure(identity, direct, 1000);
	command stage{identity, 1, operation::stage, 1};
	check(link.apply(stage, relay) && link.get().active == direct && link.probe(7) == relay);
	command commit{identity, 2, operation::commit, 1};
	check(!link.apply(commit) && std::string_view(link.get().reason) == "candidate_unverified" && link.get().active == direct);
	check(!link.prove(relay, 6) && !link.prove(direct, 7));
	check(link.prove(relay, 7) && link.get().verified_proposal == 1 && link.get().active == direct);
	commit.serial = 3;
	check(link.apply(commit) && link.get().active == relay && link.get().active_proposal == 1 && !link.probe(7));
	check(!link.apply(commit) && link.get().command_serial == 3);
	stage.serial = stage.proposal = 4;
	stage.identity.epoch = 6;
	check(!link.apply(stage, direct) && std::string_view(link.get().reason) == "stale_context" && link.get().active == relay);
}

PBD_TEST(PbdPeerRecovery, ANewEpochCancelsOldProbesAndProofs)
{
	using namespace pbd::peer_recovery;
	const pbd::peer_endpoint::endpoint direct{pbd::peer_endpoint::family::v4, {10, 0, 0, 2}, 0x1234};
	const pbd::peer_endpoint::endpoint relay{pbd::peer_endpoint::family::v4, {127, 0, 0, 1}, 0x2345};
	state link;
	context identity{"session", "set", 42, 7};
	link.configure(identity, direct, 1000);
	link.received(7, 1000);
	check(link.apply({identity, 1, operation::stage, 1}, relay) && link.prove(relay, 7));
	identity.epoch = 8;
	identity.set_id = "next-set";
	link.configure(identity, direct, 2000);
	check(!link.probe(7) && !link.probe(8) && link.get().verified_proposal == 0 && link.get().command_serial == 1);
	link.poll(90502000);
	check(!link.get().recovering && link.get().request_id == 0 && !link.expired(90502000),
		"a fresh key must finish its own handshake before silence can request path recovery");
	link.received(7, 90502000);
	link.poll(90502001);
	check(!link.get().recovering && link.get().last_valid_us == 0, "a retired key cannot establish the new path");
	check(!link.prove(relay, 7));
	check(!link.apply({identity, 2, operation::commit, 1}) && link.get().active == direct);
	check(link.apply({identity, 3, operation::stage, 3}, relay));
	check(link.apply({identity, 4, operation::cancel, 3}) && !link.probe(8) && link.get().active == direct);
	link.received(8, 90503000);
	link.poll(91003000);
	check(link.get().recovering && link.get().request_id == 1, "the new key's first input must enable normal loss detection");
}

PBD_TEST(PbdPeerRecovery, ReconfiguringTheSameContextPreservesLossDetection)
{
	using namespace pbd::peer_recovery;
	const pbd::peer_endpoint::endpoint direct{pbd::peer_endpoint::family::v4, {10, 0, 0, 2}, 0x1234};
	const context identity{"session", "set", 42, 7};
	state link;
	link.configure(identity, direct, 1000, 47001);
	link.received(7, 1000);
	link.configure(identity, direct, 2000, 47002);
	link.poll(501000);
	check(link.get().recovering && link.get().request_id == 1 && link.get().local_port == 47002,
		"an unchanged context refresh must not forget established traffic or restart its loss timer");
	link.configure(identity, direct, 90000000, 47002);
	check(link.expired(90501000), "repeating one configuration must not extend its recovery lease");
}

PBD_TEST(PbdPeerRecovery, PathDocumentsAreBoundedAndDoNotCarryKeys)
{
	using namespace pbd::peer_recovery;
	const std::string text = R"({"schema":"pbd-network-path-1","sessionId":"session","setId":"set","peerSession":42,"keyEpoch":7,"serial":1,"operation":"stage","proposalSerial":1,"address":"127.0.0.1","port":47003})";
	const auto parsed = parse(text, 1);
	check(parsed.value && parsed.value->address == "127.0.0.1" && parsed.value->port == 47003);
	check(!parse(text, 2).value);
	check(!parse(text.substr(0, text.size() - 1) + R"(,"key":"never accepted"})", 1).value);
	check(!parse(std::string(4097, ' '), 1).value);
	state link;
	link.configure({"session", "set", 42, 7}, {pbd::peer_endpoint::family::v4, {10, 0, 0, 2}, 0x1234}, 1000);
	link.received(7, 1000);
	link.poll(501000);
	check(link.expired(90501000));
	check(link.timeout(1000, 90501000, 60000000, 0) == wait_result::recovery_exhausted);
	const auto document = pbd::json::detail::json_parser(status_json(link.get(), 501000)).parse();
	check(document.value && document.value->find("recovering")->truth && document.value->find("keyEpoch")->text == "7");
}

PBD_TEST(PbdPeerRecovery, AProofMustFollowStagingAndStayFreshUntilTheTargetChanges)
{
	using namespace pbd::peer_recovery;
	const pbd::peer_endpoint::endpoint direct{pbd::peer_endpoint::family::v4, {10, 0, 0, 2}, 0x1234};
	const pbd::peer_endpoint::endpoint relay{pbd::peer_endpoint::family::v4, {127, 0, 0, 1}, 0x2345};
	const context identity{"session", "set", 42, 7};
	state link;
	link.configure(identity, direct, 1000, 47001);
	check(link.apply({identity, 1, operation::stage, 1}, relay, {}, 2000));
	check(!link.prove(relay, 7, 1999));
	check(link.prove(relay, 7, 3000));
	check(!link.apply({identity, 2, operation::commit, 1}, {}, {}, 1003001));
	check(std::string_view(link.get().reason) == "candidate_stale" && link.get().active == direct);
	check(link.prove(relay, 7, 1004000));
	bool target_changed = false;
	check(link.apply({identity, 3, operation::commit, 1}, {}, [&](const auto& target)
		{
			check(target == relay);
			target_changed = true;
		},
		1005000));
	check(target_changed && link.get().active_proposal == 1);
	const auto status = pbd::json::detail::json_parser(status_json(link.get(), 1005000)).parse();
	check(status.value->find("family")->text == "4" && status.value->find("localPort")->text == "47001");
}

PBD_TEST(PbdPeerRecovery, OneSidedCommitKeepsTheOldPathForSixtySecondsWithoutChangingKeys)
{
	using namespace pbd::peer_recovery;
	const pbd::peer_endpoint::endpoint direct{pbd::peer_endpoint::family::v4, {10, 0, 0, 2}, 0x1234};
	const pbd::peer_endpoint::endpoint relay{pbd::peer_endpoint::family::v4, {127, 0, 0, 1}, 0x2345};
	context identity{"session", "set", 42, 7};
	state link;
	link.configure(identity, direct, 1000);
	check(link.apply({identity, 1, operation::stage, 1}, relay, {}, 2000) && link.prove(relay, 7, 3000));
	check(link.apply({identity, 2, operation::commit, 1}, {}, {}, 4000));
	check(link.previous(7, 60003999) == direct && link.allows(direct, 7, 60003999));
	check(!link.previous(7, 60004001) && !link.allows(direct, 7, 60004001));
	check(!link.previous(8, 5000) && !link.allows(direct, 8, 5000));
	check(link.get().active == relay && link.get().identity == identity);
	identity.epoch = 8;
	identity.set_id = "next-set";
	link.configure(identity, relay, 5000);
	check(!link.previous(7, 5001) && !link.previous(8, 5001) && !link.allows(direct, 8, 5001));
}

namespace
{
	namespace auth = pbd::peer_auth;
	namespace message = pbd::peer_message;
	using bytes = std::vector<std::uint8_t>;
	constexpr std::uint32_t session = 0x1a2b3c4d;

	bytes from_hex(std::string_view hex)
	{
		bytes result;
		for (std::size_t i = 0; i < hex.size(); i += 2)
		{
			const auto digit = [](char c)
			{
				return c <= '9' ? c - '0' : c - 'a' + 10;
			};
			result.push_back(static_cast<std::uint8_t>((digit(hex[i]) << 4) | digit(hex[i + 1])));
		}
		return result;
	}

	auth::key public_key()
	{
		auth::key result;
		result.fill(0x0b);
		return result;
	}

	bytes signed_end(std::uint64_t sequence = 0x0102030405060708)
	{
		const auto payload = message::encode_end({0, session, 7, 900});
		bytes result(auth::maximum_bytes);
		const auto size = auth::seal(public_key(), sequence, payload, result);
		check(size != 0);
		result.resize(size);
		return result;
	}
} // namespace

PBD_TEST(PbdPeerAuth, LostTerminalAnnouncementsCanDrainBeforeTheServiceResultAck)
{
	auth::state left, right;
	check(left.install(public_key(), 0) && right.install(public_key(), 1));
	const auto announce = [&](auth::state& sender, std::uint32_t role)
	{
		message::input_message value;
		value.role = role;
		value.confirmed = 899;
		value.sync.session = session;
		value.segment.number = value.digests.state_segment = 9;
		value.segment.first_index = 900;
		std::array<std::uint8_t, message::max_input_bytes> raw{};
		const auto size = message::encode_input(value, raw);
		bytes signed_bytes(auth::maximum_bytes);
		signed_bytes.resize(sender.send(std::span(raw).first(size), signed_bytes));
		return signed_bytes;
	};
	// Only the right-to-left terminal announcement gets through the first asymmetric burst.
	for (unsigned lost = 0; lost < 3; ++lost)
	{
		check(!announce(left, 0).empty());
		const auto inbound = announce(right, 1);
		check(left.receive(inbound));
	}
	using pbd::set_channel::command_kind;
	check(!pbd::lobby_set::terminal_results_acknowledged(true, true, command_kind::choice_rejected));
	check(left.can_learn() && right.can_learn());
	const auto retransmit = announce(left, 0);
	const auto received = right.receive(retransmit);
	check(received, "terminal completion must retain the key needed for a lost announcement's resend");
	message::input_message decoded;
	check(message::read_input(received->payload, 1, session, decoded) == message::input_check::valid);
	check(decoded.segment.number == 9 && decoded.segment.first_index == 900 && decoded.confirmed == 899);
	check(!right.receive(retransmit), "draining never permits an authenticated replay");
	check(!pbd::lobby_set::terminal_results_acknowledged(true, false, command_kind::show_post_set));
	check(pbd::lobby_set::terminal_results_acknowledged(true, true, command_kind::show_post_set));
	const auto epoch = left.epoch();
	left.retire();
	right.retire();
	check(left.epoch() != epoch && !right.receive(retransmit));
	auto next_key = public_key();
	next_key[0] ^= 7;
	check(left.install(next_key, 0) && right.install(next_key, 1));
	check(!right.receive(retransmit));
	check(right.receive(announce(left, 0)), "the fresh next set must still exchange authenticated input");
}

PBD_TEST(PbdPeerAuth, MatchesRfc4231TruncatedSha256Vectors)
{
	// Public conformance inputs, never a live session key. https://www.rfc-editor.org/rfc/rfc4231#section-4.2
	bytes secret(20, 0x0b);
	const auto data = from_hex("4869205468657265");
	const auto digest = auth::authenticate(secret, data);
	check(digest);
	check(bytes(digest->begin(), digest->end()) == from_hex("b0344c61d8db38535ca8afceaf0bf12b"));
	const auto second = auth::authenticate(from_hex("4a656665"),
		from_hex("7768617420646f2079612077616e7420666f72206e6f7468696e673f"));
	check(second);
	check(bytes(second->begin(), second->end()) == from_hex("5bdcc146bf60754e6a042426089575c7"));
}

PBD_TEST(PbdPeerAuth, WritesAndReadsLiteralAuthenticatedEndBytes)
{
	// Header, sequence, payload length, production end codec, then the independent HMAC-SHA256/128 result.
	const auto golden = from_hex("5042442d4155544801000000080706050403020120000000"
								 "5042442d454e442101000000000000004d3c2b1a070000008403000000000000"
								 "694d6805a8081d9c5b1fbec185a0199d");
	check(signed_end() == golden);
	const auto opened = auth::open(public_key(), golden);
	check(opened && opened->sequence == 0x0102030405060708);
	const auto end = message::read_end(opened->payload, 1, session);
	check(end && end->role == 0 && end->session == session && end->reason == 7 && end->tick == 900);
}

PBD_TEST(PbdPeerAuth, RefusesEveryTamperedByteWrongKeyAndLength)
{
	const auto golden = signed_end();
	for (std::size_t i = 0; i < golden.size(); ++i)
	{
		auto spoof = golden;
		spoof[i] ^= 1;
		check(!auth::open(public_key(), spoof), "a changed header, game field or tag must refuse");
	}
	auto wrong_key = public_key();
	wrong_key[0] ^= 1;
	check(!auth::open(wrong_key, golden));
	for (std::size_t size = 0; size < golden.size(); ++size)
		check(!auth::open(public_key(), std::span(golden).first(size)));
	auto oversized = golden;
	oversized.push_back(0);
	check(!auth::open(public_key(), oversized));
}

PBD_TEST(PbdPeerAuth, GatesOlderPeerInBothDirections)
{
	const auto raw = message::encode_end({0, session, 7, 900});
	check(!auth::open(public_key(), raw));
	check(!message::read_end(signed_end(), 1, session));
	message::input_message input;
	check(message::read_input(signed_end(), 1, session, input) == message::input_check::bad_message);
}

PBD_TEST(PbdPeerAuth, RefusesInvalidWritesAndAcceptsMaximumPayload)
{
	bytes output(auth::maximum_bytes), payload(auth::maximum_payload_bytes, 0x42);
	check(auth::seal(public_key(), 1, payload, output) == auth::maximum_bytes);
	check(auth::open(public_key(), output));
	check(auth::seal(public_key(), 0, payload, output) == 0);
	check(auth::seal(public_key(), 1, {}, output) == 0);
	check(auth::seal(public_key(), 1, payload, std::span(output).first(output.size() - 1)) == 0);
	payload.push_back(0);
	check(auth::seal(public_key(), 1, payload, output) == 0);
}

PBD_TEST(PbdPeerAuth, RejectsReplaysButAllowsReorderingWithinTheWindow)
{
	auth::sequence_window window;
	check(!window.accept(0));
	check(window.accept(100));
	check(window.accept(98));
	check(!window.accept(98));
	check(!window.accept(100));
	check(window.accept(37));
	check(!window.accept(36));
	check(window.accept(164));
	check(!window.accept(100));
	check(window.accept(163));
	check(window.accept(std::numeric_limits<std::uint64_t>::max()));
	check(!window.accept(164));
	check(!window.accept(1));
	check(!window.accept(std::numeric_limits<std::uint64_t>::max()));
}

PBD_TEST(PbdPeerAuth, NoKeyPinsTheEndpointAndKeepsTheOlderWire)
{
	auth::state peer;
	check(!peer.can_learn());
	const auto raw = message::encode_end({0, session, 7, 900});
	std::array<std::uint8_t, auth::maximum_bytes> output;
	const auto size = peer.send(raw, output);
	check(size == raw.size() && std::equal(raw.begin(), raw.end(), output.begin()));
	const auto received = peer.receive(raw);
	check(received && received->epoch == 0 && peer.current(received->epoch));
	check(message::read_end(received->payload, 1, session));
}

PBD_TEST(PbdPeerAuth, SpoofCannotConsumeSequenceAndRetiredQueuedPacketsStayRetired)
{
	auth::state peer;
	check(peer.install(public_key(), 1));
	check(peer.can_learn());
	const auto raw = message::encode_end({0, session, 7, 900});
	auth::state sender;
	check(sender.install(public_key(), 0));
	bytes valid(auth::maximum_bytes);
	valid.resize(sender.send(raw, valid));
	auto spoof = valid;
	spoof.back() ^= 1;
	check(!peer.receive(spoof));
	const auto received = peer.receive(valid);
	check(received && message::read_end(received->payload, 1, session));
	check(!peer.receive(valid));
	check(!peer.install(public_key(), 1), "reinstalling one key must not reset its replay window");
	check(!peer.receive(valid));
	auto rotated = public_key();
	rotated[0] ^= 1;
	check(peer.install(rotated, 1));
	check(!peer.current(received->epoch));
	check(!peer.receive(valid));
	std::array<std::uint8_t, auth::maximum_bytes> output;
	const auto size = peer.send(raw, output);
	const auto outgoing_key = auth::direction_key(rotated, 1);
	check(outgoing_key);
	const auto opened = auth::open(*outgoing_key, std::span(output).first(size));
	check(opened && opened->sequence == 1);
}

PBD_TEST(PbdPeerAuth, ReflectionCannotConsumeAnInboundSequence)
{
	const auto left_key = auth::direction_key(public_key(), 0), right_key = auth::direction_key(public_key(), 1);
	check(left_key && right_key);
	check(bytes(left_key->begin(), left_key->end()) == from_hex("aa29577698a5b5e7b08e89b58f9d1f901698cb87a714a27d71eb5e6fcb7da0e8"));
	check(bytes(right_key->begin(), right_key->end()) == from_hex("3177e323497cf4d890249d52f6a64b7995950a29b3a2df7833b148eb01f378cd"));
	check(!auth::direction_key(public_key(), 2));
	auth::state left, right;
	check(left.install(public_key(), 0) && right.install(public_key(), 1));
	const auto raw_left = message::encode_end({0, session, 7, 900});
	const auto raw_right = message::encode_end({1, session, 7, 900});
	bytes outbound(auth::maximum_bytes), inbound(auth::maximum_bytes);
	outbound.resize(left.send(raw_left, outbound));
	inbound.resize(right.send(raw_right, inbound));
	check(!left.receive(outbound));
	check(left.receive(inbound));
	check(!left.receive(inbound));
	check(right.receive(outbound));
}

PBD_TEST(PbdPeerAuth, RetirementDoesNotDowngradeAndFreshContinuationRestartsTheSequence)
{
	auth::state left, right;
	check(left.install(public_key(), 0) && right.install(public_key(), 1));
	const auto raw = message::encode_end({1, session, 7, 900});
	bytes old(auth::maximum_bytes);
	old.resize(right.send(raw, old));
	const auto admitted = left.receive(old);
	check(admitted);
	left.retire();
	check(left.retired() && !left.can_learn() && !left.current(admitted->epoch));
	check(!left.receive(old) && !left.receive(raw));
	bytes output(auth::maximum_bytes);
	check(left.send(raw, output) == 0);
	check(!left.install(public_key(), 0));
	auto fresh = public_key();
	fresh[0] ^= 1;
	check(left.install(fresh, 0) && right.install(fresh, 1));
	check(!left.retired() && left.can_learn() && !left.receive(old));
	output.resize(right.send(raw, output));
	check(auth::detail::load<std::uint64_t>(output, 12) == 1);
	const auto continued = left.receive(output);
	check(continued);
	check(!left.install(public_key(), 0) && !right.install(public_key(), 1),
		"a previously used key cannot restart either sequence after an intervening set");
	check(left.current(continued->epoch) && left.can_learn() && !left.receive(old));
	output.resize(auth::maximum_bytes);
	output.resize(right.send(raw, output));
	check(auth::detail::load<std::uint64_t>(output, 12) == 2);
	check(left.receive(output));
}

PBD_TEST(PbdPeerKeyPipe, WritesExactBoundedRequestAndRequiresThePrivateResponseVersion)
{
	namespace pipe = pbd::peer_key_pipe;
	pipe::nonce nonce{};
	nonce.fill(0x42);
	const auto request = pipe::request(0, session, "set-a", nonce);
	check(request);
	check(std::string_view(reinterpret_cast<const char*>(request->data()), 8) == "PBDKEY01");
	check(auth::detail::load<std::uint32_t>(*request, 8) == 1);
	check(auth::detail::load<std::uint32_t>(*request, 12) == 0);
	check(auth::detail::load<std::uint32_t>(*request, 16) == session);
	check(auth::detail::load<std::uint32_t>(*request, 20) == 5);
	check(std::equal(nonce.begin(), nonce.end(), request->begin() + 24));
	check(std::string_view(reinterpret_cast<const char*>(request->data() + 56), 5) == "set-a");
	check(std::all_of(request->begin() + 61, request->end(), [](auto c)
		{
			return c == 0;
		}));
	check(!pipe::request(2, session, "set-a", nonce));
	check(!pipe::request(0, session, "", nonce));
	check(!pipe::request(0, session, std::string(65, 'a'), nonce));
	check(!pipe::request(0, session, "bad id", nonce));
	pipe::response_bytes response{};
	std::copy(pipe::magic.begin(), pipe::magic.end(), response.begin());
	auth::detail::store(std::span(response), 8, std::uint32_t{1});
	auth::detail::store(std::span(response), 12, std::uint32_t{1});
	const auto secret = public_key();
	std::copy(secret.begin(), secret.end(), response.begin() + 16);
	check(pipe::read_response(response) == secret);
	for (std::size_t length = 0; length < response.size(); ++length)
		check(!pipe::read_response(std::span(response).first(length)));
	for (const auto offset : {0, 8, 12})
	{
		auto bad = response;
		bad[offset] ^= 1;
		check(!pipe::read_response(bad));
	}
}

PBD_TEST(PbdPeerKeyPipe, FailedAcknowledgementRetiresTheInstalledEpochWithoutDowngrading)
{
	for (const bool acknowledged : {false, true})
	{
		auth::state receiver, sender;
		check(receiver.install(public_key(), 0) && sender.install(public_key(), 1));
		const auto raw = message::encode_end({1, session, 7, 900});
		bytes packet(auth::maximum_bytes);
		packet.resize(sender.send(raw, packet));
		const auto queued = receiver.receive(packet);
		check(queued);
		check(pbd::peer_key_pipe::complete_install(receiver, acknowledged) == acknowledged);
		check(receiver.current(queued->epoch) == acknowledged);
		check(receiver.can_learn() == acknowledged && receiver.retired() == !acknowledged);
		packet.resize(auth::maximum_bytes);
		packet.resize(sender.send(raw, packet));
		check(receiver.receive(packet).has_value() == acknowledged);
		check(!receiver.receive(raw), "a failed private handoff cannot fall back to unauthenticated input");
		bytes output(auth::maximum_bytes);
		check((receiver.send(raw, output) != 0) == acknowledged);
	}
}

PBD_TEST(PbdPeerRecovery, RealAuthenticatedInputsResumeAfterSixtySecondsAndProveBothRoutes)
{
	using namespace pbd::peer_recovery;
	const pbd::peer_endpoint::endpoint direct{pbd::peer_endpoint::family::v4, {10, 0, 0, 2}, 0x1234};
	const pbd::peer_endpoint::endpoint relay{pbd::peer_endpoint::family::v4, {127, 0, 0, 1}, 0x2345};
	auth::state sender, receiver;
	check(sender.install(public_key(), 0) && receiver.install(public_key(), 1));
	const context identity{"session", "set", session, receiver.epoch()};
	state link;
	link.configure(identity, direct, 1000, 47002);
	message::input_message input;
	input.role = 0;
	input.confirmed = 99;
	input.combat = {11, 22, 33};
	input.sync.session = session;
	input.sync.boundary = 100;
	input.segment.number = 1;
	input.segment.scene = 17;
	input.count = 1;
	input.inputs[0] = {100, 0x1234};
	std::array<std::uint8_t, message::max_input_bytes> payload{};
	const auto payload_size = message::encode_input(input, payload);
	check(payload_size);
	const auto packet = [&]
	{
		bytes result(auth::maximum_bytes);
		result.resize(sender.send(std::span(payload).first(payload_size), result));
		return result;
	};
	const auto accept = [&](const bytes& wire, const pbd::peer_endpoint::endpoint& from, std::uint64_t now)
	{
		const auto received = receiver.receive(wire);
		check(received);
		message::input_message decoded;
		check(message::read_session_input(received->payload, 1, session, {0, 1000, 100, 16, 1}, decoded) == message::input_check::valid);
		check(decoded.confirmed == 99 && decoded.combat == input.combat && decoded.inputs[0].tick == 100 && decoded.inputs[0].value == 0x1234);
		check(from == direct || link.allows(from, received->epoch, now));
		link.received(received->epoch, now);
		if (link.allows(from, received->epoch, now))
			check(link.prove(from, received->epoch, now));
	};
	accept(packet(), direct, 1000);
	const auto credit = link.paused(1000);
	link.poll(501000);
	for (std::uint64_t elapsed = 1000000; elapsed <= 60000000; elapsed += 1000000)
	{
		packet(); // The direct path drops complete authenticated datagrams; no neutral input or replacement key is invented.
		link.poll(1000 + elapsed);
		check(link.traffic().recovering && !link.expired(1000 + elapsed));
	}
	check(link.apply({identity, 1, operation::stage, 1}, relay, {}, 60002000));
	accept(packet(), direct, 60003000); // A faster old-route packet must not consume the candidate's distinct sequence.
	const auto candidate_packet = packet();
	accept(candidate_packet, relay, 60004000);
	check(!receiver.receive(candidate_packet));
	check(link.get().verified_proposal == 1 && link.get().active == direct);
	check(link.apply({identity, 2, operation::commit, 1}, {}, {}, 60005000));
	check(link.get().active == relay && link.get().identity == identity && receiver.epoch() == identity.epoch);
	check(link.timeout(1000, 60005000, 60000000, credit) == wait_result::pending);
	auto corrupt = packet();
	corrupt.back() ^= 1;
	check(!receiver.receive(corrupt));
}

PBD_TEST(PbdPeerRecovery, BothSidesKeepInputAndExitWaitsThroughAOneWaySixtySecondOutage)
{
	using namespace pbd::peer_recovery;
	state legacy;
	check(legacy.timeout(1000, 60001000, 60000000, 0) == wait_result::pending);
	check(legacy.timeout(1000, 60002000, 60000000, 0) == wait_result::deadline);
	for (unsigned lost_sender = 0; lost_sender < 2; ++lost_sender)
	{
		auth::state peers[2];
		state links[2];
		std::uint64_t credit[2]{};
		for (unsigned side = 0; side < 2; ++side)
		{
			check(peers[side].install(public_key(), side));
			links[side].configure({"session", "set", session, peers[side].epoch()},
				{pbd::peer_endpoint::family::v4, {10, 0, 0, static_cast<std::uint8_t>(2 - side)}, 0x1234}, 1000);
			links[side].received(peers[side].epoch(), 1000);
			credit[side] = links[side].paused(1000);
		}
		const auto transmit = [&](unsigned side, std::uint64_t now, bool dropped)
		{
			message::input_message input;
			input.role = side;
			input.confirmed = 99;
			input.combat = {11, 22, 33};
			input.sync.session = session;
			input.sync.send_us = now;
			input.sync.boundary = 100;
			input.segment.number = 1;
			input.segment.scene = 17;
			input.gate = {1, 101, 100};
			input.count = 1;
			input.inputs[0] = {100, static_cast<std::uint16_t>(0x1234 + side)};
			std::array<std::uint8_t, message::max_input_bytes> payload{};
			const auto size = message::encode_input(input, payload);
			bytes wire(auth::maximum_bytes);
			wire.resize(peers[side].send(std::span(payload).first(size), wire));
			check(!wire.empty());
			if (dropped)
				return;
			const auto received = peers[1 - side].receive(wire);
			check(received);
			message::input_message decoded;
			check(message::read_session_input(received->payload, 1 - side, session, {0, 1000, 100, 16, 1}, decoded) == message::input_check::valid);
			check(decoded.combat == input.combat && decoded.inputs[0].value == input.inputs[0].value);
			check(decoded.sync.boundary == 100 && decoded.gate.segment == 1 && decoded.gate.ready == 101 && decoded.gate.through == 100);
			links[1 - side].received(received->epoch, now);
		};
		pbd::session_wait::leave_read leave_reads[2];
		constexpr std::uint64_t resumed = 60251000; // Full60s blackout plus250ms transit after the original1000us timestamp.
		for (std::uint64_t now = 1000; now <= resumed; now += 250000)
		{
			for (unsigned side = 0; side < 2; ++side)
				transmit(side, now, side == lost_sender && now < resumed);
			for (unsigned side = 0; side < 2; ++side)
			{
				links[side].poll(now);
				// These are the production input-hold/exit-gate60s and segment120s timeout calls.
				check(links[side].timeout(1000, now, 60000000, credit[side]) == wait_result::pending);
				check(links[side].timeout(1000, now, 120000000, credit[side]) == wait_result::pending);
				const auto pad = (now - 1000) % 500000 ? pbd::session_wait::leave_button : std::uint16_t{0};
				auto& read = leave_reads[side];
				read = pbd::session_wait::read_leave(std::chrono::milliseconds((now - 1000) / 1000),
					pbd::session_wait::leave_kind::set, pad, read);
				check(!read.chosen, "a Circle attack aborted same-game input/exit recovery");
			}
		}
		for (unsigned side = 0; side < 2; ++side)
		{
			check(links[side].matches("session", "set", session, peers[side].epoch()));
			check(!links[side].traffic().recovering);
			credit[side] = links[side].paused(resumed);
		}
		// A fresh blocked wait gets its own lease. Frozen authenticated keepalives cannot extend it indefinitely.
		for (std::uint64_t elapsed = 0; elapsed <= 90000000; elapsed += 250000)
		{
			const auto now = resumed + elapsed;
			for (unsigned side = 0; side < 2; ++side)
				transmit(side, now, side == lost_sender);
			for (unsigned side = 0; side < 2; ++side)
			{
				links[side].poll(now);
				const auto expected = elapsed < 90000000 ? wait_result::pending : wait_result::recovery_exhausted;
				check(links[side].timeout(resumed, now, 60000000, credit[side]) == expected);
				check(links[side].timeout(resumed, now, 120000000, credit[side]) == expected);
			}
		}
	}
}

PBD_TEST(PbdPeerRecovery, ASecondHandoverProofSurvives250MillisecondsAlongsideFastActiveTraffic)
{
	using namespace pbd::peer_recovery;
	const pbd::peer_endpoint::endpoint direct{pbd::peer_endpoint::family::v4, {10, 0, 0, 2}, 0x1234};
	const pbd::peer_endpoint::endpoint active{pbd::peer_endpoint::family::v4, {127, 0, 0, 1}, 0x2345};
	const pbd::peer_endpoint::endpoint candidate{pbd::peer_endpoint::family::v4, {127, 0, 0, 1}, 0x3456};
	auth::state sender, receiver;
	check(sender.install(public_key(), 0) && receiver.install(public_key(), 1));
	const context identity{"session", "set", session, receiver.epoch()};
	state link;
	link.configure(identity, direct, 1000);
	check(link.apply({identity, 1, operation::stage, 1}, active, {}, 1000) && link.prove(active, identity.epoch, 1000));
	check(link.apply({identity, 2, operation::commit, 1}, {}, {}, 1000));
	check(link.apply({identity, 3, operation::stage, 3}, candidate, {}, 1000));
	message::input_message input;
	input.role = 0;
	input.sync.session = session;
	input.sync.boundary = 100;
	input.segment.number = 1;
	input.segment.scene = 17;
	input.count = 1;
	input.inputs[0] = {100, 0x1234};
	std::array<std::uint8_t, message::max_input_bytes> payload{};
	const auto size = message::encode_input(input, payload);
	std::uint64_t sent = 0;
	const auto packet = [&]
	{
		bytes wire(auth::maximum_bytes);
		wire.resize(sender.send(std::span(payload).first(size), wire));
		check(!wire.empty());
		++sent;
		return wire;
	};
	const auto accept = [&](const bytes& wire, endpoint from, std::uint64_t now)
	{
		const auto received = receiver.receive(wire);
		check(received, "fast active traffic must not age the slower candidate out of authentication");
		check(from == active || link.allows(from, received->epoch, now));
		message::input_message decoded;
		check(message::read_session_input(received->payload, 1, session, {0, 1000, 100, 16, 1}, decoded) == message::input_check::valid);
		check(decoded.inputs[0].value == 0x1234);
		if (from == candidate)
			check(link.prove(from, received->epoch, now));
	};
	struct delayed
	{
		unsigned step;
		endpoint from;
		bytes wire;
	};
	std::vector<delayed> pending;
	unsigned candidate_packets = 0, previous_packets = 0;
	bool turn = false;
	for (unsigned step = 0; step < 125; ++step)
	{
		const auto now = 1000 + std::uint64_t{step} * 10000;
		if (step < 100)
		{
			accept(packet(), active, now); // The active path stays fast during make-before-break.
			const auto target = resend_target(link.probe(identity.epoch), link.previous(identity.epoch, now), turn);
			check(target);
			pending.push_back({step + 25, *target, packet()}); // 100Hz held retransmits, 250ms extra latency.
			candidate_packets += *target == candidate;
			previous_packets += *target == direct;
		}
		if (!pending.empty() && pending.front().step == step)
		{
			accept(pending.front().wire, pending.front().from, now);
			pending.erase(pending.begin());
		}
	}
	check(pending.empty() && sent == 200 && candidate_packets == 50 && previous_packets == 50);
	check(link.get().verified_proposal == 3 && link.get().active == active);
	check(link.apply({identity, 4, operation::commit, 3}, {}, {}, 1251000));
	check(link.get().active == candidate && receiver.epoch() == identity.epoch);
}

PBD_TEST(PbdPeerRecovery, ThreeHoursOfAuthenticatedTrafficSurvivesAsymmetricLossAndJitter)
{
	using namespace pbd::peer_recovery;
	struct delayed_packet
	{
		std::uint64_t arrival;
		unsigned receiver;
		bytes wire;
	};
	for (const unsigned loss : {5, 20, 50})
	{
		auth::state peers[2];
		state links[2];
		std::uint64_t accepted[2]{};
		for (unsigned side = 0; side < 2; ++side)
		{
			check(peers[side].install(public_key(), side));
			links[side].configure({"session", "set", session, peers[side].epoch()},
				{pbd::peer_endpoint::family::v4, {10, 0, 0, static_cast<std::uint8_t>(2 - side)}, 0x1234}, 1000);
		}
		std::vector<delayed_packet> pending;
		// Virtual time exercises the real MAC, replay window and input decoder, without a wall-clock three-hour test.
		// The frozen boundary/input/hash tuple is the same one retransmitted while a game is held.
		for (std::uint64_t step = 0; step <= 108004; ++step)
		{
			const auto elapsed = step * 100000, now = 1000 + elapsed;
			if (step <= 108000)
				for (unsigned side = 0; side < 2; ++side)
				{
					message::input_message input;
					input.role = side;
					input.confirmed = 99;
					input.combat = {11, 22, 33};
					input.sync.session = session;
					input.sync.send_us = now;
					input.sync.boundary = 100;
					input.segment.number = 1;
					input.segment.scene = 17;
					input.count = 1;
					input.inputs[0] = {100, static_cast<std::uint16_t>(0x1234 + side)};
					std::array<std::uint8_t, message::max_input_bytes> payload{};
					const auto size = message::encode_input(input, payload);
					bytes wire(auth::maximum_bytes);
					wire.resize(peers[side].send(std::span(payload).first(size), wire));
					check(!wire.empty() && auth::detail::load<std::uint64_t>(wire, 12) == step + 1);
					const bool blackout = side == 0 ? elapsed >= 1800000000 && elapsed < 1860000000 :
					                                  elapsed >= 3600000000 && elapsed < 3660000000;
					const unsigned direction_loss = side == 0 ? loss : 5;
					if (!blackout && (step * 37 + side * 17) % 100 >= direction_loss)
						pending.push_back({now + 100000 + (step * 7919 + side * 8347) % 150001, 1 - side, std::move(wire)});
				}
			for (auto& link : links)
				link.poll(now);
			for (std::size_t index = 0; index < pending.size();)
			{
				if (pending[index].arrival > now)
				{
					++index;
					continue;
				}
				const auto side = pending[index].receiver;
				const auto received = peers[side].receive(pending[index].wire);
				check(received);
				message::input_message decoded;
				check(message::read_session_input(received->payload, side, session, {0, 1000, 100, 16, 1}, decoded) == message::input_check::valid);
				check(decoded.confirmed == 99 && decoded.combat == std::array<std::uint64_t, 3>{11, 22, 33});
				check(decoded.inputs[0].tick == 100 && decoded.inputs[0].value == 0x1234 + (1 - side));
				links[side].received(received->epoch, now);
				++accepted[side];
				pending[index] = std::move(pending.back());
				pending.pop_back();
			}
			check(!links[0].expired(now) && !links[1].expired(now));
		}
		check(pending.empty());
		for (unsigned side = 0; side < 2; ++side)
		{
			check(accepted[side] > 40000 && links[side].get().request_id >= 1);
			check(!links[side].traffic().recovering && links[side].paused(10800401000) >= 59000000);
			check(links[side].matches("session", "set", session, peers[side].epoch()));
		}
	}
}

// A failed send direction is detected through current-key echoes; a peer receiving nothing needs launcher coordination.
// Both cases recover through the same stage/proof/commit sequence, including fresh continuation keys.
PBD_TEST(PbdPeerRecovery, FirstInputLossRecoversBothDirectionsAndFreshContinuationKeys)
{
	using namespace pbd::peer_recovery;
	const endpoint direct{pbd::peer_endpoint::family::v4, {10, 0, 0, 2}, 0x1234};
	const endpoint relay{pbd::peer_endpoint::family::v4, {127, 0, 0, 1}, 0x2345};
	const auto document = [](const state& link, std::uint64_t now)
	{
		auto parsed = pbd::json::detail::json_parser(status_json(link.get(), now)).parse();
		check(parsed.value && parsed.value->find("sent") && parsed.value->find("sentAgoMs") && parsed.value->find("peerHeard"),
			"the status does not say whether this game is sending under the key");
		return std::move(*parsed.value);
	};
	for (const bool continuation : {false, true})
		for (unsigned delivery = 0; delivery < 3; ++delivery)
		{
			std::array<auth::state, 2> peers;
			std::array<state, 2> links;
			std::array<context, 2> identities;
			// The newest send time each side has received from the other, which its own inputs echo. A continuation's
			// previous key leaves its last old-key echo in place.
			std::array<std::uint64_t, 2> newest{};
			for (unsigned side = 0; side < 2; ++side)
			{
				check(peers[side].install(public_key(), side));
				links[side].configure({"session", "old-set", session, peers[side].epoch()}, direct, 1000);
				if (continuation)
				{
					links[side].sent(peers[side].epoch(), 900);
					links[side].received(peers[side].epoch(), 1000, 900);
					newest[side] = 950;
					auto next_key = public_key();
					next_key[0] ^= 1;
					check(peers[side].install(next_key, side));
				}
				identities[side] = {"session", "set", session, peers[side].epoch()};
				links[side].configure(identities[side], direct, 2000);
				links[side].poll(60000000);
				check(!links[side].get().recovering, "healthy boot/key staging was counted as an active path fault");
				const auto booted = document(links[side], 60000000);
				check(!booted.find("sent")->truth && booted.find("sentAgoMs")->text == "0",
					"a game that has sent nothing under the key was reported as sending");
			}
			const auto send = [&](unsigned side, std::uint64_t now, bool deliver, bool via_relay)
			{
				message::input_message input;
				input.role = side;
				input.confirmed = 99;
				input.combat = {11, 22, 33};
				input.sync.session = session;
				input.sync.send_us = now;
				input.sync.echo_us = newest[side];
				input.sync.boundary = 100;
				input.segment.number = 1;
				input.segment.scene = 17;
				input.count = 1;
				input.inputs[0] = {100, static_cast<std::uint16_t>(0x1234 + side)};
				std::array<std::uint8_t, message::max_input_bytes> raw{};
				const auto size = message::encode_input(input, raw);
				check(size);
				bytes wire(auth::maximum_bytes);
				wire.resize(peers[side].send(std::span(raw).first(size), wire));
				check(!wire.empty());
				links[side].sent(peers[side].epoch(), now);
				if (!deliver)
					return;
				const auto other = 1 - side;
				const auto packet = peers[other].receive(wire);
				check(packet);
				message::input_message decoded;
				check(message::read_session_input(packet->payload, other, session, {0, 1000, 100, 16, 1}, decoded) == message::input_check::valid);
				check(decoded.count == 1 && decoded.inputs[0].tick == 100 && decoded.inputs[0].value == 0x1234 + side && decoded.combat == input.combat);
				check(message::input_echo_us(packet->payload) == decoded.sync.echo_us && decoded.sync.echo_us == newest[side]);
				if (via_relay)
					check(links[other].allows(relay, packet->epoch, now) && links[other].prove(relay, packet->epoch, now));
				newest[other] = decoded.sync.send_us;
				links[other].received(packet->epoch, now, message::input_echo_us(packet->payload));
			};
			constexpr std::uint64_t activated = 60001000;
			for (std::uint64_t elapsed = 0; elapsed <= 60000000; elapsed += 250000)
			{
				for (unsigned side = 0; side < 2; ++side)
					send(side, activated + elapsed, delivery == side + 1, false);
				for (auto& link : links)
				{
					link.poll(activated + elapsed);
					check(link.timeout(activated, activated + elapsed, 180000000, 0) == wait_result::pending);
				}
			}
			if (delivery)
			{
				// delivery 1: side 0's inputs reach side 1, side 1's are lost.
				const unsigned hears = delivery == 1 ? 1 : 0;
				check(links[hears].get().recovering && links[hears].get().request_id == 1,
					"the side whose sends never arrive did not ask for recovery");
				check(!links[1 - hears].get().recovering && links[1 - hears].get().request_id == 0,
					"a side that never heard the peer counted it as a lost path");
			}
			else
				for (const auto& link : links)
				{
					check(!link.get().recovering && link.get().request_id == 0, "an unheard peer is indistinguishable from one still loading");
					// Both games have sent for a minute and heard nothing: what the launcher stages the relay from.
					const auto waiting = document(link, activated + 60000000);
					check(waiting.find("sent")->truth && !waiting.find("peerHeard")->truth && waiting.find("sentAgoMs")->text == "60000",
						"the status did not say how long this game has sent unheard");
				}
			constexpr auto restored = activated + 60250000;
			for (unsigned side = 0; side < 2; ++side)
				check(links[side].apply({identities[side], 1, operation::stage, 1}, relay, {}, restored));
			send(0, restored + 1, true, true);
			send(1, restored + 2, true, true);
			for (unsigned side = 0; side < 2; ++side)
				check(links[side].apply({identities[side], 2, operation::commit, 1}, {}, {}, restored + 3));
			// The first inputs over the committed relay echo each side's sends.
			send(0, restored + 4, true, false);
			send(1, restored + 5, true, false);
			for (unsigned side = 0; side < 2; ++side)
			{
				check(!links[side].get().recovering && links[side].get().active == relay && links[side].get().identity == identities[side]);
				check(peers[side].epoch() == identities[side].epoch && !links[side].expired(restored + 5));
				check(document(links[side], restored + 5).find("peerHeard")->truth);
			}
		}
}
