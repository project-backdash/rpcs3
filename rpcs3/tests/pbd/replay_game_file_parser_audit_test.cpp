#include "pbd_test.h"

#include "Emu/Cell/pbd_replay_game_file.h"
#include "Emu/PBD/pbd_replay_carry_codec.h"
#include "Emu/PBD/pbd_replay_difference.h"
#include "Emu/PBD/pbd_replay_capsule_start.h"
#include "Emu/PBD/pbd_replay_standby.h"
#include "replay_dense_input_fixture.h"

namespace
{
	namespace replay = pbd::replay_game_file;
	using pbd::test::check;

	// The emulator that recorded the fixture, by its SHA-256. The reader takes a file from any emulator build.
	const std::string recorded_build = "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef";
	const std::string closed_game =
		"{\"format\":\"pbd-replay-v2\",\"version\":2,\"draftRevision\":2,\"build\":{"
		"\"emulatorSha256\":\"0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef\","
		"\"gameExecutableSha256\":\"b56e25e9210ed3c4fc19f3dfae686b7246aef386cdd4f88c80e0b89141ff887d\","
		"\"capsuleSchema\":\"pbd-game-start-capsule-draft-2\"},\"games\":[{\"number\":1,"
		"\"capsule\":{\"qualified\":false,\"selection\":{\"roundsToWin\":1,\"stage\":17}},"
		"\"match\":{\"number\":1,\"endReason\":\"completed\",\"resultQualified\":false,\"roundsToWin\":1,"
		"\"stage\":17,\"score\":{\"left\":1,\"right\":0},\"winner\":\"left\"},\"roundCapsules\":[],"
		"\"timeline\":{\"qualified\":false,\"ticks\":1,\"endTick\":1},"
		"\"inputStream\":[{\"ticks\":1,\"counter\":0,\"sourceSegment\":0,\"startTick\":0,\"changes\":[[0,0,0]]}]}]}";

	std::string replace(std::string text, std::string_view before, std::string_view after)
	{
		const auto offset = text.find(before);
		check(offset != std::string::npos);
		text.replace(offset, before.size(), after);
		return text;
	}

	// A game start's carry naming each carried field but `except`. The decoder checks their values, not the reader.
	std::string carry_over(std::string_view except = {})
	{
		std::string fields;
		for (const auto name : pbd::replay_carry::carried_fields)
			if (name != except)
				fields += (fields.empty() ? "\"" : ",\"") + std::string(name) + "\":{}";
		return "\"carryOver\":{" + fields + "},";
	}

	// The file with one tick's sync facts, 48 bytes in the file's 64 letters: scene 8 (byte 12) and every other byte 0.
	std::string with_sync_facts(const std::string& file)
	{
		const std::string identity = "\"encoding\":\"pbd-sync-facts-v1\",\"definition\":\"pbd-peer-sync-v3\"";
		const std::string sync = "\"syncFacts\":{" + identity + ",\"data\":\"" + std::string(16, 'A') + "CAAA" +
		                         std::string(44, 'A') + "\"},";
		return replace(file, "\"roundCapsules\":", sync + "\"roundCapsules\":");
	}

