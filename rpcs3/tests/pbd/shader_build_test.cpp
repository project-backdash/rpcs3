#include "pbd_test.h"

#include "Emu/PBD/pbd_session_health.h"
#include "Emu/PBD/pbd_shader_build.h"

#include <array>
#include <cstring>
#include <string>
#include <vector>

using pbd::test::check;

namespace
{
	constexpr std::uint64_t second = 1'000'000'000, second_us = 1'000'000;

	// A battle whose last boundary call ended at 100 s while the other PC's input keeps arriving.
	pbd::session_health::stall_facts battle_at(std::uint64_t now_ns)
	{
		return {.idle = true, .battle = true, .held = false, .boundary_seen = true, .quiet_from_ns = 100 * second, .now_ns = now_ns, .peer_heard_us = now_ns / 1000 - 16'000, .now_us = now_ns / 1000};
	}

	struct record
	{
		pbd::shader_build::record_header header;
		std::array<unsigned char, 40> state;
	};

	record make_record(std::uint64_t options)
	{
		record result{};
		for (std::size_t i = 0; i < result.state.size(); ++i)
			result.state[i] = static_cast<unsigned char>(i * 7 + 3);
		result.header = {.magic = pbd::shader_build::record_magic, .version = pbd::shader_build::record_version, .vendor_id = 0x1002, .device_id = 0x747e, .options = options, .state_bytes = static_cast<std::uint32_t>(result.state.size()), .reserved = 0};
		return result;
	}

	std::string name_of(const record& value)
	{
		return pbd::shader_build::record_name(value.header.options, pbd::shader_build::fnv1a64(value.state.data(), value.state.size()));
	}

	bool valid(const std::string& name, const record& value, std::uint64_t file_bytes = sizeof(pbd::shader_build::record_header) + 40)
	{
		return pbd::shader_build::record_valid(name, value.header, file_bytes, 0x1002, 0x747e, value.state.data(), static_cast<std::uint32_t>(value.state.size()));
	}
} // namespace

PBD_TEST(PbdShaderBuild, AShaderBuildStretchesTheStallBoundToSixtySeconds)
{
	using namespace pbd::session_health;
	auto facts = battle_at(110 * second);
	check(stalled(facts), "a 10 s stall with no shader build was not ended");
	facts.renderer_building = true;
	check(!stalled(facts), "a game waiting on a renderer shader build was ended at 10 s");
	facts = battle_at(160 * second - 1);
	facts.renderer_building = true;
	check(!stalled(facts), "a shader build was ended before 60 s");
	facts = battle_at(160 * second);
	facts.renderer_building = true;
	check(stalled(facts), "a shader build that held the game for 60 s was never ended");
}

PBD_TEST(PbdShaderBuild, TheQuietTimeRestartsWhenAShaderBuildEnds)
{
	using namespace pbd::session_health;
	// The build held the game from 101 s to 130 s.
	auto facts = battle_at(139 * second);
	facts.renderer_built_ns = 130 * second;
	check(!stalled(facts), "the quiet time did not restart at the build's end");
	facts = battle_at(140 * second);
	facts.renderer_built_ns = 130 * second;
	check(stalled(facts), "10 s of quiet after a build ended was not a stall");
	// A build that ended before the last boundary changes nothing.
	facts = battle_at(110 * second);
	facts.renderer_built_ns = 50 * second;
	check(stalled(facts), "an old build delayed the stall watch");
	// A build stamped after the watch read its clock is not in the future.
	facts = battle_at(112 * second);
	facts.renderer_built_ns = 113 * second;
	check(!stalled(facts), "a build ending just after the clock read was a stall");
}

PBD_TEST(PbdShaderBuild, AStreamOfBuildsCannotStretchTheQuietPastSixtySeconds)
{
	using namespace pbd::session_health;
	// Builds back to back from 100 s: the last ended at 155 s and another is in progress.
	auto facts = battle_at(160 * second - 1);
	facts.renderer_built_ns = 155 * second;
	facts.renderer_building = true;
	check(!stalled(facts), "a stretch of builds was ended before 60 s");
	facts = battle_at(160 * second);
	facts.renderer_built_ns = 155 * second;
	facts.renderer_building = true;
	check(stalled(facts), "a stretch of builds held the game past 60 s");
	facts.renderer_building = false;
	check(stalled(facts), "60 s of quiet after a build was not a stall");
}

PBD_TEST(PbdShaderBuild, OtherStallRulesStayAsTheyWere)
{
	using namespace pbd::session_health;
	auto facts = battle_at(170 * second);
	facts.renderer_building = true;
	facts.idle = false;
	check(!stalled(facts), "a wait inside the boundary call was ended by the stall watch");
	facts = battle_at(170 * second);
	facts.renderer_building = true;
	facts.battle = false;
	check(!stalled(facts), "a load outside battle was ended as a stall");
	facts = battle_at(170 * second);
	facts.renderer_building = true;
	facts.held = true;
	check(!stalled(facts), "a held game was ended as a stall");
	facts = battle_at(170 * second);
	facts.renderer_building = true;
	facts.peer_heard_us = facts.now_us - peer_silence_us;
	check(!stalled(facts), "a silent peer was blamed on this game");
}

PBD_TEST(PbdShaderBuild, LiveStateTracksTheBuildInProgressAndTheLongest)
{
	pbd::shader_build::live state;
	auto facts = state.read();
	check(facts.current == pbd::shader_build::kind::none && !facts.started_ns && !facts.ended_ns && !facts.builds, "a fresh state invented a build");
	state.begin(pbd::shader_build::kind::wait, 0x45, 0xabc, 12, 34, 5 * second);
	facts = state.read();
	check(facts.current == pbd::shader_build::kind::wait && facts.started_ns == 5 * second && facts.options == 0x45 && facts.state_hash == 0xabc &&
			  facts.vertex_id == 12 && facts.fragment_id == 34,
		"the build in progress was not visible");
	state.end(11 * second);
	facts = state.read();
	check(facts.current == pbd::shader_build::kind::none && facts.ended_ns == 11 * second && facts.builds == 1 && facts.longest_ns == 6 * second,
		"the finished build was not recorded");
	state.begin(pbd::shader_build::kind::variant, 0x1, 0x2, 0, 0, 20 * second);
	state.end(22 * second);
	facts = state.read();
	check(facts.builds == 2 && facts.longest_ns == 6 * second && facts.ended_ns == 22 * second, "a shorter build replaced the longest");
	state.end(30 * second);
	check(state.read().builds == 2, "an end without a build was counted");
	check(!state.fight_started(), "a fresh state saw a fight");
	state.mark_fight();
	check(state.fight_started(), "the fight start was not kept");
}

PBD_TEST(PbdShaderBuild, TheProbeLineNamesTheBuildInProgress)
{
	pbd::shader_build::facts facts{};
	facts.current = pbd::shader_build::kind::wait;
	facts.started_ns = 100 * second;
	facts.options = 0x45;
	facts.state_hash = 0x0123456789abcdefull;
	facts.vertex_id = 12;
	facts.fragment_id = 34;
	facts.builds = 3;
	facts.longest_ns = 5'712'000'000;
	facts.prebuild_queued = 64;
	facts.prebuild_built = 40;
	check(pbd::shader_build::describe(facts, 104'500'000'000) ==
			  "RSX shader build: inside wait for a program building elsewhere for 4500 ms (options 0x0045, state 0123456789abcdef, vp 12 fp 34), "
			  "builds 3, longest 5712 ms, interpreter variants prebuilt 40 of 64",
		"the probe line for a wait changed");
	facts.current = pbd::shader_build::kind::variant;
	check(pbd::shader_build::describe(facts, 101 * second) ==
			  "RSX shader build: inside interpreter variant build for 1000 ms (options 0x0045, state 0123456789abcdef), builds 3, longest 5712 ms, "
			  "interpreter variants prebuilt 40 of 64",
		"the probe line for a variant build changed");
	facts.current = pbd::shader_build::kind::none;
	facts.ended_ns = 90 * second;
	check(pbd::shader_build::describe(facts, 100 * second) ==
			  "RSX shader build: none (last ended 10000 ms ago), builds 3, longest 5712 ms, interpreter variants prebuilt 40 of 64",
		"the probe line without a build changed");
	check(pbd::shader_build::describe({}, 100 * second) == "RSX shader build: none, builds 0, longest 0 ms, interpreter variants prebuilt 0 of 0",
		"the probe line before any build changed");
}

PBD_TEST(PbdShaderBuild, ARecordIsAcceptedOnlyFromThisGpuIntactAndUnderItsOwnName)
{
	const auto good = make_record(0x0045);
	const auto name = name_of(good);
	check(name.size() == 25 && name.ends_with(".bin") && name.starts_with("0045-"), "the record name format changed");
	check(valid(name, good), "an intact record was rejected");
	check(!valid(name, good, sizeof(pbd::shader_build::record_header) + 39), "a short file was accepted");
	auto changed = good;
	changed.header.magic ^= 1;
	check(!valid(name, changed), "a wrong magic was accepted");
	changed = good;
	changed.header.version++;
	check(!valid(name, changed), "another record version was accepted");
	changed = good;
	changed.header.vendor_id = 0x10de;
	check(!valid(name, changed), "a record from another vendor's GPU was accepted");
	changed = good;
	changed.header.device_id++;
	check(!valid(name, changed), "a record from another GPU was accepted");
	changed = good;
	changed.state[7] ^= 0x10;
	check(!valid(name, changed), "a damaged state was accepted under the old name");
	changed = good;
	changed.header.options = 0x0046;
	check(!valid(name, changed), "options that differ from the name were accepted");
	changed = make_record(0x10000);
	check(!valid(name_of(changed), changed), "options beyond the 16 interpreter bits were accepted");
	changed = make_record(0x0c00);
	check(!valid(name_of(changed), changed), "two alpha test comparisons were accepted");
	changed = make_record(0x0401);
	check(valid(name_of(changed), changed), "one alpha test comparison was rejected");
}

PBD_TEST(PbdShaderBuild, TheNewestValidRecordsAreTakenUpToTheLimit)
{
	using pbd::shader_build::record_file;
	const auto name = [](int index)
	{
		return pbd::shader_build::record_name(static_cast<std::uint64_t>(index), 0x1000 + index);
	};
	std::vector<record_file> files;
	for (int index = 0; index < 100; ++index)
		files.push_back({name(index), 1000 + index, 72});
	files.push_back({name(200), 5000, 71});   // newest, but a torn write
	files.push_back({"notes.txt", 6000, 72}); // not a record
	files.push_back({"0001-0000000000001001.tmp", 6000, 72});
	std::vector<std::string> offered;
	const auto taken = pbd::shader_build::take_newest(files, 72, 64, [&](const std::string& candidate)
		{
			offered.push_back(candidate);
			return candidate != name(98); // one damaged record among the newest
		});
	check(taken == 64, "the limit was not filled from the valid records");
	check(offered.size() == 65 && offered.front() == name(99) && offered[1] == name(98) && offered.back() == name(35),
		"records were not offered newest first, or a damaged one used up the limit");
	std::vector<record_file> few{{name(1), 10, 72}, {name(2), 10, 72}, {name(3), 5, 72}};
	offered.clear();
	check(pbd::shader_build::take_newest(few, 72, 64, [&](const std::string& candidate)
			  {
				  offered.push_back(candidate);
				  return true;
			  }) == 3,
		"fewer records than the limit were not all taken");
	check(offered == std::vector<std::string>{name(1), name(2), name(3)}, "records with the same time were not ordered by name");
}
