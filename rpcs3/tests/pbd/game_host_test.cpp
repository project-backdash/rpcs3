#include "pbd_test.h"

#include "rpcs3qt/pbd_game_host_policy.h"
#include "rpcs3qt/pbd_game_host_visibility.h"
#include "rpcs3qt/pbd_game_host_shell.h"
#include "Emu/PBD/pbd_screen_check.h"

#include <string>

using pbd::test::check;

PBD_TEST(PbdGameHost, AudioRequiresExactTokenRangeAndMute)
{
	using namespace pbd::game_host;
	audio_control audio{33, false};
	for (const int volume : {0, 35, 100})
		for (const int muted : {0, 1})
		{
			check(parse_control("PBD1 42 audio " + std::to_string(volume) + " " + std::to_string(muted) + " 17\n", 42, &audio) == 9);
			check(audio.volume == volume && audio.muted == (muted == 1) && audio.revision == 17);
		}
	for (const auto request : {"PBD1 41 audio 50 0\n", "PBD1 42 audio -1 0\n", "PBD1 42 audio 101 0\n",
			 "PBD1 42 audio 50 2\n", "PBD1 42 audio 50 true\n", "PBD1 42 audio 50\n", "PBD1 42 audio 050 0\n",
			 "PBD1 42 audio 50 0 \n", "PBD1 42 audio 50 0\nclose\n", "PBD1 42 audio 50.0 0\n", "PBD1 42 audio +50 0\n",
			 "PBD1 42 audio 50 0 0\n", "PBD1 42 audio 50 0 01\n", "PBD1 42 audio 50 0 4294967296\n",
			 "PBD1 42 audio 50 0 17 extra\n", "PBD1 41 audio 50 0 17\n"})
	{
		const auto previous = audio;
		check(parse_control(request, 42, &audio) == 0);
		check(audio.volume == previous.volume && audio.muted == previous.muted && audio.revision == previous.revision);
	}
}

PBD_TEST(PbdGameHost, RecordingKeysNeverToggleFullscreen)
{
	using pbd::game_host::fullscreen_key;
	check(fullscreen_key(true, false));
	check(!fullscreen_key(true, true)); // Ctrl+F11 and Ctrl+Alt+F11 belong to recorders.
	check(!fullscreen_key(false, false));
}

PBD_TEST(PbdGameHost, ShiftF11IsThePlayersWayOutAndNoOtherKey)
{
	using pbd::game_host::escape_key;
	check(escape_key(true, true, false));
	check(!escape_key(true, false, false)); // F11 alone is full screen
	check(!escape_key(true, true, true));   // Ctrl, Alt or Win with it belong to recorders and overlays
	check(!escape_key(false, true, false));
	check(!pbd::game_host::fullscreen_key(true, true)); // and Shift+F11 never toggles full screen as well
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
	check(parse_control("PBD1 42 visible\n", 42) == 5);
	check(parse_control("PBD1 42 recover\n", 42) == 6);
	check(parse_control("PBD1 42 escape\n", 42) == 8);
	check(parse_control("PBD1 41 escape\n", 42) == 0);
	for (const auto request : {"", "PBD1 42 close", "PBD1 41 close\n", "PBD1 -42 close\n", "PBD1 4294967296 close\n",
			 "PBD1 42 kill\n", "PBD1 42 close extra\n", "PBD1 42 close\nclose\n", "PBD1 42  close\n", "PBD1 42close\n",
			 "PBD1  close\n", "PBD1 42 \n"})
		check(parse_control(request, 42) == 0);
	check(parse_control("PBD1 0 close\n", 0) == 0);
	check(parse_control("PBD1 42 " + std::string(128, 'x') + "\n", 42) == 0);
}

PBD_TEST(PbdGameHost, VisibilityCommandsRequireExactTokenAndMessage)
{
	for (const auto command : {"visible", "recover"})
	{
		check(pbd::game_host::parse_control("PBD1 41 " + std::string(command) + "\n", 42) == 0);
		check(pbd::game_host::parse_control("PBD1 42 " + std::string(command) + " extra\n", 42) == 0);
		check(pbd::game_host::parse_control("PBD1 42 " + std::string(command), 42) == 0);
		check(pbd::game_host::parse_control("PBD1 42 " + std::string(command) + "\nclose\n", 42) == 0);
	}
}