	replay::json pbd_open_game_capsule()
	{
		using json = replay::json;
		const auto rng = json::object({{"sharedLibc140Hex", json::string(std::string(280, '0'))},
			{"flowLcg", json::array({json::number(4), json::number(5)})},
			{"mt", json::object({{"index", json::number(624)},
					   {"words", json::array(std::vector<json>(624, json::number(0)))}})},
			{"network", json::object({{"matchSeedLcg", json::number(1)}, {"state", json::number(2)}, {"count", json::number(3)}})},
			{"mokujin", json::object({{"stylesHex", json::string(std::string(36, '0'))},
							{"eligibilityHex", json::string(std::string(16, '0'))}})}});
		auto carry = pbd::replay_carry::encode_game_values({});
		carry.fields.insert(carry.fields.end(), {{"appliedHandicapPercent", json::array({json::number(100), json::number(125)})},
													{"nativeReplayFlag", json::number(0)}, {"nativeOrdinal", json::number(0)},
													{"styleProducer", json::object({{"nativeReplayFlag", json::number(0)}, {"nativeOrdinal", json::number(0)},
																		  {"leftRow", json::number(0)}, {"rightRow", json::number(1)}, {"generatorContext", rng}})},
													{"roundOptionBytesHex", json::string(std::string(26, '0'))}, {"roundsOptionHex", json::string("03")},
													{"gameRuleWordsHex", json::string("000000000000000200000003" + std::string(64, '0'))}});
		return json::object({{"schema", json::string("pbd-game-start-capsule-draft-2")},
			{"anchor", json::string("before-native-match-initializer")}, {"initializer", json::number(2)},
			{"sessionSeed", json::number(123)},
			{"selection", json::object({{"leftRow", json::number(0)}, {"rightRow", json::number(1)},
							  {"stage", json::number(17)}, {"roundsToWin", json::number(3)}, {"roundTimeIndex", json::number(2)}})},
			{"hostContext", json::object({{"matchReseedsBefore", json::number(2)}, {"mobBatchesBefore", json::number(7)},
								{"segmentCounter", json::number(90)}})},
			{"carryOver", std::move(carry)}, {"generatorContext", rng}});
	}
} // namespace

PBD_TEST(PbdReplayFileParserAudit, ThePrivateDraftParserRefusesSharedAndOtherVersions)
{
	check(replay::parse(closed_game).value);
	for (const auto wrong : {"pbd-replay-v1", "pbd-replay-v3"})
		check(!replay::parse(replace(closed_game, "pbd-replay-v2", wrong)).value);
	check(!replay::parse(replace(closed_game, "\"version\":2", "\"version\":1")).value);
	check(!replay::parse(replace(closed_game, "\"draftRevision\":2", "\"draftRevision\":3")).value);
}

PBD_TEST(PbdReplayFileParserAudit, AFileFromAnyEmulatorBuildReadsAndNamesThatBuild)
{
	const auto recorded = replay::parse(closed_game);
	check(recorded.value && recorded.value->emulator == recorded_build, "the recording emulator is not named");
	const std::string other_build(64, 'e');
	const auto other = replay::parse(replace(closed_game, recorded_build, other_build));
	check(other.value, "a file recorded on another emulator build is refused");
	check(other.value->emulator == other_build, "another build's file names the wrong emulator");
}

PBD_TEST(PbdReplayFileParserAudit, RecordedTerminalResultMustMatchBothScores)
{
	const auto three_rounds = replace(replace(closed_game, "\"roundsToWin\":1", "\"roundsToWin\":3"),
		"\"roundsToWin\":1", "\"roundsToWin\":3");
	const auto recorded = replay::parse(replace(three_rounds, "\"left\":1,\"right\":0", "\"left\":3,\"right\":1"));
	check(recorded.value && recorded.value->final_score == std::array<std::uint32_t, 2>{3, 1});
	const auto& expected = recorded.value->final_score;
	using pbd::replay_difference::terminal_refusal;
	check(!terminal_refusal(expected, {3, 1}, true));
	check(std::string_view(terminal_refusal(expected, {1, 3}, true)) == "native_replay_final_score_mismatch");
	check(std::string_view(terminal_refusal(expected, {3, 2}, true)) == "native_replay_final_score_mismatch");
	check(std::string_view(terminal_refusal(expected, {3, 1}, false)) == "native_replay_final_score_unavailable");
}

PBD_TEST(PbdReplayFileParserAudit, ARecordingMayNameItsLauncherVersion)
{
	const auto file = replace(closed_game, "\"capsuleSchema\":", "\"launcherVersion\":\"0.40.20\",\"capsuleSchema\":");
	const auto recorded = replay::parse(with_sync_facts(file));
	check(recorded.value && recorded.value->emulator == recorded_build, "the launcher version prevents playback");
	check(replay::playback_refusal(recorded).empty(), "the recording build refuses its launcher label");
}

