#include "pbd_test.h"

#include "Emu/Cell/pbd_match_recording.h"

using pbd::test::check;

using namespace pbd::match_recording;

static events at(lifecycle& capture, u32 boundary, phase p, u32 left = 0, u32 right = 0,
	bool result = false, bool draw = false, winner outcome = winner::none)
{
	return capture.observe({boundary, p, true, false, result, draw, left, right, outcome});
}

static void round_lifecycle_recording()
{
	{
		lifecycle capture;
		check(capture.begin(2, 1000, 400, 3, true).values[0].type == kind::clean_start);
		check(at(capture, 1000, phase::loading).count == 0);
		check(at(capture, 1010, phase::vs).values[0].boundary == 1010);
		check(at(capture, 1011, phase::vs).count == 0);
		check(at(capture, 1100, phase::character_intro).values[0].type == kind::intro);
		u32 boundary = 1500;
		u32 left = 0, right = 0;
		for (u32 round = 1; round <= 4; ++round)
		{
			const auto start = at(capture, boundary, phase::round_intro);
			check(start.count == 1 && start.values[0].type == kind::round_start && start.values[0].round == round);
			const u32 fight = boundary + round * 7; // Native transitions can have different intro lengths.
			const auto fighting = at(capture, fight, phase::fight);
			check(fighting.count == 1 && fighting.values[0].type == kind::round_fight && fighting.values[0].boundary == fight);
			check(at(capture, fight + 1, phase::fight).count == 0);
			if (round == 2)
				++right;
			else
				++left;
			const auto ended = at(capture, fight + 2, phase::round_result, left, right, true);
			check(ended.count == 1 && ended.values[0].boundary == fight + 1 && ended.values[0].type == kind::round_end);
			boundary = fight + 100;
		}
		const auto end = at(capture, boundary, phase::match_end, 3, 1, true, false, winner::left);
		check(end.count == 1 && end.values[0].type == kind::match_end && capture.complete());
		check(end.values[0].score_left == 3 && end.values[0].score_right == 1);
	}
}

PBD_TEST(PbdMatchRecording, Checks)
{
	{
		check(classify(8, 999, false).value == phase::vs);
		check(classify(17, 1, false).value == phase::character_intro);
		check(classify(17, 2, false).value == phase::character_intro);
		check(classify(17, 4, false).value == phase::round_intro);
		check(classify(17, 5, false).value == phase::fight);
		check(classify(17, 6, false).value == phase::round_result);
		check(classify(16, 0, true).value == phase::round_result);
		check(!classify(17, 39, false).verified);
	}
	round_lifecycle_recording();
	{
		lifecycle capture;
		check(capture.begin(1, 0, 0, 3, false).refused == refusal::clean_load_missing);
		capture.begin(1, 0, 0, 3, true);
		check(capture.observe({0, phase::vs, false}).refused == refusal::phase_unverified);
		capture.begin(1, 0, 0, 3, true);
		at(capture, 0, phase::vs);
		at(capture, 1, phase::character_intro);
		check(at(capture, 2, phase::fight).refused == refusal::round_order);
		check(!capture.complete());
	}
	{
		lifecycle capture;
		capture.begin(1, 0, 0, 1, true);
		check(at(capture, 0, static_cast<phase>(255)).refused == refusal::phase_unverified);
		capture.begin(1, 0, 0, 1, true);
		at(capture, 0, phase::vs);
		at(capture, 1, phase::character_intro);
		at(capture, 2, phase::round_intro);
		at(capture, 3, phase::fight);
		at(capture, 4, phase::round_result, 1, 0, true);
		check(at(capture, max_ticks, phase::match_end, 1, 0, true, false, winner::left).count == 1);
		check(capture.complete());
	}
	{
		lifecycle capture;
		capture.begin(1, 0, 0, 1, true);
		at(capture, 0, phase::vs);
		at(capture, 1, phase::character_intro);
		at(capture, 2, phase::round_intro);
		at(capture, 3, phase::fight);
		const auto ignored = capture.observe({1, phase::vs, true, true});
		check(ignored.count == 0 && capture.active());
		const auto result = at(capture, 4, phase::round_result, 1, 1, true, true);
		check(result.count == 1 && result.refused == refusal::none);
		check(at(capture, 5, phase::match_end, 1, 1, true, true, winner::draw).count == 1);
		check(capture.complete());
	}
	{
		lifecycle capture;
		capture.begin(1, 0, 0, 1, true);
		at(capture, 0, phase::vs);
		at(capture, 1, phase::character_intro);
		at(capture, 2, phase::round_intro);
		at(capture, 3, phase::fight);
		check(at(capture, 4, phase::round_result, 1, 1, true, false).refused == refusal::outcome_invalid);
		check(!capture.complete());
	}
}
