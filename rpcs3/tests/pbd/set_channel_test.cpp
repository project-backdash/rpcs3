#include "pbd_test.h"

#include "Emu/Cell/pbd_set_channel.h"
#include "Emu/PBD/pbd_features.h"
#include "Emu/PBD/pbd_replay_journal.h"

#include <regex>
#ifdef _WIN32
#define NOMINMAX
#include <Windows.h>
#include <thread>
#endif

using pbd::test::check;

PBD_TEST(PbdSetChannel, SlowCommandReadsNeverHoldTheGameOrReturnAnotherSerial)
{
	using namespace pbd::set_channel;
	std::atomic<bool> entered{false}, release{false}, allow_second{false};
	std::array<std::atomic<unsigned>, 3> attempts{};
	command_reader reader([&](std::uint64_t serial) -> result<std::string>
		{
			++attempts.at(serial - 1);
			if (serial == 1)
			{
				entered.store(true);
				while (!release.load())
					std::this_thread::yield();
				return {"first command", {}};
			}
			if (serial == 2)
				return !allow_second.load() ? result<std::string>{{}, "read_failed"} : result<std::string>{"second command", {}};
			return {{}, "file_too_large"};
		});
	struct unblock
	{
		std::atomic<bool>& value;
		~unblock()
		{
			value.store(true);
		}
	} cleanup{release};
	const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
	while (!entered.load() && std::chrono::steady_clock::now() < deadline)
		std::this_thread::yield();
	check(entered.load());
	// The filesystem reader stays blocked throughout these production polls.
	for (unsigned i = 0; i < 10000; ++i)
		check(reader.poll(1).error == "unavailable");
	release.store(true);
	const auto await = [&](std::uint64_t serial, std::string_view error = {})
	{
		result<std::string> value;
		const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(5);
		do
		{
			value = reader.poll(serial);
			if (value.value || (!error.empty() && value.error == error))
				return value;
			std::this_thread::sleep_for(std::chrono::milliseconds(1));
		} while (std::chrono::steady_clock::now() < until);
		return value;
	};
	check(await(1).value == "first command" && attempts[0].load() == 1);
	check(reader.poll(1).value == "first command" && attempts[1].load() == 0);
	check(!reader.poll(2).value);
	check(await(2, "read_failed").error == "read_failed");
	allow_second.store(true);
	check(await(2).value == "second command" && attempts[1].load() >= 2);
	check(await(3, "file_too_large").error == "file_too_large");
	check(reader.poll(3).error == "file_too_large" && attempts[2].load() == 1);
}