PBD_TEST(PbdReplayFileParserAudit, AnOpenGameStartReadsWithoutFinalScoreOrInputs)
{
	const auto cap = pbd_open_game_capsule();
	const auto text = pbd::set_channel::detail::serialize(cap);
	const auto start = pbd::replay_start::pbd_read_capsule_start(std::string_view(text), 123);
	check(start.value, "a live start needs a finalized game file");
	check(start.value->left == 0 && start.value->right == 1 && start.value->stage == 17 &&
			  start.value->prior == 2 && start.value->mob_batches == 7 && start.value->counter == 90,
		"the live capsule start lost its resource choices or reseed context");
	check(start.value->handicaps == std::array<std::uint32_t, 2>{100, 125} && start.value->before_match->mt_index == 624,
		"the live capsule start lost its rules or random generator context");
	check(!pbd::replay_start::pbd_read_capsule_start(cap, 124).value, "another session's start is admitted");
	for (const auto* absent : {"\"stage\":14", "\"stage\":16", "\"stage\":35", "\"stage\":36", "\"stage\":38"})
		check(!pbd::replay_start::pbd_read_capsule_start(
				  std::string_view(replace(text, "\"stage\":17", absent)), 123)
				  .value,
			"an unsupported stage is admitted");
	for (const auto* loadable : {"\"stage\":0", "\"stage\":13", "\"stage\":15", "\"stage\":37"})
	{
		const auto with_stage = replace(text, "\"stage\":17", loadable);
		const auto stage_start = pbd::replay_start::pbd_read_capsule_start(std::string_view(with_stage), 123);
		check(stage_start.value && stage_start.value->stage == std::stoul(std::string(loadable).substr(8)),
			"an original stage or Inferno is refused at a replay's start");
	}
	check(!pbd::replay_start::pbd_read_capsule_start(
			  std::string_view(replace(text, "\"roundsOptionHex\":\"03\"", "\"roundsOptionHex\":\"02\"")), 123)
			  .value,
		"contradictory round settings are admitted");
}

PBD_TEST(PbdReplayFileParserAudit, RecordedPlacementNoticesFollowTheSameIntroAndRoundScreens)
{
	using namespace pbd::replay_notices;
	namespace notices = pbd::set_notices;
	notices::set_view live;
	live.mode = notices::set_mode::ranked;
	live.placements = {pbd::rank_stakes::placement{2, 5}, std::nullopt};
	auto shown = notices::compose(live);
	shown.generation = 7;
	const auto recorded = capture(shown, 7);
	check(recorded && !capture(shown, 8), "another emulation's counts were recorded");
	auto cap = pbd_open_game_capsule();
	check(!pbd::replay_start::pbd_read_capsule_start(cap, 123).value->placements, "an older start invents placement");
	cap.fields.emplace_back("placements", *recorded);
	const auto start = pbd::replay_start::pbd_read_capsule_start(
		std::string_view(pbd::set_channel::detail::serialize(cap)), 123);
	check(start.value && start.value->placements, "a recorded placement does not reach the viewer start");
	check((*start.value->placements)[0] == pbd::rank_stakes::placement{2, 5} && !(*start.value->placements)[1]);
	const auto& counts = *start.value->placements;
	check(notices::intro_pill(compose(counts, 17, 1, true, {}, {}, {})), "the placing player's intro lost its pill");
	check(!notices::intro_pill(compose(counts, 17, 2, true, {}, {}, {})), "the placed opponent acquired a pill");
	check(!notices::intro_pill(compose(counts, 17, 1, false, {}, {}, {})), "a later round repeated the first intro");
	check(!notices::intro_pill(compose(counts, 17, 1, true, {}, {}, {0, 1})), "placement covers the native Ranked pill");
	for (const auto phase : {4u, 5u, 8u})
	{
		const auto round = compose(counts, 17, phase, false, {true, true}, {}, {});
		check(notices::life_plate(round, 0) && !notices::life_plate(round, 1), "the round call/fight lost the correct plate");
		check(notices::placing_words(round.sides[0].shown.done, round.sides[0].shown.total) == "Placing 2/5");
	}
	check(!notices::life_plate(compose(counts, 17, 8, false, {true, true}, {true, false}, {}), 0), "promotion leaves placement on top");
	check(!notices::life_plate(compose(counts, 17, 9, false, {true, true}, {}, {}), 0), "a result screen keeps the fight plate");
	check(!notices::life_plate(compose(counts, 17, 4, false, {}, {}, {}), 0), "a hidden native plate gains an overlay");
	const placements both{pbd::rank_stakes::placement{0, 5}, pbd::rank_stakes::placement{5, 5}};
	check(notices::intro_pill(compose(both, 16, 2, true, {}, {}, {})), "a placing right side lost its own intro");
	check(!notices::intro_pill(compose(placements{}, 17, 1, true, {}, {}, {})), "already placed players gained a notice");
	for (const auto text : {"null", "[]", "[null]", "[null,null,null]", "[{},null]", "[{\"done\":0,\"total\":0},null]",
			 "[{\"done\":6,\"total\":5},null]", "[null,{\"done\":1,\"total\":11}]", "[{\"done\":-1,\"total\":5},null]",
			 "[{\"done\":1,\"total\":5,\"extra\":0},null]"})
	{
		auto malformed = pbd_open_game_capsule();
		malformed.fields.emplace_back("placements", *pbd::json::detail::json_parser(text).parse().value);
		const auto refused = pbd::replay_start::pbd_read_capsule_start(malformed, 123);
		check(!refused.value && refused.error == "capsule_start_placements");
	}
	shown.mode = notices::set_mode::quick;
	check(!capture(shown, 7), "Quick Match was labeled as placement");
	shown.mode = notices::set_mode::ranked;
	shown.active = false;
	check(!capture(shown, 7), "an inactive set was recorded");
}

