#include "pbd_test.h"
#include "Emu/PBD/pbd_practice_precision.h"

namespace
{
	namespace precision = pbd::practice::precision;
	namespace movement = pbd::practice::movement;
	using pbd::test::check;
	constexpr auto f = movement::forward, d = movement::down;
	constexpr std::uint16_t df = f | d;
	const precision::performed idle{};

	std::optional<precision::result> motion(precision::tracker& tracker, std::uint32_t row, bool shortcut = false,
		std::uint16_t button = precision::two, std::uint32_t held_forward = 1, const char* name = "performed_attack")
	{
		for (std::uint32_t frame = 0; frame < held_forward; ++frame)
			tracker.frame(f, row, idle);
		tracker.frame(0, row, idle);
		if (!shortcut)
			tracker.frame(d, row, idle);
		return tracker.frame(df | button, row, {0x21000000, 1, 11, name});
	}
} // namespace

PBD_TEST(PbdPracticePrecision, FullMotionAndKazuyaShortcut)
{
	for (const auto row : {0x22u, 0x23u, 0x24u, 0x25u, 0x44u, 0x45u})
	{
		precision::tracker tracker;
		const auto result = motion(tracker, row);
		check(result && result->total == 14 && result->optimal == 14);
		check(result->motion.count == 4 && result->motion.inputs[3].word == (df | precision::two));
		tracker.reset();
		const auto shortcut = motion(tracker, row, true);
		check(shortcut.has_value() == (row == 0x24 || row == 0x25));
		if (shortcut)
			check(shortcut->total == 13 && shortcut->optimal == 13);
	}
}

PBD_TEST(PbdPracticePrecision, TimingAndPerformedMoveStayIndependent)
{
	precision::tracker tracker;
	const auto result = motion(tracker, 0x24, false, precision::two, 4, "actual_non_electric");
	check(result && result->total == 17 && result->optimal == 14);
	check(result->move.allocation == 0x21000000 && result->move.cursor == 1);
	check(result->move.name == "actual_non_electric" && result->move.startup == 11);
	check(result->motion.count == 4);
	check(result->motion.inputs[0].word == f && result->motion.inputs[0].frames == 4);
	check(result->motion.inputs[1].word == 0 && result->motion.inputs[1].frames == 1);
	check(result->motion.inputs[2].word == d && result->motion.inputs[2].frames == 1);
	check(result->motion.inputs[3].word == (df | precision::two) && result->motion.inputs[3].frames == 1);
	check(precision::text(*result).find(result->move.name) != std::string::npos);
	tracker.reset();
	const auto kick = motion(tracker, 0x24, false, precision::four);
	check(kick && kick->motion.button == precision::four && precision::text(*kick).find("d/f+4 1f") != std::string::npos);
}

PBD_TEST(PbdPracticePrecision, DelayedCommitIncludesBufferWait)
{
	precision::tracker tracker;
	tracker.frame(f, 0x24, idle);
	tracker.frame(0, 0x24, idle);
	tracker.frame(d, 0x24, idle);
	check(!tracker.frame(df | precision::two, 0x24, idle));
	tracker.frame(0, 0x24, idle);
	const auto result = tracker.frame(0, 0x24, {0x21000000, 1, 11, "buffered_attack"});
	check(result && result->total == 16 && result->motion.inputs[3].frames == 1);
	check(!tracker.frame(0, 0x24, {0x21000000, 2, 11, "buffered_attack"}));
}

PBD_TEST(PbdPracticePrecision, HeldAttacksKeepTheirNotation)
{
	for (const std::uint16_t held : {0x0080, 0x0040, 0x00c0})
	{
		for (const auto button : {precision::two, precision::four})
		{
			precision::tracker tracker;
			tracker.frame(f | held, 0x24, idle);
			tracker.frame(f | held, 0x24, idle);
			tracker.frame(held, 0x24, idle);
			tracker.frame(d | held, 0x24, idle);
			const auto result = tracker.frame(df | held | button, 0x24, {0x21000000, 1, 11, "held_attack"});
			check(result && result->total == 15 && result->optimal == 14);
			check(result->motion.button == button && result->motion.count == 4);
			check(result->motion.inputs[0].word == (f | held) && result->motion.inputs[0].frames == 2);
			check(result->motion.inputs[1].word == held && result->motion.inputs[1].frames == 1);
			check(result->motion.inputs[2].word == (d | held) && result->motion.inputs[2].frames == 1);
			check(result->motion.inputs[3].word == (df | held | button) && result->motion.inputs[3].frames == 1);
			const std::string held_text = held == 0x0080 ? "1" : held == 0x0040 ? "3" :
			                                                                      "1+3";
			const std::string chord = button == precision::two ?
			                              (held == 0x0080 ? "1+2" : held == 0x0040 ? "2+3" :
																					 "1+2+3") :
			                              (held == 0x0080 ? "1+4" : held == 0x0040 ? "3+4" :
																					 "1+3+4");
			const auto text = precision::text(*result);
			check(text.find("f+" + held_text + " 2f") != std::string::npos);
			check(text.find("n+" + held_text + " 1f") != std::string::npos);
			check(text.find("d+" + held_text + " 1f") != std::string::npos);
			check(text.find("d/f+" + chord + " 1f") != std::string::npos);
		}
	}
}