PBD_TEST(PbdGameHost, VisibilityRequiresEveryObservedCondition)
{
	using pbd::game_host::window_visibility;
	const window_visibility shown{true, true, false, true, false, true, true};
	check(shown.verified());
	for (const auto field : {&window_visibility::exists, &window_visibility::visible, &window_visibility::cloak_known,
			 &window_visibility::on_screen, &window_visibility::foreground})
	{
		auto missing = shown;
		missing.*field = false;
		check(!missing.verified());
	}
	for (const auto field : {&window_visibility::minimized, &window_visibility::cloaked})
	{
		auto hidden = shown;
		hidden.*field = true;
		check(!hidden.verified());
	}
}

PBD_TEST(PbdGameHost, VisibilityReplyDistinguishesObservationFromQueuedCommand)
{
	using pbd::game_host::control_response;
	check(control_response(5, true, true) == "OK\n");
	check(control_response(5, true, false) == "NO\n");
	check(control_response(5, false, true) == "ERR\n");
	// A game not yet confirmed says whether the player is in front of it; a confirmed one is just OK.
	check(control_response(5, true, false, true) == "NO FRONT\n");
	check(control_response(5, true, true, true) == "OK\n");
	check(control_response(5, false, false, true) == "ERR\n");
	// One whose screen check still runs in its own window, shown and in front, says so; once confirmed it is just OK.
	check(control_response(5, true, false, true, true) == "NO PENDING\n");
	check(control_response(5, true, true, true, true) == "OK\n");
	check(control_response(5, false, false, true, true) == "ERR\n");
	check(control_response(6, true, false, true) == "OK\n");
	for (int action : {1, 2, 3, 4, 6})
	{
		check(control_response(action, true) == "OK\n");
		check(control_response(action, false) == "ERR\n");
	}
	check(control_response(0, false) == "ERR\n");
}

namespace
{
	using pbd::game_host::judge_screen;
	using pbd::game_host::screen_sample;
	using pbd::game_host::screen_verdict;

	// A sample whose 16 points all read `screen` from the desktop and `frame` from the game, every point usable.
	screen_sample uniform(std::uint32_t screen, std::uint32_t frame, bool framed = true)
	{
		screen_sample sample;
		sample.screen.fill(screen);
		sample.frame.fill(frame);
		sample.usable = 0xffff;
		sample.framed = framed;
		return sample;
	}

	// A frame with a different bright colour at each point, shifted by `phase` to make it move.
	screen_sample moving(std::uint32_t screen_base, int phase, bool shown)
	{
		screen_sample sample;
		for (std::uint32_t i = 0; i < pbd::game_host::screen_points; ++i)
		{
			const std::uint32_t level = 64 + ((i * 11 + phase * 40) % 160);
			sample.frame[i] = (level << 16) | (0x30 << 8) | (255 - level);
			sample.screen[i] = shown ? sample.frame[i] : screen_base;
		}
		sample.usable = 0xffff;
		sample.framed = true;
		return sample;
	}
} // namespace

PBD_TEST(PbdGameHost, ScreenCheckConfirmsTheGamesOwnPicture)
{
	const auto first = moving(0, 0, true), latest = moving(0, 1, true);
	check(judge_screen(nullptr, first, latest) == screen_verdict::confirmed);
	check(pbd::game_host::screen_confirmed(first, nullptr));
	// Small differences from the scaling filter still match.
	auto filtered = first;
	for (auto& colour : filtered.screen)
		colour += 0x0a0a0a;
	check(pbd::game_host::screen_confirmed(filtered, nullptr));
	// A match only counts where the launcher's own view did not already have that colour.
	auto launcher = first;
	check(!pbd::game_host::screen_confirmed(first, &launcher));
	// Points under another program's window prove nothing.
	auto covered = first;
	covered.usable = 0x0007;
	check(!pbd::game_host::screen_confirmed(covered, nullptr));
	// Near-black frames never confirm: the launcher's page is dark too.
	check(!pbd::game_host::screen_confirmed(uniform(0x020202, 0x000000), nullptr));
}