PBD_TEST(PbdReplayFileParserAudit, RecordedRanksComeFromTheGameStartAndOlderRecordingsRemainReadable)
{
	auto cap = pbd_open_game_capsule();
	const auto older = pbd::replay_start::pbd_read_capsule_start(cap, 123);
	check(older.value && older.value->ranks == std::array<std::uint32_t, 2>{0, 0});
	cap.fields.emplace_back("players", replay::json::array({replay::json::object({{"rank", replay::json::number(22)}}),
										   replay::json::object({{"rank", replay::json::number(15)}})}));
	const auto text = pbd::set_channel::detail::serialize(cap);
	const auto recorded = pbd::replay_start::pbd_read_capsule_start(std::string_view(text), 123);
	check(recorded.value && recorded.value->ranks == std::array<std::uint32_t, 2>{22, 15});
	for (const auto* bad : {"-1", "37", "1.5", "null", "\"22\""})
		check(!pbd::replay_start::pbd_read_capsule_start(std::string_view(replace(text, "\"rank\":22", std::string("\"rank\":") + bad)), 123).value);
}

PBD_TEST(PbdReplayFileParserAudit, RankedRoundNumbersFollowRecordedInputsAfterAJump)
{
	const std::array<std::uint32_t, 5> starts{140, 2947, 5561, 8143, 10905};
	for (std::size_t i = 0; i < starts.size(); ++i)
	{
		check(replay::recorded_round(0b11111, starts, starts[i]).round == i + 1,
			"a jumped round uses the recorded native ordinal, including during its intro");
		check(replay::recorded_round(0b11111, starts, starts[i] + 1).round == i + 1,
			"the boundary after a jump keeps the same round without another fight transition");
	}
}

PBD_TEST(PbdReplayFileParserAudit, EveryBuildPlaysAFileOnlyWithItsSyncFacts)
{
	for (const auto& build : {recorded_build, std::string(64, 'e')})
	{
		const auto text = replace(closed_game, recorded_build, build);
		const auto bare = replay::parse(text);
		check(replay::playback_refusal(bare) == "replay_sync_facts_required", "a file with no sync checks is played");
		const auto checked = replay::parse(with_sync_facts(text));
		check(checked.value && checked.value->sync.size() == 1, "the sync facts do not read");
		check(replay::playback_refusal(checked).empty(), "a file with sync checks is refused");
	}
	check(replay::playback_refusal(replay::parse("")) == "private_file_extent", "a file that does not parse plays");
}

