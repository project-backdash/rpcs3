#include "pbd_test.h"

#include "Emu/PBD/pbd_session_policy.h"

#include <cstdint>
#include <map>
#include <string>
#include <vector>

using pbd::test::check;

namespace
{
	using fields = std::map<std::string, std::string>;

	pbd::session_policy::description launch(const fields& values)
	{
		return pbd::session_policy::read([&](const char* key) -> const char*
			{
				const auto found = values.find(key);
				return found == values.end() ? nullptr : found->second.c_str();
			});
	}
} // namespace

PBD_TEST(PbdSessionPolicy, OnlyTheDirectPolicySkipsReplayDrawsAndWithholdsExactly)
{
	using pbd::session_policy::replay_presentation_of;
	// A friend room, a lobby set, its held game and its hidden preparation, with stage sync and the loading gate on or off.
	for (const fields& direct : {fields{{"PBD_SESSION_POLICY", "direct-v1"}},
			 fields{{"PBD_SESSION_POLICY", "direct-v1"}, {"PBD_SET_CHANNEL", "set"}},
			 fields{{"PBD_SESSION_POLICY", "direct-v1"}, {"PBD_MATCH_STANDBY_CHANNEL", "standby"}},
			 fields{{"PBD_SESSION_POLICY", "direct-v1"}, {"PBD_PREPARE_FOR", "lobby"}},
			 fields{{"PBD_SESSION_POLICY", "direct-v1"}, {"PBD_SET_CHANNEL", "set"}, {"PBD_SESSION_STAGE_SYNC", "0"}, {"PBD_SESSION_EXIT_GATE", "0"}}})
	{
		const auto replay = replay_presentation_of(launch(direct));
		check(std::vector<std::uint32_t>(replay.skips.begin(), replay.skips.end()) == std::vector<std::uint32_t>{0x158d6c, 0x260d44, 0x2649a8, 0x268778});
		check(replay.player_draw_gate && replay.stage_camera_gate && replay.withhold_exact);
	}
	// The diagnostic route, and a policy name that is refused, take skips, gates and withholding from their own switches.
	for (const fields& other : {fields{}, fields{{"PBD_SET_CHANNEL", "set"}}, fields{{"PBD_SESSION_POLICY", "direct-v2"}},
			 fields{{"PBD_SESSION_POLICY", "Direct-v1"}}})
	{
		const auto replay = replay_presentation_of(launch(other));
		check(replay.skips.empty() && !replay.player_draw_gate && !replay.stage_camera_gate && !replay.withhold_exact);
	}
}