PBD_TEST(PbdGameHost, ScreenCheckFindsTheLauncherBackgroundWhereTheGameDraws)
{
	const auto hidden = uniform(pbd::game_host::launcher_background, 0x8040c0);
	check(judge_screen(nullptr, hidden, hidden) == screen_verdict::launcher_background);
	// Without the game's frame the launcher's background colour is still evidence.
	const auto unframed = uniform(0x150709, 0, false);
	check(judge_screen(nullptr, unframed, unframed) == screen_verdict::launcher_background);
	// A game drawing that colour itself is not hidden.
	const auto same = uniform(pbd::game_host::launcher_background, pbd::game_host::launcher_background);
	check(judge_screen(nullptr, same, same) == screen_verdict::undecided);
	// One sample alone never decides it; both must show it.
	check(judge_screen(nullptr, hidden, moving(0, 1, true)) == screen_verdict::confirmed);
	check(judge_screen(nullptr, hidden, uniform(0x303030, 0x8040c0)) == screen_verdict::undecided);
	// Fewer than ten usable points are not enough.
	auto few = hidden;
	few.usable = 0x01ff;
	check(judge_screen(nullptr, few, few) == screen_verdict::undecided);
}

PBD_TEST(PbdGameHost, ScreenCheckFindsTheLauncherStillShownOverTheGame)
{
	// The launcher's page before the show, and the same page after it while the game draws something else.
	auto page = uniform(0x5a0e18, 0, false);
	page.screen[3] = 0xe0e0e0;
	auto first = moving(0, 0, false), latest = moving(0, 0, false);
	first.screen = page.screen;
	latest.screen = page.screen;
	check(judge_screen(&page, first, latest) == screen_verdict::launcher_unchanged);
	// Without the view from before the show this static game proves nothing.
	check(judge_screen(nullptr, first, latest) == screen_verdict::undecided);
	// A game whose frame looks like the page proves nothing either.
	auto lookalike = first;
	lookalike.frame = page.screen;
	check(judge_screen(&page, lookalike, lookalike) == screen_verdict::undecided);
	// Without the frame's colours there is nothing to compare.
	auto unframed = first;
	unframed.framed = false;
	check(judge_screen(&page, unframed, unframed) == screen_verdict::undecided);
	// A desktop read that comes back black is no evidence: directly flipped pictures can read so.
	auto black = uniform(0, 0, false);
	auto black_first = moving(0, 0, false), black_latest = moving(0, 0, false);
	check(judge_screen(&black, black_first, black_latest) == screen_verdict::undecided);
}

PBD_TEST(PbdGameHost, ScreenCheckFindsAFrozenScreenWhileTheGameMoves)
{
	const auto first = moving(0x5a0e18, 0, false), latest = moving(0x5a0e18, 1, false);
	check(judge_screen(nullptr, first, latest) == screen_verdict::frozen);
	// A still game and a still screen: inconclusive.
	check(judge_screen(nullptr, first, moving(0x5a0e18, 0, false)) == screen_verdict::undecided);
	// A black still screen is a read that could not see the picture, not a frozen one.
	check(judge_screen(nullptr, moving(0, 0, false), moving(0, 1, false)) == screen_verdict::undecided);
	// A screen that follows the game is the game.
	check(judge_screen(nullptr, moving(0, 0, true), moving(0, 1, true)) == screen_verdict::confirmed);
	// Without both frames' colours, no change of the game can be seen.
	auto unframed = latest;
	unframed.framed = false;
	check(judge_screen(nullptr, first, unframed) == screen_verdict::undecided);
}

PBD_TEST(PbdGameHost, ScreenVerdictsThatFailAndTheirNames)
{
	using pbd::game_host::screen_hidden;
	check(!screen_hidden(screen_verdict::undecided));
	check(!screen_hidden(screen_verdict::confirmed));
	check(screen_hidden(screen_verdict::launcher_background));
	check(screen_hidden(screen_verdict::launcher_unchanged));
	check(screen_hidden(screen_verdict::frozen));
	check(pbd::game_host::screen_verdict_name(screen_verdict::undecided) == "inconclusive");
	check(pbd::game_host::colour_distance(0x102030, 0x132a2c) == 10);
	check(pbd::game_host::colour_distinct(0x003000) && !pbd::game_host::colour_distinct(0x2f2f2f));
}