PBD_TEST(PbdReplayFileParserAudit, ALaterGameRequiresItsCarryAndSyncFacts)
{
	// parse ties the game's number to its match's.
	const auto second = replace(replace(closed_game, "\"games\":[{\"number\":1,", "\"games\":[{\"number\":2,"),
		"\"match\":{\"number\":1,", "\"match\":{\"number\":2,");
	const auto carried = replace(second, "\"capsule\":{", "\"capsule\":{" + carry_over());
	const auto first = replay::parse(closed_game), bare = replay::parse(second), whole = replay::parse(carried);
	check(first.value && bare.value && whole.value && bare.value->number == 2, "the fixtures do not read");
	for (const auto name : pbd::replay_carry::carried_fields)
	{
		const auto part = replay::parse(with_sync_facts(replace(second, "\"capsule\":{", "\"capsule\":{" + carry_over(name))));
		check(part.value && replay::playback_refusal(part) == "replay_previous_game_carry_required",
			"a later game without one carried field is played");
	}
	check(replay::playback_refusal(bare) == "replay_sync_facts_required",
		"a later game without its carry plays with nothing to check against");
	check(replay::playback_refusal(whole) == "replay_sync_facts_required", "a carried later game bypasses sync checks");
	check(replay::playback_refusal(replay::parse(with_sync_facts(carried))).empty(), "a complete older game is refused");
	check(replay::playback_refusal(replay::parse(with_sync_facts(second))) == "replay_previous_game_carry_required",
		"a later game without its carry is played with the wrong start");
}

PBD_TEST(PbdReplayFileParserAudit, AGameFromARecorderBeforeTheCarriedFieldsReadsWithoutInventingThem)
{
	// The carry such a recorder wrote at a game start: the match history and the rules, no carried field.
	const std::string older = "\"carryOver\":{\"nativeOrdinal\":0,\"vsMatchHistory\":{\"schema\":"
							  "\"dr-vs-match-history-draft-1\",\"completedGames\":1,\"wins\":[0,1]},"
							  "\"appliedHandicapPercent\":[100,100],\"nativeReplayFlag\":0,"
							  "\"roundsOptionHex\":\"03\"},";
	const auto start = [&](const std::string& file)
	{
		return replay::parse(replace(file, "\"capsule\":{", "\"capsule\":{" + older));
	};
	const auto first = start(closed_game);
	const auto second = start(replace(replace(closed_game, "\"games\":[{\"number\":1,", "\"games\":[{\"number\":2,"),
		"\"match\":{\"number\":1,", "\"match\":{\"number\":2,"));
	check(first.value && second.value, "a game start without the carried fields is refused");
	const auto values = pbd::replay_carry::decode_game_values(*second.value->capsule.find("carryOver"));
	check(values.value && values.value->history && values.value->history->wins == std::array<std::uint16_t, 2>{0, 1},
		"the match history does not decode");
	check(!values.value->flow && !values.value->records && !values.value->planar && !values.value->bone_segments,
		"a carried field absent from the file is invented");
}

PBD_TEST(PbdReplayFileParserAudit, AStopIsInTheLastCapturedRoundStartedAtOrBeforeIt)
{
	const std::array<std::uint32_t, 3> starts{100, 700, 1300};
	const auto at = [&](std::uint32_t captured, std::size_t offset)
	{
		return replay::recorded_round(captured, starts, offset);
	};
	check(at(0b111, 40).round == 0 && at(0b111, 40).start == 0, "an input before round 1 is in a round");
	check(at(0b111, 699).round == 1, "the input before round 2 is not in round 1");
	check(at(0b111, 700).round == 2 && at(0b111, 700).start == 700, "round 2's first input is not in round 2");
	check(at(0b101, 900).round == 1 && at(0b101, 900).start == 100, "an input of a round never captured is not in the one before");
	check(at(0b101, 1300).round == 3 && at(0b101, 1300).start == 1300, "a round after one never captured is lost");
	check(at(0, 1300).round == 0, "an input is in a round with none captured");
}

PBD_TEST(PbdReplayFileParserAudit, AllTruncatedPrefixesOversizedInputAndDuplicateKeysAreRefused)
{
	for (std::size_t size = 0; size < closed_game.size(); size++)
		check(!replay::parse(std::string_view(closed_game).substr(0, size)).value);
	check(!replay::parse(std::string(replay::max_file_bytes + 1, ' ')).value);
	check(!replay::parse(closed_game + "{}").value);
	check(!replay::parse(replace(closed_game, "\"version\":2", "\"version\":1,\"version\":2")).value);
}

