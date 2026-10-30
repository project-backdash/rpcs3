#include "pbd_test.h"

#include "Emu/PBD/pbd_practice_presentation.h"
#include "Emu/Cell/pbd_practice_controls.h"
#include "Emu/Cell/pbd_practice_mouse.h"
#include "Emu/PBD/pbd_movement_observation.h"
#include "Emu/Cell/pbd_game_overlay.h"

#include <numeric>
#include <vector>

namespace
{
	using pbd::test::check;

	void late_select_pairing()
	{
		using namespace pbd::practice::controls;
		constexpr std::uint16_t select = 0x0100, cross = 0x0040, circle = 0x0020, back = 0x8000, start = 0x0800;
		{
			chord_window window;
			check(window.update(cross, cross, 100) == cross, "Cross alone is an ordinary press");
			check(window.update(select | cross, select, 101) == (select | cross), "SELECT one frame after Cross is SELECT+Cross");
		}
		{
			chord_window window;
			window.update(cross, cross, 100);
			window.update(cross, 0, 101);
			check(window.update(select | cross, select, 102) == (select | cross), "SELECT two frames after Cross is SELECT+Cross");
		}
		{
			chord_window window;
			window.update(cross, cross, 100);
			window.update(cross, 0, 101);
			window.update(cross, 0, 102);
			check(window.update(select | cross, select, 103) == select, "a Cross held longer before SELECT is not a chord");
		}
		{
			chord_window window;
			window.update(cross, cross, 100);
			window.update(0, 0, 101);
			check(window.update(select, select, 102) == select, "a released Cross is not paired with a later SELECT");
			check(window.update(select | cross, cross, 103) == cross, "SELECT first: the Cross edge itself forms the chord");
		}
		{
			chord_window window;
			window.update(cross, cross, 100);
			check(window.update(select | cross, select, 101) == (select | cross), "the late SELECT pairs once");
			window.update(cross, 0, 102);
			check(window.update(select | cross, select, 102) == select, "SELECT pressed again does not pair the spent Cross a second time");
		}
		{
			chord_window window;
			window.update(select | cross, cross, 100);
			window.update(cross, 0, 101);
			check(window.update(select | cross, select, 102) == select, "a Cross already pressed under SELECT is not paired again");
		}
		{
			chord_window window;
			window.update(back, back, 100);
			check(window.update(select | back, select, 101) == select, "a direction just before SELECT is never paired: walls need a fresh press");
		}
	}

	void record_stop_tail_and_trim()
	{
		using namespace pbd::practice::controls;
		constexpr std::uint16_t select = 0x0100, cross = 0x0040, circle = 0x0020, back = 0x8000, start = 0x0800;
		{
			// record_stop.update(pad, pressed, boundary, recorded, tail)
			std::size_t tail = 99;
			record_stop stop;
			check(!stop.update(circle, circle, 100, 10, tail) && tail == 0, "Circle alone is part of the take");
			check(!stop.update(circle, 0, 101, 11, tail), "a held Circle does not stop the take");
			check(stop.update(select | circle, select, 102, 12, tail) && tail == 2,
				"SELECT two frames after Circle is the stop, and the two frames Circle reached are its tail");
		}
		{
			std::size_t tail = 99;
			record_stop stop;
			stop.update(circle, circle, 100, 40, tail);
			stop.update(circle, 0, 101, 41, tail);
			stop.update(circle, 0, 102, 42, tail);
			check(stop.update(select | circle, select, 103, 43, tail) && tail == 0,
				"SELECT with a Circle held longer still stops the take, and that Circle stays in it");
		}
		{
			std::size_t tail = 99;
			record_stop stop;
			check(!stop.update(select, select, 100, 5, tail), "SELECT alone does not stop the take");
			check(stop.update(select | circle, circle, 101, 6, tail) && tail == 0, "Circle under SELECT stops it with no tail");
			check(!stop.update(select | circle, 0, 102, 6, tail), "a chord still held is not pressed again");
			check(!stop.update(select, 0, 103, 6, tail) && stop.update(select | circle, circle, 104, 6, tail), "Circle tapped again under SELECT is a new stop");
		}
		{
			std::size_t tail = 99;
			record_stop stop;
			check(stop.update(select | circle, select | circle, 100, 7, tail) && tail == 0, "both pressed together stop the take");
		}
		{
			std::size_t tail = 99;
			record_stop stop;
			stop.update(circle, circle, 100, 3, tail);
			stop.update(0, 0, 101, 4, tail);
			check(!stop.update(select, select, 102, 4, tail), "a released Circle is not paired with a later SELECT");
		}
		{
			const std::array<std::uint16_t, 6> take{0, 0x80, 0x80, 0, 0x20, 0x20};
			const auto body = take_body(take, 2);
			check(body.size() == 4 && body.data() == take.data(), "the stop chord's tail is left out of the body");
			check(take_body(take, 0).size() == 6 && take_body(take, 99).empty(), "no tail keeps the take; an oversized tail stays inside it");
			check(trailing_neutral_frames(body) == 1 && playback_frames(body, 1, 1, true).size() == 2 && take.size() == 6,
				"the neutral ends are measured inside the tail, and the take keeps every frame");
		}
	}

