#include "Emu/PBD/pbd_game_overlay_parser.h"
#include "lobby_set_driver_model.h"
#include "Emu/PBD/pbd_peer_auth_state.h"
#include "Emu/PBD/pbd_peer_message.h"
#include "Emu/PBD/pbd_rank_screens.h"
#include "Emu/PBD/pbd_replay_switches.h"

#include <algorithm>
#include <map>

using pbd::test::check;
using namespace lobby_test_state;

PBD_TEST(PbdLobbySetDriver, HealthProgressUsesOnlyConfirmedUnfinishedRounds)
{
	sample_document test(420);
	check(test.begin() && !test.driver.confirmed_progress());
	check(test.record(0, phase::vs) && test.record(1, phase::character_intro) && test.publish(2));
	check(!test.driver.confirmed_progress(), "selection and character intro cannot supply a live round score");
	check(test.record(2, phase::round_intro) && test.record(3, phase::fight) && test.publish(4));
	const pbd::session_health::progress initial{1, {0, 0}};
	check(test.driver.confirmed_progress() == initial);
	check(test.record(4, phase::round_result, 1, 0));
	check(test.driver.confirmed_progress() == initial, "a speculative round win escaped before confirmation");
	check(test.record(4, phase::round_result, 0, 1) && test.publish(5));
	check(test.driver.confirmed_progress() == pbd::session_health::progress{1, {0, 1}},
		"corrected confirmation did not replace the speculative winner");
	check(test.record(5, phase::round_intro) && test.record(6, phase::fight) && test.publish(7));
	check(test.record(7, phase::round_result, 3, 1) && !test.publish(8));
	check(!test.driver.confirmed_progress(), "a refused native result retained usable health evidence");
	sample_document decided(423);
	check(decided.begin() && decided.record(0, phase::vs) && decided.record(1, phase::character_intro));
	for (unsigned round = 0; round < 3; ++round)
	{
		const auto boundary = 2 + round * 3;
		check(decided.record(boundary, phase::round_intro) && decided.record(boundary + 1, phase::fight));
		check(decided.record(boundary + 2, phase::round_result, round + 1, 0));
	}
	check(decided.publish(11) && !decided.driver.complete() && !decided.driver.confirmed_progress(),
		"the deciding round must clear unfinished progress before the later match-end observation");
	sample_document completed(421), drawn(422);
	check(completed.begin() && drawn.begin());
	completed.won_game();
	drawn.drawn_game();
	check(!completed.driver.confirmed_progress() && !drawn.driver.confirmed_progress(),
		"a decided game must use its agreed result rather than unfinished-round health");
}

PBD_TEST(PbdLobbySetDriver, ConfirmedRefusalRetainsTheRejectedObservationRatherThanTheNewestSample)
{
	for (unsigned variant = 0; variant < 3; ++variant)
	{
		sample_document test(410 + variant);
		check(test.begin());
		check(test.record(0, phase::vs) && test.record(1, phase::character_intro));
		check(test.record(2, phase::round_intro) && test.record(3, phase::fight));
		check(test.publish(4) && !test.driver.last_refusal());
		observation bad;
		bad.boundary = 4;
		bad.native_phase = variant ? phase::round_result : phase::loading;
		bad.phase_verified = variant != 0;
		bad.result_verified = variant != 2;
		auto picks = applied(17, 12, 32);
		picks.pending_phase = variant ? 6 : 13;
		check(test.driver.record(bad, picks));
		check(test.record(5, phase::fight)); // A newer speculative sample cannot explain an earlier confirmed refusal.
		check(!test.publish(6));
		const auto rejected = test.driver.last_refusal();
		check(rejected && rejected->phase.boundary == 4 && rejected->picks.pending_phase == picks.pending_phase);
		check(rejected->phase.phase_verified == bad.phase_verified && rejected->phase.result_verified == bad.result_verified);
		check(rejected->reason == (variant == 0 ? refusal::phase_unverified : variant == 1 ? refusal::outcome_invalid :
																							 refusal::result_unverified));
		check(!test.driver.complete() && !test.driver.sealed_game());
		const auto serial = test.events.last_event();
		check(detail::is(test.read(serial).find("code"), "native_result_unverified"));
		check(!test.publish(7) && test.events.last_event() == serial && test.driver.last_refusal()->phase.boundary == 4);
	}
}

PBD_TEST(PbdLobbySetDriver, AsymmetricTerminalLossDrainsBothResultsBeforeKeyRetirement)
{
	namespace auth = pbd::peer_auth;
	namespace message = pbd::peer_message;
	sample_document left(401), right(402);
	check(left.begin() && right.begin());
	auth::key secret{};
	secret.fill(0x42); // Public test key.
	auth::state a, b;
	check(a.install(secret, 0) && b.install(secret, 1));
	const auto announcement = [&](auth::state& sender, std::uint32_t role)
	{
		message::input_message value;
		value.role = role;
		value.confirmed = 10;
		value.sync.session = 123;
		value.segment = {9, 11, 1, 0, 17, 50, 49};
		value.digests.state_segment = 9;
		std::array<std::uint8_t, message::max_input_bytes> raw{};
		const auto count = message::encode_input(value, raw);
		std::vector<std::uint8_t> bytes(auth::maximum_bytes);
		bytes.resize(sender.send(std::span(raw).first(count), bytes));
		return bytes;
	};
	// B's exit reaches A. Every A-to-B announcement during this burst is dropped, including A's final barrier send.
	for (unsigned dropped = 0; dropped < 4; ++dropped)
	{
		check(a.receive(announcement(b, 1)));
		check(!announcement(a, 0).empty());
	}
	left.won_game();
	check(left.driver.complete() && left.driver.sealed_game()->terminal && !right.driver.complete());
	check(!pbd::lobby_set::terminal_results_acknowledged(left.driver.complete(), true, command_kind::choice_rejected));
	check(a.can_learn() && b.can_learn());
	// The menu's retained authenticated resend gets B through the exit after connectivity returns.
	const auto resent = announcement(a, 0);
	const auto received = b.receive(resent);
	check(received);
	message::input_message decoded;
	check(message::read_session_input(received->payload, 1, 123, {0, 100, 11, 16, 9}, decoded) == message::input_check::valid);
	check(decoded.segment.number == 9 && decoded.segment.first_index == 11);
	check(!b.receive(resent));
	right.won_game();
	check(left.publish(12) && right.publish(12));
	for (auto* peer : {&left, &right})
	{
		check(peer->driver.complete() && peer->events.last_event() == 4);
		check(detail::is(peer->read(3).find("type"), "game_result"));
		check(detail::is(peer->read(4).find("type"), "set_ended"));
	}
	// Only now can the service agree on both results and supply each side's post-set command.
	for (auto* peer : {&left, &right})
	{
		command agreed;
		agreed.serial = 1;
		agreed.kind = command_kind::show_post_set;
		check(peer->events.acknowledge(agreed));
		check(pbd::lobby_set::terminal_results_acknowledged(peer->driver.complete(), peer->driver.sealed_game()->terminal, agreed.kind));
	}
	a.retire();
	b.retire();
	check(a.retired() && b.retired() && !b.receive(resent));
	check(announcement(a, 0).empty());
	secret[0] ^= 1;
	check(a.install(secret, 0) && b.install(secret, 1));
	check(!b.receive(resent) && b.receive(announcement(a, 0)));
}

PBD_TEST(PbdLobbySetDriver, InitialResourcePolicyIsRecordedAtTheCleanOrigin)
{
	sample_document test(350);
	pbd::lobby_set::clean_start start{123, 7, std::string(64, 'b'), "NPUB30009", true, true};
	start.direct_start = true;
	check(test.driver.begin(0, 0x8000007b, std::move(start)));
	check(test.record(0, phase::vs));
	check(test.record(1, phase::character_intro));
	check(test.publish(2));
	const auto started = test.read(1);
	check(started.find("start")->find("inputPolicy")->find("directStart")->truth);
}

PBD_TEST(PbdLobbySetDriver, CurrentGameCapsuleKeepsItsNativeRecordAndRefusesOtherGames)
{
	for (const auto source_game : {2u, 33u, 65535u})
		for (unsigned variant = 0; variant < 6; ++variant)
		{
			const auto bytes = std::make_shared<const std::string>(detail::serialize(json_value::object({{"type", json_value::string("game_start")}, {"draftRevision", json_value::number(2)},
				{"source", json_value::string(variant == 4 ? "fixture-live-runtime" : "live-game")},
				{"game", json_value::number(variant == 5 ? 65536u : source_game)}, {"generation", json_value::number(variant == 1 ? 8 : 7)},
				{"boundary", json_value::number(variant == 2 ? 20 : 0)},
				{"capsule", json_value::object({{"schema", json_value::string("pbd-game-start-capsule-draft-2")},
								{"sessionSeed", json_value::number(variant == 3 ? 124 : 123)}})}})));
			sample_document test(340 + variant, 1, false, false, [bytes]
				{
					return bytes;
				});
			check(test.begin());
			test.won_game();
			const auto started = test.read(1);
			const auto* record = started.find("gameStartCapsule");
			check((record != nullptr) == (variant == 0));
			if (record)
			{
				check(detail::serialize(*record) == *bytes);
				check(started.find("gameStartEmulatorSha256")->text == std::string(64, 'e'));
			}
			check(test.driver.complete());
		}
}

PBD_TEST(PbdLobbySetDriver, AnUnavailableCapsuleKeepsTheOrdinaryGameAndStream)
{
	sample_document test(345, 1, false, false, []() -> std::shared_ptr<const std::string>
		{
			throw 1;
		});
	check(test.begin());
	test.won_game();
	check(!test.read(1).find("gameStartCapsule") && test.driver.complete());
}