PBD_TEST(PbdGameHost, ScreenCheckPointsAndFrameExchange)
{
	using namespace pbd::screen_check;
	check(points == pbd::game_host::screen_points);
	check(point_x(0, 1920) == 240 && point_x(3, 1920) == 1680 && point_y(0, 1080) == 135 && point_y(15, 1080) == 945);
	for (std::uint32_t i = 0; i < points; ++i)
		check(point_x(i, 160) < 160 && point_y(i, 90) < 90);
	exchange colours;
	check(!colours.claim());
	check(!colours.answered(1));
	const auto first = colours.request(), second = colours.request();
	check(second > first);
	// The renderer answers the newest waiting request once; an answer serves every request up to it.
	check(colours.claim() == second);
	check(!colours.claim());
	frame_colours frame;
	frame.request = second;
	frame.rgb[5] = 0x123456;
	colours.answer(frame);
	check(colours.answered(first) && colours.answered(second)->rgb[5] == 0x123456);
	check(!colours.answered(second + 1));
	// An older answer never replaces a newer one.
	frame.request = first;
	frame.rgb[5] = 0;
	colours.answer(frame);
	check(colours.answered(second)->rgb[5] == 0x123456);
}

PBD_TEST(PbdGameHost, LogNamesArePrintableWithoutSeparators)
{
	check(pbd::game_host::printable_ascii(L"AMD Radeon(TM) Graphics") == "AMD Radeon(TM) Graphics");
	check(pbd::game_host::printable_ascii(L"a[b]c|dé\n") == "a?b?c?d??");
}

PBD_TEST(PbdGameHost, RecoveryNeverMovesALauncherThatIsFineWhileTheScreenCheckRuns)
{
	using pbd::game_host::recovery_skipped;
	// The launcher asks for a recovery while native says NO, which it does while the check runs: then nothing is done.
	check(recovery_skipped(true, true));
	// A minimized, hidden or off-screen launcher is repaired even then, and any launcher once the check is over.
	check(!recovery_skipped(true, false));
	check(!recovery_skipped(false, true));
	check(!recovery_skipped(false, false));
}

PBD_TEST(PbdGameHost, WindowRepairRestoresOnlyWhatIsHiddenAndMovesOnlyWhatNoMonitorShows)
{
	using pbd::game_host::plan_window_repair;
	// minimized, hidden, on a monitor, maximized, full screen
	const auto fine = plan_window_repair(false, false, true, false, false);
	check(!fine.restore && !fine.reposition);
	const auto minimized = plan_window_repair(true, false, true, false, false);
	check(minimized.restore && !minimized.reposition);
	check(plan_window_repair(false, true, true, false, false).restore);
	const auto lost = plan_window_repair(false, false, false, false, false);
	check(!lost.restore && lost.reposition);
	// Full screen and maximized launchers are never moved or resized, wherever Windows reports them.
	check(!plan_window_repair(false, false, true, false, true).reposition);
	check(!plan_window_repair(true, false, false, true, false).reposition);
	check(!plan_window_repair(false, false, false, false, true).reposition);
}

PBD_TEST(PbdGameHost, AnOwnedGameWindowTakesTheKeyboardBackOnlyWhenThatCancelsNothing)
{
	using pbd::game_host::hand_back;
	using pbd::game_host::plan_hand_back;
	// game in front, launcher in front, launcher moving/sizing/in a menu, mouse button down
	check(plan_hand_back(false, true, false, false) == hand_back::now);
	// A drag or resize by the launcher's frame, its window menu, or a press that may start one: later, not now.
	check(plan_hand_back(false, true, true, false) == hand_back::later);
	check(plan_hand_back(false, true, false, true) == hand_back::later);
	// The game has it already, or the player is in another program: nothing to do.
	check(plan_hand_back(true, false, false, false) == hand_back::none);
	check(plan_hand_back(false, false, false, false) == hand_back::none);
	check(plan_hand_back(false, false, true, true) == hand_back::none);
}