	void resume_acknowledgement()
	{
		using namespace pbd::practice::controls;
		using outcome = resume_watch::outcome;
		{
			resume_watch watch;
			check(watch.update(5, true, true, true) == outcome::none && !watch.in_flight(), "nothing sent, nothing watched");
			watch.sent(100);
			check(watch.update(101, true, true, true) == outcome::waiting && watch.in_flight(), "DR still paused just after the press");
			check(watch.update(102, true, false, true) == outcome::confirmed && !watch.in_flight() && watch.presses == 0,
				"the game running answers the press");
		}
		{
			resume_watch watch;
			watch.sent(100);
			check(watch.update(100 + resume_retry_frames - 1, true, true, true) == outcome::waiting, "not lost before resume_retry_frames");
			check(watch.update(100 + resume_retry_frames, true, true, true) == outcome::resend && !watch.in_flight() && watch.presses == 1,
				"a press DR ignored is sent again while the resume is still wanted");
			for (std::uint32_t press = 2; press <= resume_presses; ++press)
			{
				watch.sent(200 * press);
				const auto result = watch.update(200 * press + resume_retry_frames, true, true, true);
				check(press < resume_presses ? result == outcome::resend : result == outcome::given_up, "resume_presses at most");
			}
			check(!watch.in_flight() && watch.presses == 0, "a given-up resume leaves nothing behind");
		}
		{
			resume_watch watch;
			watch.sent(100);
			check(watch.update(100 + resume_retry_frames, true, true, false) == outcome::given_up,
				"a new START or the menu reopening gives the resume up instead of repeating it");
			watch.sent(100);
			check(watch.update(100 + resume_retry_frames, false, true, true) == outcome::given_up, "an unreadable pause repeats nothing");
			watch.sent(100);
			check(watch.update(90, true, true, true) == outcome::given_up, "a rewind that took the press away gives it up");
		}
	}
} // namespace