static void confirmed_selection_prefix()
{
	{
		// Confirmed selection reaches the stream before the round witness; a natural continuation keeps the same origin law.
		sample_document test(120, 2, true);
		check(test.begin(true, true, true, false, true));
		const auto prepared = test.read(1);
		const auto* start = prepared.find("start");
		check(detail::is(prepared.find("type"), "game_preparing") && !prepared.find("stageId"));
		check(start && start->find("roundWitnessRequired")->truth && !start->find("nativeRound"));
		check(detail::is(start->find("kind"), "clean_character_select") && start->find("originBoundary")->text == "0");
		auto actual = applied(32, 12, 32);
		for (std::uint32_t i = 0; i < 14; ++i)
			check(test.record(i, phase::vs, 0, 0, winner::none, actual));
		check(test.publish(14) && test.events.last_event() == 1);
		check(test.record(14, phase::vs, 0, 0, winner::none, actual));
		check(test.publish(15) && test.events.last_event() == 2);
		for (std::uint32_t i = 15; i < 29; ++i)
			check(test.record(i, phase::vs, 0, 0, winner::none, actual));
		check(test.publish(29) && test.events.last_event() == 2);
		check(test.record(29, phase::character_intro, 0, 0, winner::none, actual));
		check(test.publish(30) && test.events.last_event() == 3);
		const auto prefix = test.read(2);
		check(detail::is(prefix.find("type"), "confirmed_inputs") && prefix.find("firstFrame")->text == "0" &&
			  prefix.find("frameCount")->text == "15" && prefix.find("syncFacts") && prefix.find("appliedData"));
		const auto second = test.read(3);
		check(detail::is(second.find("type"), "confirmed_inputs") && second.find("firstFrame")->text == "15" &&
			  second.find("frameCount")->text == "15" && second.find("syncFacts") && second.find("appliedData"));
		actual.pending_phase = 4;
		actual.round_ordinal = 1;
		actual.round_random = {0x1234, 0xabcd};
		actual.round_verified = true;
		for (std::uint32_t won = 1, boundary = 30; won <= 3; ++won)
		{
			check(test.record(boundary++, phase::round_intro, won - 1, 0, winner::none, actual));
			check(test.record(boundary++, phase::fight, won - 1, 0, winner::none, actual));
			check(test.record(boundary++, phase::round_result, won, 0, winner::none, actual));
		}
		check(test.record(39, phase::match_end, 3, 0, winner::left, actual));
		check(test.publish(39));
		const auto started = test.read(4);
		check(detail::is(started.find("type"), "game_started") && started.find("start")->find("nativeRound") &&
			  started.find("start")->find("roundWitnessRequired")->truth);
		check(test.events.last_event() == 7 && test.driver.sealed_game()->games.left == 1);
		command next;
		next.kind = command_kind::continue_game;
		next.serial = 1;
		next.game_number = 2;
		next.games = {1, 0};
		check(test.driver.continue_game(next, 39, 0x800000b4));
		const auto natural = test.read(9);
		check(detail::is(natural.find("type"), "game_preparing") && natural.find("gameNumber")->text == "2" &&
			  detail::is(natural.find("start")->find("kind"), "natural_vs") &&
			  natural.find("start")->find("originBoundary")->text == "39");
		for (std::uint32_t i = 39; i < 69; ++i)
			check(test.record(i, phase::vs));
		check(test.publish(69) && test.events.last_event() == 11);
		check(test.read(10).find("firstFrame")->text == "0" && test.read(10).find("gameNumber")->text == "2");
		check(test.read(11).find("firstFrame")->text == "15" && test.read(11).find("gameNumber")->text == "2");
	}
}

static void selection_prefix_and_timeout()
{
	confirmed_selection_prefix();
	{
		sample_document test(121, 2, true);
		check(test.begin(true, true, true, false, true));
		for (std::uint32_t i = 0; i < 48; ++i)
			check(test.record(i, phase::vs));
		check(test.publish(48));
		check(test.record(48, phase::round_intro));
		check(!test.publish(49));
		check(detail::is(test.read(5).find("code"), "native_round_start_unverified"));
		sample_document invalid(122);
		check(!invalid.begin(true, true, false, false, true));
		check(detail::is(invalid.read(1).find("code"), "native_round_start_required"));
	}
	{
		// Flushing a live selection prefix does not remove its finite selection-time bound.
		sample_document test(123, 2, true);
		check(test.begin(true, true, true, false, true));
		const auto limit = pbd::lobby_set::presentation_limit(test.events.identity());
		for (std::uint32_t i = 0; i <= limit; ++i)
		{
			check(test.record(i, phase::vs));
			const bool accepted = test.publish(i + 1);
			check(accepted == (i < limit));
		}
		check(detail::is(test.read(test.events.last_event()).find("code"), "presentation_buffer_exceeded"));
	}
}

static void clean_start_options_agreement()
{
	{
		// No truncation/mask of the existing options agreement, and keyed mode changes the clean-start signature.
		constexpr u32 options = 0x12345678u;
		check(pbd::lobby_set::start_policy_digest(options, false) != pbd::lobby_set::start_policy_digest(options, true));
		for (bool keyed : {false, true})
			check(pbd::lobby_set::start_policy_digest(options, keyed, false) != pbd::lobby_set::start_policy_digest(options, keyed, true));
		for (u32 bit = 0; bit < 32; ++bit)
			check(pbd::lobby_set::start_policy_digest(options, false) != pbd::lobby_set::start_policy_digest(options ^ (1u << bit), false));
		for (bool keyed : {false, true})
		{
			sample_document f(keyed ? 101 : 100);
			check(f.begin(true, true, false, keyed));
			check(f.record(0, phase::vs));
			check(f.record(1, phase::character_intro));
			check(f.publish(2));
			const auto start = f.read(1);
			const auto* policy = start.find("start")->find("inputPolicy");
			check(policy && policy->find("keyedDraws") && policy->find("keyedDraws")->kind == json_kind::boolean);
			check(policy->find("keyedDraws")->truth == keyed);
			check(policy->find("banJinpachi") && !policy->find("banJinpachi")->truth);
		}
		sample_document banned(102, 1, false, true);
		check(banned.begin(true, true, false, false, false, true));
		check(banned.record(0, phase::vs));
		check(banned.record(1, phase::character_intro));
		check(banned.publish(2));
		check(banned.read(1).find("start")->find("inputPolicy")->find("banJinpachi")->truth);
		sample_document mismatch(103, 1, false, true);
		check(!mismatch.begin());
		check(detail::is(mismatch.read(1).find("code"), "start_character_rule_mismatch"));
		sample_document bad_pick(104, 1, true, true);
		check(bad_pick.begin(true, true, false, false, false, true));
		check(bad_pick.record(0, phase::vs));
		check(bad_pick.record(1, phase::character_intro, 0, 0, winner::none, applied(17, 81, 32)));
		check(!bad_pick.publish(2));
		check(detail::is(bad_pick.read(bad_pick.events.last_event()).find("code"), "applied_character_rule_mismatch"));
	}
}

static void start_options_and_round_lifecycle()
{
	clean_start_options_agreement();
	{
		// Reload and terminal paths seen in two-PC play: 6/8/9/12 -> scene16/37 -> next round, or 6/8/9/10 -> scene17/37 -> exit.
		lifecycle rounds;
		check(rounds.begin(1, 0, 123, 3, true).refused == refusal::none);
		const auto observe = [&](std::uint32_t boundary, std::uint32_t scene, std::uint32_t pending, std::uint32_t left = 0)
		{
			const auto native = classify(scene, pending, rounds.round_closed(), rounds.match_decided());
			observation o;
			o.boundary = boundary;
			o.native_phase = native.value;
			o.phase_verified = native.verified;
			o.result_verified = true;
			o.score_left = left;
			return rounds.observe(o);
		};
		check(observe(0, 8, 0).refused == refusal::none);
		check(observe(1, 17, 2).refused == refusal::none);
		check(observe(2, 17, 4).values[0].type == kind::round_start);
		check(observe(3, 17, 5).values[0].type == kind::round_fight);
		check(observe(4, 17, 6, 1).values[0].type == kind::round_end);
		check(observe(5, 17, 12, 1).refused == refusal::none);
		const auto reload = observe(6, 16, 37, 1);
		check(reload.refused == refusal::none && reload.count == 0 && rounds.round_closed());
		const auto next = observe(7, 17, 4, 1);
		check(next.refused == refusal::none && next.count == 1 && next.values[0].type == kind::round_start && next.values[0].round == 2);
		check(observe(8, 17, 5, 1).refused == refusal::none);
		check(!classify(16, 37, false).verified && !classify(17, 37, true).verified);
		check(!classify(17, 37, false, true).verified);
		check(observe(9, 17, 6, 2).refused == refusal::none && !rounds.match_decided());
		check(observe(10, 16, 37, 2).refused == refusal::none);
		check(observe(11, 17, 4, 2).values[0].round == 3);
		check(observe(12, 17, 5, 2).refused == refusal::none);
		check(observe(13, 17, 6, 3).refused == refusal::none && rounds.match_decided());
		check(observe(14, 17, 10, 3).refused == refusal::none);
		const auto terminal = observe(15, 17, 37, 3);
		check(terminal.refused == refusal::none && terminal.count == 0 && !rounds.complete());
		observation end;
		end.boundary = 16;
		end.native_phase = phase::match_end;
		end.phase_verified = true;
		end.result_verified = true;
		end.score_left = 3;
		end.outcome = winner::left;
		const auto ended = rounds.observe(end);
		check(ended.refused == refusal::none && ended.count == 1 && ended.values[0].type == kind::match_end && rounds.complete());
		for (const std::uint32_t pending : {13u, 36u, 38u, 39u, 0xffffffffu})
			check(!classify(16, pending, true, true).verified && !classify(17, pending, true, true).verified);
	}
}

static void clean_origin_and_initial_publication()
{
	const auto published = pbd::game_overlay::parse("PBD-MENU-1\ngeneration=8\nvisible=1\nkind=post_set\ncommand=12\nclose=0\ntitle=Results\n");
	check(published.value && published.value->command_serial == 12 && published.value->kind == pbd::game_overlay::panel_kind::post_set);
	check(!pbd::game_overlay::parse("PBD-MENU-1\ngeneration=8\nvisible=0\ncommand=12\ncommand=13\n").value);
	command menu;
	menu.serial = 12;
	menu.kind = command_kind::show_post_set;
	pbd::game_overlay::snapshot panel;
	panel.generation = 8;
	panel.command_serial = menu.serial;
	panel.visible = true;
	panel.close_on_action = false;
	panel.kind = pbd::game_overlay::panel_kind::post_set;
	panel.title = "Results";
	panel.subtitle = "Any translated display text";
	check(pbd::lobby_set::menu_applied(menu, 7, panel));
	check(pbd::lobby_set::menu_applied(menu, 7, *published.value));
	check(!pbd::lobby_set::menu_applied(menu, 8, panel));
	panel.command_serial--;
	check(!pbd::lobby_set::menu_applied(menu, 7, panel));
	panel.command_serial = menu.serial;
	panel.close_on_action = true;
	check(!pbd::lobby_set::menu_applied(menu, 7, panel));
	panel.close_on_action = false;
	menu.kind = command_kind::choice_rejected;
	check(!pbd::lobby_set::menu_applied(menu, 7, panel));
	panel.kind = pbd::game_overlay::panel_kind::choice_rejected;
	menu.reason = "Different from the displayed subtitle";
	check(pbd::lobby_set::menu_applied(menu, 7, panel));
	menu.kind = command_kind::end_set;
	panel.kind = pbd::game_overlay::panel_kind::set_ended;
	check(!pbd::lobby_set::menu_applied(menu, 7, panel));
	panel.visible = false;
	check(pbd::lobby_set::menu_applied(menu, 7, panel));
	check((pbd::lobby_set::pad_record(0x1234) == std::array<std::uint8_t, 8>{0x34, 0x12, 0, 0, 0, 0, 0, 0}));
	check(pbd::lobby_set::base64url({0x34, 0x12, 0, 0, 0, 0, 0, 0}) == "NBIAAAAAAAA");
	{
		sample_document test(1);
		check(!test.begin(false));
		check(!test.driver.healthy());
		const auto event = test.read(1);
		check(detail::is(event.find("type"), "contract_error"));
		check(detail::is(event.find("code"), "selection_driver_unavailable"));
		check(test.events.last_command() == 0);
	}
	{
		sample_document test(2);
		check(test.begin());
		check(test.record(0, phase::vs));
		check(test.record(1, phase::character_intro, 0, 0, winner::none, applied(18, 12, 32)));
		check(test.publish(1));
		check(test.events.last_event() == 0);
		check(!test.publish(2));
		check(detail::is(test.read(1).find("code"), "applied_selection_mismatch"));
	}
}

