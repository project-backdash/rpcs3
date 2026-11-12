#include "pbd_test.h"

#include "Emu/PBD/core/practice/pbd_advantage_tone.h"

using pbd::practice::advantage_tone;
using pbd::practice::advantage_tone_of;
using pbd::test::check;

PBD_TEST(PbdAdvantageTone, NumericBoundaries)
{
	check(advantage_tone_of("+1") == advantage_tone::green);
	check(advantage_tone_of("0") == advantage_tone::plain);
	check(advantage_tone_of("-1") == advantage_tone::yellow);
	check(advantage_tone_of("-7") == advantage_tone::yellow);
	check(advantage_tone_of("-8") == advantage_tone::orange);
	check(advantage_tone_of("-10") == advantage_tone::orange);
	check(advantage_tone_of("-11") == advantage_tone::red);
	check(advantage_tone_of("-40") == advantage_tone::red);
}

PBD_TEST(PbdAdvantageTone, LeadingNumberChoosesTheTone)
{
	check(advantage_tone_of("+16 Crumple (-14)") == advantage_tone::green);
	check(advantage_tone_of("-14 Crumple (+16)") == advantage_tone::red);
	check(advantage_tone_of("1") == advantage_tone::green);
	check(advantage_tone_of("+0") == advantage_tone::plain);
	check(advantage_tone_of("-0") == advantage_tone::plain);
	check(advantage_tone_of("-007") == advantage_tone::yellow);
}

PBD_TEST(PbdAdvantageTone, LabelsAndMissingNumbersKeepTheirTone)
{
	for (const auto value : {"Launch", "KND", "Juggle", "Wall", "Throw", "---", "", "+", "-", "Launch (+16)"})
		check(advantage_tone_of(value) == advantage_tone::plain, value);
}

PBD_TEST(PbdAdvantageTone, LargeNumbersKeepTheirTone)
{
	check(advantage_tone_of("+999999999999999999999999999999") == advantage_tone::green);
	check(advantage_tone_of("-999999999999999999999999999999") == advantage_tone::red);
}