PBD_TEST(PbdPractice, HistoryChecks)
{
	pbd::practice::history_tracker track;
	using pads = std::array<std::uint16_t, 2>;
	check(track.sample(100, pads{0x2080, 0x0040}), "first boundary must be sampled");
	check(track.inputs()[0].rows[0].mask == 0x2080 && track.inputs()[1].rows[0].mask == 0x0040, "ports must remain independent");
	check(!track.sample(100, pads{0x2010, 0}) && !track.sample(99, pads{0x2010, 0}), "duplicate and re-executed boundaries must be ignored");
	check(track.inputs()[0].count == 1 && track.inputs()[0].rows[0].frames == 1, "repeated boundary must not add history or held frames");
	check(!track.sample(101, pads{0x2010, 0}, false), "pause poll must not be sampled");
	check(track.sample(101, pads{0x2080, 0x0040}) && track.inputs()[0].rows[0].frames == 2, "resume at paused boundary must count exactly once");
	check(track.sample(102, pads{0x2010, 0}), "input edge must produce a new row");
	check(track.inputs()[0].count == 2 && track.inputs()[0].rows[0].frames == 1 &&
			  track.inputs()[0].rows[1].mask == 0x2080 && track.inputs()[0].rows[1].frames == 2,
		"newest input must retain previous held duration");
	check(track.inputs()[1].rows[0].mask == 0 && track.inputs()[1].rows[1].frames == 2, "neutral periods must be retained");
	check(track.sample(1000, pads{0x2010, 0}) && track.inputs()[0].rows[0].frames == 2, "unsampled boundary gaps must not invent held frames");
	track.reset();
	check(track.inputs()[0].count == 0 && track.inputs()[1].count == 0, "restore must clear both histories");
	check(track.sample(10, pads{0xffff, 0x0f00}) && track.inputs()[0].rows[0].mask == 0xf0ff &&
			  track.inputs()[1].rows[0].mask == 0,
		"restore must accept earlier boundary and exclude system gestures");
	track.reset();
	for (std::uint64_t boundary = 0; boundary < 20; ++boundary)
	{
		const auto mask = static_cast<std::uint16_t>((1 + boundary % 15) << 4);
		track.sample(boundary, pads{mask, 0});
	}
	static_assert(pbd::practice::history_size == 16, "the Practice HUD shows sixteen history rows");
	check(track.inputs()[0].count == pbd::practice::history_size && track.inputs()[1].count == 1, "bounded history must preserve each port's own edges");
	for (std::size_t row = 0; row < pbd::practice::history_size; ++row)
	{
		const auto mask = static_cast<std::uint16_t>((1 + (19 - row) % 15) << 4);
		check(track.inputs()[0].rows[row].mask == mask && track.inputs()[0].rows[row].frames == 1, "bounded history must retain newest sixteen changes in order");
	}
	check(track.inputs()[1].rows[0].frames == 20, "unchanged other port must retain held frames while first port changes");
	pbd::practice::history_tracker wide;
	check(wide.sample(0, std::array<std::uint32_t, 2>{0x12080, 0x0040}) && wide.inputs()[0].rows[0].mask == 0x2080, "wide masks must retain only PS3 gameplay bits");
	check(pbd::practice::command_text(0x6000 | 0x0090) == "D+R+1+2", "diagonal and simultaneous attacks must remain explicit");
	check(pbd::practice::command_text(0x0f00) == "N", "system-only mask must display neutral");
}

PBD_TEST(PbdPractice, ScreenHistoryChecks)
{
	using pbd::practice::command_text;
	using pads = std::array<std::uint16_t, 2>;
	constexpr std::uint16_t L = 0x8000, R = 0x2000, D = 0x4000;

	// Arrows are the screen directions as pushed: forward is Right for P1 on the left and Left for P2 on the right.
	pbd::practice::history_tracker track;
	check(track.sample(1, pads{R, L}) && command_text(track.inputs()[0].rows[0].mask) == "R" && command_text(track.inputs()[1].rows[0].mask) == "L", "forward must show the direction pushed on each side");

	// Holding a direction through a cross-up stays one row: the stick did not move.
	check(track.sample(2, pads{R, L}) && track.inputs()[0].count == 1 && track.inputs()[0].rows[0].frames == 2,
		"a held direction must stay one row");

	// An EWGF on the 2P side: screen Left, neutral, Down, Down+Left+2 reads as pushed.
	track.reset();
	check(track.sample(30, pads{L, 0}) && track.sample(31, pads{0, 0}) && track.sample(32, pads{D, 0}) &&
			  track.sample(33, pads{static_cast<std::uint16_t>(D | L | 0x0010), 0}),
		"an EWGF's frames must be sampled");
	const auto& p1 = track.inputs()[0];
	check(p1.count == 4 && command_text(p1.rows[3].mask) == "L" && command_text(p1.rows[2].mask) == "N" &&
			  command_text(p1.rows[1].mask) == "D" && command_text(p1.rows[0].mask) == "D+L+2",
		"an EWGF on the 2P side must read Left, neutral, Down, Down+Left+2");
}