static void result_and_guard_failures()
{
	{
		sample_document test(3);
		check(test.begin());
		check(test.record(0, phase::vs));
		check(test.record(1, phase::character_intro));
		check(test.record(2, phase::round_intro));
		check(test.record(3, phase::fight));
		check(test.record(4, phase::round_result, 1));
		check(test.record(5, phase::round_intro));
		check(test.record(6, phase::fight));
		check(test.record(7, phase::round_result, 2));
		check(test.record(8, phase::round_intro));
		check(test.record(9, phase::fight));
		check(test.record(10, phase::round_result, 3, 1));
		check(test.record(11, phase::match_end, 3, 1, winner::right));
		check(test.publish(10));
		check(test.events.last_event() == 1);
		check(test.record(10, phase::round_result, 3, 0));
		check(test.record(11, phase::match_end, 3, 0, winner::left));
		check(test.publish(11));
		check(test.driver.complete());
		check(test.events.last_event() == 4);
		const auto inputs = test.read(2), result = test.read(3), ended = test.read(4), started = test.read(1);
		check(detail::is(inputs.find("type"), "confirmed_inputs") && detail::is(inputs.find("side"), "left"));
		check(detail::is(result.find("type"), "game_result") && detail::is(result.find("winner"), "left"));
		check(inputs.find("firstFrame")->text == "0" && inputs.find("frameCount")->text == "11" && result.find("finalFrame")->text == "11");
		check(detail::is(inputs.find("pairedData")->find("encoding"), "pbd-paired-pad-v1"));
		check(detail::is(inputs.find("appliedData")->find("encoding"), "pbd-applied-pad-v1"));
		check(detail::is(inputs.find("syncFacts")->find("encoding"), "pbd-sync-facts-v1"));
		check(detail::is(inputs.find("syncFacts")->find("definition"), "pbd-peer-sync-v3"));
		check(started.find("start")->find("originBoundary")->text == "0");
		check(detail::is(started.find("start")->find("kind"), "clean_character_select"));
		check(detail::is(started.find("start")->find("pairedEncoding"), "pbd-paired-pad-v1"));
		check(detail::is(started.find("start")->find("appliedEncoding"), "pbd-applied-pad-v1"));
		check(detail::is(ended.find("type"), "set_ended"));
		check(started.find("cleanStart")->find("seed")->text == "123" && started.find("cleanStart")->find("generation")->text == "7");
		check(!started.find("cleanStart")->find("checkpointSha256"));
		check(test.events.last_command() == 0);
	}
	{
		sample_document test(4);
		check(test.begin());
		check(test.record(0, phase::vs));
		check(test.publish(1));
		check(!test.publish(2));
		check(detail::is(test.read(1).find("code"), "confirmed_phase_missing"));
	}
	{
		sample_document test(5);
		check(test.begin());
		command status;
		status.kind = command_kind::stream_status;
		status.serial = 1;
		status.status = "paused_slow";
		check(test.driver.apply_stream_status(status));
		check(test.events.last_command() == 1);
		check(detail::is(test.read(1).find("type"), "command_applied"));
		check(test.driver.active());
	}
	{
		sample_document test(6);
		check(test.begin());
		for (std::uint32_t i = 0; i < 4096; ++i)
			check(test.record(i, phase::loading));
		check(!test.record(4096, phase::loading));
		check(detail::is(test.read(1).find("code"), "confirmation_window_exceeded"));
	}
}

static void forfeit_cause_and_waiting_leave()
{
	{
		sample_document test(7);
		check(test.begin(true, true, true));
		check(test.record(0, phase::vs));
		check(test.record(1, phase::character_intro));
		check(test.publish(2));
		check(!std::filesystem::exists(test.directory / "set-event-1.json"));
		auto actual = applied(17, 12, 32);
		actual.pending_phase = 4;
		actual.round_ordinal = 1;
		actual.round_random = {0x1234, 0xabcd};
		actual.round_verified = true;
		check(test.record(2, phase::round_intro, 0, 0, winner::none, actual));
		check(test.publish(3));
		const auto started = test.read(1);
		const auto* round = started.find("start")->find("nativeRound");
		check(detail::is(started.find("type"), "game_started") && round);
		check(detail::is(round->find("encoding"), "pbd-native-round-start-v1"));
		check(round->find("pendingPhase")->text == "4" && round->find("roundOrdinal")->text == "1");
		check(round->find("randomWords")->items[0].text == "4660" && round->find("randomWords")->items[1].text == "43981");
	}
	{
		sample_document test(8);
		check(test.begin(true, true, true));
		check(test.record(0, phase::round_intro));
		check(!test.publish(1));
		check(detail::is(test.read(1).find("code"), "native_round_start_unverified"));
	}
	{
		// A deliberate quit gives up the seat with its cause, after the facts already published.
		sample_document test(9);
		check(test.begin());
		check(test.record(0, phase::vs));
		check(test.record(1, phase::character_intro));
		check(test.publish(2));
		check(test.driver.leave_seat("start_select"));
		check(test.events.last_event() == 2);
		const auto left = test.read(2);
		check(detail::is(left.find("type"), "leave") && detail::is(left.find("action"), "leave_seat"));
		check(detail::is(left.find("cause"), "start_select"));
	}
	{
		// Leaving while waiting for the other PC's game is its own event (no result), with no cause, before or after the start.
		sample_document test(31);
		check(test.driver.leave_peer_wait());
		check(test.events.last_event() == 1);
		const auto left = test.read(1);
		check(detail::is(left.find("type"), "leave") && detail::is(left.find("action"), "peer_wait") && !left.find("cause"));
	}
	{
		// A refused set never turns into a forfeit.
		sample_document test(10);
		check(!test.begin(false));
		check(!test.driver.leave_seat("window_closed"));
		check(!test.driver.leave_peer_wait());
		check(test.events.last_event() == 1 && detail::is(test.read(1).find("type"), "contract_error"));
	}
	{
		// The cause is a closed vocabulary: anything else poisons the channel instead of publishing.
		sample_document test(11);
		check(test.begin());
		check(!test.driver.leave_seat("peer_lost"));
		check(!test.driver.healthy() && test.events.last_event() == 0);
	}
}

#include "lobby_continuation_checks.h"

static void first_end_and_remote_status()
{
	{
		// The first end gives up the seat only for a deliberate quit while the other PC is still sending (the update latch's finish).
		using pbd::lobby_set::end_facts;
		using pbd::lobby_set::leave_on_end;
		end_facts quit;
		quit.user_stop = true;
		quit.still_collecting = true;
		quit.peer_watched = true;
		quit.peer_heard_us = 5000000;
		quit.now_us = 5400000;
		check(leave_on_end(quit));
		auto closed = quit;
		closed.user_stop = false;
		closed.window_close = true;
		check(leave_on_end(closed));
		// A crash, a kill or any other stop is not a quit.
		auto other = quit;
		other.user_stop = false;
		check(!leave_on_end(other));
		// A guard failure, a desync stop, a refused contract or the lobby's end_set came first.
		auto later = quit;
		later.still_collecting = false;
		check(!leave_on_end(later));
		auto closed_later = closed;
		closed_later.still_collecting = false;
		check(!leave_on_end(closed_later));
		// The other PC ended, fell silent for a second, or never sent a valid datagram.
		auto ended = quit;
		ended.peer_ended = true;
		check(!leave_on_end(ended));
		auto silent = quit;
		silent.now_us = quit.peer_heard_us + pbd::lobby_set::leave_peer_silence_us;
		check(!leave_on_end(silent));
		auto recent = silent;
		recent.now_us -= 1;
		check(leave_on_end(recent));
		auto closed_silent = closed;
		closed_silent.now_us = silent.now_us;
		check(!leave_on_end(closed_silent));
		auto never = quit;
		never.peer_heard_us = 0;
		check(!leave_on_end(never));
		// A datagram stamped after this end read the clock.
		auto ahead = quit;
		ahead.now_us = quit.peer_heard_us - 1;
		check(leave_on_end(ahead));
		// Without the fail-stop receiver nothing watches the other PC, and the quit stands as made.
		auto unwatched = never;
		unwatched.peer_watched = false;
		check(leave_on_end(unwatched));
	}
}

static void applied_picks_and_full_health()
{
	{
		// In-game selection: the events report what the game applied (here stage 17, rows 12 and 32), never the manifest's
		// empty picks, under the contract 2 schema.
		sample_document test(16, 2, true);
		check(test.begin());
		test.won_game();
		const auto started = test.read(1), result = test.read(3);
		check(detail::is(started.find("schema"), "pbd-set-contract-2") && detail::is(started.find("type"), "game_started"));
		check(started.find("stageId")->text == "17" && started.find("leftCharacterRow")->text == "12" && started.find("rightCharacterRow")->text == "32");
		check(detail::is(result.find("type"), "game_result") && result.find("stageId")->text == "17");
		check(result.find("leftCharacterRow")->text == "12" && result.find("rightCharacterRow")->text == "32");
		// The next game is chosen in the select again: its start and result report that game's own picks.
		command next;
		next.kind = command_kind::continue_game;
		next.serial = 1;
		next.game_number = 2;
		next.games = {1, 0};
		check(test.driver.continue_game(next, 11, 0x8000008c));
		const auto second = applied(0x0b, 0x4f, 0x4d);
		check(test.record(11, phase::vs, 0, 0, winner::none, second));
		check(test.record(12, phase::character_intro, 0, 0, winner::none, second));
		for (std::uint32_t won = 1, boundary = 13; won <= 3; ++won)
		{
			check(test.record(boundary++, phase::round_intro, 0, 0, winner::none, second));
			check(test.record(boundary++, phase::fight, 0, 0, winner::none, second));
			check(test.record(boundary++, phase::round_result, 0, won, winner::none, second));
		}
		check(test.record(22, phase::match_end, 0, 3, winner::right, second));
		check(test.publish(22));
		const auto restarted = test.read(6), decided = test.read(8);
		check(detail::is(restarted.find("type"), "game_started") && restarted.find("gameNumber")->text == "2");
		check(restarted.find("stageId")->text == "11" && restarted.find("leftCharacterRow")->text == "79" && restarted.find("rightCharacterRow")->text == "77");
		check(detail::is(decided.find("type"), "game_result") && detail::is(decided.find("winner"), "right"));
		check(decided.find("stageId")->text == "11" && decided.find("leftCharacterRow")->text == "79");
	}
	{
		// Both life bars start full in every lobby game, fixed or chosen in the game: any other applied handicap refuses the game.
		unsigned id = 60;
		for (const bool chosen : {false, true})
			for (const auto& handicap : {std::array<std::uint32_t, 2>{90, 100}, std::array<std::uint32_t, 2>{100, 0}, std::array<std::uint32_t, 2>{0, 0}})
			{
				sample_document test(id++, 1, chosen);
				check(test.begin());
				auto uneven = applied(17, 12, 32);
				uneven.handicap = handicap;
				check(test.record(0, phase::vs));
				check(test.record(1, phase::character_intro, 0, 0, winner::none, uneven));
				check(!test.publish(2));
				check(test.events.last_event() == 1);
				check(detail::is(test.read(1).find("code"), "applied_handicap_uneven"));
			}
	}
}