PBD_TEST(PbdGameHost, ABlackFrameOverThePageOrTheBackgroundReadsAsHidden)
{
	using pbd::game_host::launcher_background;
	// A game still black at its start, composed under the launcher: the screen shows the launcher's background.
	const auto background = uniform(launcher_background, 0x000000);
	check(judge_screen(nullptr, background, background) == screen_verdict::launcher_background);
	// The same in the limited output range, where black is 16, 16, 16.
	const auto limited = uniform(launcher_background, 0x101010);
	check(judge_screen(nullptr, limited, limited) == screen_verdict::launcher_background);
	// Or the screen still shows the launcher's page, dark parts included, where the game presents black.
	screen_sample page;
	for (std::uint32_t i = 0; i < pbd::game_host::screen_points; ++i)
		page.screen[i] = i % 4 == 0 ? 0x1a0a0c : 0x5a0e18 + i;
	page.usable = 0xffff;
	auto over_page = page;
	over_page.frame.fill(0x000000);
	over_page.framed = true;
	check(judge_screen(&page, over_page, over_page) == screen_verdict::launcher_unchanged);
	// A game that shows its black frame on screen is never hidden: black reads prove nothing either way.
	const auto shown_black = uniform(0x000000, 0x000000);
	check(judge_screen(&page, shown_black, shown_black) == screen_verdict::undecided);
	check(judge_screen(nullptr, shown_black, shown_black) == screen_verdict::undecided);
}

PBD_TEST(PbdGameHost, APresentedImageAPixelOrTwoOffTheWindowSizeIsStillTheGamesFrame)
{
	using pbd::game_host::frame_fits_window;
	check(frame_fits_window(1920, 1080, 1920, 1080));
	check(frame_fits_window(1920, 1079, 1920, 1080)); // the one-pixel surface refresh
	check(frame_fits_window(1918, 1082, 1920, 1080));
	check(!frame_fits_window(1917, 1080, 1920, 1080));
	check(!frame_fits_window(160, 90, 1920, 1080)); // a surface made before the window had its size
	check(!frame_fits_window(0, 0, 0, 0));
}