PBD_TEST(PbdPractice, MappingChecks)
{
	using namespace pbd::practice::controls;
	button_map map = defaults;
	check(parse_mapping("128,16,64,32,144,96,160,80", map), "owner mapping must accept face attacks and shoulder pairs");
	check(map_attacks(0x0004, map) == 0x0090 && map_attacks(0x0008, map) == 0x0060, "shoulders must emit configured attack pairs");
	check(map_attacks(0x0001, map) == 0x00a0 && map_attacks(0x0002, map) == 0x0050, "triggers must emit configured attack pairs");
	check(map_attacks(0xa900 | 0x000c, map) == 0xa9f0, "remap must preserve directions and system buttons while combining attacks");
	check(map_attacks(0xff00, map) == 0xff00 && map_attacks(0x000f, defaults) == 0, "unmapped shoulders must be off and system bits preserved");
	const auto original = map;
	for (const auto invalid : {"128,16,64,32,0,0,0", "128,16,64,32,0,0,0,0,0", "128,16,64,32,0,0,0,", "128,16,64,32,-1,0,0,0",
			 "128,16,64,32,256,0,0,0", "128,16,64,32,4,0,0,0", "128,16,64,32, 0,0,0,0"})
	{
		check(!parse_mapping(invalid, map), "malformed mapping or unsupported macro must be refused");
		check(map == original, "refused mapping must leave last accepted mapping intact");
	}
	for (const auto mask : {208, 176, 224, 112, 240})
	{
		check(parse_mapping("128,16,64,32," + std::to_string(mask) + ",0,0,0", map), "triple and four-button macros must be accepted");
		check(map_attacks(0x0004, map) == mask, "the physical shoulder must emit every attack in its macro");
	}
	check(parse_mapping("16,128,32,64,0,0,0,0", map) && map_attacks(0x0080 | 0x0040, map) == 0x0030, "owner face-button swaps must apply too");
	check(parse_mapping("0,0,0,0,0,0,0,0", map) && map_attacks(0xffff, map) == 0xff00, "off mapping must not suppress system buttons");
	// The stick clicks follow the eight; eight masks alone leave both off, as the launcher sends them while neither has an action.
	check(parse_mapping("128,16,64,32,0,0,0,0,144,0", map) && map_attacks(0x0200 | 0x1000, map) == (0x1000 | 0x0090) &&
			  map_attacks(0x0400, map) == 0x0400 && r3_remembers(map),
		"L3 given 1+2 is 1+2 in the fight; R3 with no action stays the click, Remember");
	check(parse_mapping("128,16,64,32,0,0,0,0,0,32", map) && map_attacks(0x0400 | 0x0040, map) == 0x0060 && !r3_remembers(map),
		"R3 given 4 is 4 in the fight and no longer Remember");
	check(parse_mapping("128,16,64,32,0,0,0,0", map) && map[8] == 0 && map[9] == 0 && r3_remembers(map), "eight masks: both clicks off");
	for (const auto invalid : {"128,16,64,32,0,0,0,0,0", "128,16,64,32,0,0,0,0,0,0,0", "128,16,64,32,0,0,0,0,0,4"})
		check(!parse_mapping(invalid, map), "nine or eleven masks, or a click with no attack's bits, are refused");
}

