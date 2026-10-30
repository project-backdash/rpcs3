#include "pbd_test.h"

#include "Emu/PBD/pbd_practice_presentation.h"
#include "Emu/PBD/pbd_practice_input_delay.h"
#include "Emu/PBD/pbd_practice_menu.h"

#include <cstdint>
#include <string>
#include <vector>

namespace
{
	using pbd::test::check;

	// A different word on every frame and never neutral, so a word that runs twice, early or out of order shows.
	std::uint16_t word(std::uint32_t frame)
	{
		return static_cast<std::uint16_t>(frame + 1);
	}
} // namespace

PBD_TEST(PbdPracticeInputDelay, NotSetRunsEachWordOnItsOwnFrame)
{
	pbd::practice::input_delay line;
	for (std::uint32_t frame = 0; frame < 20; ++frame)
		check(line.next(word(frame), 0) == word(frame), "Not Set passes every word at once");
}

PBD_TEST(PbdPracticeInputDelay, EachDelayRunsEveryWordThatManyFramesLate)
{
	for (std::uint32_t frames = 1; frames <= pbd::practice::max_input_delay; ++frames)
	{
		pbd::practice::input_delay line;
		for (std::uint32_t frame = 0; frame < 40; ++frame)
		{
			const auto ran = line.next(word(frame), frames);
			check(ran == (frame < frames ? 0 : word(frame - frames)), "the word read that many frames earlier, neutral before it");
		}
	}
	pbd::practice::input_delay line;
	for (std::uint32_t frame = 0; frame < 12; ++frame)
		check(line.next(word(frame), 99) == (frame < pbd::practice::max_input_delay ? 0 : word(frame - pbd::practice::max_input_delay)),
			"a delay past the menu's last choice is its last choice");
}

PBD_TEST(PbdPracticeInputDelay, AChangedDelayOrALeftFightStartsTheLineEmpty)
{
	pbd::practice::input_delay line;
	for (std::uint32_t frame = 0; frame < 10; ++frame)
		line.next(word(frame), 3);
	// From 3 frames to 2: the words still waiting are dropped, never run twice or early.
	check(line.next(word(10), 2) == 0 && line.next(word(11), 2) == 0 && line.next(word(12), 2) == word(10),
		"the new delay starts from an empty line");
	// A pause or one of the game's own screens: the word passes at once and the line empties.
	check(line.frame(word(13), 2, false) == word(13), "outside the fight nothing waits");
	check(line.frame(word(14), 2, true) == 0 && line.frame(word(15), 2, true) == 0 && line.frame(word(16), 2, true) == word(14),
		"back in the fight nothing pressed before arrives");
	line.clear();
	check(line.next(word(17), 2) == 0, "a restart empties the line");
}

PBD_TEST(PbdPracticeInputDelay, BothTrainingModesListInputDelayFramesAboveRestoreDefaults)
{
	using namespace pbd::practice;
	check(input_delay_row(training_mode::offense, practice_rows::input_delay) &&
			  input_delay_row(training_mode::defense, practice_rows::first + dummy_rows::input_delay),
		"each mode's own row is Input Delay Frames");
	check(!input_delay_row(training_mode::defense, practice_rows::input_delay) &&
			  !input_delay_row(training_mode::offense, practice_rows::first + dummy_rows::input_delay) &&
			  !input_delay_row(training_mode::cpu, practice_rows::input_delay) &&
			  !input_delay_row(training_mode::offense, practice_rows::restore),
		"no other row is");
	check(!game_option_row(training_mode::offense, practice_rows::input_delay) &&
			  !defense_row(training_mode::offense, practice_rows::input_delay),
		"it is neither a game option nor a Defense row in Offense");
}

PBD_TEST(PbdPracticeInputDelay, InputDelayFramesOffersNotSetThenOneToSixFrames)
{
	const auto list = pbd::practice::menu::input_delay_choices(0);
	check(list.options.size() == pbd::practice::max_input_delay + 1 && list.options.size() <= pbd::practice::max_options,
		"Not Set and each delay fit one list");
	check(list.options[0].is_default && list.current == 0, "zero delay is the default");
	for (const auto& option : list.options)
		check(option.enabled && !option.help.empty(), "every choice can be taken and says what it does");
	check(pbd::practice::menu::input_delay_choices(4).current == 4 && pbd::practice::menu::input_delay_choices(7).current == 0,
		"the option in effect is the delay, and an impossible one shows Not Set");
}