static void selection_bounds_and_preparing_prefix()
{
	const std::array<std::pair<unsigned, pbd::lobby_set::context>, 4> unread{{{17, applied(0x0b, 0x4f, 0x4d, 5)},
		{18, applied(0x0b, 0x4f, 0x4d, 3, false)}, {19, applied(0x0b, max_character_row + 1, 0x4d)}, {20, applied(max_stage_id + 1, 0x4f, 0x4d)}}};
	for (const auto& [id, applied] : unread)
	{
		// Another round rule, an unread pick or a pick outside the native tables refuses the game before it is reported.
		sample_document test(id, 1, true);
		check(test.begin());
		check(test.record(0, phase::vs));
		check(test.record(1, phase::character_intro, 0, 0, winner::none, applied));
		check(!test.publish(2));
		check(test.events.last_event() == 1);
		check(detail::is(test.read(1).find("code"), "applied_selection_unverified"));
	}
	{
		// The bound covers every countdown the manifest admits: 30 s and 15 s keep presentation_frames, the longest countdowns get
		// their own length plus the margin, and a fixed set keeps the pre-selection's bound.
		auto longest = identity(1, true);
		check(pbd::lobby_set::presentation_limit(longest) == pbd::lobby_set::presentation_frames);
		check(pbd::lobby_set::presentation_limit(identity()) == pbd::lobby_set::fixed_presentation_frames);
		longest.selection.character_seconds = max_selection_seconds;
		longest.selection.stage_seconds = max_selection_seconds;
		check(pbd::lobby_set::presentation_limit(longest) == 2u * max_selection_seconds * 60u + pbd::lobby_set::presentation_margin_frames);
	}
	{
		// Before a game starts, confirmed input of the select and the load is held for at most presentation_frames frames.
		sample_document test(21, 1, true);
		check(test.begin());
		const auto bound = static_cast<std::uint32_t>(pbd::lobby_set::presentation_frames);
		for (std::uint32_t i = 0; i < bound; ++i)
		{
			check(test.record(i, phase::loading));
			check(test.publish(i + 1));
		}
		check(test.driver.healthy() && test.events.last_event() == 0);
		check(test.record(bound, phase::loading));
		check(!test.publish(bound + 1));
		check(detail::is(test.read(1).find("code"), "presentation_buffer_exceeded"));
	}
}

static void random_stage_and_readback_changes()
{
	for (bool chosen : {false, true})
		for (unsigned random = 0; random < 2; ++random)
		{
			sample_document test(150 + random + (chosen ? 2 : 0), 2, chosen);
			check(test.begin());
			test.won_game();
			const auto previous = test.driver.rematch_state();
			check(previous && previous->epoch == 0);
			check(pbd::rematch::read_state(test.read(3).find("rematchState")) == previous);
			command next;
			next.kind = command_kind::continue_game;
			next.serial = 1;
			next.game_number = 2;
			next.games = {1, 0};
			auto selected = previous->selected;
			if (random)
				selected.stage = 23;
			next.rematch = pbd::rematch::load{1, 2, previous->end_frame, 1, selected, previous->rules, random != 0};
			check(test.driver.continue_game(next, 11, 0x8000008c));
			if (!chosen)
				check(test.events.identity().stage_id == selected.stage);
			const auto context = applied(selected.stage, selected.rows[0], selected.rows[1]);
			check(test.record(11, phase::vs, 0, 0, winner::none, context));
			check(test.record(12, phase::character_intro, 0, 0, winner::none, context));
			check(test.publish(13));
			const auto started = test.read(6);
			check(pbd::rematch::read_load(started.find("start")->find("load")) == next.rematch);
		}
	for (unsigned changed = 0; changed < 7; ++changed)
	{
		sample_document test(160 + changed, 2, true);
		check(test.begin());
		test.won_game();
		const auto previous = *test.driver.rematch_state();
		command next;
		next.kind = command_kind::continue_game;
		next.serial = 1;
		next.game_number = 2;
		next.games = {1, 0};
		next.rematch = pbd::rematch::load{1, 2, previous.end_frame, 1, previous.selected, previous.rules, false};
		auto& load = *next.rematch;
		switch (changed)
		{
		case 0: load.epoch++; break;
		case 1: load.end_frame++; break;
		case 2: load.selected.rows[0]++; break;
		case 3: load.rules.time_index++; break;
		case 4: load.selected.stage++; break;
		case 5: load.previous_game++; break;
		case 6: load.next_game++; break;
		}
		check(!test.driver.continue_game(next, 11, 0x8000008c));
		check(!test.driver.healthy() && test.events.last_command() == 0);
	}
}

static void loser_repick_keeps_winner_and_stage()
{
	for (unsigned kept_changed = 0; kept_changed < 3; ++kept_changed)
	{
		// Change character: Left won, so the Right side picks again. Its new row is this game's pick; the kept row, the stage and
		// the rules must still be the frozen ones, and the next Rematch freezes the new row.
		sample_document test(190 + kept_changed, 3, true);
		check(test.begin());
		test.won_game();
		const auto previous = *test.driver.rematch_state();
		command next;
		next.kind = command_kind::continue_game;
		next.serial = 1;
		next.game_number = 2;
		next.games = {1, 0};
		next.rematch = pbd::rematch::load{1, 2, previous.end_frame, 1, previous.selected, previous.rules, false};
		next.rematch->repick = 1;
		check(test.driver.continue_game(next, 11, 0x8000008c));
		auto context = applied(previous.selected.stage, previous.selected.rows[0], 61);
		if (kept_changed == 1)
			context.left_row = 13;
		if (kept_changed == 2)
			context.stage = 23;
		check(test.record(11, phase::vs, 0, 0, winner::none, context));
		check(test.record(12, phase::character_intro, 0, 0, winner::none, context));
		if (kept_changed)
		{
			check(!test.publish(13) && detail::is(test.read(6).find("code"), "rematch_applied_state_changed"));
			continue;
		}
		check(test.publish(13));
		const auto started = test.read(6);
		check(detail::is(started.find("type"), "game_started") && started.find("rightCharacterRow")->text == "61");
		check(started.find("leftCharacterRow")->text == "12" && started.find("stageId")->text == "17");
		const auto frozen = pbd::rematch::read_load(started.find("start")->find("load"));
		check(frozen == next.rematch && frozen->repick == 1u);
		for (std::uint32_t won = 1, boundary = 13; won <= 3; ++won)
		{
			check(test.record(boundary++, phase::round_intro, won - 1, 0, winner::none, context));
			check(test.record(boundary++, phase::fight, won - 1, 0, winner::none, context));
			check(test.record(boundary++, phase::round_result, won, 0, winner::none, context));
		}
		check(test.record(22, phase::match_end, 3, 0, winner::left, context));
		check(test.publish(22));
		const auto after = test.driver.rematch_state();
		check(after && after->game == 2 && after->epoch == 1 && after->selected.rows == (std::array<std::uint32_t, 2>{12, 61}));
	}
}

static void scene_barrier_final_input()
{
	{
		// A scene barrier can confirm the final input before its natural end observation arrives at the same boundary.
		sample_document test(180, 1, true, true);
		check(test.begin(true, true, false, false, false, true));
		const auto picks = applied(23, 78, 74);
		check(test.record(0, phase::vs));
		check(test.record(1, phase::character_intro, 0, 0, winner::none, picks));
		for (std::uint32_t won = 1, boundary = 2; won <= 3; ++won)
		{
			check(test.record(boundary++, phase::round_intro, 0, 0, winner::none, picks));
			check(test.record(boundary++, phase::fight, 0, 0, winner::none, picks));
			check(test.record(boundary++, phase::round_result, won, 0, winner::none, picks));
		}
		check(test.publish(11) && !test.driver.complete());
		check(test.record(11, phase::match_end, 3, 0, winner::left, picks));
		check(test.publish(11) && test.driver.complete());
		const auto ended = test.driver.sealed_game();
		check(ended && ended->end_boundary == 11 && ended->final_frame == 11 && ended->confirmed_through == 11);
		check(test.read(2).find("frameCount")->text == "11");
		check(detail::is(test.read(3).find("type"), "game_result") && test.read(3).find("finalFrame")->text == "11");
		check(detail::is(test.read(4).find("type"), "set_ended"));
		const auto serial = test.events.last_event();
		check(test.publish(11) && test.publish(10) && test.events.last_event() == serial);
	}
}

PBD_TEST(PbdLobbySetDriver, Checks)
{
	selection_prefix_and_timeout();
	start_options_and_round_lifecycle();
	clean_origin_and_initial_publication();
	result_and_guard_failures();
	forfeit_cause_and_waiting_leave();
	draw_result_and_replay_marker();
	continuation_number_and_score();
	first_end_and_remote_status();
	applied_picks_and_full_health();
	selection_bounds_and_preparing_prefix();
	random_stage_and_readback_changes();
	loser_repick_keeps_winner_and_stage();
	repick_rejects_winner_and_draw();
	scene_barrier_final_input();
}

namespace
{
	// Join Battle's command: the next set, first to `first_to`, at game 1 at 0-0.
	command next_set_command(std::uint32_t first_to = 2)
	{
		command value;
		value.kind = command_kind::next_set;
		value.serial = 1;
		value.game_number = 1;
		value.next_set_id = "set-next";
		value.next_match_id = "match-next";
		value.first_to_games = first_to;
		return value;
	}