PBD_TEST(PbdPractice, SelectModifierChecks)
{
	using namespace pbd::practice::controls;
	constexpr std::uint16_t select = 0x0100, cross = 0x0040, circle = 0x0020, back = 0x8000, start = 0x0800;
	{
		select_modifier modifier;
		check(modifier.filter(back | cross, false, back | cross) == (back | cross), "without SELECT the word passes unchanged and undelayed");
		check(modifier.filter(back | cross, true, select | back | cross) == 0, "SELECT withholds attacks and directions");
		check(modifier.filter(start | back | cross, true, select | start | back | cross) == 0, "SELECT withholds START from the game");
		check(modifier.filter(back | cross, false, back | cross) == back, "a direction returns when SELECT lifts; a chord button held across it does not");
		check(modifier.filter(back | cross, false, back | cross) == back, "the chord button stays withheld while it is still held");
		check(modifier.filter(back, false, back) == back && modifier.filter(back | cross, false, back | cross) == (back | cross),
			"once released, the button presses normally again");
	}
	{
		select_modifier modifier;
		modifier.filter(cross, true, select | cross);
		check(modifier.filter(cross, false, 0) == 0, "a stale device read must not deliver a button the host already released");
		check(modifier.filter(0, false, 0) == 0 && modifier.filter(cross, false, cross) == cross, "both readings released: the next press is real");
	}
	{
		select_modifier modifier;
		check(modifier.filter(circle, true, circle) == 0, "SELECT seen only in the device read still withholds");
		check(modifier.filter(circle, false, cross) == 0, "a button held in another reading is not let through");
	}
	{
		// SELECT+R3 restarts at the remembered position: an action on R3 must not follow when SELECT lifts first.
		constexpr std::uint16_t r3 = 0x0400;
		select_modifier modifier;
		check(modifier.filter(r3, true, select | r3) == 0 && modifier.filter(r3, false, r3) == 0, "R3 held across SELECT stays withheld");
		check(modifier.filter(0, false, 0) == 0 && modifier.filter(r3, false, r3) == r3, "pressed again, it is a press");
	}
	{
		select_modifier modifier;
		modifier.filter(cross, true, select | cross);
		check(modifier.filter(0, true, select) == 0, "the chord's Cross is released while SELECT stays down");
		check(modifier.filter(cross, false, cross) == cross, "a Cross pressed again on the boundary SELECT lifts is a new press");
	}
	{
		select_modifier modifier;
		modifier.filter(cross, true, select | cross);
		check(modifier.filter(cross, true, select) == 0 && modifier.filter(cross, false, 0) == 0,
			"a Cross the device read still shows is the same press, not a new one");
	}
	late_select_pairing();
	record_stop_tail_and_trim();
	check(restart_retryable("native_boundary_unavailable") && restart_retryable("native_statistics_reset_failed") &&
			  restart_retryable("native_pause_owned") && restart_retryable("pose_or_demo_active"),
		"refusals that can clear by waiting are retried");
	check(!restart_retryable("stale_scope") && !restart_retryable("stale_placement") && !restart_retryable("native_body_mismatch") &&
			  !restart_retryable("native_reset_exception"),
		"refusals that cannot clear are not retried");
	check(restart_retry_frames == 600 && late_select_frames == 2, "retry and pairing windows stay as specified");
	check(restart_retry_frames > 180, "a restart pressed early in a long throw outlasts it instead of giving up inside it");
	// pause_press_lost(injected, injected_at, boundary, readable, paused)
	check(!pause_press_lost(true, 100, 101, true, false) && !pause_press_lost(true, 100, 103, true, false),
		"DR shows the pause one boundary after the press; the press is not repeated before pause_retry_frames");
	check(pause_press_lost(true, 100, 104, true, false), "a press DR has not answered in pause_retry_frames is armed again");
	check(!pause_press_lost(true, 100, 140, true, true) && !pause_press_lost(true, ~0u, 140, true, false),
		"a shown pause, and a press DR already answered, are never repeated");
	check(!pause_press_lost(false, 100, 140, true, false) && !pause_press_lost(true, 100, 140, false, false),
		"nothing injected, or an unreadable pause state, arms nothing");
	check(pause_press_lost(true, 100, 90, true, false), "a rewind that took the press's frame away arms it again");
}

PBD_TEST(PbdPractice, MenuStartChecks)
{
	using pbd::practice::controls::menu_start;
	// update(start_down, start_pressed, select_down, restart_select, idle, ready)
	{
		menu_start boundary_state;
		check(boundary_state.update(true, true, false, false, true, true), "a plain START opens the menu at its own press");
		check(!boundary_state.update(true, false, false, false, true, true), "a START still held after acting does not act again");
	}
	{
		menu_start boundary_state;
		check(!boundary_state.update(true, true, false, false, true, false), "a START during a pending Restart waits");
		check(!boundary_state.update(true, false, false, false, true, false), "it keeps waiting while the Restart is pending");
		check(boundary_state.update(true, false, false, false, true, true), "it acts at the first boundary after the Restart while still held");
		check(!boundary_state.update(true, false, false, false, true, true), "and only once");
	}
	{
		menu_start boundary_state;
		boundary_state.update(true, true, false, false, true, false);
		check(!boundary_state.update(false, false, false, false, true, false) && !boundary_state.update(false, false, false, false, true, true),
			"a START released before the Restart finished is dropped");
	}
	{
		menu_start boundary_state;
		check(boundary_state.update(true, true, true, true, true, true), "START with SELECT still held from a restart chord opens the menu");
		menu_start waiting;
		check(!waiting.update(true, true, true, true, true, false) && waiting.update(true, false, true, true, true, true),
			"START under the restart chord's SELECT, pressed while that Restart is pending, opens the menu after it");
	}
	{
		menu_start boundary_state;
		check(!boundary_state.update(true, true, true, false, true, true), "SELECT+START from neutral is the Exit hold, never the menu");
		check(!boundary_state.update(true, false, false, false, true, true), "lifting SELECT first must not turn the Exit hold into a menu press");
		check(!boundary_state.update(true, false, false, false, true, true), "nor later while START stays held");
		check(!boundary_state.update(false, false, false, false, true, true) && boundary_state.update(true, true, false, false, true, true),
			"a fresh START after releasing opens the menu as usual");
	}
	{
		menu_start boundary_state;
		check(!boundary_state.update(true, true, false, false, false, true), "a START the open menu owns (closing it) is not kept");
		check(!boundary_state.update(true, false, false, false, true, true), "so the closing START cannot reopen the menu");
	}
	{
		menu_start boundary_state;
		boundary_state.update(true, true, false, false, true, false);
		check(!boundary_state.update(true, false, false, false, false, true) && !boundary_state.update(true, false, false, false, true, true),
			"a kept START is dropped once something else owns START");
	}
}