PBD_TEST(PbdGameHost, AnImageSizedToTheMonitorIsComparedWhereTheWindowShowsIt)
{
	using namespace pbd::screen_check;
	// A 1601x1004 window whose driver fixes the surface at the 2560x1440 monitor: the renderer draws the game into the
	// image's top-left 1601x1004 and places the points there; the rest of the image is black.
	constexpr std::uint32_t image_w = 2560, image_h = 1440, client_w = 1601, client_h = 1004;
	const auto image = [](std::uint32_t x, std::uint32_t y) -> std::uint32_t
	{
		if (x >= client_w || y >= client_h)
			return 0;
		return ((64 + x * 160 / client_w) << 16) | (0x30 << 8) | (64 + y * 160 / client_h);
	};
	// The window shows the image at its own size from its top left, or stretched over it: each window pixel shows the
	// image pixel under its centre.
	const auto shown = [&](std::uint32_t x, std::uint32_t y)
	{
		return image(x, y);
	};
	const auto stretched = [&](std::uint32_t x, std::uint32_t y)
	{
		return image(static_cast<std::uint32_t>((2ull * x + 1) * image_w / (2 * client_w)),
			static_cast<std::uint32_t>((2ull * y + 1) * image_h / (2 * client_h)));
	};
	// Both readings of a scaled sample, as the game host takes them, of a window presenting one way.
	const auto sample_of = [&](const auto& window)
	{
		screen_sample sample;
		for (std::uint32_t i = 0; i < points; ++i)
		{
			for (const bool stretch : {false, true})
				check(client_x(i, client_w, image_w, client_w, stretch) < client_w && client_y(i, client_h, image_h, client_h, stretch) < client_h);
			sample.frame[i] = image(point_x(i, client_w), point_y(i, client_h));
			sample.screen[i] = window(client_x(i, client_w, image_w, client_w, false), client_y(i, client_h, image_h, client_h, false));
			sample.stretched[i] = window(client_x(i, client_w, image_w, client_w, true), client_y(i, client_h, image_h, client_h, true));
		}
		sample.usable = 0xffff;
		sample.framed = true;
		sample.scaled = !pbd::game_host::frame_fits_window(image_w, image_h, client_w, client_h);
		return sample;
	};
	// Whichever way the driver presents it, one reading confirms the game.
	const auto at_size = sample_of(shown), over_client = sample_of(stretched);
	check(at_size.scaled && over_client.scaled);
	check(judge_screen(nullptr, at_size, at_size, true) == screen_verdict::confirmed);
	check(judge_screen(nullptr, over_client, over_client, true) == screen_verdict::confirmed);
	// The second reading counts only for a scaled sample.
	auto fitted = over_client;
	fitted.scaled = false;
	check(judge_screen(nullptr, fitted, fitted) == screen_verdict::undecided);
	// Without the frame, as before, nothing could confirm it.
	auto unframed = at_size;
	unframed.framed = false;
	check(judge_screen(nullptr, unframed, unframed, true) == screen_verdict::undecided);
	// The drawn part at the client size, or no frame: the window's own grid in the first reading.
	for (std::uint32_t i = 0; i < points; ++i)
	{
		check(client_x(i, client_w, image_w, client_w, false) == point_x(i, client_w) && client_y(i, client_h, image_h, client_h, false) == point_y(i, client_h));
		check(client_x(i, 1920, 1920, 1920, true) == point_x(i, 1920) && client_y(i, 1080, 1080, 1080, true) == point_y(i, 1080));
		check(client_x(i, 0, 0, client_w, true) == point_x(i, client_w) && client_y(i, 0, 0, client_h, false) == point_y(i, client_h));
		// A drawn part wider than the client (a window that shrank before the renderer followed) stays within the client.
		check(client_x(i, 4000, 4000, client_w, false) < client_w && client_x(i, 4000, 4000, client_w, true) < client_w);
	}
	// A screen unchanged since before the show tells wherever the frame's points are, so a scaled sample still finds the
	// launcher shown in the launcher window, where that moves the game into a window of its own; in the game's own window,
	// where only the log would follow, it decides nothing.
	auto page = uniform(0x5a0e18, 0, false);
	page.screen[3] = 0xe0e0e0;
	auto first = moving(0, 0, false), latest = moving(0, 0, false);
	first.screen = latest.screen = page.screen;
	check(judge_screen(&page, first, latest) == screen_verdict::launcher_unchanged);
	first.scaled = latest.scaled = true;
	check(judge_screen(&page, first, latest) == screen_verdict::launcher_unchanged);
	check(judge_screen(&page, first, latest, true) == screen_verdict::undecided);
	// A still screen while the frame moves needs the frame's points where the screen is read: never for a scaled sample.
	auto still_first = moving(0x5a0e18, 0, false), still_latest = moving(0x5a0e18, 1, false);
	check(judge_screen(nullptr, still_first, still_latest) == screen_verdict::frozen);
	still_first.scaled = still_latest.scaled = true;
	check(judge_screen(nullptr, still_first, still_latest) == screen_verdict::undecided);
}

PBD_TEST(PbdGameHost, AGameInItsOwnWindowIsDecidedWithinTheBoundAndSaysSoUntilThen)
{
	using namespace pbd::game_host;
	// Samples held by a wait (the player elsewhere, a surface refresh) cannot keep the game's own window undecided...
	check(!screen_check_overdue(true, screen_decision_ms - 1, false));
	check(screen_check_overdue(true, screen_decision_ms, false));
	check(screen_check_overdue(true, 52422, false));
	check(!screen_check_overdue(true, 52422, true));
	// ...while in the launcher window the check waits for its samples: a picture found missing there moves the game.
	check(!screen_check_overdue(false, 52422, false));
	// Samples that nothing holds pass the check before the bound.
	check(screen_first_sample_ms + screen_undecided_ms + screen_sample_ms + screen_answer_ms < screen_decision_ms);
	// Until it is decided, the visible answer says the check runs on the game's own window, shown and in front.
	check(screen_check_pending(true, true, false));
	check(!screen_check_pending(true, true, true));
	check(!screen_check_pending(true, false, false));
	check(!screen_check_pending(false, true, false));
}