PBD_TEST(PbdPracticePrecision, FailedMotionCannotClaimAnAttack)
{
	precision::tracker tracker;
	tracker.frame(f, 0x24, idle);
	tracker.frame(0, 0x24, idle);
	tracker.frame(d, 0x24, idle);
	tracker.frame(df | precision::two, 0x24, idle);
	for (unsigned frame = 0; frame < 9; ++frame)
		tracker.frame(0, 0x24, idle);
	check(!tracker.frame(0, 0x24, {0x21000000, 1, 11, "unrelated_attack"}));
	tracker.reset();
	check(!motion(tracker, 26));
	tracker.reset();
	check(!motion(tracker, 0x24, false, precision::two | precision::four));
	tracker.reset();
	check(!motion(tracker, 0x24, false, precision::two, 17));
	tracker.reset();
	check(!motion(tracker, 0x24, false, precision::two, 1, ""));
}

PBD_TEST(PbdPracticePrecision, EarlyAndLateButtonsShowWhatActuallyCameOut)
{
	precision::tracker tracker;
	tracker.frame(f, 0x22, idle);
	tracker.frame(0, 0x22, idle);
	const auto early = tracker.frame(d | precision::two, 0x22, {0x21000000, 1, 10, "crouching_punch"});
	check(early && !early->motion.shortcut && early->total == 12 && early->optimal == 14);
	check(precision::text(*early).find("d+2 1f") != std::string::npos);
	tracker.reset();
	tracker.frame(f, 0x24, idle);
	tracker.frame(0, 0x24, idle);
	tracker.frame(d, 0x24, idle);
	tracker.frame(df, 0x24, idle);
	tracker.frame(df, 0x24, idle);
	const auto late = tracker.frame(df | precision::two, 0x24, {0x21000000, 1, 11, "late_motion_attack"});
	check(late && late->total == 16 && late->optimal == 14);
	check(precision::text(*late).find("d/f 2f, d/f+2 1f") != std::string::npos);
}

PBD_TEST(PbdPracticePrecision, ResetAndCharacterChangeClearMotion)
{
	precision::tracker tracker;
	tracker.frame(f, 0x24, idle);
	tracker.frame(0, 0x24, idle);
	tracker.frame(d, 0x24, idle);
	check(tracker.frame(df | precision::two, 0x24, {0x21000000, 1, 11, "attack"}));
	tracker.reset();
	check(!tracker.frame(df | precision::two, 0x24, {0x21000000, 1, 11, "attack"}));
	tracker.frame(f, 0x24, idle);
	tracker.frame(0, 0x24, idle);
	tracker.frame(d, 0x24, idle);
	check(!tracker.frame(df | precision::two, 0x22, {0x21000000, 1, 11, "other_character"}));
}

PBD_TEST(PbdPracticePrecision, MoveNameReaderIsBounded)
{
	constexpr std::uint32_t base = 0x21000000;
	const auto byte = [](std::uint32_t address) -> std::uint32_t
	{
		constexpr std::string_view name = "actual_move";
		return address - base < name.size() ? name[address - base] : 0;
	};
	check(precision::move_name(base, byte) == "actual_move");
	check(precision::move_name(0, byte).empty());
	check(precision::move_name(base, [](auto)
		{
			return ~0u;
		}).empty());
	check(precision::move_name(base, [](auto)
		{
			return 'x';
		}).empty());
	check(precision::move_name(base, [](auto)
		{
			return '{';
		}).empty());
}