PBD_TEST(PbdPractice, StartGateChecks)
{
	using namespace pbd::practice::controls;
	// DR's pause as read: a set flag on a boundary whose frame ran is stale.
	check(frame_ran(true, 10, true, 11) && !frame_ran(true, 10, true, 10), "the counter moving means the frame ran");
	check(!frame_ran(false, 10, true, 11) && !frame_ran(true, 10, false, 11), "an unreadable counter proves nothing");
	check(native_paused(true, false) && !native_paused(true, true) && !native_paused(false, false),
		"the flag counts only while the game stands still");

	resume_acknowledgement();
	// Who owns DR's pause: pause_scene{fight, flag, paused, dr_menu, menu, requested, resuming}.
	const pause_scene stray{true, true, true, true, false, false, false};
	check(pause_unowned(stray), "DR's pause menu with nothing of Practice's around it is unowned");
	check(!pause_unowned({true, true, true, true, true, false, false}) && !pause_unowned({true, true, true, true, false, true, false}) &&
			  !pause_unowned({true, true, true, true, false, false, true}),
		"the menu, a pause asked for and a resume in flight own it");
	check(!pause_unowned({false, true, true, true, false, false, false}), "outside the fight, or on DR's own screens, it is DR's");
	check(!pause_unowned({true, true, false, true, false, false, false}), "a stale flag over a running game is no pause");
	check(!pause_unowned({true, true, true, false, false, false, false}), "a hold of DR's without its pause menu is not taken over");
	check(claims_paint(stray, false) && claims_input(stray), "an unowned pause is neither painted nor driven while it waits");
	check(claims_paint({true, true, false, false, false, false, false}, false), "the flag alone already withholds DR's paint");
	check(!claims_input({true, true, false, false, false, false, false}), "but not the controller of a running game");
	check(!claims_paint({}, false) && !claims_input({}), "ordinary fighting claims nothing");
	check(claims_input({false, false, false, false, false, false, true}) && claims_paint({false, false, false, false, false, false, true}, false),
		"a resume in flight keeps both");

	{
		pause_takeover takeover;
		check(!takeover.update(true) && takeover.update(true), "taken over on its second boundary in a row");
		check(!takeover.update(true) && !takeover.update(false) && !takeover.update(true), "a break in the run starts it again");
	}

	check(fight_word(0x0800 | 0x0040 | 0x1000, true) == (0x0040 | 0x1000), "the fight never passes a controller's START");
	check(fight_word(0x0800, false) == 0x0800, "DR's own screens keep it");
}