PBD_TEST(PbdGameHost, TheScreenCheckFollowsTheScreenOnceTheLauncherHidesItsPage)
{
	using pbd::game_host::launcher_background;
	// The first sample saw the launcher's page, which the launcher hides once the check passes; the game composed under it
	// shows the launcher's background from then on.
	auto page = moving(0x5a0e18, 0, false);
	page.screen[0] = 0xe0e0e0;
	const auto background_then = moving(launcher_background, 3, false), background_now = moving(launcher_background, 4, false);
	check(judge_screen(nullptr, page, background_now) == screen_verdict::undecided);
	check(judge_screen(nullptr, page, background_then, background_now) == screen_verdict::launcher_background);
	// A game the screen follows is confirmed whichever pair shows it.
	check(judge_screen(nullptr, page, background_then, moving(0, 5, true)) == screen_verdict::confirmed);
}

PBD_TEST(PbdGameHost, OverlaysAreInThePresentedImageSoAStillOverlayIsNoFrozenScreen)
{
	// The frame points come from the image as presented, overlays included (VKPresent.cpp). An opaque menu over most of the
	// points stays still on the screen and in the frame alike, while the game moves behind it: nothing hidden.
	constexpr std::uint32_t menu = 0x201418;
	auto first = moving(0x5a0e18, 0, true), latest = moving(0x5a0e18, 1, true);
	for (std::uint32_t i = 0; i < 12; ++i)
	{
		first.screen[i] = first.frame[i] = menu;
		latest.screen[i] = latest.frame[i] = menu;
	}
	// The four points the menu leaves free show the game moving: confirmed.
	check(judge_screen(nullptr, first, latest) == screen_verdict::confirmed);
	// With those covered by another program too, nothing is decided, and never frozen.
	first.usable = latest.usable = 0x0fff;
	check(judge_screen(nullptr, first, latest) == screen_verdict::undecided);
	// Read before the overlays were drawn, the frame would move under the still menu: a frozen screen, wrongly.
	auto before_first = first, before_latest = latest;
	before_first.frame = moving(0, 0, true).frame;
	before_latest.frame = moving(0, 1, true).frame;
	check(judge_screen(nullptr, before_first, before_latest) == screen_verdict::frozen);
}

PBD_TEST(PbdGameHost, TheScreenCheckKeepsSamplingThenPassesAndReChecksSlowly)
{
	using namespace pbd::game_host;
	// Undecided samples come every half second until the bound...
	auto next = schedule_after_undecided(0, 1, false);
	check(!next.pass && next.next_ms == screen_sample_ms);
	next = schedule_after_undecided(screen_undecided_ms - 1, 13, false);
	check(!next.pass && next.next_ms == screen_sample_ms);
	// ...then the check passes as inconclusive and re-checks every two seconds...
	next = schedule_after_undecided(screen_undecided_ms, 14, false);
	check(next.pass && next.next_ms == screen_recheck_sample_ms);
	// ...but never on one sample alone, as when the player was elsewhere for the whole bound.
	next = schedule_after_undecided(screen_undecided_ms + 5000, 1, false);
	check(!next.pass && next.next_ms == screen_sample_ms);
	next = schedule_after_undecided(screen_undecided_ms + 4000, 16, true);
	check(!next.pass && next.next_ms == screen_recheck_sample_ms);
	// The re-check ends after its time.
	next = schedule_after_undecided(screen_undecided_ms + screen_recheck_ms - 1000, 24, true);
	check(!next.pass && next.next_ms == 0);
	// The whole first check stays well inside the launcher's ten seconds before it offers its way out.
	check(screen_first_sample_ms + screen_undecided_ms + screen_sample_ms < 8000);
}

PBD_TEST(PbdGameHost, AForcedHiddenVerdictFailsTheCheckForHardwareTests)
{
	check(pbd::game_host::screen_hidden(screen_verdict::forced_hidden));
	check(pbd::game_host::screen_verdict_name(screen_verdict::forced_hidden) == "hidden (forced for a test)");
}

