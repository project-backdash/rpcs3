#include "pbd_test.h"

#include "Emu/PBD/pbd_practice_stage.h"
#include "Emu/PBD/pbd_practice_menu.h"
#include "Emu/PBD/pbd_stage_rule.h"

namespace
{
	namespace stage = pbd::practice::stage;
	using pbd::test::check;

	stage::snapshot arena(std::uint32_t id)
	{
		return {1, 100, 10, 17, 44, id, 1000, {78, 77}, false, true};
	}

} // namespace

PBD_TEST(PbdPracticeStage, CheckCatalog)
{
	// The Dark Resurrection stages first, then the original Tekken 5 stages, then Random.
	constexpr std::array<std::uint32_t, 34> expected{17, 18, 19, 20, 21, 22, 23, 24, 25, 26, 27, 28, 29, 30, 31, 32,
		33, 34, 37, 0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 15};
	check(stage::row_count == 35 && stage::row_name(34) == "Random", "thirty-four stages plus Random");
	check(stage::row_name(0) == "Western Gate" && stage::row_name(19) == "Dragon's Nest" &&
			  stage::row_name(33) == "Final Stage",
		"rows carry their stage names");
	for (std::uint32_t row = 0; row < expected.size(); ++row)
	{
		check(stage::resolve(row, 0) == expected[row], "ordered native stage IDs");
		check(stage::resolve(row, 0xffffffff) == expected[row], "explicit stage ignores outside randomness");
		check(stage::valid(expected[row]) && !stage::row_name(row).empty(), "all explicit stages are named");
		check(pbd::stage_rule::loadable(expected[row]), "every row is a stage the game loads");
	}
	check(!stage::resolve(35, 0) && !stage::resolve(0xffffffff, 0), "invalid rows have no load target");
	check(!stage::valid(14) && !stage::valid(16) && !stage::valid(35) && !stage::valid(36),
		"unlisted native stage IDs are refused");
	for (std::uint32_t at = 0; at < 65536; ++at)
	{
		const auto id = stage::resolve(34, at * 65537u);
		check(id && stage::valid(*id), "outside random word resolves to a supported fixed stage");
	}
	check(stage::resolve(34, 0) == 17u && stage::resolve(34, 0xffffffff) == 15u, "random boundary words");
}

PBD_TEST(PbdPracticeStage, CheckLoadWatch)
{
	for (const auto& named : pbd::stage_names::names)
	{
		stage::load_watch watch;
		auto now = arena(named.first);
		watch.begin(1, 100, named.first, now.rows);
		check(watch.step(now) == stage::progress::waiting, "old arena cannot complete a same-stage reload");
		now.scene = 2;
		check(watch.step(now) == stage::progress::waiting, "stock teardown is observed");
		now.scene = 17;
		now.models_ready = false;
		check(watch.step(now) == stage::progress::waiting, "new models must be readable");
		now.models_ready = true;
		now.paused = true;
		check(watch.step(now) == stage::progress::waiting, "paused arrival cannot complete the load");
		now.paused = false;
		for (unsigned frame = 0; frame < 4; ++frame)
		{
			++now.boundary;
			++now.counter;
			check(watch.step(now) == (frame == 3 ? stage::progress::complete : stage::progress::waiting),
				"new arena completes only after consecutive native fight updates");
		}
		check(watch.step(now) == stage::progress::none, "completion is consumed once");
	}
	for (unsigned failure = 0; failure < 5; ++failure)
	{
		stage::load_watch watch;
		auto now = arena(31);
		watch.begin(1, 100, 31, now.rows);
		now.scene = 2;
		watch.step(now);
		now.scene = 17;
		if (failure == 0)
			now.stage = 30;
		if (failure == 1)
			now.rows[0] = 79;
		if (failure == 2)
			now.rows[1] = 78;
		if (failure == 3)
			now.mode = 4;
		if (failure == 4)
			now.boundary = 1900;
		check(watch.step(now) == (failure == 4 ? stage::progress::timed_out : stage::progress::invalid),
			"wrong stage, fighters, mode or deadline cannot complete a load");
	}
	stage::load_watch watch;
	auto now = arena(31);
	watch.begin(1, 0xfffffffbu, 31, now.rows);
	now.boundary = 3;
	check(watch.step(now) == stage::progress::waiting, "boundary rollover preserves a live request");
	now.generation = 2;
	check(watch.step(now) == stage::progress::none && !watch.active, "new emulation cancels the old load");
}
