#include "pbd_test.h"

#include "Emu/Cell/pbd_set_channel.h"

#include <string>

namespace
{
	namespace channel = pbd::set_channel;
	using pbd::test::check;

	channel::manifest session()
	{
		channel::manifest value;
		value.session_id = "session-a";
		value.set_id = "set-a";
		value.game_number = 1;
		value.first_to_games = 2;
		value.rounds_per_game = 3;
		return value;
	}

	std::string command(std::string_view fields)
	{
		return "{\"schema\":\"pbd-set-contract-1\",\"sessionId\":\"session-a\","
		       "\"setId\":\"set-a\",\"serial\":1," +
		       std::string(fields) + "}";
	}
} // namespace

PBD_TEST(PbdSetParserAudit, UnknownKindsAndMissingCommandFieldsAreRefused)
{
	const auto expected = session();
	for (const auto fields : {"\"type\":\"unknown\"", "\"type\":null", "\"reason\":\"left\"",
			 "\"type\":\"end_set\"", "\"type\":\"chat_overlay\",\"visible\":true",
			 "\"type\":\"continue_game\",\"gameNumber\":2"})
		check(!channel::parse_command(command(fields), expected, 1, 1).value);
}

PBD_TEST(PbdSetParserAudit, TruncatedOversizedAndDuplicateKeyCommandsAreRefused)
{
	const auto expected = session();
	const auto wire = command("\"type\":\"end_set\",\"reason\":\"left\"");
	check(channel::parse_command(wire, expected, 1, 1).value);
	for (std::size_t size = 0; size < wire.size(); size++)
		check(!channel::parse_command(std::string_view(wire).substr(0, size), expected, 1, 1).value);
	check(!channel::parse_command(wire + std::string(channel::max_file_bytes, ' '), expected, 1, 1).value);
	check(!channel::parse_command(command("\"type\":\"end_set\",\"type\":\"end_set\",\"reason\":\"left\""),
		expected, 1, 1)
			.value);
}

PBD_TEST(PbdSetParserAudit, NegativeFractionalAndUnsafeCommandSerialsAreRefused)
{
	const auto expected = session();
	const auto wire = command("\"type\":\"end_set\",\"reason\":\"left\"");
	for (const auto serial : {"-1", "0", "1.5", "9007199254740992", "\"1\""})
	{
		auto bad = wire;
		const auto at = bad.find("\"serial\":1");
		bad.replace(at, 10, std::string("\"serial\":") + serial);
		check(!channel::parse_command(bad, expected, 1, 1).value);
	}
}