PBD_TEST(PbdPractice, TrimChecks)
{
	using namespace pbd::practice::controls;
	const std::array<std::uint16_t, 9> original{0, 0, 0x4000, 0x4000, 0, 0x80, 0, 0, 0};
	const auto leading = leading_neutral_frames(original);
	const auto trailing = trailing_neutral_frames(original);
	const auto trimmed = playback_frames(original, leading, trailing, true);
	check(leading == 2 && trailing == 3 && trimmed.size() == 4, "Trim Neutral removes the contiguous neutral frames at both ends");
	check(trimmed[0] == 0x4000 && trimmed[1] == 0x4000 && trimmed[2] == 0 && trimmed[3] == 0x80,
		"crouch setup and the neutral between two inputs must survive; the take ends on its last input");
	check(trimmed.data() == original.data() + 2, "the trimmed range is a view of the kept take");
	check(original[0] == 0 && original[8] == 0 && original.size() == 9, "trim must retain the full original take");
	const auto full = playback_frames(original, leading, trailing, false);
	check(full.data() == original.data() && full.size() == original.size(), "trim off must restore the identical original");
	check(playback_frames(original, leading, 0, true).size() == 7, "a take kept with no trailing count (the custom movement) keeps its trailing neutral");
	const std::array<std::uint16_t, 3> attack{0x80, 0, 0x10};
	check(leading_neutral_frames(attack) == 0 && trailing_neutral_frames(attack) == 0 &&
			  playback_frames(attack, 0, 0, true).data() == attack.data() && playback_frames(attack, 0, 0, true).size() == 3,
		"a take that starts and ends on an input plays whole");
	const std::array<std::uint16_t, 4> setup{0, 0x8000, 0x6000, 0x2080};
	check(leading_neutral_frames(setup) == 1 && playback_frames(setup, 1, 0, true)[0] == 0x8000, "charge and diagonal stance setup must remain");
	const std::array<std::uint16_t, 1800> neutral{};
	check(leading_neutral_frames(neutral) == 1800 && trailing_neutral_frames(neutral) == 1800 &&
			  playback_frames(neutral, 1800, 1800, true).empty(),
		"an all-neutral take is retained but has no trimmed playback");
	check(playback_frames(neutral, 1800, 1800, false).size() == 1800 && !has_input(playback_frames(neutral, 1800, 1800, false)),
		"an all-neutral original kept with trim off is still never an action");
	check(has_input(attack) && has_input(trimmed) && !has_input(std::span<const std::uint16_t>{}), "an action needs at least one input");
	check(playback_frames(attack, 1800, 0, true).empty() && playback_frames(attack, 0, 1800, true).empty() &&
			  playback_frames(attack, 2, 0, true).size() == 1 && playback_frames(attack, 2, 0, true)[0] == 0x10 &&
			  playback_frames(attack, 2, 2, true).empty(),
		"stale or oversized trim counts must stay inside the take");
	check(leading_neutral_frames(std::span<const std::uint16_t>{}) == 0 && trailing_neutral_frames(std::span<const std::uint16_t>{}) == 0 &&
			  playback_frames(std::span<const std::uint16_t>{}, 0, 0, true).empty(),
		"empty take must remain safe");
}

PBD_TEST(PbdPractice, PercentageChecks)
{
	using weights = std::array<unsigned, 8>;
	for (const auto& input : {weights{1, 1, 1, 1, 1, 1, 1, 1}, weights{1, 2, 3, 4, 5, 6, 7, 8}, weights{0, 1, 0, 10, 0, 3, 0, 2}, weights{10, 0, 0, 0, 0, 0, 0, 0}})
	{
		const auto result = pbd::practice::controls::percentages(input);
		check(std::accumulate(result.begin(), result.end(), 0u) == 100, "enabled slot percentages must sum to exactly 100");
		const auto total = std::accumulate(input.begin(), input.end(), 0u);
		for (std::size_t slot = 0; slot < input.size(); ++slot)
		{
			if (!input[slot])
				check(result[slot] == 0, "disabled or empty slot must have zero displayed weight");
			else
				check(result[slot] == input[slot] * 100 / total || result[slot] == input[slot] * 100 / total + 1, "rounding must stay within one percentage point");
		}
	}
	check(pbd::practice::controls::percentages(weights{}) == weights{}, "no enabled slots must display all zero percentages");
	check(pbd::practice::controls::percentages(weights{1, 1, 1, 0, 0, 0, 0, 0}) == weights{34, 33, 33, 0, 0, 0, 0, 0}, "equal fractional weights must round predictably without activating disabled slots");
}

PBD_TEST(PbdPractice, AvailabilityChecks)
{
	using weights = std::array<unsigned, 8>;
	using pbd::practice::controls::playback_available;
	for (unsigned mode = 0; mode < 3; ++mode)
		check(!playback_available(weights{}, mode, 0), "empty or all-disabled recordings must refuse every playback mode");
	const weights enabled{0, 0, 4, 0, 0, 0, 0, 1};
	check(!playback_available(enabled, 0, 0), "one-slot mode must refuse an empty selected slot even if another recording exists");
	check(playback_available(enabled, 0, 2) && playback_available(enabled, 0, 7), "one-slot mode must admit a populated enabled selected slot");
	check(!playback_available(enabled, 0, 8), "one-slot mode must reject an out-of-range selection");
	check(playback_available(enabled, 1, 0) && playback_available(enabled, 2, 0), "cycle and weighted random must admit any populated enabled recording");
	check(!playback_available(enabled, 3, 2), "unknown playback modes must be refused");
	weights disabled = enabled;
	disabled[2] = disabled[7] = 0;
	check(!playback_available(disabled, 1, 2) && !playback_available(disabled, 2, 7), "disabling all populated slots must immediately remove playback availability");
}