	// A first-to-one set that the left player has won with its only game (boundaries 0..11).
	struct completed_set : sample_document
	{
		explicit completed_set(unsigned id, bool chosen = false)
			: sample_document(id, 1, chosen)
		{
			check(begin());
			won_game();
			check(detail::is(read(4).find("type"), "set_ended") && driver.sealed_game()->terminal);
		}
	};
} // namespace

// Join Battle against the same opponent: a completed set continues into the next one from its natural end, at game 1 at 0-0.
PBD_TEST(PbdLobbySetDriver, NextSetRematch)
{
	completed_set test(230);
	const auto previous = *test.driver.rematch_state();
	auto value = next_set_command(1);
	auto selected = previous.selected;
	selected.stage = 23;
	value.rematch = pbd::rematch::load{1, 1, previous.end_frame, previous.epoch + 1, selected, previous.rules, true};
	check(test.driver.next_set(value, 11, 0x8000008c));
	check(test.events.identity().stage_id == 23 && test.events.identity().game_number == 1);
	const auto context = applied(selected.stage, selected.rows[0], selected.rows[1]);
	check(test.record(11, phase::vs, 0, 0, winner::none, context));
	check(test.record(12, phase::character_intro, 0, 0, winner::none, context));
	check(test.publish(13));
	check(pbd::rematch::read_load(test.read(6).find("start")->find("load")) == value.rematch);
}

PBD_TEST(PbdLobbySetDriver, DirectMatchesRequireResolvedRematchesAcrossGamesAndSets)
{
	for (const auto first_to : {1u, 2u})
		for (unsigned variant = 0; variant < 4; ++variant)
		{
			sample_document test(360 + first_to * 4 + variant, first_to);
			pbd::lobby_set::clean_start start{123, 7, std::string(64, 'b'), "NPUB30009", true, true};
			start.direct_start = true;
			check(test.driver.begin(0, 0x8000007b, std::move(start)));
			test.won_game();
			const auto previous = *test.driver.rematch_state();
			auto command = next_set_command(1);
			if (first_to == 2)
			{
				command.kind = command_kind::continue_game;
				command.game_number = 2;
				command.games = {1, 0};
			}
			auto selected = previous.selected;
			if (variant == 1)
				selected.stage = 23;
			if (variant != 2)
				command.rematch = pbd::rematch::load{1, command.game_number, previous.end_frame, 1,
					selected, previous.rules, variant == 1};
			if (variant == 3)
				command.rematch->repick = 1;
			const bool accepted = first_to == 1 ? test.driver.next_set(command, 11, 0x8000008c) :
			                                      test.driver.continue_game(command, 11, 0x8000008c);
			check(accepted == (variant < 2));
			if (!accepted)
			{
				check(test.events.last_command() == 0 && detail::is(test.read(5).find("code"), "direct_start_load_required"));
				continue;
			}
			const auto context = applied(selected.stage, selected.rows[0], selected.rows[1]);
			check(test.record(11, phase::vs, 0, 0, winner::none, context));
			check(test.record(12, phase::character_intro, 0, 0, winner::none, context));
			check(test.publish(13));
			const auto started = test.read(6);
			check(pbd::rematch::read_load(started.find("start")->find("load")) == command.rematch);
			check(started.find("start")->find("inputPolicy")->find("directStart")->truth);
			check(test.events.identity().stage_id == selected.stage && test.events.identity().left.row == selected.rows[0] &&
				  test.events.identity().right.row == selected.rows[1]);
		}
}

PBD_TEST(PbdLobbySetDriver, NextSetStartsAtGameOne)
{
	completed_set test(200, true);
	check(test.driver.next_set(next_set_command(), 11, 0x8000008c));
	check(test.driver.healthy() && !test.driver.complete() && !test.driver.sealed_game());
	const auto acknowledged = test.read(5);
	check(detail::is(acknowledged.find("type"), "command_applied") && detail::is(acknowledged.find("setId"), "set-test"));
	const auto& adopted = test.events.identity();
	check(adopted.set_id == "set-next" && adopted.match_id == "match-next" && adopted.first_to_games == 2);
	check(adopted.game_number == 1 && adopted.games.left == 0 && adopted.games.right == 0 && test.events.last_command() == 1);
	// The left player wins again. Had the score been kept, this would end a first-to-two set at 2-0; at 0-0 the set stays open.
	const auto second = applied(0x0b, 0x4f, 0x4d);
	check(test.record(11, phase::vs, 0, 0, winner::none, second));
	check(test.record(12, phase::character_intro, 0, 0, winner::none, second));
	for (std::uint32_t won = 1, boundary = 13; won <= 3; ++won)
	{
		check(test.record(boundary++, phase::round_intro, 0, 0, winner::none, second));
		check(test.record(boundary++, phase::fight, 0, 0, winner::none, second));
		check(test.record(boundary++, phase::round_result, won, 0, winner::none, second));
	}
	check(test.record(22, phase::match_end, 3, 0, winner::left, second));
	check(test.publish(22) && test.driver.complete());
	const auto started = test.read(6), result = test.read(8), ready = test.read(9);
	check(detail::is(started.find("type"), "game_started") && detail::is(started.find("setId"), "set-next"));
	check(started.find("gameNumber")->text == "1" && started.find("stageId")->text == "11");
	check(started.find("leftCharacterRow")->text == "79");
	check(detail::is(result.find("type"), "game_result") && detail::is(result.find("winner"), "left"));
	check(result.find("gameNumber")->text == "1");
	// The new set's Rematch epochs start at 0, as the lobby's do for a new set.
	check(result.find("rematchState")->find("epoch")->text == "0");
	check(detail::is(ready.find("type"), "between_games_ready") && test.events.last_event() == 9);
	const auto sealed = test.driver.sealed_game();
	check(sealed && !sealed->terminal && sealed->games.left == 1 && sealed->games.right == 0 && sealed->game_number == 1);
	// The new set continues like any other.
	command game_two;
	game_two.kind = command_kind::continue_game;
	game_two.serial = 2;
	game_two.game_number = 2;
	game_two.games = {1, 0};
	check(test.driver.continue_game(game_two, 22, 0x80000097));
	check(test.events.identity().game_number == 2 && test.events.identity().set_id == "set-next");
}

// Only a completed set has a next set, and a completed set has no next game.
PBD_TEST(PbdLobbySetDriver, NextSetFollowsACompletedSet)
{
	{
		completed_set test(201);
		command more;
		more.kind = command_kind::continue_game;
		more.serial = 1;
		more.game_number = 2;
		more.games = {1, 0};
		check(!test.driver.continue_game(more, 11, 0x8000008c) && !test.driver.healthy());
		check(detail::is(test.read(5).find("code"), "natural_continuation_unverified"));
	}
	{
		// A set that is still open: its next game comes first.
		sample_document test(202, 2);
		check(test.begin());
		test.won_game();
		check(!test.driver.next_set(next_set_command(), 11, 0x8000008c) && !test.driver.healthy());
		check(detail::is(test.read(5).find("code"), "next_set_unverified") && test.events.last_command() == 0);
		check(test.events.identity().set_id == "set-test");
	}
	{
		// A drawn game never ends a set.
		sample_document test(203);
		check(test.begin());
		test.drawn_game();
		check(!test.driver.next_set(next_set_command(), 17, 0x8000008c) && !test.driver.healthy());
		check(detail::is(test.read(6).find("code"), "next_set_unverified"));
	}
	{
		// The next set starts exactly at the completed set's end.
		completed_set test(204);
		check(!test.driver.next_set(next_set_command(), 12, 0x8000008c) && !test.driver.healthy());
		check(detail::is(test.read(5).find("code"), "next_set_unverified") && test.events.identity().set_id == "set-test");
	}
}

// A next set is game 1 at 0-0 with no Rematch record, under a new identity; anything else is refused before it is acknowledged.
PBD_TEST(PbdLobbySetDriver, NextSetRefusals)
{
	for (unsigned variant = 0; variant < 4; ++variant)
	{
		completed_set test(205 + variant);
		auto value = next_set_command();
		if (variant == 0)
			value.game_number = 2;
		else if (variant == 1)
			value.games = {1, 0};
		else if (variant == 2)
			value.rematch = pbd::rematch::load{1, 2, 11, 1, {{12, 32}, 17}, {}, false};
		else
			value.kind = command_kind::continue_game;
		check(!test.driver.next_set(value, 11, 0x8000008c) && !test.driver.healthy());
		check(detail::is(test.read(5).find("code"), variant == 2 ? "rematch_load_changed" : "next_set_unverified") && test.events.last_command() == 0);
	}
	// The channel refuses an identity the completed set already had.
	completed_set test(210);
	auto value = next_set_command();
	value.next_set_id = "set-test";
	check(!test.driver.next_set(value, 11, 0x8000008c) && !test.driver.healthy());
	check(detail::is(test.read(5).find("code"), "next_set_channel_failed") && test.events.last_command() == 0);
}

namespace
{
	void write_wait_command(const std::filesystem::path& directory, std::uint64_t serial, std::string_view fields)
	{
		const auto temporary = directory / "pending-command.tmp";
		{
			std::ofstream file(temporary, std::ios::binary);
			file << R"({"schema":"pbd-set-contract-1","sessionId":"session-test","setId":"set-test","serial":)"
				 << serial << ',' << fields << '}';
			check(file.good());
		}
		std::filesystem::rename(temporary, directory / ("set-command-" + std::to_string(serial) + ".json"));
	}
} // namespace

// Safe commands ahead of an ending must be applied and acknowledged without releasing the held guest boundary.
PBD_TEST(PbdLobbySetDriver, SafeCommandsCannotObstructAnEndingAtAHeldBoundary)
{
	const std::array<std::pair<command_kind, std::string_view>, 4> predecessors{{{command_kind::chat_overlay, R"("type":"chat_overlay","visible":true,"additions":[])"},
		{command_kind::vs_cards, R"("type":"vs_cards","visible":true,"left":{"language":"EN","region":""},"right":{"language":"","region":"Europe"})"},
		{command_kind::rank_stakes, R"("type":"rank_stakes","left":{"win":"promote","winRank":11,"loss":"none","lossRank":10},"right":{"win":"none","winRank":10,"loss":"demote","lossRank":9})"},
		{command_kind::stream_status, R"("type":"stream_status","status":"paused_slow")"}}};
	for (const bool background : {false, true})
	{
		for (const auto& [kind, fields] : predecessors)
		{
			sample_document test(437);
			channel source(test.directory, identity(), background);
			std::optional<command> pending;
			std::vector<command_kind> applied;
			const auto poll = [&]
			{
				return pbd::lobby_set::poll_wait_commands(source, pending, [&](const command& value)
					{
						applied.push_back(value.kind);
						check(source.acknowledge(value));
						return true;
					},
					[&](const command& value)
					{
						check(value.kind == command_kind::end_set && value.reason == "left");
						applied.push_back(value.kind);
						check(source.acknowledge(value));
					},
					[](const std::string& error)
					{
						check(false, error);
					});
			};
			check(!poll() && source.last_command() == 0);
			write_wait_command(test.directory, 1, fields);
			if (!background)
				write_wait_command(test.directory, 2, R"("type":"end_set","reason":"left")");
			else
			{
				// The background reader may deliver the predecessor before the ending is published.
				const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
				while (source.last_command() == 0 && std::chrono::steady_clock::now() < deadline)
				{
					check(!poll());
					std::this_thread::sleep_for(std::chrono::milliseconds(1));
				}
				check(source.last_command() == 1 && applied == std::vector<command_kind>{kind});
				check(!poll());
				write_wait_command(test.directory, 2, R"("type":"end_set","reason":"left")");
			}
			const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
			bool ended = false;
			while (!ended && std::chrono::steady_clock::now() < deadline)
			{
				ended = poll();
				if (!ended)
					std::this_thread::sleep_for(std::chrono::milliseconds(1));
			}
			check(ended && source.last_command() == 2 && source.last_event() == 2 && source.flush());
			check(applied == std::vector<command_kind>{kind, command_kind::end_set} && !pending);
		}
	}
}