PBD_TEST(PbdReplayFileParserAudit, DenseLauncherRecordingKeepsEveryInputAndSyncFact)
{
	std::string text;
	for (const auto part : pbd_dense_input_fixture)
		text += part;
	const auto file = replay::parse(text);
	check(file.value, file.error);
	check(replay::playback_refusal(file).empty());
	check(file.value->inputs.size() == 9000 && file.value->sync.size() == 9000);
	for (std::uint32_t i = 0; i < 9000; ++i)
	{
		const auto& input = file.value->inputs[i];
		check(input.segment == 26 && input.counter == i &&
			  input.words == std::array<std::uint16_t, 2>{i % 2 ? std::uint16_t{32} : std::uint16_t{64}, 0});
		check(file.value->sync[i].boundary == 101 + i && file.value->sync[i].segment == 26);
	}
}

PBD_TEST(PbdReplayFileParserAudit, EveryAdmittedFrameMayStartItsOwnInputSegment)
{
	const auto ticks = std::to_string(replay::max_inputs);
	auto text = replace(closed_game, "\"ticks\":1,\"endTick\":1", "\"ticks\":" + ticks + ",\"endTick\":" + ticks);
	const auto stream = text.find("\"inputStream\":");
	check(stream != std::string::npos);
	text.resize(stream);
	text += "\"inputStream\":[";
	for (std::size_t i = 0; i < replay::max_inputs; ++i)
	{
		if (i)
			text += ',';
		text += "{\"ticks\":1,\"counter\":0,\"sourceSegment\":0,\"startTick\":" + std::to_string(i) +
		        ",\"changes\":[[0,64,32]]}";
	}
	text += "]}]}";
	const auto file = replay::parse(text);
	check(file.value, file.error);
	check(file.value->inputs.size() == replay::max_inputs);
	for (const auto& input : file.value->inputs)
		check(input.counter == 0 && input.words == std::array<std::uint16_t, 2>{64, 32});
}

PBD_TEST(PbdReplayFileParserAudit, MissingIdentityAndUnsafeInputExtentsAreRefused)
{
	for (const auto [before, after] : {
			 std::pair{"\"build\":", "\"missingBuild\":"},
			 std::pair{"\"emulatorSha256\":", "\"emulator\":"},
			 std::pair{"\"emulatorSha256\":\"0", "\"emulatorSha256\":\""},
			 std::pair{"\"emulatorSha256\":\"0", "\"emulatorSha256\":\"G"},
			 std::pair{"\"emulatorSha256\":\"0123456789abcdef", "\"emulatorSha256\":\"0123456789ABCDEF"},
			 std::pair{"\"number\":1", "\"number\":0"},
			 std::pair{"\"ticks\":1", "\"ticks\":100001"},
			 std::pair{"\"startTick\":0", "\"startTick\":1"},
			 std::pair{"[0,0,0]", "[0,65536,0]"},
			 std::pair{"[0,0,0]", "[0,-1,0]"},
			 std::pair{"\"qualified\":false", "\"qualified\":true"}})
		check(!replay::parse(replace(closed_game, before, after)).value);
}

PBD_TEST(PbdReplayFileParserAudit, TheRecordedPlayersNamesAreReadForDisplayOnly)
{
	const auto bare = replay::parse(closed_game);
	check(bare.value && bare.value->names[0].empty() && bare.value->names[1].empty(), "a file without players names somebody");
	const auto named = replay::parse(replace(closed_game, "\"stage\":17,\"score\"",
		"\"players\":{\"left\":{\"id\":\"p1\",\"name\":\"Ana\",\"row\":0},\"right\":{\"id\":\"p2\",\"name\":\"Kai\",\"row\":1}},"
		"\"stage\":17,\"score\""));
	check(named.value && named.value->names[0] == "Ana" && named.value->names[1] == "Kai", "the recorded names are not read");
	// Anything else where a name belongs names nobody and still plays: the names never decide whether a file loads.
	const auto odd = replay::parse(replace(closed_game, "\"stage\":17,\"score\"",
		"\"players\":{\"left\":{\"name\":7},\"right\":\"Kai\"},\"stage\":17,\"score\""));
	check(odd.value && odd.value->names[0].empty() && odd.value->names[1].empty(), "an odd players field is not ignored");
}

