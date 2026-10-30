#include "pbd_test.h"

#include "Emu/Cell/pbd_update_script.h"
#include "Emu/PBD/pbd_json_reader.h"
#include "Emu/PBD/pbd_spectator_channel_input.h"
#include "Emu/PBD/pbd_spectator_standby.h"

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>

using pbd::test::check;
using namespace pbd::json::detail;

namespace
{
	using namespace pbd::spectator_standby;

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

	std::string read(const std::filesystem::path& file)
	{
		std::ifstream in(file, std::ios::binary);
		return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
	}

	std::filesystem::path fresh_directory(const char* name)
	{
		const auto root = std::filesystem::temp_directory_path() / name;
		std::filesystem::remove_all(root);
		std::filesystem::create_directories(root);
		return root;
	}
} // namespace

PBD_TEST(PbdSpectatorStandby, TheReadyReportCarriesIdentityAndInputPolicy)
{
	ready value;
	value.native_generation = 9007199254740993ull;
	value.owner = 0x1000100;
	value.checkpoint = std::string(64, 'a');
	value.emulator = std::string(64, 'b');
	value.recipe = std::string(64, 'c');
	for (const auto input : {policy{}, policy{false, true, true, 1, 7}})
	{
		value.input = input;
		const auto text = ready_text(value);
		check(text.ends_with('\n'));
		const auto report = json_parser(text).parse();
		check(report.value);
		std::uint32_t number = 0;
		check(is(report.value->find("schema"), "pbd-spectator-standby-1") && is(report.value->find("type"), "ready") &&
				  is(report.value->find("nativeGeneration"), "9007199254740993"),
			"the native generation is a string, as in the watch reports");
		check(number32(report.value->find("owner"), value.owner, value.owner, number) && number32(report.value->find("origin"), 0, 0, number) &&
			  number32(report.value->find("scene"), 3, 3, number) && number32(report.value->find("subscene"), 44, 44, number));
		check(is(report.value->find("checkpointSha256"), value.checkpoint) && is(report.value->find("emulatorSha256"), value.emulator) &&
			  is(report.value->find("recipeSha256"), value.recipe));
		const auto* encoded = report.value->find("inputPolicy");
		check(encoded);
		bool flag = false;
		check(pbd::json::detail::boolean(encoded->find("sessionStartSkip"), flag) && flag == input.start_skip &&
			  pbd::json::detail::boolean(encoded->find("exitGate"), flag) && flag == input.exit_gate &&
			  pbd::json::detail::boolean(encoded->find("keyedDraws"), flag) && flag == input.keyed_draws &&
			  number32(encoded->find("battleClockVersion"), input.battle_clock_version, input.battle_clock_version, number) &&
			  number32(encoded->find("menuHashMask"), input.menu_hash_mask, input.menu_hash_mask, number));
	}
}

PBD_TEST(PbdSpectatorStandby, TheEndReportNamesItsReason)
{
	const auto text = end_text("spectator_input_policy_mismatch", 1234);
	check(text.ends_with('\n'));
	const auto report = json_parser(text).parse();
	std::uint64_t waited = 0;
	check(report.value && is(report.value->find("schema"), "pbd-spectator-standby-1") && is(report.value->find("type"), "ended") &&
		  is(report.value->find("reason"), "spectator_input_policy_mismatch") && number(report.value->find("waitedMs"), 1234, 1234, waited));
	const auto quoted = json_parser(end_text("a\"b", 0)).parse();
	check(quoted.value && is(quoted.value->find("reason"), "a\"b"), "the reason is a JSON string");
}

PBD_TEST(PbdSpectatorStandby, AReportIsPublishedWholeAndNeverReplaced)
{
	const auto root = fresh_directory("pbd-spectator-standby-test");
	const auto file = root / std::string(ready_file);
	check(publish(file, "first\n"));
	check(read(file) == "first\n");
	check(!publish(file, "second\n"), "an existing report is never replaced");
	check(read(file) == "first\n");
	check(!std::filesystem::exists(root / ".spectator-standby.json.tmp"), "no temporary file is left behind");
	check(!publish(root / "missing" / std::string(end_file), "x"), "a report that cannot be written says so");
	std::filesystem::remove_all(root);
}

PBD_TEST(PbdSpectatorStandby, OnlyTheExactSwitchAsksForAStandby)
{
	put("PBD_SPECTATOR_STANDBY", nullptr);
	check(!requested());
	put("PBD_SPECTATOR_STANDBY", "1");
	check(requested());
	put("PBD_SPECTATOR_STANDBY", "true");
	check(!requested());
	put("PBD_SPECTATOR_STANDBY", nullptr);
}

PBD_TEST(PbdSpectatorStandby, ABindTakesOnlyASetWithTheStandbysInputPolicy)
{
	const policy standby;
	pbd::spectator::manifest set;
	check(policy_refusal(standby, set) == nullptr, "a set from a source that declares no policy is taken");
	set.policy_present = true;
	check(policy_refusal(standby, set) == nullptr, "the standard policy on both sides");
	auto keyed = set;
	keyed.keyed_draws = true;
	keyed.menu_hash_mask = 7;
	check(policy_refusal(standby, keyed) == keyed_draws_refusal, "keyed draws are named first, for the launcher's sentence");
	for (const auto change : {+[](pbd::spectator::manifest& m) { m.start_skip = false; },
			 +[](pbd::spectator::manifest& m) { m.exit_gate = false; },
			 +[](pbd::spectator::manifest& m) { m.battle_clock_version = 1; },
			 +[](pbd::spectator::manifest& m) { m.menu_hash_mask = 2; }})
	{
		auto other = set;
		change(other);
		check(policy_refusal(standby, other) == input_policy_refusal, "any other policy difference refuses the bind");
	}
	auto phase = set;
	phase.phase_delay = true;
	check(policy_refusal(standby, phase) == nullptr, "the source's phase delay is not applied to a watcher");
	check(std::string_view(keyed_draws_refusal) == "spectator_keyed_draws_mismatch" &&
		  std::string_view(input_policy_refusal) == "spectator_input_policy_mismatch", "the reasons the wire note lists");
}

PBD_TEST(PbdSpectatorStandby, ABriefDenialOfTheBoundManifestIsPolledAgain)
{
	using std::chrono::milliseconds;
	const std::chrono::steady_clock::time_point start{};
	bind_wait wait;
	check(wait.again("unavailable", start), "a manifest not written yet is waited for");
	check(wait.again("read_failed", start + milliseconds(10)), "a read denied while the file is moved into place is tried again");
	check(wait.again("read_failed", start + milliseconds(1009)), "for up to a second");
	check(!wait.again("read_failed", start + milliseconds(1010)), "a read denied for longer ends the standby");
	bind_wait later;
	check(later.again("read_failed", start) && later.again("unavailable", start + milliseconds(1500)));
	check(later.again("read_failed", start + milliseconds(1600)), "a later denial has a second of its own");
	check(!later.again("spectator_session_invalid", start + milliseconds(1610)), "a manifest that does not parse ends it");
	check(!later.again("file_too_large", start + milliseconds(1620)));
}