PBD_TEST(PbdLobbySetDriver, HeldBoundariesDoNotSkipContinuationsOrPendingMenus)
{
	sample_document test(438);
	write_wait_command(test.directory, 1, R"("type":"continue_game","gameNumber":2,"score":{"left":1,"right":0})");
	channel source(test.directory, identity(2));
	write_wait_command(test.directory, 2, R"("type":"end_set","reason":"left")");
	std::optional<command> pending;
	unsigned applied = 0;
	const auto poll = [&]
	{
		return pbd::lobby_set::poll_wait_commands(source, pending, [&](const command&)
			{
				++applied;
				return true;
			},
			[&](const command&)
			{
				++applied;
			},
			[](const std::string& error)
			{
				check(false, error);
			});
	};
	check(!poll() && !poll() && source.last_command() == 0 && applied == 0 && !pending);
	for (const auto kind : {command_kind::show_post_set, command_kind::choice_rejected})
	{
		pending.emplace();
		pending->kind = kind;
		check(!poll() && pending && pending->kind == kind && source.last_command() == 0 && applied == 0);
	}
	pending->kind = command_kind::end_set;
	check(poll() && !pending && applied == 1);
}

namespace
{
	using pbd::game_overlay::panel_kind;
	// The game's side of the in-game menu in the order it reads: each newer menu.pbd panel as it is applied
	// (overlay_controls::apply_panel) and each command as the channel's reader thread delivers it (lobby_poll_command).
	// `applied` lists the serial of each command acknowledged.
	struct menu_reader
	{
		pbd::game_overlay::snapshot shown;
		std::optional<command> pending;
		std::uint64_t before = 0;
		std::vector<std::uint64_t> applied;

		void panel(const pbd::game_overlay::snapshot& next)
		{
			// A panel no newer than the shown one is not applied again (overlay_controls::poll_command).
			if (next.generation <= shown.generation)
				return;
			shown = next;
			if (const auto carried = pbd::lobby_set::menu_panel_applied(shown, pending, before))
				applied.push_back(carried->serial);
		}
		void take(command value)
		{
			if (const auto now = pbd::lobby_set::take_menu_command(std::move(value), shown, pending, before))
				applied.push_back(now->serial);
		}
	};
	command menu_command(std::uint64_t serial, command_kind kind)
	{
		command value;
		value.serial = serial;
		value.kind = kind;
		return value;
	}
	pbd::game_overlay::snapshot menu_panel(std::uint64_t generation, std::uint64_t serial, panel_kind kind, bool visible)
	{
		pbd::game_overlay::snapshot panel;
		panel.generation = generation;
		panel.command_serial = serial;
		panel.kind = kind;
		panel.visible = visible;
		panel.close_on_action = false;
		return panel;
	}
	pbd::game_overlay::snapshot results_panel(std::uint64_t generation, std::uint64_t serial)
	{
		return menu_panel(generation, serial, panel_kind::post_set, true);
	}
	pbd::game_overlay::snapshot ending_panel(std::uint64_t generation, std::uint64_t serial)
	{
		return menu_panel(generation, serial, panel_kind::set_ended, false);
	}
} // namespace

// Panel and command publication may be observed in either order because they have independent readers.
// A matching panel already shown must apply its command without waiting for another panel generation.
PBD_TEST(PbdLobbySetDriver, AResultsMenuReadBeforeItsCommandIsAcknowledged)
{
	menu_reader game;
	game.panel(results_panel(7, 10));
	check(game.applied.empty() && !game.pending, "a panel with no command waiting applied one");
	const auto results = menu_command(10, command_kind::show_post_set);
	check(!pbd::lobby_set::menu_applied(results, 7, game.shown), "only a newer panel applies a command it waits for");
	game.take(results);
	check(game.applied == std::vector<std::uint64_t>{10} && !game.pending, "the results menu read before show_post_set was not acknowledged");
	// The launcher draws an unacknowledged results menu again while a later command waits; once acknowledged that is harmless.
	game.panel(results_panel(8, 10));
	check(game.applied == std::vector<std::uint64_t>{10} && !game.pending, "a results menu drawn again was acknowledged twice");

	menu_reader rejected;
	rejected.panel(menu_panel(3, 5, panel_kind::choice_rejected, true));
	rejected.take(menu_command(5, command_kind::choice_rejected));
	check(rejected.applied == std::vector<std::uint64_t>{5} && !rejected.pending, "a rejection read before its command was not acknowledged");
}

PBD_TEST(PbdLobbySetDriver, AnEndingPanelReadBeforeEndSetEndsTheSet)
{
	menu_reader game;
	game.panel(results_panel(7, 10));
	game.take(menu_command(10, command_kind::show_post_set));
	game.panel(results_panel(8, 10));
	game.panel(ending_panel(9, 11));
	check(!game.pending);
	const auto ending = menu_command(11, command_kind::end_set);
	check(!pbd::lobby_set::menu_applied(ending, 9, game.shown), "only a newer panel applies a command it waits for");
	game.take(ending);
	check(game.applied.size() == 2 && game.applied.back() == 11 && !game.pending, "the ending panel read before end_set did not end the set");
	// The launcher writes an unacknowledged ending panel again every 250 ms: a newer copy and a rewrite of the same copy.
	game.panel(ending_panel(10, 11));
	game.panel(ending_panel(10, 11));
	check(game.applied.size() == 2 && !game.pending, "an ending panel written again ended the set twice");
}

// The usual order, the command first and then its panel, still applies each command once, and a panel already shown applies
// only the command it names, in the form that command needs.
PBD_TEST(PbdLobbySetDriver, AMenuCommandWaitsForThePanelThatCarriesIt)
{
	menu_reader game;
	game.panel(menu_panel(6, 9, panel_kind::menu, false));
	game.take(menu_command(10, command_kind::show_post_set));
	check(game.applied.empty() && game.pending && game.before == 6, "a command applied before its panel came");
	game.panel(results_panel(7, 10));
	check(game.applied == std::vector<std::uint64_t>{10} && !game.pending, "the results menu shown after show_post_set was not acknowledged");
	game.panel(results_panel(8, 10));
	game.take(menu_command(11, command_kind::end_set));
	check(game.applied == std::vector<std::uint64_t>{10} && game.pending, "the results menu applied the ending");
	game.panel(menu_panel(9, 11, panel_kind::set_ended, true));
	check(game.applied == std::vector<std::uint64_t>{10} && game.pending, "a shown ending panel applied end_set, which needs it hidden");
	game.panel(ending_panel(10, 11));
	game.panel(ending_panel(11, 11));
	check(game.applied == (std::vector<std::uint64_t>{10, 11}) && !game.pending, "the ending panel did not end the set once");

	// Already shown at the command but not in its form: a results menu that closes on a press, or the ending drawn visibly.
	menu_reader closing;
	auto pressed = results_panel(4, 3);
	pressed.close_on_action = true;
	closing.panel(pressed);
	closing.take(menu_command(3, command_kind::show_post_set));
	check(closing.applied.empty() && closing.pending, "a results menu that closes on a press acknowledged show_post_set");
	menu_reader drawn;
	drawn.panel(menu_panel(4, 3, panel_kind::set_ended, true));
	drawn.take(menu_command(3, command_kind::end_set));
	check(drawn.applied.empty() && drawn.pending, "a shown ending panel applied end_set at the command");
	// A panel of an earlier command never applies a later one, before or after it is taken.
	menu_reader stale;
	stale.panel(ending_panel(4, 2));
	stale.take(menu_command(3, command_kind::end_set));
	check(stale.applied.empty() && stale.pending);
	stale.panel(ending_panel(5, 2));
	check(stale.applied.empty() && stale.pending, "an earlier command's panel ended the set");
	// Before the first panel nothing can apply a command.
	menu_reader start;
	start.take(menu_command(1, command_kind::end_set));
	check(start.applied.empty() && start.pending && start.before == 0);
}

namespace
{
	using stake_event = pbd::rank_stakes::event;
	using pbd::rank_stakes::game_values;
	// Left (2nd Dan, 11) ranks up to 3rd Dan with a set win; right (1st Dan, 10) has nothing at stake.
	const pbd::rank_stakes::stakes climbing{{{stake_event::promote, stake_event::none, 12, 11}, {stake_event::none, stake_event::none, 10, 10}}};
	game_values values(std::array<std::uint32_t, 2> stake, std::array<int, 2> promote)
	{
		game_values out;
		out.stake = stake;
		out.promote = promote;
		return out;
	}
} // namespace