PBD_TEST(PbdReplayFileParserAudit, TheFightersRecordsReadWholeFromAGameStart)
{
	// The records are one string of 9,024 digits, longer than a set channel string may be.
	std::string digits;
	while (digits.size() < 2 * pbd::replay_memory::actor_records_bytes)
		digits += "0123456789abcdef";
	const auto file = replay::parse(replace(closed_game, "\"capsule\":{",
		"\"capsule\":{\"carryOver\":{\"actorRecords\":{\"bytesHex\":\"" + digits + "\",\"schema\":\"pbd-actor-records-1\"}},"));
	check(file.value, "a game start with the records is refused");
	const auto* carry = file.value->capsule.find("carryOver");
	check(carry, "the carried values are dropped");
	const auto values = pbd::replay_carry::decode_game_values(*carry);
	check(values.value && values.value->records, "the records do not decode");
	const auto& bytes = values.value->records->bytes;
	check(bytes.front() == 0x01 && bytes[1] == 0x23 && bytes.back() == 0xef, "the records decode to other bytes");
}

PBD_TEST(PbdReplayFileParserAudit, CompleteRoundPointsKeepFlowAndFighterRecords)
{
	using namespace pbd::replay_carry;
	using json = replay::json;
	actor_records records;
	records.bytes.fill(0x7a);
	const game_values values{vs_match_history{2, {1, 0}}, previous_game_flow{{11, 22, 33, 44, 55}},
		actor_planar_terms{}, std::uint8_t{0}, actor_transform_sum{}, actor_descriptor_code{}, actor_descriptor_deltas{},
		actor_reaction_cursor{}, actor_motion_step{}, actor_last_reaction_code{}, actor_reaction_conditions{},
		actor_bone_segments{}, actor_reaction_result{}, actor_hit_contacts{}, actor_segment_deltas{},
		actor_scalar_bits{}, actor_scalar_bits{}, actor_scalar_bits{}, records};
	const auto point = json::object({{"initializer", json::number(4)}, {"nativeOrdinal", json::number(0)},
		{"frameCounter", json::number(90)}, {"inputOffset", json::number(0)}, {"sourceSegment", json::number(0)},
		{"spawnSelector", json::number(0)},
		{"scoreResult", json::array({json::array({json::number(0), json::number(0)}),
							json::array({json::number(0), json::number(0)})})},
		{"generatorContext", *pbd_open_game_capsule().find("generatorContext")}, {"carryOver", encode_game_values(values)}});
	const auto cap = json::object({{"schema", json::string("pbd-round-start-capsule-draft-2")},
		{"anchor", json::string("before-native-round-initializer")}, {"round", json::number(1)},
		{"qualified", json::boolean(false)}, {"cleanup", json{}}, {"ready", point}});
	const auto text = pbd::set_channel::detail::serialize(cap);
	const auto file = replace(closed_game, "\"roundCapsules\":[]", "\"roundCapsules\":[" + text + "]");
	const auto read = replay::parse(file);
	check(read.value && read.value->rounds.size() == 1 && read.value->rounds[0].ready.carry,
		"a complete round point lost its retained values");
	const auto& saved = *read.value->rounds[0].ready.carry;
	check(saved.flow && saved.flow->words == values.flow->words && saved.records && *saved.records == records,
		"a complete round point changed its flow words or fighter bytes");
	check(!replay::parse(replace(file, "\"actorRecords\":", "\"missingRecords\":")).value,
		"a complete round point admits absent fighter records");
	const auto legacy = replay::parse(replace(file, "pbd-round-start-capsule-draft-2", "pbd-round-start-capsule-draft-1"));
	check(legacy.value && !legacy.value->rounds[0].ready.carry,
		"an older coordinate-only point invents complete reconstruction data");
}

PBD_TEST(PbdReplayFileParserAudit, SyncFactsRefuseWrongExtentAlphabetAndNativePolicy)
{
	for (const auto text : {std::string(63, 'A'), std::string(65, 'A'), std::string(64, '!'), std::string(64, 'A')})
	{
		bool refused = false;
		try
		{
			(void)replay::sync_point(text);
		}
		catch (const std::invalid_argument&)
		{
			refused = true;
		}
		check(refused);
	}
}