PBD_TEST(PbdReplayJournal, KeepsRoundBarriersAndConsecutiveGamesOnTheWriter)
{
	const auto parent = std::filesystem::temp_directory_path();
	const auto directory = parent / ("pbd-replay-journal-test-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
	check(std::filesystem::create_directory(directory));
	{
		pbd::replay::journal output;
		output.start();
		const auto first = directory / "game-1.partial.jsonl", second = directory / "game-2.partial.jsonl";
		check(output.open(first));
		check(output.append("game_start\n", true));
		check(output.append("input-1\n", false));
		check(output.append("round_start\n", true));
		check(output.append("input-2\n", false));
		check(output.append("game_end\n", true));
		check(output.close() && output.open(second));
		check(output.append("next_game_start\n", true) && output.close());
		check(output.drain() && output.healthy());
		check(pbd::json::detail::read_text(first).value == "game_start\ninput-1\nround_start\ninput-2\ngame_end\n");
		check(pbd::json::detail::read_text(second).value == "next_game_start\n");
	}
	{
		pbd::replay::journal collision;
		collision.start();
		check(collision.open(directory / "game-1.partial.jsonl"));
		check(!collision.drain() && !collision.healthy());
		check(!collision.append("must not overwrite the saved prefix\n", true));
		check(pbd::json::detail::read_text(directory / "game-1.partial.jsonl").value == "game_start\ninput-1\nround_start\ninput-2\ngame_end\n");
	}
	const auto resolved = std::filesystem::canonical(directory);
	check(std::filesystem::equivalent(resolved.parent_path(), parent) && resolved.filename().string().starts_with("pbd-replay-journal-test-"));
	std::filesystem::remove_all(resolved);
}

PBD_TEST(PbdSetChannel, SlowPublicationNeverHoldsTheProducerOrOvertakesASerial)
{
	using namespace pbd::set_channel;
	std::atomic<bool> entered{false}, release{false};
	std::vector<std::pair<std::uint64_t, std::string>> published;
	publication_writer<4> writer([&](std::uint64_t serial, const std::string& text)
		{
			entered.store(true);
			while (!release.load())
				std::this_thread::yield();
			published.emplace_back(serial, text);
			return publication_result{true, false, nullptr};
		});
	struct unblock
	{
		std::atomic<bool>& value;
		~unblock()
		{
			value.store(true);
		}
	} cleanup{release};
	check(writer.submit(1, "input-1"));
	while (!entered.load())
		std::this_thread::yield();
	// The real consumer is inside a stalled filesystem operation. Submitting and filling the bounded queue still returns.
	check(writer.submit(2, "input-2") && writer.submit(3, "game_result") && writer.submit(4, "command_applied"));
	check(!writer.submit(5, "must not overwrite serial 1"));
	release.store(true);
	check(writer.flush(4));
	check((published == std::vector<std::pair<std::uint64_t, std::string>>{{1, "input-1"}, {2, "input-2"}, {3, "game_result"}, {4, "command_applied"}}));
	check(writer.submit(5, "next_set") && writer.flush(5));
	check(published.back() == std::pair<std::uint64_t, std::string>{5, "next_set"});
}

PBD_TEST(PbdSetChannel, FailedBackgroundPublicationCannotPublishALaterResult)
{
	using namespace pbd::set_channel;
	std::vector<std::uint64_t> attempted;
	publication_writer<> writer([&](std::uint64_t serial, const std::string&)
		{
			attempted.push_back(serial);
			return serial == 2 ? publication_result{false, false, "event_write_failed"} : publication_result{true, false, nullptr};
		});
	check(writer.submit(1, "first input"));
	// A consumer failure may race submission; neither schedule may let the terminal result pass serial 2.
	writer.submit(2, "second input");
	writer.submit(3, "game_result");
	check(!writer.flush(3) && std::string_view(writer.error()) == "event_write_failed");
	check((attempted == std::vector<std::uint64_t>{1, 2}));
	check(!writer.submit(4, "next result"));
}

PBD_TEST(PbdSetChannel, TransientPublicationKeepsTheAppliedCommand)
{
	using namespace pbd::set_channel;
	auto now = std::chrono::steady_clock::time_point{};
	unsigned attempts = 0, published = 0;
	const std::string pending = "same command_applied serial and payload";
	std::vector<std::string> writes;
	const auto result = retry_publication([&]() -> publication_result
		{
			++attempts;
			writes.push_back(pending);
			if (attempts == 1)
				return {false, true, "event_publish_failed"};
			++published;
			return {true, false, nullptr};
		},
		[&]
		{
			now += std::chrono::milliseconds(1);
		},
		[&]
		{
			return now;
		});
	check(result.published && attempts == 2 && published == 1);
	check(writes.size() == 2 && writes[0] == writes[1]);
	attempts = 0;
	const auto failed = retry_publication([&]() -> publication_result
		{
			++attempts;
			return {false, true, "event_open_failed"};
		},
		[&]
		{
			now += std::chrono::milliseconds(1);
		},
		[&]
		{
			return now;
		});
	check(!failed.published && attempts == 17 && std::string_view(failed.error) == "event_open_failed");
	attempts = 0;
	const auto collision = retry_publication([&]() -> publication_result
		{
			++attempts;
			return {false, false, "event_publish_failed"};
		},
		[] {}, [&]
		{
			return now;
		});
	check(!collision.published && attempts == 1);
}

namespace
{
	using namespace pbd::set_channel;
	const capabilities supported{"43zr", "fingerprint", {12, 32, 51}, {17, 23}};
	const std::string sample_document = R"({"schema":"pbd-set-contract-1","sessionId":"session-1","setId":"set-1","matchId":"match-1","gameNumber":1,"localMemberId":"left-member","localSide":"left","left":{"memberId":"left-member","name":"Left","character":{"id":"nina_williams","row":12}},"right":{"memberId":"right-member","name":"Right","character":{"id":"bryan_fury","row":32}},"stage":{"rule":"fixed","id":17},"rules":{"firstToGames":2,"roundsPerGame":3},"score":{"left":0,"right":0},"build":"43zr","compat":"fingerprint","spectator":{"publishConfirmedInputs":true,"encoding":"pbd-pad-v1"}})";

	std::string changed(std::string text, std::string_view from, std::string_view to)
	{
		const auto at = text.find(from);
		check(at != std::string::npos);
		text.replace(at, from.size(), to);
		return text;
	}
	std::string envelope(std::string_view fields, std::uint64_t serial = 1)
	{
		return "{\"schema\":\"pbd-set-contract-1\",\"sessionId\":\"session-1\",\"setId\":\"set-1\",\"serial\":" + std::to_string(serial) + ',' + std::string(fields) + '}';
	}
	json_value picks(std::uint32_t game = 1, std::uint32_t stage = 17, std::uint32_t left = 12, std::uint32_t right = 32)
	{
		return json_value::object({{"gameNumber", json_value::number(game)}, {"stageId", json_value::number(stage)},
			{"leftCharacterRow", json_value::number(left)}, {"rightCharacterRow", json_value::number(right)}});
	}
	// Contract 2 with the game's own selection: no characters, the Right side picks the stage, and both countdowns.
	const std::string in_game_manifest_text = R"({"schema":"pbd-set-contract-2","sessionId":"session-1","setId":"set-1","matchId":"match-1","gameNumber":1,"localMemberId":"right-member","localSide":"right","left":{"memberId":"left-member","name":"Left","character":null},"right":{"memberId":"right-member","name":"Right","character":null},"stage":{"rule":"in_game","picker":"right"},"rules":{"firstToGames":2,"roundsPerGame":3},"score":{"left":0,"right":0},"build":"43zr","compat":"fingerprint","spectator":{"publishConfirmedInputs":true,"encoding":"pbd-pad-v1"},"selection":{"mode":"in_game","characterSeconds":30,"stageSeconds":15}})";
	std::string envelope_in_game(std::string_view fields, std::uint64_t serial = 1)
	{
		return "{\"schema\":\"pbd-set-contract-2\",\"sessionId\":\"session-1\",\"setId\":\"set-1\",\"serial\":" + std::to_string(serial) + ',' + std::string(fields) + '}';
	}
} // namespace

PBD_TEST(PbdSetChannel, BackgroundChannelDrainsExactEnvelopesAndRefusesACollision)
{
	using namespace pbd::set_channel;
	const auto admitted = parse_manifest(sample_document, supported);
	check(admitted.value);
	const auto parent = std::filesystem::temp_directory_path();
	const auto directory = parent / ("pbd-set-background-test-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
	check(std::filesystem::create_directory(directory));
	{
		channel output(directory, *admitted.value, true);
		check(output.write_event("game_started", picks()));
		{
			std::ofstream file(directory / "pending-command.tmp", std::ios::binary);
			file << envelope("\"type\":\"stream_status\",\"status\":\"active\"");
		}
		std::filesystem::rename(directory / "pending-command.tmp", directory / "set-command-1.json");
		result<command> command;
		const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
		do
		{
			command = output.next_command(1);
			if (command.value || command.error != "unavailable")
				break;
			std::this_thread::sleep_for(std::chrono::milliseconds(1));
		} while (std::chrono::steady_clock::now() < deadline);
		check(output.last_command() == 0 && command.value && output.next_command(1).value);
		check(command.value && output.acknowledge(*command.value));
		check(!output.acknowledge(*command.value) && !output.next_command(1).value);
		check(output.flush() && output.last_event() == 2 && output.last_command() == 1);
		const auto first = detail::read_text(directory / "set-event-1.json");
		const auto second = detail::read_text(directory / "set-event-2.json");
		check(first.value && second.value);
		const auto start = detail::json_parser(*first.value).parse();
		const auto ack = detail::json_parser(*second.value).parse();
		check(start.value && detail::is(start.value->find("type"), "game_started"));
		check(ack.value && detail::is(ack.value->find("type"), "command_applied"));
		check(start.value->find("serial")->text == "1" && ack.value->find("serial")->text == "2");
	}
	{
		channel collision(directory, *admitted.value, true);
		check(collision.write_event("game_started", picks()));
		check(!collision.flush() && !collision.healthy() && collision.output_failed());
		check(!collision.write_event("game_started", picks()));
		check(collision.last_event() == 1);
	}
	const auto resolved = std::filesystem::canonical(directory);
	check(std::filesystem::equivalent(resolved.parent_path(), parent) && resolved.filename().string().starts_with("pbd-set-background-test-"));
	std::filesystem::remove_all(resolved);
}

PBD_TEST(PbdSetChannel, AGameStartCarriesTheNativeCapsuleStringBudget)
{
	using namespace pbd::set_channel;
	const auto admitted = parse_manifest(sample_document, supported);
	check(admitted.value);
	const auto parent = std::filesystem::temp_directory_path();
	const auto directory = parent / ("pbd-set-capsule-test-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
	check(std::filesystem::create_directory(directory));
	const std::string native_hex(9024, 'a');
	{
		channel output(directory, *admitted.value);
		auto payload = picks();
		payload.fields.emplace_back("gameStartCapsule", json_value::object({{"nativeResourceState", json_value::string(native_hex)}}));
		check(output.write_event("game_started", payload) && output.healthy());
		const auto text = detail::read_text(directory / "set-event-1.json");
		check(text.value && !detail::json_parser(*text.value).parse().value);
		const auto parsed = detail::json_parser(*text.value, max_json_nodes * 4, max_file_bytes, 65536).parse();
		check(parsed.value && parsed.value->find("gameStartCapsule")->find("nativeResourceState")->text == native_hex);
	}
	{
		channel output(directory, *admitted.value);
		auto payload = picks();
		payload.fields.emplace_back("unrelated", json_value::string(native_hex));
		check(!output.write_event("game_started", payload) && output.error() == "invalid_event_payload");
	}
	const auto resolved = std::filesystem::canonical(directory);
	check(std::filesystem::equivalent(resolved.parent_path(), parent) && resolved.filename().string().starts_with("pbd-set-capsule-test-"));
	std::filesystem::remove_all(resolved);
}

namespace
{
	void manifest_rule_checks(const manifest& session)
	{
		check(session.local_side == side::left && session.left.row == 12 && session.right.row == 32);
		check(session.stage_id == 17 && session.first_to_games == 2 && session.rounds_per_game == 3);
		check(!session.allow_jinpachi);
		const auto allowed = parse_manifest(changed(sample_document, "\"roundsPerGame\":3", "\"roundsPerGame\":3,\"allowJinpachi\":true"), supported);
		check(allowed.value && allowed.value->allow_jinpachi);
		for (const auto row : {81u, 82u, 83u, 84u})
		{
			auto broad = supported;
			broad.character_rows.push_back(row);
			const auto jinpachi = changed(sample_document, "\"row\":12", "\"row\":" + std::to_string(row));
			check(!parse_manifest(jinpachi, broad).value);
			check(parse_manifest(changed(jinpachi, "\"roundsPerGame\":3", "\"roundsPerGame\":3,\"allowJinpachi\":true"), broad).value);
		}
		for (const auto invalid : {"null", "1", "\"true\""})
			check(!parse_manifest(changed(sample_document, "\"roundsPerGame\":3", std::string("\"roundsPerGame\":3,\"allowJinpachi\":") + invalid), supported).value);
		check(parse_manifest(changed(sample_document, "\"schema\":", "\"future\":[true,null,{\"ratio\":1.25e2}],\"schema\":"), supported).value);
		check(!parse_manifest(changed(sample_document, "\"localSide\":\"left\"", "\"localSide\":\"wrong\""), supported).value);
		check(!parse_manifest(changed(sample_document, "\"row\":12", "\"row\":99"), supported).value);
		check(!parse_manifest(changed(sample_document, "\"id\":17", "\"id\":99"), supported).value);
		check(!parse_manifest(changed(sample_document, "\"roundsPerGame\":3", "\"roundsPerGame\":2"), supported).value);
		check(!parse_manifest(changed(sample_document, "\"firstToGames\":2", "\"firstToGames\":11"), supported).value);
		check(!parse_manifest(changed(sample_document, "\"localMemberId\":\"left-member\"", "\"localMemberId\":\"right-member\""), supported).value);
		check(!parse_manifest(changed(sample_document, "\"gameNumber\":1", "\"gameNumber\":2"), supported).value);
		check(!parse_manifest(changed(sample_document, "\"compat\":\"fingerprint\"", "\"compat\":\"other\""), supported).value);
		check(!parse_manifest(changed(sample_document, "\"name\":\"Left\"", "\"name\":\"Left\\n\""), supported).value);
		check(!parse_manifest(changed(sample_document, "\"row\":12", "\"row\":12,\"row\":32"), supported).value);
		check(!parse_manifest(sample_document + "{}", supported).value);
		check(!parse_manifest(std::string(max_file_bytes + 1, ' '), supported).value);
		check(!detail::json_parser("{\"x\":01}").parse().value);
		check(!detail::json_parser("{\"x\":1.,\"y\":2}").parse().value);
		check(!detail::json_parser("{\"x\":1e}").parse().value);
		check(!detail::json_parser("{\"x\":NaN}").parse().value);
		check(!detail::json_parser("{\"x\":\"\\uD800\"}").parse().value);
		check(!detail::json_parser("{\"x\":\"\\uDC00\"}").parse().value);
		const auto unicode = detail::json_parser("{\"x\":\"\\uD83D\\uDE00\"}").parse();
		check(unicode.value && unicode.value->find("x")->text.size() == 4);
		std::string nested(18, '[');
		nested += "null";
		nested += std::string(18, ']');
		check(!detail::json_parser(nested).parse().value);
	}

	void fixed_command_checks(const manifest& session, const std::string& next)
	{
		check(parse_command(next, session, 1, 1).value);
		check(!parse_command(next, session, 2, 1).value);
		check(!parse_command(changed(next, "\"gameNumber\":2", "\"gameNumber\":3"), session, 1, 1).value);
		check(!parse_command(changed(next, "\"left\":1", "\"left\":2"), session, 1, 1).value);
		check(!parse_command(envelope(R"("type":"continue_game","gameNumber":2,"score":{"left":1,"right":0},"stage":{"rule":"fixed","id":23})"), session, 1, 1).value);
		check(!parse_command(envelope(R"("type":"continue_game","gameNumber":2,"score":{"left":1,"right":0},"rules":{"firstToGames":3,"roundsPerGame":3})"), session, 1, 1).value);
		// A drawn game's replay keeps its number and score, and only the explicit marker allows that.
		const auto replay = envelope(R"("type":"continue_game","gameNumber":1,"score":{"left":0,"right":0},"replay":"draw")");
		const auto replayed = parse_command(replay, session, 1, 1);
		check(replayed.value && replayed.value->replay && replayed.value->game_number == 1);
		check(!parse_command(changed(replay, "\"replay\":\"draw\"", "\"replay\":\"other\""), session, 1, 1).value);
		check(!parse_command(changed(replay, "\"replay\":\"draw\"", "\"replay\":true"), session, 1, 1).value);
		check(!parse_command(envelope(R"("type":"continue_game","gameNumber":1,"score":{"left":0,"right":0})"), session, 1, 1).value);
		check(!parse_command(envelope(R"("type":"continue_game","gameNumber":2,"score":{"left":1,"right":0},"replay":"draw")"), session, 1, 1).value);
		check(parse_command(next, session, 1, 1).value && !parse_command(next, session, 1, 1).value->replay);
		auto later = session;
		later.game_number = 2;
		later.games = {1, 0};
		const auto replay_two = envelope(R"("type":"continue_game","gameNumber":2,"score":{"left":1,"right":0},"replay":"draw")");
		check(parse_command(replay_two, later, 1, 2).value);
		check(!parse_command(changed(replay_two, "\"left\":1,\"right\":0", "\"left\":0,\"right\":1"), later, 1, 2).value);
		check(parse_command(envelope(R"("type":"show_post_set","role":"loser","allowedActions":["rematch_same","random_other","change_fighter","leave_seat"],"challengerWaiting":false)"), session, 1, 1).value);
		check(!parse_command(envelope(R"("type":"show_post_set","role":"winner","allowedActions":["random_other"],"challengerWaiting":false)"), session, 1, 1).value);
		check(parse_command(envelope(R"("type":"choice_rejected","reason":"fighter_not_in_reviewed_roster")"), session, 1, 1).value);
		check(parse_command(envelope(R"("type":"end_set","reason":"service_lost")"), session, 1, 1).value);
		check(!parse_command(envelope(R"("type":"end_set","reason":"invented_winner")"), session, 1, 1).value);
		check(parse_command(envelope(R"("type":"chat_overlay","visible":true,"additions":[{"messageId":"chat-1","at":"2026-10-01T00:00:00Z","authorName":"Left","text":"GGs"}])"), session, 1, 1).value);
		check(parse_command(envelope(R"("type":"stream_status","status":"paused_slow")"), session, 1, 1).value);
		// The VS cards' language and region per side: both sides required, either text may be empty, bounded and plain.
		{
			const auto cards = parse_command(envelope(R"("type":"vs_cards","visible":true,"left":{"language":"EN","region":"N. America"},"right":{"language":"","region":"Europe"})"), session, 1, 1);
			check(cards.value && cards.value->kind == command_kind::vs_cards && cards.value->visible);
			check(cards.value->origins[0].language == "EN" && cards.value->origins[0].region == "N. America");
			check(cards.value->origins[1].language.empty() && cards.value->origins[1].region == "Europe");
			check(parse_command(envelope(R"("type":"vs_cards","visible":false,"left":{"language":"","region":""},"right":{"language":"","region":""})"), session, 1, 1).value);
			check(!parse_command(envelope(R"("type":"vs_cards","visible":true,"left":{"language":"EN","region":"N. America"})"), session, 1, 1).value);
			check(!parse_command(envelope(R"("type":"vs_cards","left":{"language":"EN","region":""},"right":{"language":"EN","region":""})"), session, 1, 1).value);
			check(!parse_command(envelope(R"("type":"vs_cards","visible":true,"left":{"language":"EN"},"right":{"language":"EN","region":""})"), session, 1, 1).value);
			check(!parse_command(envelope(R"("type":"vs_cards","visible":true,"left":{"language":"EN\nX","region":""},"right":{"language":"EN","region":""})"), session, 1, 1).value);
			check(!parse_command(envelope(R"("type":"vs_cards","visible":true,"left":{"language":"ENGLISH-ENGLISH-X","region":""},"right":{"language":"EN","region":""})"), session, 1, 1).value);
			check(parse_command(envelope(R"("type":"vs_cards","visible":true,"left":{"language":"EN","region":"xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx"},"right":{"language":"EN","region":""})"), session, 1, 1).value);
			check(!parse_command(envelope(R"("type":"vs_cards","visible":true,"left":{"language":"EN","region":"xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx"},"right":{"language":"EN","region":""})"), session, 1, 1).value);
		}
		check(!parse_command(envelope(R"("type":"erase_memory")"), session, 1, 1).value);
		check(std::regex_match(utc_now(), std::regex("[0-9]{4}-[0-9]{2}-[0-9]{2}T[0-9]{2}:[0-9]{2}:[0-9]{2}\\.[0-9]{3}Z")));
	}

	void event_payload_checks(const manifest& session)
	{
		const auto neutral_pad = json_value::string("AAAAAAAAAAA");
		check(pad_data(&neutral_pad, 1));
		const auto leave = [](std::string_view cause)
		{
			auto payload = json_value::object({{"action", json_value::string("leave_seat")}});
			if (!cause.empty())
				payload.fields.emplace_back("cause", json_value::string(std::string(cause)));
			return payload;
		};
		for (const std::string_view cause : {"", "start_select", "stop_request", "window_closed"})
			check(valid_event_payload("leave", leave(cause), session, std::nullopt));
		check(!valid_event_payload("leave", leave("peer_lost"), session, std::nullopt));
		// The waiting box's Leave: no result, and no cause.
		{
			auto waited = json_value::object({{"action", json_value::string("peer_wait")}});
			check(valid_event_payload("leave", waited, session, std::nullopt));
			waited.fields.emplace_back("cause", json_value::string("start_select"));
			check(!valid_event_payload("leave", waited, session, std::nullopt));
		}
		const auto drawn = [](std::uint32_t left, std::uint32_t right, json_value named, std::string_view outcome)
		{
			auto payload = picks();
			payload.fields.emplace_back("winner", std::move(named));
			payload.fields.emplace_back("outcome", json_value::string(std::string(outcome)));
			payload.fields.emplace_back("roundWins", json_value::object({{"left", json_value::number(left)}, {"right", json_value::number(right)}}));
			return payload;
		};
		check(valid_event_payload("game_result", drawn(3, 3, json_value{}, "draw"), session, std::nullopt));
		check(!valid_event_payload("game_result", drawn(3, 2, json_value{}, "draw"), session, std::nullopt));
		check(!valid_event_payload("game_result", drawn(3, 3, json_value::string("left"), "draw"), session, std::nullopt));
		check(!valid_event_payload("game_result", drawn(3, 3, json_value{}, "win"), session, std::nullopt));
	}

	void selection_manifest_checks()
	{
		// A left-out character means the same as null.
		check(parse_manifest(changed(in_game_manifest_text, "\"name\":\"Left\",\"character\":null", "\"name\":\"Left\""), supported).value);
		// A fixed character, a fixed stage, another picker or a stage id cannot be honoured by the game's own select.
		check(parse_manifest(changed(in_game_manifest_text, "\"character\":null", "\"character\":{\"id\":\"nina_williams\",\"row\":12}"), supported).error == "bad_fighters");
		check(parse_manifest(changed(in_game_manifest_text, "{\"rule\":\"in_game\",\"picker\":\"right\"}", "{\"rule\":\"fixed\",\"id\":17}"), supported).error == "bad_stage");
		check(parse_manifest(changed(in_game_manifest_text, "\"picker\":\"right\"", "\"picker\":\"left\""), supported).error == "bad_stage");
		check(parse_manifest(changed(in_game_manifest_text, "\"picker\":\"right\"", "\"picker\":\"right\",\"id\":17"), supported).error == "bad_stage");
		// The selection is required in contract 2, with a known mode and bounded countdowns.
		check(parse_manifest(changed(in_game_manifest_text, ",\"selection\":{\"mode\":\"in_game\",\"characterSeconds\":30,\"stageSeconds\":15}", ""), supported).error == "bad_selection");
		check(parse_manifest(changed(in_game_manifest_text, "\"mode\":\"in_game\"", "\"mode\":\"draft\""), supported).error == "bad_selection");
		check(parse_manifest(changed(in_game_manifest_text, "\"characterSeconds\":30", "\"characterSeconds\":0"), supported).error == "bad_selection");
		check(parse_manifest(changed(in_game_manifest_text, "\"stageSeconds\":15", "\"stageSeconds\":100"), supported).error == "bad_selection");
		check(parse_manifest(changed(in_game_manifest_text, "\"stageSeconds\":15", "\"stageSeconds\":\"15\""), supported).error == "bad_selection");
		// Contract 2 can still carry a fixed set (the automatic pre-selection); it then needs fighters and a fixed stage.
		const auto fixed_two = parse_manifest(changed(changed(sample_document, "pbd-set-contract-1", "pbd-set-contract-2"),
												  "\"spectator\":", "\"selection\":{\"mode\":\"fixed\"},\"spectator\":"),
			supported);
		check(fixed_two.value && fixed_two.value->contract == 2 && !in_game(*fixed_two.value) && fixed_two.value->left.row == 12 && fixed_two.value->stage_id == 17);
		check(!parse_manifest(changed(changed(in_game_manifest_text, "\"mode\":\"in_game\",\"characterSeconds\":30,\"stageSeconds\":15", "\"mode\":\"fixed\""),
								  "\"character\":null", "\"character\":{\"id\":\"nina_williams\",\"row\":12}"),
			supported)
				.value);
		// An unknown contract refuses: the launcher sees lobby_manifest_invalid at start instead of a set that waits forever.
		check(parse_manifest(changed(in_game_manifest_text, "pbd-set-contract-2", "pbd-set-contract-3"), supported).error == "bad_schema");
		check(parse_manifest(changed(sample_document, "\"schema\":\"pbd-set-contract-1\",", ""), supported).error == "bad_schema");
	}

	void rematch_load_checks(const manifest& session, const manifest& chosen)
	{
		{
			const pbd::rematch::load load{1, 2, 25, 1, {{12, 32}, 23}, {}, true};
			const auto encoded = detail::serialize(pbd::rematch::load_json<json_value>(load));
			const std::string base = R"("type":"continue_game","gameNumber":2,"score":{"left":0,"right":1},"load":)";
			const auto text = envelope_in_game(base + encoded);
			const auto parsed = parse_command(text, chosen, 1, 1);
			check(parsed.value && parsed.value->rematch == load && !parsed.value->random_stage);
			check(!parse_command(envelope_in_game(base + encoded + R"(,"stage":"random")"), chosen, 1, 1).value);
			check(!parse_command(changed(text, "\"previousGame\":1", "\"previousGame\":2"), chosen, 1, 1).value);
			check(!parse_command(changed(text, "\"nextGame\":2", "\"nextGame\":3"), chosen, 1, 1).value);
			check(!parse_command(changed(text, "\"epoch\":1", "\"epoch\":1.0"), chosen, 1, 1).value);
			check(!parse_command(changed(text, "\"randomStage\":true", "\"seed\":7,\"randomStage\":true"), chosen, 1, 1).value);
		}
		{
			// Change character: the loser's side picks again in an in-game set's own select. A fixed set has no select to pick in, a
			// drawn game's replay has no loser, and the stage stays, so none of them carries a repick.
			pbd::rematch::load load{1, 2, 25, 1, {{12, 32}, 23}, {}, false};
			load.repick = 1;
			const auto encoded = detail::serialize(pbd::rematch::load_json<json_value>(load));
			const auto parsed = parse_command(envelope_in_game(R"("type":"continue_game","gameNumber":2,"score":{"left":1,"right":0},"load":)" + encoded), chosen, 1, 1);
			check(parsed.value && parsed.value->rematch == load && parsed.value->rematch->repick == 1u && !parsed.value->random_stage);
			const auto fixed = parse_command(envelope(R"("type":"continue_game","gameNumber":2,"score":{"left":1,"right":0},"load":)" + encoded), session, 1, 1);
			check(fixed.error == "bad_rematch_load");
			auto kept = load;
			kept.repick.reset();
			check(parse_command(envelope(R"("type":"continue_game","gameNumber":2,"score":{"left":1,"right":0},"load":)" + detail::serialize(pbd::rematch::load_json<json_value>(kept))), session, 1, 1).value);
			auto again = load;
			again.next_game = 1;
			const auto replay_repick = parse_command(envelope_in_game(R"("type":"continue_game","gameNumber":1,"score":{"left":0,"right":0},"replay":"draw","load":)" + detail::serialize(pbd::rematch::load_json<json_value>(again))), chosen, 1, 1);
			check(replay_repick.error == "bad_rematch_load");
			again.repick.reset();
			check(parse_command(envelope_in_game(R"("type":"continue_game","gameNumber":1,"score":{"left":0,"right":0},"replay":"draw","load":)" + detail::serialize(pbd::rematch::load_json<json_value>(again))), chosen, 1, 1).value);
			const auto text = envelope_in_game(R"("type":"continue_game","gameNumber":2,"score":{"left":1,"right":0},"load":)" + encoded);
			check(parse_command(changed(text, "\"randomStage\":false", "\"randomStage\":true"), chosen, 1, 1).error == "bad_rematch_load");
			check(parse_command(changed(text, "\"repick\":\"right\"", "\"repick\":\"loser\""), chosen, 1, 1).error == "bad_rematch_load");
			check(!parse_command(envelope_in_game(R"("type":"continue_game","gameNumber":2,"score":{"left":1,"right":0},"stage":"random","load":)" + encoded), chosen, 1, 1).value);
		}
	}

	void selection_command_checks(const manifest& session, const manifest& chosen, const std::string& next, const std::string& next_in_game)
	{
		check(parse_command(next, chosen, 1, 1).error == "bad_command_envelope");
		check(parse_command(next_in_game, session, 1, 1).error == "bad_command_envelope");
		check(parse_command(envelope_in_game(R"("type":"continue_game","gameNumber":2,"score":{"left":0,"right":1},"stage":{"rule":"in_game","picker":"right"},"right":{"memberId":"right-member","character":null})"), chosen, 1, 1).value);
		check(parse_command(envelope_in_game(R"("type":"continue_game","gameNumber":2,"score":{"left":0,"right":1},"stage":{"rule":"fixed","id":17})"), chosen, 1, 1).error == "changed_set_values");
		check(parse_command(envelope_in_game(R"("type":"continue_game","gameNumber":2,"score":{"left":0,"right":1},"left":{"memberId":"left-member","character":{"id":"nina_williams","row":12}})"), chosen, 1, 1).error == "changed_set_values");
		check(parse_command(envelope_in_game(R"("type":"continue_game","gameNumber":1,"score":{"left":0,"right":0},"replay":"draw")"), chosen, 1, 1).value);
		// Rematch with random stage: "stage":"random" marks an in-game set's next game, whose stage countdown starts at zero. A drawn
		// game's replay has no loser to ask for it, a fixed set has no roulette to stop, and no other string is a stage.
		{
			const auto random = parse_command(envelope_in_game(R"("type":"continue_game","gameNumber":2,"score":{"left":0,"right":1},"stage":"random")"), chosen, 1, 1);
			check(random.value && random.value->random_stage && !random.value->replay && random.value->game_number == 2);
			check(!parse_command(next_in_game, chosen, 1, 1).value->random_stage);
			check(!parse_command(envelope_in_game(R"("type":"continue_game","gameNumber":2,"score":{"left":0,"right":1},"stage":{"rule":"in_game","picker":"right"})"), chosen, 1, 1).value->random_stage);
			check(parse_command(envelope_in_game(R"("type":"continue_game","gameNumber":1,"score":{"left":0,"right":0},"replay":"draw","stage":"random")"), chosen, 1, 1).error == "bad_random_stage");
			check(parse_command(envelope_in_game(R"("type":"continue_game","gameNumber":2,"score":{"left":0,"right":1},"stage":"same")"), chosen, 1, 1).error == "bad_random_stage");
			check(parse_command(envelope_in_game(R"("type":"continue_game","gameNumber":2,"score":{"left":0,"right":1},"stage":"")"), chosen, 1, 1).error == "bad_random_stage");
			check(parse_command(envelope(R"("type":"continue_game","gameNumber":2,"score":{"left":1,"right":0},"stage":"random")"), session, 1, 1).error == "bad_random_stage");
			check(parse_command(envelope_in_game(R"("type":"continue_game","gameNumber":2,"score":{"left":0,"right":1},"stage":true)"), chosen, 1, 1).error == "changed_set_values");
		}
	}

	void command_file_retry_checks(const std::filesystem::path& directory, const manifest& session, const std::string& next)
	{
#ifdef _WIN32
		{
			channel retry(directory, session);
			const auto file = directory / "set-command-1.json";
			const HANDLE locked = CreateFileW(file.c_str(), GENERIC_READ, 0, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
			check(locked != INVALID_HANDLE_VALUE);
			check(retry.next_command(1).error == "unavailable" && retry.last_command() == 0);
			check(CloseHandle(locked));
			check(retry.next_command(1).value && retry.last_command() == 0);
			const HANDLE denied = CreateFileW(file.c_str(), GENERIC_READ, 0, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
			check(denied != INVALID_HANDLE_VALUE);
			check(retry.next_command(1).error == "unavailable");
			std::this_thread::sleep_for(std::chrono::milliseconds(1100));
			check(retry.next_command(1).error == "read_failed" && retry.last_command() == 0);
			check(CloseHandle(denied));
			check(retry.next_command(1).value);
			{
				std::ofstream malformed(file, std::ios::binary | std::ios::trunc);
				malformed << "{";
			}
			check(!retry.next_command(1).value && retry.next_command(1).error != "unavailable");
			{
				std::ofstream restored(file, std::ios::binary | std::ios::trunc);
				restored << next;
			}
		}
#endif
	}

	void fixed_channel_checks(const std::filesystem::path& directory, const manifest& session, const std::string& next)
	{
		{
			std::ofstream file(directory / "set-session.json", std::ios::binary);
			file << sample_document;
		}
		check(read_manifest(directory, supported).value);
		check(!read_manifest("relative", supported).value);
		channel output(directory, session);
		check(output.write_event("game_started", picks()));
		check(output.last_event() == 1);
		const auto event = detail::read_text(directory / "set-event-1.json");
		check(event.value);
		const auto parsed_event = detail::json_parser(*event.value).parse();
		check(parsed_event.value && detail::is(parsed_event.value->find("type"), "game_started"));
		check(detail::is(parsed_event.value->find("sessionId"), "session-1"));
		check(parsed_event.value->find("serial")->text == "1");
		check(std::regex_match(parsed_event.value->find("at")->text, std::regex("[0-9]{4}-[0-9]{2}-[0-9]{2}T[0-9]{2}:[0-9]{2}:[0-9]{2}\\.[0-9]{3}Z")));
		{
			std::ofstream file(directory / "set-command-1.json", std::ios::binary);
			file << next;
		}
		const auto command = output.next_command(1);
		check(command.value && output.last_command() == 0);
		command_file_retry_checks(directory, session, next);
		check(output.acknowledge(*command.value));
		check(output.last_command() == 1 && output.last_event() == 2);
		check(!output.acknowledge(*command.value));
		check(!output.next_command(2).value && output.next_command(2).error == "unavailable");
		{
			std::ofstream file(directory / "set-event-3.json", std::ios::binary);
			file << "immutable previous fact";
		}
		check(!output.write_event("between_games_ready", json_value::object({{"gameNumber", json_value::number(1)}})));
		check(output.last_event() == 2 && !output.healthy());
		check(detail::read_text(directory / "set-event-3.json").value == "immutable previous fact");
		check(!output.write_event("game_started", picks()));
	}

	void selected_channel_checks(const std::filesystem::path& directory, const manifest& chosen, const std::string& next_in_game)
	{
		// An in-game set writes its own schema, and a game's result must repeat the picks its start reported.
		const auto chosen_result = [](std::uint32_t stage, std::uint32_t left, std::uint32_t right)
		{
			auto payload = picks(1, stage, left, right);
			payload.fields.emplace_back("winner", json_value::string("right"));
			payload.fields.emplace_back("roundWins", json_value::object({{"left", json_value::number(1)}, {"right", json_value::number(3)}}));
			return payload;
		};
		const auto in_game_dir = directory / "in-game";
		check(std::filesystem::create_directory(in_game_dir));
		channel chosen_output(in_game_dir, chosen);
		check(chosen_output.write_event("game_started", picks(1, 0x0b, 0x4f, 0x4d)));
		const auto chosen_event = detail::read_text(in_game_dir / "set-event-1.json");
		check(chosen_event.value);
		const auto parsed_event = detail::json_parser(*chosen_event.value).parse();
		check(parsed_event.value && detail::is(parsed_event.value->find("schema"), "pbd-set-contract-2"));
		{
			const auto other_dir = directory / "in-game-other";
			check(std::filesystem::create_directory(other_dir));
			channel other_output(other_dir, chosen);
			check(!other_output.write_event("game_result", chosen_result(0x0b, 0x4f, 0x4d)));
			check(other_output.error() == "event_picks_changed" && other_output.last_event() == 0);
		}
		{
			const auto other_dir = directory / "in-game-changed";
			check(std::filesystem::create_directory(other_dir));
			channel other_output(other_dir, chosen);
			check(other_output.write_event("game_started", picks(1, 0x0b, 0x4f, 0x4d)));
			check(!other_output.write_event("game_result", chosen_result(0x0b, 0x4f, 0x0c)));
			check(other_output.error() == "event_picks_changed" && other_output.last_event() == 1);
		}
		check(chosen_output.write_event("game_result", chosen_result(0x0b, 0x4f, 0x4d)));
		{
			std::ofstream file(in_game_dir / "set-command-1.json", std::ios::binary);
			file << next_in_game;
		}
		const auto chosen_next = chosen_output.next_command(1);
		check(chosen_next.value && chosen_output.continue_naturally(*chosen_next.value));
		check(chosen_output.identity().game_number == 2 && chosen_output.last_event() == 3);
		// The next game chooses again: its result needs its own start.
		auto second_result = chosen_result(0x0b, 0x4f, 0x4d);
		second_result.fields[0].second = json_value::number(2);
		check(!chosen_output.write_event("game_result", second_result) && chosen_output.error() == "event_picks_changed");
	}
} // namespace

PBD_TEST(PbdSetChannel, Checks)
{
	using namespace pbd::set_channel;
	const auto admitted = parse_manifest(sample_document, supported);
	check(admitted.value);
	const auto& session = *admitted.value;
	manifest_rule_checks(session);
	const auto next = envelope(R"("type":"continue_game","gameNumber":2,"score":{"left":1,"right":0})");
	fixed_command_checks(session, next);
	event_payload_checks(session);
	// Contract 1 stays a fixed set with its own schema.
	check(session.contract == 1 && session.selection.mode == selection_mode::fixed && schema_of(session) == "pbd-set-contract-1");
	check(session.left.has_character && session.right.has_character);
	// Contract 2, in-game selection: no characters, the Right side's stage select, and both countdowns.
	const auto in_game_admitted = parse_manifest(in_game_manifest_text, supported);
	check(in_game_admitted.value);
	const auto& chosen = *in_game_admitted.value;
	check(chosen.contract == 2 && in_game(chosen) && schema_of(chosen) == "pbd-set-contract-2");
	check(chosen.selection.character_seconds == 30 && chosen.selection.stage_seconds == 15 && chosen.selection.stage_picker == side::right);
	check(!chosen.left.has_character && !chosen.right.has_character && chosen.local_side == side::right);
	selection_manifest_checks();
	// Commands of an in-game set carry its schema, and repeated set values may only repeat the in-game rule.
	const auto next_in_game = envelope_in_game(R"("type":"continue_game","gameNumber":2,"score":{"left":0,"right":1})");
	check(parse_command(next_in_game, chosen, 1, 1).value);
	rematch_load_checks(session, chosen);
	selection_command_checks(session, chosen, next, next_in_game);
	// Events of an in-game set report what the game applied, within the native table bounds.
	check(valid_event_payload("game_started", picks(1, 0x0b, 0x4f, 0x4d), chosen, std::nullopt));
	check(!valid_event_payload("game_started", picks(1, 0x0b, 0x4f, 0x4d), session, std::nullopt));
	check(!valid_event_payload("game_started", picks(1, max_stage_id + 1, 12, 32), chosen, std::nullopt));
	check(!valid_event_payload("game_started", picks(1, 17, max_character_row + 1, 32), chosen, std::nullopt));

	const auto parent = std::filesystem::temp_directory_path();
	const auto directory = parent / ("pbd-set-channel-test-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
	check(std::filesystem::create_directory(directory));
	fixed_channel_checks(directory, session, next);
	selected_channel_checks(directory, chosen, next_in_game);
	const auto poisoned_dir = directory / "invalid";
	check(std::filesystem::create_directory(poisoned_dir));
	channel poisoned(poisoned_dir, session);
	auto result_payload = picks();
	result_payload.fields.emplace_back("winner", json_value::string("left"));
	result_payload.fields.emplace_back("roundWins", json_value::object({{"left", json_value::number(2)}, {"right", json_value::number(1)}}));
	check(!poisoned.write_event("game_result", result_payload));
	check(poisoned.last_event() == 0 && poisoned.error() == "invalid_event_payload");
	// The test owns only its freshly created, uniquely named temporary directory.
	const auto cleanup_directory = std::filesystem::canonical(directory);
	const auto cleanup_parent = std::filesystem::canonical(parent);
	check(std::filesystem::equivalent(cleanup_directory.parent_path(), cleanup_parent) &&
		  cleanup_directory.filename().string().starts_with("pbd-set-channel-test-"));
	std::filesystem::remove_all(cleanup_directory);
}

namespace
{
	// Join Battle's command: the next set, set-2, first to 3.
	const std::string next_set_fields = R"("type":"next_set","nextSetId":"set-2","matchId":"match-2","gameNumber":1,)"
										R"("score":{"left":0,"right":0},"rules":{"firstToGames":3,"roundsPerGame":3})";
} // namespace

// A fighter's optional rank is the game's own rank index, which the fight draws beside the name under the life bar.
PBD_TEST(PbdSetChannel, AFighterMayCarryTheGamesRank)
{
	using namespace pbd::set_channel;
	// Left out, or null, outside Ranked: rank 0 (Beginner).
	const auto plain = parse_manifest(sample_document, supported);
	check(plain.value && plain.value->left.rank == 0 && plain.value->right.rank == 0);
	const auto ranked = parse_manifest(
		changed(changed(sample_document, R"("name":"Left",)", R"("name":"Left","rank":10,)"), R"("name":"Right",)", R"("name":"Right","rank":null,)"),
		supported);
	check(ranked.value && ranked.value->left.rank == 10 && ranked.value->right.rank == 0);
	check(parse_manifest(changed(sample_document, R"("name":"Left",)", R"("name":"Left","rank":36,)"), supported).value);
	// A rank the game does not have, or one that is not a whole number, refuses the manifest.
	for (const auto bad : {R"("rank":37,)", R"("rank":-1,)", R"("rank":"1st Dan",)", R"("rank":1.5,)"})
		check(!parse_manifest(changed(sample_document, R"("name":"Left",)", std::string(R"("name":"Left",)") + bad), supported).value);
	// The in-game contract reads it the same way.
	const auto in_game = parse_manifest(changed(in_game_manifest_text, R"("name":"Right",)", R"("name":"Right","rank":33,)"), supported);
	check(in_game.value && in_game.value->right.rank == 33 && in_game.value->left.rank == 0);
}

// Join Battle against the same opponent: next_set names the new set under the completed set's envelope, and only its
// identity and first-to may differ.
PBD_TEST(PbdSetChannel, NextSetCommand)
{
	using namespace pbd::set_channel;
	const auto session = *parse_manifest(sample_document, supported).value;
	const auto chosen = *parse_manifest(in_game_manifest_text, supported).value;
	const auto& fields = next_set_fields;
	const auto text = envelope(fields);
	const auto parsed = parse_command(text, session, 1, 1);
	check(parsed.value && parsed.value->kind == command_kind::next_set);
	check(parsed.value->next_set_id == "set-2" && parsed.value->next_match_id == "match-2" && parsed.value->first_to_games == 3);
	check(parsed.value->game_number == 1 && parsed.value->games.left == 0 && parsed.value->games.right == 0 && !parsed.value->rematch);
	// The game number of the completed set does not matter: a set may end at any game.
	check(parse_command(text, session, 1, 3).value);
	check(parse_command(envelope_in_game(fields), chosen, 1, 2).value);
	// The envelope still names the completed set and its schema.
	check(parse_command(changed(text, "\"setId\":\"set-1\"", "\"setId\":\"set-2\""), session, 1, 1).error == "bad_command_envelope");
	check(parse_command(envelope_in_game(fields), session, 1, 1).error == "bad_command_envelope");
	check(parse_command(text, session, 2, 1).error == "bad_command_envelope");
	// A new identity, game 1 at 0-0, and a first-to the manifest would admit.
	for (const auto& [from, to] : std::initializer_list<std::pair<std::string_view, std::string_view>>{
			 {"\"nextSetId\":\"set-2\"", "\"nextSetId\":\"set-1\""}, {"\"matchId\":\"match-2\"", "\"matchId\":\"match-1\""},
			 {"\"nextSetId\":\"set-2\",", ""}, {"\"matchId\":\"match-2\",", ""}, {"\"nextSetId\":\"set-2\"", "\"nextSetId\":\"set 2\""},
			 {"\"nextSetId\":\"set-2\"", "\"nextSetId\":2"}, {"\"gameNumber\":1", "\"gameNumber\":2"}, {"\"left\":0", "\"left\":1"},
			 {"\"firstToGames\":3", "\"firstToGames\":0"}, {"\"firstToGames\":3", "\"firstToGames\":11"}, {"\"firstToGames\":3,", ""},
			 {",\"rules\":{\"firstToGames\":3,\"roundsPerGame\":3}", ""}, {"\"gameNumber\":1,", ""}})
		check(parse_command(changed(text, from, to), session, 1, 1).error == "bad_next_set");
	// The refusal of the command with `extra` fields added; empty when it is accepted.
	const auto with = [&](std::string_view extra, bool in_game_set = false)
	{
		const auto added = fields + std::string(extra);
		if (in_game_set)
			return parse_command(envelope_in_game(added), chosen, 1, 1).error;
		return parse_command(envelope(added), session, 1, 1).error;
	};
	const auto edited = [&](std::string_view from, std::string_view to)
	{
		return parse_command(changed(text, from, to), session, 1, 1).error;
	};
	check(with(R"(,"replay":"draw")") == "bad_next_set" && with(R"(,"load":{})") == "bad_rematch_load");
	// Everything but the first-to stays, and may only be repeated.
	check(edited("\"roundsPerGame\":3", "\"roundsPerGame\":5") == "changed_set_values");
	check(edited("\"roundsPerGame\":3}", "\"roundsPerGame\":3,\"allowJinpachi\":false}").empty());
	check(edited("\"roundsPerGame\":3}", "\"roundsPerGame\":3,\"allowJinpachi\":true}") == "changed_set_values");
	check(with(R"(,"stage":{"rule":"fixed","id":17})").empty());
	check(with(R"(,"stage":{"rule":"fixed","id":23})") == "changed_set_values");
	check(with(R"(,"stage":"random")") == "changed_set_values" && with(R"(,"stage":"random")", true) == "changed_set_values");
	check(with(R"(,"stage":{"rule":"in_game","picker":"right"})", true).empty());
	const std::string nina = R"("character":{"id":"nina_williams","row":12}})";
	check(with(R"(,"left":{"memberId":"left-member",)" + nina).empty());
	check(with(R"(,"left":{"memberId":"right-member",)" + nina) == "changed_set_values");
	check(with(R"(,"right":{"memberId":"right-member",)" + nina) == "changed_set_values");
	check(with(R"(,"right":{"memberId":"right-member","character":null})", true).empty());
	// Each player's rank for the new set: optional per side, null or left out for none, with or without a repeated member or
	// character, in either kind of set.
	{
		const auto ranked = parse_command(envelope(fields + R"(,"left":{"memberId":"left-member","rank":11},"right":{"rank":null})"), session, 1, 1);
		check(ranked.value && ranked.value->ranks[0] == 11u && !ranked.value->ranks[1]);
		check(!parsed.value->ranks[0] && !parsed.value->ranks[1]);
		const auto both = parse_command(envelope_in_game(fields + R"(,"left":{"rank":0},"right":{"memberId":"right-member","character":null,"rank":36})"), chosen, 1, 1);
		check(both.value && both.value->ranks[0] == 0u && both.value->ranks[1] == 36u);
		check(with(R"(,"left":{"memberId":"left-member",)" + nina.substr(0, nina.size() - 1) + R"(,"rank":12})").empty());
		for (const auto bad : {R"(,"left":{"rank":37})", R"(,"left":{"rank":-1})", R"(,"left":{"rank":"1st Dan"})", R"(,"left":11)"})
			check(with(bad) == "bad_next_set");
		check(with(R"(,"left":{"memberId":"right-member","rank":11})") == "changed_set_values");
		check(with(R"(,"right":{"rank":11,)" + nina) == "changed_set_values");
	}
	// Join Battle is offered to both players after a set.
	for (const std::string role : {"winner", "loser"})
	{
		const auto offer = parse_command(envelope(R"("type":"show_post_set","role":")" + role +
												  R"(","allowedActions":["join_battle","leave_seat"],"challengerWaiting":false)"),
			session, 1, 1);
		check(offer.value && offer.value->allowed_actions == std::vector<std::string>{"join_battle", "leave_seat"});
	}
	const auto twice = R"("type":"show_post_set","role":"winner","allowedActions":["join_battle","join_battle"],"challengerWaiting":false)";
	check(parse_command(envelope(twice), session, 1, 1).error == "bad_actions");
}

// A Ranked set's stakes: what a set win and a set loss give each player, as game rank indices. Display only, so the command
// changes nothing in the set; a malformed one is refused like any other command.
PBD_TEST(PbdSetChannel, RankStakesCommand)
{
	using namespace pbd::set_channel;
	using pbd::rank_stakes::event;
	const auto session = *parse_manifest(sample_document, supported).value;
	const std::string left = R"("left":{"win":"promote","winRank":11,"loss":"none","lossRank":10})";
	const std::string right = R"("right":{"win":"none","winRank":10,"loss":"demote","lossRank":9})";
	const auto read = [&](const std::string& sides)
	{
		return parse_command(envelope(R"("type":"rank_stakes",)" + sides), session, 1, 1);
	};
	const auto parsed = read(left + "," + right);
	check(parsed.value && parsed.value->kind == command_kind::rank_stakes);
	const pbd::rank_stakes::side promoting{event::promote, event::none, 11, 10}, demoting{event::none, event::demote, 10, 9};
	check(parsed.value->stakes[0] == promoting && parsed.value->stakes[1] == demoting);
	// Placement names its event on both outcomes; Beginner (0) and Dark Lord (34) are the bounds.
	const auto placing = read(R"("left":{"win":"place","winRank":34,"loss":"place","lossRank":0},)" + right);
	check(placing.value && placing.value->stakes[0].win == event::place && placing.value->stakes[0].win_rank == 34);
	// An unknown event, a rank past Dark Lord, a missing side or field, or a side that is not an object is refused.
	check(read(left).error == "bad_rank_stakes" && read(right).error == "bad_rank_stakes");
	for (const auto& [from, to] : std::initializer_list<std::pair<std::string_view, std::string_view>>{{"\"promote\"", "\"promoted\""},
			 {"\"winRank\":11", "\"winRank\":35"}, {"\"winRank\":11", "\"winRank\":-1"}, {"\"winRank\":11", "\"winRank\":\"11\""},
			 {",\"lossRank\":10", ""}, {"\"win\":\"promote\",", ""}, {"\"none\"", "null"}})
		check(read(changed(left, from, to) + "," + right).error == "bad_rank_stakes");
	check(read(R"("left":1,)" + right).error == "bad_rank_stakes");
	// The envelope names the running set, as for every command.
	check(parse_command(changed(envelope(R"("type":"rank_stakes",)" + left + "," + right), "\"setId\":\"set-1\"", "\"setId\":\"set-2\""),
			  session, 1, 1)
			  .error == "bad_command_envelope");
}

// What this build speaks beyond its first lobby contract, as `rpcs3 --pbd-features` prints it without booting. The launcher sends
// a Ranked set's stakes, the next set's ranks and the rank= key, and expects rankScreens, only from a build naming rank-stakes-1.
PBD_TEST(PbdSetChannel, FeaturesNameTheRankStakes)
{
	using namespace pbd::set_channel;
	const auto parsed = detail::json_parser(std::string(pbd::features::answer)).parse();
	check(parsed.value && detail::is(parsed.value->find("schema"), "pbd-features-1"));
	const auto* features = parsed.value->find("features");
	check(features && features->kind == json_kind::array);
	check(std::any_of(features->items.begin(), features->items.end(), [](const json_value& item)
		{
			return item.kind == json_kind::string && item.text == "rank-stakes-1";
		}));
	check(pbd::features::answer.find('\n') == std::string_view::npos, "the answer is printed as one line");
	check(std::any_of(features->items.begin(), features->items.end(), [](const json_value& item)
		{
			return item.kind == json_kind::string && item.text == "ranked-notices-1";
		}));
}

// ranked-notices-1's set command fields: vs_cards' mode and fighter captions, rank_stakes' placement counts. Display only: an
// invalid one is read as left out and named for the log, and never refuses the command, which would end the set.
PBD_TEST(PbdSetChannel, RankedNoticeFields)
{
	using namespace pbd::set_channel;
	const auto session = *parse_manifest(sample_document, supported).value;
	const auto cards = [&](const std::string& fields)
	{
		return parse_command(envelope(R"("type":"vs_cards","visible":true,)" + fields), session, 1, 1);
	};
	const std::string sides = R"("left":{"language":"EN","region":"N. America","character":"LILI"},"right":{"language":"","region":"","character":"SERGEI DRAGUNOV"})";
	const auto ranked = cards(R"("mode":"ranked",)" + sides);
	check(ranked.value && ranked.value->mode == card_mode::ranked && ranked.value->ignored.empty());
	check(ranked.value->characters[0] == "LILI" && ranked.value->characters[1] == "SERGEI DRAGUNOV");
	check(cards(R"("mode":"quick",)" + sides).value->mode == card_mode::quick);
	// Left out: a lobby room, and a set that picks in the game's own select.
	const auto plain = cards(R"("left":{"language":"EN","region":""},"right":{"language":"","region":""})");
	check(plain.value && plain.value->mode == card_mode::none && plain.value->characters[0].empty() && plain.value->ignored.empty());
	// Invalid: read as left out, named, never refused.
	const auto odd = cards(R"("mode":"casual","left":{"language":"EN","region":"","character":"LILIé"},"right":{"language":"","region":"","character":""})");
	check(odd.value && odd.value->mode == card_mode::none && odd.value->characters[0].empty() && odd.value->characters[1].empty());
	check(odd.value->ignored == std::vector<std::string>{"vs_cards.left.character", "vs_cards.right.character", "vs_cards.mode"});
	const auto long_name = cards(R"("left":{"language":"","region":"","character":"XXXXXXXXXXXXXXXXXXXXXXXXX"},"right":{"language":"","region":"","character":7})");
	check(long_name.value && long_name.value->characters[0].empty() && long_name.value->ignored.size() == 2);
	check(cards(R"("left":{"language":"","region":"","character":"XXXXXXXXXXXXXXXXXXXXXXXX"},"right":{"language":"","region":""})").value->characters[0].size() == 24);

	const auto stakes = [&](const std::string& left_extra, const std::string& right_extra)
	{
		return parse_command(envelope(R"("type":"rank_stakes","left":{"win":"place","winRank":0,"loss":"place","lossRank":0)" + left_extra +
									  R"(},"right":{"win":"none","winRank":12,"loss":"demote","lossRank":11)" + right_extra + "}"),
			session, 1, 1);
	};
	const auto placing = stakes(R"(,"placement":{"done":2,"total":5})", "");
	check(placing.value && placing.value->placements[0] == pbd::rank_stakes::placement{2, 5} && !placing.value->placements[1]);
	check(placing.value->ignored.empty());
	// The count is not part of the stakes the two PCs agree on.
	check(pbd::rank_stakes::stakes_word(placing.value->stakes) == pbd::rank_stakes::stakes_word(stakes("", "").value->stakes));
	check(stakes(R"(,"placement":{"done":4,"total":4})", R"(,"placement":null)").value->ignored.empty());
	for (const auto bad : {R"(,"placement":{"done":6,"total":5})", R"(,"placement":{"done":0,"total":0})", R"(,"placement":{"done":0,"total":11})",
			 R"(,"placement":{"done":1})", R"(,"placement":5)", R"(,"placement":{"done":-1,"total":5})"})
	{
		const auto read = stakes(bad, "");
		check(read.value && !read.value->placements[0] && read.value->ignored == std::vector<std::string>{"rank_stakes.left.placement"}, bad);
	}
}

// The channel acknowledges next_set under the completed set, then writes and reads under the new one; both cursors continue.
PBD_TEST(PbdSetChannel, NextSetChannel)
{
	using namespace pbd::set_channel;
	const auto session = *parse_manifest(sample_document, supported).value;
	// The left player was promoted by the finished set; the right player's rank is not named and stays.
	const auto text = envelope(next_set_fields + R"(,"left":{"rank":11})");
	const auto parent = std::filesystem::temp_directory_path();
	const auto stamp = std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());
	const auto directory = parent / ("pbd-set-channel-test-" + stamp + "-next");
	check(std::filesystem::create_directory(directory));
	{
		channel output(directory, session);
		const auto write = [&](std::uint64_t serial, const std::string& value)
		{
			std::ofstream file(directory / ("set-command-" + std::to_string(serial) + ".json"), std::ios::binary);
			file << value;
		};
		const auto event = [&](std::uint64_t serial)
		{
			const auto source = detail::read_text(directory / ("set-event-" + std::to_string(serial) + ".json"));
			check(source.value);
			auto value = detail::json_parser(*source.value).parse();
			check(value.value);
			return *value.value;
		};
		write(1, text);
		const auto next = output.next_command(2);
		check(next.value);
		// Only a next_set command with a new identity moves the channel to a new set.
		auto other = *next.value;
		other.kind = command_kind::continue_game;
		check(!output.next_set(other) && output.healthy() && output.last_event() == 0);
		auto same = *next.value;
		same.next_set_id = "set-1";
		check(!output.next_set(same) && output.last_event() == 0);
		check(output.next_set(*next.value));
		check(output.last_command() == 1 && output.last_event() == 1);
		const auto acknowledged = event(1);
		check(detail::is(acknowledged.find("type"), "command_applied") && detail::is(acknowledged.find("setId"), "set-1"));
		const auto& adopted = output.identity();
		check(adopted.set_id == "set-2" && adopted.match_id == "match-2" && adopted.first_to_games == 3 && adopted.game_number == 1);
		check(adopted.games.left == 0 && adopted.games.right == 0 && adopted.session_id == "session-1" && adopted.local_side == side::left);
		check(adopted.left.row == 12 && adopted.right.row == 32 && adopted.stage_id == 17 && adopted.rounds_per_game == 3);
		check(adopted.left.rank == 11 && adopted.right.rank == 0);
		check(output.write_event("game_started", picks()));
		const auto started = event(2);
		check(detail::is(started.find("setId"), "set-2") && started.find("serial")->text == "2" && started.find("gameNumber")->text == "1");
		// The completed set's envelope is refused from now on; the new set's commands continue the serial.
		write(2, envelope(R"("type":"continue_game","gameNumber":2,"score":{"left":1,"right":0})", 2));
		check(output.next_command(1).error == "bad_command_envelope");
		const auto third = envelope(R"("type":"continue_game","gameNumber":3,"score":{"left":2,"right":0})", 2);
		write(2, changed(third, "\"setId\":\"set-1\"", "\"setId\":\"set-2\""));
		// First to 3: a score of 2 is now within the set.
		const auto continued = output.next_command(2);
		check(continued.value && continued.value->games.left == 2);
	}
	const auto cleanup_directory = std::filesystem::canonical(directory);
	check(std::filesystem::equivalent(cleanup_directory.parent_path(), std::filesystem::canonical(parent)) &&
		  cleanup_directory.filename().string().starts_with("pbd-set-channel-test-"));
	std::filesystem::remove_all(cleanup_directory);
}

namespace
{
	// Sets a variable for this process, or removes it when value is null.
	void put(const char* name, const char* value)
	{
#ifdef _WIN32
		_putenv_s(name, value ? value : "");
#else
		if (value)
			setenv(name, value, 1);
		else
			unsetenv(name);
#endif
	}

	// Puts the named switches back as they were, even when a failed check ends the test.
	struct saved_switches
	{
		std::vector<std::pair<const char*, std::optional<std::string>>> values;
		saved_switches(std::initializer_list<const char*> names)
		{
			for (const char* name : names)
			{
				const char* value = pbd::env::find(name);
				values.emplace_back(name, value ? std::optional<std::string>(value) : std::nullopt);
			}
		}
		~saved_switches()
		{
			for (const auto& [name, value] : values)
				put(name, value ? value->c_str() : nullptr);
		}
	};
} // namespace

// The renderer makes the set's screens for a lobby set configured at launch and for a game held ready for one, which binds
// its set only after the overlays exist. The set channel itself stays the one configured at launch.
PBD_TEST(PbdSetChannel, AHeldGameAsksForTheSetScreens)
{
	using pbd::set_channel::requested_directory;
	using pbd::set_channel::set_screens_requested;
	const saved_switches restore{"PBD_SET_CHANNEL", "PBD_MATCH_STANDBY_CHANNEL"};
	put("PBD_SET_CHANNEL", nullptr);
	put("PBD_MATCH_STANDBY_CHANNEL", nullptr);
	check(!set_screens_requested(), "a game with no set makes no set screens");
	put("PBD_SET_CHANNEL", "");
	put("PBD_MATCH_STANDBY_CHANNEL", "");
	check(!set_screens_requested(), "empty switches are off");

	const std::string channel = (std::filesystem::temp_directory_path() / "pbd-held-set").string();
	put("PBD_MATCH_STANDBY_CHANNEL", channel.c_str());
	check(set_screens_requested(), "a game held ready for a set makes its screens");
	check(!requested_directory(), "a held game has no set channel until it binds one");

	put("PBD_MATCH_STANDBY_CHANNEL", nullptr);
	put("PBD_SET_CHANNEL", channel.c_str());
	check(set_screens_requested() && requested_directory() == std::filesystem::path(channel), "a set configured at launch");
}

#include "Emu/PBD/pbd_match_bind.h"
#include "Emu/PBD/pbd_match_pad.h"

PBD_TEST(PbdMatchBind, AssignedSideKeepsTheConfiguredPhysicalDeviceAndRejectsAmbiguousPorts)
{
	using namespace pbd::match_pad;
	for (unsigned bits = 0; bits < 16; ++bits)
		for (unsigned role = 0; role < 3; ++role)
		{
			const std::array<port, 2> ports{{{bool(bits & 1), bool(bits & 2)}, {bool(bits & 4), bool(bits & 8)}}};
			const bool left_real = bits == 9, right_real = bits == 6;
			assignment held;
			check(held.available(ports) == (left_real || right_real));
			const bool admitted = held.bind(ports, role);
			check(admitted == ((left_real || right_real) && role < 2));
			if (admitted)
			{
				check(held.swapped() == (role != (left_real ? 0u : 1u)));
				check(!held.available(ports), "one pristine generation accepts one assignment only");
				check(!held.bind(ports, 1 - role), "a later opponent cannot remap a running match");
			}
		}
	assignment unplugged;
	check(unplugged.available({{{true, false}, {false, true}}}));
	check(!unplugged.bind({{{false, false}, {false, true}}}, 1), "disconnect between readiness and admission refuses");
	const pbd::match_bind::identity identity{9, std::string(64, 'a'), std::string(64, 'b'), std::string(64, 'c')};
	const auto unassigned = pbd::json::detail::json_parser(pbd::match_bind::ready_text(identity)).parse();
	const auto assigned = pbd::json::detail::json_parser(pbd::match_bind::ready_text(identity, true)).parse();
	check(unassigned.value && assigned.value);
	const auto* unassigned_side = unassigned.value->find("assignedSideAtBind");
	const auto* assigned_side = assigned.value->find("assignedSideAtBind");
	check(unassigned_side && unassigned_side->kind == pbd::json::json_kind::boolean && !unassigned_side->truth);
	check(assigned_side && assigned_side->kind == pbd::json::json_kind::boolean && assigned_side->truth);
}

PBD_TEST(PbdSetChannel, MatchLateBinding)
{
	using namespace pbd::json;
	const pbd::match_bind::identity expected{7, std::string(64, 'a'), std::string(64, 'b'), std::string(64, 'c')};
	const auto payload = json_value::object({{"schema", json_value::string("pbd-match-bind-1")}, {"nativeGeneration", json_value::string("7")},
		{"checkpointSha256", json_value::string(expected.checkpoint)}, {"emulatorSha256", json_value::string(expected.emulator)},
		{"recipeSha256", json_value::string(expected.recipe)}, {"setId", json_value::string("set-new")},
		{"seed", json_value::number(7123)}, {"peer", json_value::object({{"role", json_value::number(1)}, {"session", json_value::number(99)}, {"localPort", json_value::number(5000)}, {"remotePort", json_value::number(5001)}, {"address", json_value::string("127.0.0.1")}})}});
	const auto good = detail::serialize(payload);
	const auto accepted = pbd::match_bind::parse(good, expected);
	check(accepted.value && accepted.value->seed == 7123 && accepted.value->role == 1 && accepted.value->set_id == "set-new");
	auto equal_ports = payload;
	equal_ports.fields[7].second.fields[3].second = json_value::number(5000);
	equal_ports.fields[7].second.fields[4].second = json_value::string("127.0.0.2");
	check(pbd::match_bind::parse(detail::serialize(equal_ports), expected).value,
		"a distinct endpoint may have the same numeric port during held admission");
	for (unsigned variant = 0; variant < 10; ++variant)
	{
		auto bad = payload;
		if (variant == 0)
			bad.fields[1].second = json_value::string("8");
		if (variant == 1)
			bad.fields[2].second = json_value::string(std::string(64, 'd'));
		if (variant == 2)
			bad.fields[6].second = json_value::number(0);
		if (variant == 3)
			bad.fields[7].second.fields[0].second = json_value::number(2);
		if (variant == 4)
			bad.fields[7].second.fields[3].second = json_value::number(65536);
		if (variant == 5)
			bad.fields[7].second.fields[4].second = json_value::string("localhost");
		if (variant == 6)
			bad.fields.emplace_back("key", json_value::string("unexpected"));
		if (variant == 7)
			bad.fields.emplace_back("seed", json_value::number(7));
		if (variant == 8)
			bad.fields[7].second.fields[1].second = json_value::number(0);
		if (variant == 9)
			bad.fields[5].second = json_value::string("set with spaces");
		check(!pbd::match_bind::parse(detail::serialize(bad), expected).value);
	}
	check(!pbd::match_bind::parse(std::string(8193, ' '), expected).value);
	check(!pbd::match_bind::parse("{", expected).value);

	// The optional route: "relay" turns the Relay tag on, "direct" or no route leaves it off, anything else is refused.
	check(!accepted.value->relayed, "a launcher that sends no route is a direct session");
	struct route_case
	{
		const char* value;
		bool relayed, valid;
	};
	for (const route_case& c : {route_case{"relay", true, true}, route_case{"direct", false, true}, route_case{"turn", false, false}})
	{
		auto routed = payload;
		routed.fields[7].second.fields.emplace_back("route", json_value::string(c.value));
		const auto parsed = pbd::match_bind::parse(detail::serialize(routed), expected);
		check(static_cast<bool>(parsed.value) == c.valid, "route value");
		if (parsed.value)
			check(parsed.value->relayed == c.relayed, "route reaches the admission");
	}
	auto numbered = payload;
	numbered.fields[7].second.fields.emplace_back("route", json_value::number(1));
	check(!pbd::match_bind::parse(detail::serialize(numbered), expected).value, "a route that is no string is refused");
	auto extra = payload;
	extra.fields[7].second.fields.emplace_back("hint", json_value::string("relay"));
	check(!pbd::match_bind::parse(detail::serialize(extra), expected).value, "an unknown sixth peer field is refused");
	const auto ready = detail::json_parser(pbd::match_bind::ready_text(expected)).parse();
	check(ready.value);
	const auto* route = ready.value->find("routeAtBind");
	check(route && route->kind == json_kind::boolean && route->truth);
}