// The game's own Ranked screens: each game's stake words and PROMOTED TO ranks follow the score, as the VS cards' tags do.
PBD_TEST(PbdLobbySetDriver, RankScreenValuesFollowTheScore)
{
	using namespace pbd::rank_stakes;
	// First to 2: nothing at 0-0; at 1-0 the left player's win ends the set; at 0-1 neither left outcome can; at 1-1 both.
	check(plan(climbing, {0, 0}, 2) == game_values{});
	check(plan(climbing, {1, 0}, 2) == values({1, 0}, {12, -1}));
	check(plan(climbing, {0, 1}, 2) == game_values{});
	check(plan(climbing, {1, 1}, 2) == values({1, 0}, {12, -1}));
	// A demotion risk is 2; a side with both shows its promotion chance (1), since the game draws nothing for its "both".
	const stakes swinging{{{stake_event::promote, stake_event::demote, 12, 10}, {stake_event::none, stake_event::demote, 10, 9}}};
	check(plan(swinging, {1, 1}, 2) == values({1, 2}, {12, -1}));
	check(plan(swinging, {0, 1}, 2) == values({2, 0}, {-1, -1}) && plan(swinging, {1, 0}, 2) == values({1, 2}, {12, -1}));
	// A placing set shows PROMOTED TO only above Beginner, and never a stake; a demoting loss never shows PROMOTED TO.
	const stakes placing{{{stake_event::place, stake_event::place, 3, 0}, {stake_event::place, stake_event::place, 0, 0}}};
	check(plan(placing, {1, 1}, 2) == values({0, 0}, {3, -1}));
	// The stake writes go to both sides' stake words after the battle's own setup, or nowhere when nothing is at stake.
	const auto stakes_written = stake_writes(values({1, 2}, {-1, -1}));
	check(stakes_written.count == 2 && stakes_written.writes[0] == guest_write{0x106c9d2c, 4, 1} &&
		  stakes_written.writes[1] == guest_write{0x106c9d68, 4, 2});
	check(!stake_writes(game_values{}).count && !stake_writes(values({0, 0}, {12, -1})).count);
	// Watchers write the values the players recorded: the start's form reads back exactly, and only that form.
	const auto recorded = values({1, 2}, {12, -1});
	const auto form = values_json(recorded);
	check(read_values(&form) == recorded &&
		  pbd::json::detail::serialize(form) == R"({"stakeLeft":1,"stakeRight":2,"promoteLeft":12,"promoteRight":-1})");
	const auto reread = pbd::json::detail::json_parser(pbd::json::detail::serialize(form)).parse();
	check(reread.value && read_values(&*reread.value) == recorded);
	for (const auto bad : {R"({"stakeLeft":3,"stakeRight":0,"promoteLeft":-1,"promoteRight":-1})",
			 R"({"stakeLeft":0,"stakeRight":0,"promoteLeft":35,"promoteRight":-1})", R"({"stakeLeft":0,"stakeRight":0,"promoteLeft":-2,"promoteRight":-1})",
			 R"({"stakeLeft":0,"stakeRight":0,"promoteLeft":-1})", R"({"stakeLeft":0,"promoteLeft":-1,"promoteRight":-1})", R"([1,2])"})
	{
		const auto parsed = pbd::json::detail::json_parser(bad).parse();
		check(parsed.value && !read_values(&*parsed.value));
	}
	// The values are taken between games and kept from the VS screen through the fight.
	for (const std::uint32_t scene : {8u, 12u, 16u, 17u})
		check(!values_may_change(scene));
	for (const std::uint32_t scene : {0u, 1u, 2u, 3u, 9u})
		check(values_may_change(scene));
}

// PROMOTED TO: only when this game's winner is promoted by it, and then the winner's rank byte and change word.
PBD_TEST(PbdLobbySetDriver, RankScreensPromoteOnlyOnTheDecidingGame)
{
	using namespace pbd::rank_stakes;
	const std::array<std::uint32_t, 2> held{11, 10};
	const auto deciding = plan(climbing, {1, 1}, 2);
	// The left player wins the deciding game: rank 12 and change 1 on the left, the right's byte written back as held.
	const auto left_won = promotion_writes(deciding, winner_side({1, 0, 1}), held);
	check(left_won.count == 3 && left_won.writes[0] == guest_write{0x106c9a50, 1, 12} && left_won.writes[1] == guest_write{0x106c9c44, 1, 10} &&
		  left_won.writes[2] == guest_write{0x106c9d30, 4, 1});
	// The right player wins it: no promotion, both bytes as held, no change word.
	const auto right_won = promotion_writes(deciding, winner_side({2, 1, 0}), held);
	check(right_won.count == 2 && right_won.writes[0] == guest_write{0x106c9a50, 1, 11} && right_won.writes[1] == guest_write{0x106c9c44, 1, 10});
	// A draw names no winner: both bytes as held; it is played again at the same score.
	check(!winner_side({0, 0, 0}) && !winner_side({1, 1, 0}) && promotion_writes(deciding, winner_side({0, 0, 0}), held).count == 2);
	// A game that cannot end the set with a promotion writes nothing at its end.
	for (const pbd::rank_stakes::score games : {pbd::rank_stakes::score{0, 0}, pbd::rank_stakes::score{0, 1}})
		check(!promotion_writes(plan(climbing, games, 2), 0u, held).count);
	// The test switch knows no held ranks: only the winner's promotion.
	const auto tested = promotion_writes(values({0, 0}, {-1, 11}), 1u, std::nullopt);
	check(tested.count == 2 && tested.writes[0] == guest_write{0x106c9c44, 1, 11} && tested.writes[1] == guest_write{0x106c9d6c, 4, 1});
	check(!promotion_writes(values({0, 0}, {-1, 11}), 0u, std::nullopt).count);

	// The records keep the promoted rank once the game took it: change word 1 and the promoted rank in the byte.
	check(kept_promotion(deciding, 0, 1, 12) == 12u);
	check(!kept_promotion(deciding, 0, 0, 12) && !kept_promotion(deciding, 0, 1, 11) && !kept_promotion(deciding, 1, 1, 12) &&
		  !kept_promotion(deciding, 0, 3, 12) && !kept_promotion(game_values{}, 0, 1, 0));

	// Whatever the stakes, score and winner, the writes touch only the stake and change words and the rank bytes: never the
	// game's record of the last winner (0x1008f08c, which the next game's random draws depend on) nor the bonus marker (+0x10).
	const std::array<std::uint32_t, 6> allowed{0x106c9d2c, 0x106c9d68, 0x106c9d30, 0x106c9d6c, 0x106c9a50, 0x106c9c44};
	const std::array<stake_event, 4> events{stake_event::none, stake_event::promote, stake_event::demote, stake_event::place};
	std::uint32_t plans = 0;
	for (const auto win : events)
		for (const auto loss : events)
			for (const std::uint32_t first_to : {1u, 2u, 3u})
				for (std::uint32_t left = 0; left < first_to; ++left)
					for (std::uint32_t right = 0; right < first_to; ++right)
					{
						const stakes set{{{win, loss, 12, 9}, {loss == stake_event::place ? stake_event::place : win, loss, 0, 34}}};
						const auto game = plan(set, {left, right}, first_to);
						for (const auto winner : {std::optional<std::uint32_t>{0}, std::optional<std::uint32_t>{1}, std::optional<std::uint32_t>{}})
							for (const auto& written : {stake_writes(game), promotion_writes(game, winner, held), promotion_writes(game, winner, std::nullopt)})
								for (std::uint32_t i = 0; i < written.count; ++i)
								{
									const auto at = written.writes[i].address;
									check(at != pbd::game_profile::previous_winner && at != pbd::game_profile::results + pbd::game_profile::bonus_offset &&
										  at != pbd::game_profile::results + pbd::game_profile::result_stride + pbd::game_profile::bonus_offset);
									check(std::find(allowed.begin(), allowed.end(), at) != allowed.end());
									++plans;
								}
					}
	check(plans > 0);
}

// Offline overrides accept side:value pairs, with each side named at most once.
PBD_TEST(PbdLobbySetDriver, RankScreensTestSwitch)
{
	using namespace pbd::rank_stakes;
	game_values out;
	check(read_test_values("left:1,right:2", false, out) && out.stake == std::array<std::uint32_t, 2>{1, 2});
	check(read_test_values("left:11", true, out) && out.promote == std::array<int, 2>{11, -1});
	game_values edge;
	check(read_test_values("right:4", false, edge) && edge.stake[1] == 4 && read_test_values("right:36", true, edge) && edge.promote[1] == 36);
	for (const auto bad : {"", "left", "left:", "left:5", "middle:1", "left:1,left:2", "left:1;right:2", "left:-1", "left:1,"})
	{
		game_values ignored;
		check(!read_test_values(bad, false, ignored));
	}
	game_values ignored;
	check(!read_test_values("left:37", true, ignored) && !read_test_values("left:x", true, ignored));
}

namespace
{
	// The rank screens' switches as pbd::rank_screens::configure reads them, from a process environment held in a map.
	struct rank_switches
	{
		std::map<std::string, std::string, std::less<>> values;
		pbd::rank_screens::switches read;
		std::string error;
		rank_switches(std::initializer_list<std::pair<const std::string, std::string>> environment) : values(environment)
		{
			error = pbd::rank_screens::read_switches([this](std::string_view name) -> const char*
				{
					const auto found = values.find(name);
					return found == values.end() ? nullptr : found->second.c_str();
				},
				read);
		}
		bool ignored(std::string_view name) const
		{
			return error.empty() && !read.test && read.ignored && name == read.ignored;
		}
	};
} // namespace

// Overrides apply only offline so sessions keep the agreed or recorded values.
// Sessions ignore malformed overrides too; replay admission refuses any nonempty override.
PBD_TEST(PbdLobbySetDriver, RankScreensTestSwitchOnlyInOfflinePlay)
{
	const rank_switches offline({{"PBD_OFFLINE_PLAY", "1"}, {"PBD_TEST_RANK_STAKE", "left:1,right:2"}, {"PBD_TEST_RANK_PROMOTE", "left:11"}});
	check(offline.error.empty() && offline.read.test && offline.read.enabled() && !offline.read.ignored && !offline.read.screens);
	check(offline.read.test->stake == std::array<std::uint32_t, 2>{1, 2} && offline.read.test->promote == std::array<int, 2>{11, -1});
	const rank_switches malformed({{"PBD_OFFLINE_PLAY", "1"}, {"PBD_TEST_RANK_PROMOTE", "left:37"}});
	check(malformed.error.starts_with("PBD_TEST_RANK_PROMOTE:") && !malformed.read.test);

	// A Ranked set with the screens on: they follow the set's stakes, never the switch.
	const rank_switches ranked({{"PBD_SESSION", "1"}, {"PBD_SET_CHANNEL", "set"}, {"PBD_RANK_SCREENS", "1"}, {"PBD_TEST_RANK_STAKE", "left:1"}});
	check(ranked.ignored("PBD_TEST_RANK_STAKE") && ranked.read.screens && ranked.read.enabled());
	// Without the screens a set emits no hook site at all.
	const rank_switches lobby({{"PBD_SESSION", "1"}, {"PBD_SET_CHANNEL", "set"}, {"PBD_TEST_RANK_PROMOTE", "right:12"}});
	check(lobby.ignored("PBD_TEST_RANK_PROMOTE") && !lobby.read.enabled());
	check(rank_switches({{"PBD_SESSION", "1"}, {"PBD_SPECTATOR_CHANNEL", "watch"}, {"PBD_TEST_RANK_STAKE", "right:2"}}).ignored("PBD_TEST_RANK_STAKE"));
	check(rank_switches({{"PBD_SESSION", "1"}, {"PBD_REPLAY_VIEWER", "1"}, {"PBD_TEST_RANK_PROMOTE", "left:3"}}).ignored("PBD_TEST_RANK_PROMOTE"));
	// Offline play is never beside a session, and the switch is read in no other route either.
	check(rank_switches({{"PBD_OFFLINE_PLAY", "1"}, {"PBD_SESSION", "1"}, {"PBD_TEST_RANK_STAKE", "left:1"}}).ignored("PBD_TEST_RANK_STAKE"));
	check(rank_switches({{"PBD_OFFLINE_PLAY", "0"}, {"PBD_TEST_RANK_STAKE", "left:1"}}).ignored("PBD_TEST_RANK_STAKE"));
	check(rank_switches({{"PBD_TEST_RANK_STAKE", "left:1"}}).ignored("PBD_TEST_RANK_STAKE"));
	check(rank_switches({{"PBD_SESSION", "1"}, {"PBD_TEST_RANK_STAKE", "left:9"}}).ignored("PBD_TEST_RANK_STAKE"));
	// A switch set empty is off everywhere.
	const rank_switches empty({{"PBD_SESSION", "1"}, {"PBD_TEST_RANK_STAKE", ""}});
	check(empty.error.empty() && !empty.read.test && !empty.read.ignored && !empty.read.enabled());

	for (const auto& [name, promotion] : pbd::rank_screens::test_switch)
	{
		const std::string_view switch_name(name);
		check(std::find(pbd::replay_switches::refused.begin(), pbd::replay_switches::refused.end(), switch_name) !=
			  pbd::replay_switches::refused.end());
		const std::map<std::string, std::string, std::less<>> replay{{"PBD_REPLAY_VIEWER", "1"}, {std::string(name), "left:1"}};
		const char* refused = pbd::replay_switches::first_refused([&](std::string_view key) -> const char*
			{
				const auto found = replay.find(key);
				return found == replay.end() ? nullptr : found->second.c_str();
			});
		check(refused && switch_name == refused);
	}
}

