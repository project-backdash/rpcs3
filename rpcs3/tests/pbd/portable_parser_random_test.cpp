#include "pbd_test.h"

#include "Emu/Cell/pbd_update_script.h"
#include "Emu/Cell/pbd_replay_game_file.h"
#include "Emu/PBD/pbd_spectator_channel_input.h"
#include "Emu/PBD/pbd_env_parse.h"
#include "Emu/PBD/pbd_peer_message.h"
#include "Emu/PBD/pbd_practice_store.h"

#include <array>
#include <cstdint>
#include <random>
#include <span>
#include <string>
#include <string_view>
#include <vector>

using pbd::test::check;

namespace
{
	constexpr std::uint32_t seed = 0x50424431;
	constexpr unsigned trials = 1024;
	using generator = std::mt19937;

	std::string mutate(std::string text, generator& random)
	{
		// Raw engine outputs fix the corpus across standard-library implementations.
		for (unsigned i = 0, count = 1 + random() % 4; i < count; ++i)
		{
			const auto at = random() % (text.size() + 1);
			switch (random() % 4)
			{
			case 0: text.insert(at, 1, static_cast<char>(random() & 0xff)); break;
			case 1: text.resize(at); break;
			case 2:
				if (at < text.size())
					text.erase(at, 1);
				break;
			case 3:
				if (at < text.size())
					text[at] = static_cast<char>(random() & 0xff);
				break;
			}
		}
		return text;
	}

	template <typename T>
	void check_result(const pbd::json::result<T>& parsed)
	{
		check(parsed.value.has_value() == parsed.error.empty(), "a result carries either a value or a refusal reason");
	}

	const std::string set_seed =
		R"({"schema":"pbd-set-contract-1","sessionId":"session-1","setId":"set-1","matchId":"match-1",)"
		R"("gameNumber":1,"localMemberId":"left-member","localSide":"left",)"
		R"("left":{"memberId":"left-member","name":"Left","character":{"id":"nina_williams","row":12}},)"
		R"("right":{"memberId":"right-member","name":"Right","character":{"id":"bryan_fury","row":32}},)"
		R"("stage":{"rule":"fixed","id":17},"rules":{"firstToGames":2,"roundsPerGame":3},)"
		R"("score":{"left":0,"right":0},"build":"test-build","compat":"test-compat",)"
		R"("spectator":{"publishConfirmedInputs":true,"encoding":"pbd-pad-v1"}})";

	const std::string command_seed =
		R"({"schema":"pbd-set-contract-1","sessionId":"session-1","setId":"set-1","serial":1,)"
		R"("type":"end_set","reason":"completed"})";
} // namespace

PBD_TEST(PbdPortableRandom, DecimalAndSceneMutationsKeepTheirStrictContracts)
{
	generator random(seed + 2);
	for (unsigned i = 0; i < trials; ++i)
	{
		const std::uint32_t number = random();
		const auto valid = std::to_wstring(number);
		std::uint32_t out = 0;
		check(pbd::env::parse_u32_decimal(valid, out) && out == number, "every generated u32 reads exactly");
		auto invalid = valid;
		invalid.insert(random() % (invalid.size() + 1), 1, static_cast<wchar_t>(L':' + random() % 64));
		out = 0xdeadbeef;
		check(!pbd::env::parse_u32_decimal(invalid, out) && out == 0xdeadbeef, "refusal never changes the output");
		const auto pair = valid + L"/" + std::to_wstring(random());
		const auto scenes = pbd::env::parse_initial_scenes(pair);
		check(scenes && scenes->size() == 1 && scenes->front().first == number, "a generated scene pair reads");
		check(!pbd::env::parse_initial_scenes(pair + L","), "a torn next scene is refused");
	}
}

PBD_TEST(PbdPortableRandom, PeerMutationsPreserveTheOutputWhenRefused)
{
	using namespace pbd::peer_message;
	generator random(seed + 3);
	for (unsigned i = 0; i < trials; ++i)
	{
		input_message message;
		message.role = random() % 2;
		message.count = random() % (max_inputs + 1);
		message.sync.session = random();
		for (std::uint32_t k = 0; k < message.count; ++k)
			message.inputs[k] = {random(), static_cast<std::uint16_t>(random())};
		std::array<std::uint8_t, max_input_bytes> encoded{};
		const auto size = encode_input(message, encoded);
		input_message decoded;
		check(read_input(std::span(encoded).first(size), 1 - message.role, message.sync.session, decoded) == input_check::valid);
		check(decoded == message, "a generated production message round-trips");
		std::string bytes(reinterpret_cast<const char*>(encoded.data()), size);
		bytes = mutate(std::move(bytes), random);
		const auto sentinel = decoded;
		const auto status = read_input({reinterpret_cast<const std::uint8_t*>(bytes.data()), bytes.size()},
			1 - message.role, message.sync.session, decoded);
		if (status != input_check::valid)
			check(decoded == sentinel, "a refused mutated datagram leaves all fields unchanged");
		const auto end = encode_end({message.role, message.sync.session, random(), random()});
		check(read_end(end, 1 - message.role, message.sync.session), "a generated end message reads");
		check(!read_end(std::span(end).first(random() % end_bytes), 1 - message.role, message.sync.session),
			"every generated truncated end message refuses");
	}
}