#ifdef _WIN32
PBD_TEST(PbdGameHost, QtShellPolicyCoversHostedStandaloneAndQuietProcesses)
{
	using pbd::game_host::qt_shell_required;
	check(!qt_shell_required(nullptr, nullptr));
	check(!qt_shell_required("", "0"));
	check(!qt_shell_required("", "true"));
	check(!qt_shell_required("", "10"));
	check(qt_shell_required(nullptr, "1"));
	check(qt_shell_required("hosted contract", nullptr));
	check(qt_shell_required("standalone contract", "0"));
	check(qt_shell_required("Steam contract", "1"));
}

PBD_TEST(PbdGameHost, QtShellStylePreservesEveryUnrelatedBit)
{
	using namespace pbd::game_host;
	for (const DWORD original : {DWORD{0}, DWORD{WS_EX_APPWINDOW}, DWORD{WS_EX_TOOLWINDOW},
			 DWORD{WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_TOPMOST | WS_EX_NOACTIVATE}, DWORD{0xffffffff}})
	{
		const auto style = qt_shell_ex_style(original);
		check(style & WS_EX_TOOLWINDOW);
		check(!(style & WS_EX_APPWINDOW));
		check((style & ~(WS_EX_TOOLWINDOW | WS_EX_APPWINDOW)) == (original & ~(WS_EX_TOOLWINDOW | WS_EX_APPWINDOW)));
		check(qt_shell_ex_style(style) == style);
	}
	check(surface_ex_style(false) == WS_EX_TOOLWINDOW);
	check(surface_ex_style(true) == WS_EX_APPWINDOW);
}

PBD_TEST(PbdGameHost, ObservesHiddenWindowsAndRejectsWrongRoot)
{
	using pbd::game_host::observe_window_visibility;
	check(!observe_window_visibility(nullptr, nullptr).state.verified());
	struct owned_window
	{
		HWND value;
		~owned_window()
		{
			if (value)
				DestroyWindow(value);
		}
	};
	// Neither root is ever shown or activated; a visible child style cannot override a hidden parent.
	const owned_window root{CreateWindowExW(0, L"STATIC", L"", WS_POPUP, -30000, -30000, 320, 180, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr)};
	check(root.value);
	const owned_window child{CreateWindowExW(0, L"STATIC", L"", WS_CHILD | WS_VISIBLE, 0, 0, 160, 90, root.value, nullptr, GetModuleHandleW(nullptr), nullptr)};
	check(child.value);
	const auto standalone = observe_window_visibility(root.value, root.value);
	check(standalone.state.exists);
	check(!standalone.state.visible);
	check(!standalone.state.on_screen);
	check(!standalone.state.verified());
	const auto embedded = observe_window_visibility(child.value, root.value);
	check(embedded.state.exists);
	check(!embedded.state.visible);
	check(!embedded.state.on_screen);
	check(!embedded.state.verified());
	check(!observe_window_visibility(child.value, child.value).state.exists);
	// A hidden window can intersect a monitor and still must not verify as visible.
	check(SetWindowPos(root.value, nullptr, 10, 10, 320, 180, SWP_NOACTIVATE | SWP_NOZORDER));
	const auto on_screen = observe_window_visibility(child.value, root.value);
	check(on_screen.state.on_screen);
	check(!on_screen.state.verified());
	// A child outside its parent's client area draws nothing, even when its rectangle is on screen.
	check(SetWindowPos(child.value, nullptr, 400, 0, 160, 90, SWP_NOACTIVATE | SWP_NOZORDER));
	check(!observe_window_visibility(child.value, root.value).state.on_screen);
	const auto missing = pbd::game_host::recover_window_position(nullptr);
	check(!missing.restored && !missing.positioned && !missing.foreground);
	// An owner in front never makes a hidden window count as shown.
	check(!observe_window_visibility(root.value, root.value, GetForegroundWindow()).state.verified());
}

PBD_TEST(PbdGameHost, DisplayAdaptersNameEachDisplay)
{
	// Every entry is [device|primary or nothing|left,top,right,bottom|adapter]; a PC without a display lists none.
	const auto text = pbd::game_host::display_adapters();
	if (text.empty())
		return;
	check(text.front() == '[' && text.back() == ']');
	check(text.find("|primary|") != std::string::npos);
}
#endif