// The end-of-match hook runs on every frame of phase 8 at every round end; its writes are logged once per match end.
PBD_TEST(PbdLobbySetDriver, RankScreensLogOnceAtEachMatchEnd)
{
	pbd::rank_screens::end_log log;
	const auto lines = [&](std::optional<std::uint32_t> winner, bool written, std::uint32_t frames)
	{
		std::uint32_t out = 0;
		for (std::uint32_t frame = 0; frame < frames; ++frame)
			out += log.due(winner, written) ? 1 : 0;
		return out;
	};
	// A deciding game won 3-0: two round ends name no winner, the last names the left side.
	check(lines(std::nullopt, true, 119) == 0 && lines(std::nullopt, true, 119) == 0);
	check(lines(0u, true, 119) == 1);
	// A rollback re-executes the end: frames before the winner was named, the same winner, then a corrected one and back.
	check(lines(std::nullopt, true, 3) == 0 && lines(0u, true, 3) == 0);
	check(lines(1u, true, 5) == 1 && lines(0u, true, 5) == 1);
	// A failing write is named once, and so is the recovery after it.
	check(lines(0u, false, 50) == 1 && lines(0u, true, 50) == 1);
	check(lines(std::nullopt, false, 50) == 1);
	// The next battle setup starts the next match: its end is logged again, and a drawn match logs nothing.
	log.reset();
	check(lines(std::nullopt, true, 119) == 0 && lines(1u, true, 119) == 1);
	log.reset();
	check(lines(std::nullopt, true, 357) == 0);
	// A winner byte outside both sides names no winner.
	log.reset();
	check(lines(2u, true, 10) == 0);
}

// A game's start records the values its fight is written with, for watchers. They are taken as the game starts, so values given
// while it runs belong to the next game; a game without them records none.
PBD_TEST(PbdLobbySetDriver, GameStartRecordsTheRankScreens)
{
	sample_document test(434, 2);
	const auto recorded = values({1, 0}, {12, -1});
	test.driver.rank_screens(recorded);
	check(test.begin());
	test.driver.rank_screens(values({2, 2}, {-1, -1}));
	check(test.record(0, phase::vs) && test.record(1, phase::character_intro) && test.publish(2));
	const auto started = test.read(1);
	check(detail::is(started.find("type"), "game_started"));
	const auto* screens = started.find("start")->find("rankScreens");
	check(screens && pbd::rank_stakes::read_values(screens) == recorded);

	for (const auto& kept : {std::optional<game_values>{}, std::optional<game_values>{game_values{}}})
	{
		sample_document plain(435);
		plain.driver.rank_screens(kept);
		check(plain.begin());
		check(plain.record(0, phase::vs) && plain.record(1, phase::character_intro) && plain.publish(2));
		check(!plain.read(1).find("start")->find("rankScreens"));
	}
}

namespace
{
	// The set event of `type` for game `game`, by its serial; 0 when there is none.
	std::uint64_t event_of(lobby_test_state::sample_document& test, std::string_view type, std::uint32_t game)
	{
		for (std::uint64_t serial = 1; serial <= test.events.last_event(); ++serial)
		{
			const auto value = test.read(serial);
			if (detail::is(value.find("type"), type) && value.find("gameNumber") && value.find("gameNumber")->text == std::to_string(game))
				return serial;
		}
		return 0;
	}

	// A game_started's start without its round call, the one field its game_preparing cannot know yet.
	std::string start_without_round(const pbd::set_channel::json_value& started)
	{
		auto start = *started.find("start");
		std::erase_if(start.fields, [](const auto& field)
			{
				return field.first == "nativeRound";
			});
		return detail::serialize(start);
	}
} // namespace

// Watching from selection begins with a game's game_preparing, and the launcher holds its game_started to the same start: both
// name the game's rank screen values, which the game takes where its command is applied, before its game_preparing is written.
// Values given later (stakes that reached this PC late) wait for the next game, so the two records never differ.
PBD_TEST(PbdLobbySetDriver, GamePreparingNamesTheRankScreensOfItsGameStart)
{
	sample_document test(436, 2, true);
	auto actual = applied(32, 12, 32);
	actual.pending_phase = 4;
	actual.round_ordinal = 1;
	actual.round_random = {0x1234, 0xabcd};
	actual.round_verified = true;
	// Left wins three straight rounds from `origin`; the match end follows eleven boundaries later.
	const auto play = [&](std::uint32_t origin)
	{
		std::uint32_t boundary = origin;
		check(test.record(boundary++, phase::vs, 0, 0, winner::none, actual));
		check(test.record(boundary++, phase::character_intro, 0, 0, winner::none, actual));
		for (std::uint32_t won = 1; won <= 3; ++won)
		{
			check(test.record(boundary++, phase::round_intro, won - 1, 0, winner::none, actual));
			check(test.record(boundary++, phase::fight, won - 1, 0, winner::none, actual));
			check(test.record(boundary++, phase::round_result, won, 0, winner::none, actual));
		}
		check(test.record(boundary, phase::match_end, 3, 0, winner::left, actual));
		check(test.publish(boundary) && test.driver.complete());
		return boundary;
	};
	const auto same_start = [&](std::uint32_t game)
	{
		const auto preparing = event_of(test, "game_preparing", game), started = event_of(test, "game_started", game);
		check(preparing && started && preparing < started);
		const auto prepared = test.read(preparing);
		check(detail::serialize(*prepared.find("start")) == start_without_round(test.read(started)));
		return prepared;
	};

	// Game 1 at 0-0 has nothing at stake, and names no values in either record.
	check(test.begin(true, true, true, false, true));
	const auto first_end = play(0);
	check(!same_start(1).find("start")->find("rankScreens"));

	// Game 2 at 1-0 decides the set on a left win: its command's values are in both records.
	const auto deciding = values({1, 2}, {12, -1});
	test.driver.rank_screens(deciding);
	command next;
	next.kind = command_kind::continue_game;
	next.serial = 1;
	next.game_number = 2;
	next.games = {1, 0};
	check(test.driver.continue_game(next, first_end, 0x800000b4));
	test.driver.rank_screens(values({2, 0}, {-1, -1}));
	play(first_end);
	const auto second = same_start(2);
	const auto* screens = second.find("start")->find("rankScreens");
	check(screens && pbd::rank_stakes::read_values(screens) == deciding);
}

// In a session each PC's launcher gives it the set's stakes, which can reach the two PCs at different times, so both announce
// them with every scene barrier. A game's command plans from the stakes both announced at the barrier before it, and from none
// while they differ. Each PC holds the same two words there, so both plan the same values before either writes the game's start.
PBD_TEST(PbdLobbySetDriver, RankStakesAgreeBeforeTheGameStarts)
{
	using namespace pbd::rank_stakes;
	// Every side's stakes have their own word and read back from it exactly; the other side keeps its own bits.
	const std::array<stake_event, 4> events{stake_event::none, stake_event::promote, stake_event::demote, stake_event::place};
	constexpr auto top = pbd::rank_stakes::max_rank;
	const pbd::rank_stakes::side fixed{stake_event::promote, stake_event::demote, 12, 10};
	std::vector<std::uint32_t> words;
	for (std::uint32_t player = 0; player < 2; ++player)
		for (const auto win : events)
			for (const auto loss : events)
				for (std::uint32_t win_rank = 0; win_rank <= top; ++win_rank)
					for (const std::uint32_t loss_rank : {0u, 9u, 33u, top})
					{
						stakes set{fixed, fixed};
						set[player] = {win, loss, win_rank, loss_rank};
						words.push_back(stakes_word(set));
						check(read_stakes_word(words.back()) == set);
					}
	std::sort(words.begin(), words.end());
	check(std::adjacent_find(words.begin(), words.end()) == words.end(), "two different stakes gave the same word");
	// No stakes read as none, and so do words with a rank past Dark Lord on either side.
	check(!read_stakes_word(0) && !read_stakes_word(stakes_word(stakes{})));
	for (const std::uint32_t shift : {4u, 10u, 20u, 26u})
		check(!read_stakes_word((stakes_word(climbing) & ~(63u << shift)) | (35u << shift)));

	// Both PCs announced the same stakes: both plan the coming game from them.
	const auto both = stakes_word(climbing);
	check(agreed_stakes(both, both) == climbing && plan(*agreed_stakes(both, both), {1, 1}, 2) == values({1, 0}, {12, -1}));
	// The stakes reached one PC after the barrier before the command: both plan from none.
	check(!agreed_stakes(both, 0) && !agreed_stakes(0, both) && !agreed_stakes(0, 0));
	// Stakes that changed on one PC only (a resend the other has not read yet) give none as well.
	const auto resent = stakes_word(stakes{{{stake_event::promote, stake_event::none, 13, 11}, climbing[1]}});
	check(!agreed_stakes(both, resent));
	// Whichever PC decides, from its own word and the other's, it decides the same.
	for (const auto own : {both, resent, 0u})
		for (const auto other : {both, resent, 0u})
			check(agreed_stakes(own, other) == agreed_stakes(other, own));
}