PBD_TEST(PbdReplayFileParserAudit, StandbyBindRequiresACompletedFileAndEveryReadyIdentity)
{
	namespace standby = pbd::replay_standby;
#ifdef _WIN32
	const std::string path = "C:/replays/game.pbdreplay";
#else
	const std::string path = "/replays/game.pbdreplay";
#endif
	const std::string digest(64, 'a');
	const std::string text = "{\"schema\":\"pbd-replay-standby-bind-1\",\"nativeGeneration\":\"18446744073709551615\","
	                         "\"seed\":4294967295,\"replayFile\":\"" +
	                         path + "\",\"checkpointSha256\":\"" + digest +
	                         "\",\"emulatorSha256\":\"" + digest + "\",\"recipeSha256\":\"" + digest + "\",\"replaySha256\":\"" + digest + "\"}";
	const auto parsed = standby::pbd_parse(text);
	check(parsed.value && parsed.value->generation == UINT64_MAX && parsed.value->seed == UINT32_MAX);
	for (const auto& invalid : {
			 replace(text, "18446744073709551615", "18446744073709551616"),
			 replace(text, "18446744073709551615", "0"), replace(text, "4294967295", "0"),
			 replace(text, "4294967295", "4294967296"), replace(text, "4294967295", "-1"),
			 replace(text, path, "game.pbdreplay"), replace(text, ".pbdreplay", ".json"),
			 replace(text, "\"replaySha256\"", "\"unknown\""), replace(text, digest, std::string(64, 'A')),
			 text + std::string(8192, ' ')})
		check(!standby::pbd_parse(invalid).value, "standby admitted an incomplete or malformed bind");
	struct ready_identity
	{
		std::uint64_t native_generation;
		std::string checkpoint, emulator, recipe;
	};
	ready_identity ready{UINT64_MAX, digest, digest, digest};
	check(!standby::pbd_identity_refusal(*parsed.value, ready));
	ready.native_generation = 1;
	check(std::string_view(standby::pbd_identity_refusal(*parsed.value, ready)) == "replay_standby_generation");
	ready.native_generation = UINT64_MAX;
	for (auto* field : {&ready.checkpoint, &ready.emulator, &ready.recipe})
	{
		*field = std::string(64, 'b');
		check(standby::pbd_identity_refusal(*parsed.value, ready));
		*field = digest;
	}
	const auto bound = pbd::set_channel::detail::json_parser(standby::pbd_bound_text(*parsed.value), 64).parse();
	check(bound.value && pbd::set_channel::detail::is(bound.value->find("type"), "bound") &&
		  pbd::set_channel::detail::is(bound.value->find("replaySha256"), digest));
}

PBD_TEST(PbdReplayFileParserAudit, SavedReplayAdoptionRetiresOnlyItsLiveWatchingCover)
{
	namespace standby = pbd::replay_standby;
	namespace held = pbd::held_presentation;
	namespace spectator = pbd::spectator;
	const standby::pbd_binding binding{123, 456, {}, std::string(64, 'a'), std::string(64, 'b'),
		std::string(64, 'c'), std::string(64, 'd')};
	spectator::publish_presentation(binding.generation, {});
	check(spectator::load_presentation(binding.generation).active && spectator::load_presentation(binding.generation).hidden);
	held::lifecycle match(held::purpose::match);
	check(!standby::pbd_bind_presentation({}, binding, match));
	check(spectator::load_presentation(binding.generation).active, "a refused replay bind removed the live cover");
	held::lifecycle replay(held::purpose::replay);
	check(standby::pbd_bind_presentation({}, binding, replay));
	check(!spectator::load_presentation(binding.generation).active, "the live preparation cover still obscures the saved game");
	check(!replay.can_show(), "retiring the cover bypassed the held game's picture gate");
	check(spectator::load_presentation(binding.generation + 1).active, "saved playback removed another runtime's live cover");
	check(!standby::pbd_bind_presentation({}, binding, replay), "the same held game bound twice");
	standby::pbd_report() = {};
	spectator::publish_presentation(0, {});
}