PBD_TEST(PbdPortableRandom, SetManifestAndCommandMutationsReturnBoundedResults)
{
	const pbd::set_channel::capabilities supported{"test-build", "test-compat", {12, 32}, {17}};
	const auto admitted = pbd::set_channel::parse_manifest(set_seed, supported);
	check(admitted.value, "the mutation corpus starts with a valid manifest");
	check(pbd::set_channel::parse_command(command_seed, *admitted.value, 1, 1).value,
		"the command corpus starts with a valid command");
	generator random(seed + 4);
	for (unsigned i = 0; i < trials; ++i)
	{
		check_result(pbd::set_channel::parse_manifest(mutate(set_seed, random), supported));
		check_result(pbd::set_channel::parse_command(mutate(command_seed, random), *admitted.value, 1, 1));
	}
}

PBD_TEST(PbdPortableRandom, PracticeMutationsKeepAcceptedSettingsReadable)
{
	using namespace pbd::practice_store;
	const auto profile_seed = serialize(profile{}, "2000-01-01T00:00:00Z");
	const auto recordings_seed = serialize(empty_recordings(12), "2000-01-01T00:00:00Z");
	check(parse_profile(profile_seed).value && parse_recordings(recordings_seed, 12).value);
	generator random(seed + 5);
	for (unsigned i = 0; i < trials; ++i)
	{
		const auto settings = parse_profile(mutate(profile_seed, random));
		const auto actions = parse_recordings(mutate(recordings_seed, random), 12);
		check_result(settings);
		check_result(actions);
		if (settings.value)
			check(parse_profile(serialize(*settings.value, "")).value, "accepted profile values can be stored again");
		if (actions.value)
			check(parse_recordings(serialize(*actions.value, ""), 12).value, "accepted recordings can be stored again");
	}
}

PBD_TEST(PbdPortableRandom, ReplayMutationsKeepAcceptedInputExtentsConsistent)
{
	generator random(seed + 6);
	const std::string identity =
		R"({"format":"pbd-replay-v2","version":2,"draftRevision":2,"build":{)"
		R"("emulatorSha256":"0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef",)"
		R"("gameExecutableSha256":"b56e25e9210ed3c4fc19f3dfae686b7246aef386cdd4f88c80e0b89141ff887d",)"
		R"("capsuleSchema":"pbd-game-start-capsule-draft-2"},"games":[{"number":1, "capsule":{)"
		R"("qualified":false,"selection":{"roundsToWin":3,"stage":17}},"match":{)"
		R"("number":1,"endReason":"completed","resultQualified":false,"roundsToWin":3,"stage":17,)"
		R"("score":{"left":3,"right":0},"winner":"left"},"roundCapsules":[],)"
		R"("timeline":{"qualified":false,"ticks":3,"endTick":3},"inputStream":[{)"
		R"("ticks":3,"counter":100,"sourceSegment":1,"startTick":0,"changes":[[0,64,32],[2,0,0]]}]}]})";
	const auto original = pbd::replay_game_file::parse(identity);
	check(original.value && original.value->inputs.size() == 3, "the replay corpus starts with a valid input stream");
	for (unsigned i = 0; i < trials; ++i)
	{
		const auto parsed = pbd::replay_game_file::parse(mutate(identity, random));
		check_result(parsed);
		if (parsed.value)
		{
			check(!parsed.value->inputs.empty() && parsed.value->inputs.size() <= pbd::replay_game_file::max_inputs);
			for (std::size_t k = 1; k < parsed.value->inputs.size(); ++k)
				check(parsed.value->inputs[k].counter == parsed.value->inputs[k - 1].counter + 1,
					"accepted input counters advance without an overflow");
		}
	}
}

PBD_TEST(PbdPortableRandom, SpectatorRoundAndPickMutationsValidateTheirRanges)
{
	using namespace pbd::spectator;
	generator random(seed + 7);
	const std::string round =
		R"({"encoding":"pbd-native-round-start-v1","pendingPhase":4,"roundOrdinal":0,"randomWords":[0,65535]})";
	const std::string selected = R"({"stageId":17,"leftCharacterRow":12,"rightCharacterRow":32})";
	for (unsigned i = 0; i < trials; ++i)
	{
		const auto parsed = pbd::json::detail::json_parser(mutate(i % 2 ? round : selected, random)).parse();
		if (parsed.value)
		{
			if (const auto accepted = read_native_round(*parsed.value))
				check(accepted->pending_phase == 4 && accepted->ordinal <= 8);
			std::optional<picks> picked;
			if (read_picks(*parsed.value, picked) && picked)
				check(plausible(*picked), "every accepted selection fits the spectator ranges");
		}
		digest decoded{};
		unhex(mutate(std::string(64, 'a'), random), decoded);
	}
}
