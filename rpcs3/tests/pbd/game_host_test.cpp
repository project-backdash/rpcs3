#include "pbd_test.h"

#include "rpcs3qt/pbd_game_host_policy.h"

#include <string>

using pbd::test::check;

PBD_TEST(PbdGameHost, RecordingKeysNeverToggleFullscreen)
{
	using pbd::game_host::fullscreen_key;
	check(fullscreen_key(true, false));
	check(!fullscreen_key(true, true)); // Ctrl+F11 and Ctrl+Alt+F11 belong to recorders.
	check(!fullscreen_key(false, false));
}

PBD_TEST(PbdGameHost, DoesNotCoverLauncherBeforeFirstFrame)
{
	using pbd::game_host::can_show_frame;
	check(!can_show_frame(false, false));
	check(!can_show_frame(false, true));
	check(!can_show_frame(true, false));
	check(can_show_frame(true, true));
}

PBD_TEST(PbdGameHost, Checks)
{
	using pbd::game_host::parse_control;
	check(parse_control("PBD1 42 close\n", 42) == 1);
	check(parse_control("PBD1 42 resize\n", 42) == 2);
	check(parse_control("PBD1 42 launcher\n", 42) == 3);
	check(parse_control("PBD1 42 game\n", 42) == 4);
	for (const auto request : {"", "PBD1 42 close", "PBD1 41 close\n", "PBD1 -42 close\n", "PBD1 4294967296 close\n",
			 "PBD1 42 kill\n", "PBD1 42 close extra\n", "PBD1 42 close\nclose\n", "PBD1 42  close\n", "PBD1 42close\n",
			 "PBD1  close\n", "PBD1 42 \n"})
		check(parse_control(request, 42) == 0);
	check(parse_control("PBD1 0 close\n", 0) == 0);
	check(parse_control("PBD1 42 " + std::string(128, 'x') + "\n", 42) == 0);
}
