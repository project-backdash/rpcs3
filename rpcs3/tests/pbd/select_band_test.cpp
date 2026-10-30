#include "pbd_test.h"

#include "Emu/Cell/pbd_select_band.h"
#include <string_view>

using pbd::test::check;

namespace
{
	using namespace pbd::select_timer;
	using pbd::select_band::compose;
	view in_select(u32 left_state, u32 right_state)
	{
		view out;
		out.verified = true;
		out.vs_mode = true;
		out.scene = scene_select;
		out.state = {left_state, right_state};
		if (left_state == state_stage || right_state == state_stage)
		{
			out.stage_open = true;
			out.stage_owner = left_state == state_stage ? 0u : 1u;
		}
		return out;
	}
	view in_scene(u32 value)
	{
		view out;
		out.verified = true;
		out.vs_mode = true;
		out.scene = value;
		return out;
	}
} // namespace

PBD_TEST(PbdSelectBand, Checks)
{
	{
		// Before the select and outside it the band is hidden; the countdown's own state decides nothing here.
		timer t;
		check(t.begin(30, 15, 0));
		check(!compose(t.status(0, in_scene(2)), in_scene(2), 0, "Kai").visible);
		t.sample(10, in_select(3, 3), 0);
		const auto first = compose(t.status(10, in_select(3, 3)), in_select(3, 3), 0, "Kai");
		check(first.visible && !first.title.empty() && first.seconds == 30 && first.detail.find("Kai") != std::string::npos && first.detail_waiting);
		check(first.frames == 1800 && first.total == 1800 && first.hint.empty());
		// The other player's record left the character and handicap states: ready. The local player's own state never names them.
		const auto ready = compose(t.status(70, in_select(3, 10)), in_select(3, 10), 0, "Kai");
		check(ready.visible && ready.seconds == 29 && ready.detail.find("Kai") != std::string::npos && !ready.detail_waiting && ready.frames == 1740);
		check(compose(t.status(70, in_select(9, 3)), in_select(9, 3), 1, "Left").detail.find("Left") != std::string::npos);
		// An unread or non-VS view, or a scene outside the select, hides the band without touching the count.
		view unread = in_select(3, 3);
		unread.verified = false;
		check(!compose(t.status(80, unread), unread, 0, "Kai").visible);
		view other = in_select(3, 3);
		other.vs_mode = false;
		check(!compose(t.status(80, other), other, 0, "Kai").visible);
		check(!compose(t.status(80, in_scene(5)), in_scene(5), 0, "Kai").visible);
		check(!compose(t.status(80, in_select(3, 3)), in_select(3, 3), 2, "Kai").visible);
		check(compose(t.status(80, in_select(3, 3)), in_select(3, 3), 0, "").detail.find(pbd::select_band::unnamed_opponent) != std::string::npos);
		// Surrounding spaces, unprintable bytes and an over-long name never reach the band's sentences.
		using pbd::select_band::display_name;
		check(display_name("  Kai  ") == "Kai" && display_name("   ") == pbd::select_band::unnamed_opponent &&
			  display_name("\xC3\xA9") == pbd::select_band::unnamed_opponent);
		check(display_name("K\x01"
						   "ai\x7f") == "Kai" &&
			  display_name("abcdefghijklmno pqrs") == "abcdefghijklmno");
		check(compose(t.status(80, in_select(3, 3)), in_select(3, 3), 0, " ").detail.find(pbd::select_band::unnamed_opponent) != std::string::npos);
	}

	{
		// The stage: the stage owner (the Right side, role 1) picks and sees the controls; the Left side sees who picks and why.
		timer left, right;
		check(left.begin(30, 15, 0) && right.begin(30, 15, 1));
		for (auto* t : {&left, &right})
		{
			t->sample(10, in_select(3, 3), 0);
			t->sample(400, in_select(10, 13), 0);
		}
		const auto picker = compose(right.status(400, in_select(10, 13)), in_select(10, 13), 1, "Ana");
		check(picker.visible && !picker.title.empty() && picker.seconds == 15 && picker.detail.empty());
		check(picker.hint == pbd::select_band::picker_hint && picker.total == 900 && picker.frames == 900);
		const auto watcher = compose(left.status(400, in_select(10, 13)), in_select(10, 13), 0, "Kai");
		check(watcher.visible && watcher.title.find("Kai") != std::string::npos && watcher.detail == pbd::select_band::right_side_rule && watcher.hint.empty());
		// Both screens count the same seconds from the same boundary.
		check(picker.seconds == watcher.seconds && picker.frames == watcher.frames);
		const auto late = compose(right.status(1290, in_select(10, 13)), in_select(10, 13), 1, "Ana");
		check(late.seconds == 1 && late.frames == 10);
		check(compose(right.status(1400, in_select(10, 13)), in_select(10, 13), 1, "Ana").seconds == 0);
		// Once the stage owner's record leaves the stage state (the stage is taken) the band hides, and the VS load keeps it hidden.
		check(!compose(right.status(1300, in_select(10, 14)), in_select(10, 14), 1, "Ana").visible);
		check(!compose(right.status(1300, in_scene(5)), in_scene(5), 1, "Ana").visible);
		// The first battle frame finishes the selection; nothing shows after it.
		right.sample(1500, in_scene(16), 0);
		check(!compose(right.status(1501, in_select(10, 13)), in_select(10, 13), 1, "Ana").visible);
	}

	{
		// A random stage (the loser's Rematch with random stage): nobody picks, both screens say so, and the picker gets no controls.
		timer left, right;
		check(left.begin(30, 15, 0, true) && right.begin(30, 15, 1, true));
		for (auto* t : {&left, &right})
		{
			t->sample(10, in_select(3, 3), 0);
			t->sample(400, in_select(10, 13), 0);
		}
		const auto picker = compose(right.status(400, in_select(10, 13)), in_select(10, 13), 1, "Ana");
		const auto other = compose(left.status(400, in_select(10, 13)), in_select(10, 13), 0, "Kai");
		for (const auto& shown : {picker, other})
		{
			check(shown.visible && shown.title == pbd::select_band::random_stage_title && shown.detail == pbd::select_band::random_stage_detail);
			check(shown.hint.empty() && shown.seconds == 0 && shown.frames == 0 && shown.total == 0);
		}
		// Its character select reads as usual.
		timer fresh;
		check(fresh.begin(30, 15, 1, true));
		fresh.sample(10, in_select(3, 3), 0);
		const auto choosing = compose(fresh.status(10, in_select(3, 3)), in_select(3, 3), 1, "Ana");
		check(choosing.visible && !choosing.title.empty() && choosing.seconds == 30);
	}

	{
		// A stage owner other than the Right side (the timer presses for whoever the game names) shows no Right-side rule.
		timer t;
		check(t.begin(30, 15, 1));
		t.sample(10, in_select(3, 3), 0);
		t.sample(400, in_select(13, 10), 0);
		const auto other_owner = compose(t.status(400, in_select(13, 10)), in_select(13, 10), 1, "Ana");
		check(other_owner.visible && other_owner.title.find("Ana") != std::string::npos && other_owner.detail.empty());
	}

	{
		// Change character, Right side chooses: the chooser picks against the countdown, the kept player sees whether the
		// chooser is still choosing, and both screens say the stage stays, with no countdown and no controls.
		timer kept, chooser;
		check(kept.begin_repick(30, 0, 1) && chooser.begin_repick(30, 1, 1));
		for (auto* t : {&kept, &chooser})
			t->sample(10, in_select(12, 3), 0);
		const auto picking = compose(chooser.status(10, in_select(12, 3)), in_select(12, 3), 1, "Kai");
		check(picking.visible && !picking.title.empty() && picking.detail.find("Kai") != std::string::npos && !picking.detail_waiting);
		check(picking.seconds == 30 && picking.total == 1800 && picking.hint.empty());
		const auto waiting = compose(kept.status(10, in_select(12, 3)), in_select(12, 3), 0, "Ana");
		check(waiting.visible && waiting.title == pbd::select_band::kept_fighter_title && waiting.detail.find("Ana") != std::string::npos);
		check(waiting.detail_waiting && waiting.seconds == picking.seconds && waiting.frames == picking.frames);
		const auto ready = compose(kept.status(70, in_select(12, 12)), in_select(12, 12), 0, "Ana");
		check(ready.visible && ready.detail.find("Ana") != std::string::npos && !ready.detail_waiting && ready.seconds == 29);
		for (auto* t : {&kept, &chooser})
			t->sample(400, in_select(12, 13), 0);
		for (const u32 role : {0u, 1u})
		{
			const auto shown = compose((role ? chooser : kept).status(400, in_select(12, 13)), in_select(12, 13), role, "Kai");
			check(shown.visible && shown.title == pbd::select_band::same_stage_title && shown.detail == pbd::select_band::same_stage_detail);
			check(shown.hint.empty() && shown.seconds == 0 && shown.frames == 0 && shown.total == 0);
		}
		// A Left chooser reads the same way from the other side.
		timer left;
		check(left.begin_repick(30, 0, 0));
		left.sample(10, in_select(3, 12), 0);
		check(compose(left.status(10, in_select(3, 12)), in_select(3, 12), 0, "Ana").detail.find("Ana") != std::string::npos);
		// Only a real side can be the chooser.
		band bogus;
		bogus.current = phase::character;
		bogus.seconds = 5;
		bogus.chooser = 2;
		check(!compose(bogus, in_select(3, 3), 0, "Kai").visible);
	}

	{
		// Publication: an equal band does not move the revision, a changed one does, and hide() publishes a hidden band.
		const auto before = pbd::select_band::revision();
		pbd::select_band::snapshot shown;
		shown.visible = true;
		shown.title = "Choose your fighter";
		shown.seconds = 30;
		pbd::select_band::publish(7, shown);
		const auto once = pbd::select_band::revision();
		check(once == before + 1);
		pbd::select_band::publish(7, shown);
		check(pbd::select_band::revision() == once);
		check(pbd::select_band::load().visible && pbd::select_band::load().generation == 7);
		shown.seconds = 29;
		pbd::select_band::publish(7, shown);
		check(pbd::select_band::revision() == once + 1);
		pbd::select_band::hide(7);
		check(!pbd::select_band::load().visible && pbd::select_band::revision() == once + 2);
	}
}
