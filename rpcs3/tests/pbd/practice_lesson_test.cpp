#include "pbd_test.h"
#include "Emu/PBD/pbd_practice_lesson.h"
#include "Emu/PBD/pbd_practice_lesson_channel.h"

PBD_TEST(PbdPracticeLesson, RefusesChangedFightersStageAndNonPracticeBeforeInput)
{
	using namespace pbd::practice;
	using result = pbd_lesson_check;
	using pbd::test::check;
	const pbd_lesson_setup requested{{9, 9}, 28};
	pbd_lesson_game game{requested, 10, true, false};
	check(pbd_admit_lesson(requested, game) == result::ready);
	game.mode = 4;
	check(pbd_admit_lesson(requested, game) == result::not_practice);
	game.mode = 10;
	game.paused = true;
	check(pbd_admit_lesson(requested, game) == result::not_ready);
	game.paused = false;
	game.in_match = false;
	check(pbd_admit_lesson(requested, game) == result::not_ready);
	game.in_match = true;
	for (unsigned port = 0; port < 2; ++port)
	{
		game.setup.characters[port] = 0;
		check(pbd_admit_lesson(requested, game) == result::wrong_fighters);
		game.setup.characters[port] = 9;
	}
	game.setup.stage = 31;
	check(pbd_admit_lesson(requested, game) == result::wrong_stage);
	check(pbd_admit_lesson({{35, 9}, 28}, game) == result::invalid_setup);
	check(pbd_admit_lesson({{9, 9}, 35}, game) == result::unsupported_stage);
}

PBD_TEST(PbdPracticeLesson, PlaybackConsumesOnlyDeliveredAdvancingFrames)
{
	using namespace pbd::practice::lesson;
	using pbd::test::check;
	for (unsigned port = 0; port < 2; ++port)
	{
		playback play{{0x2000, 0, 0x2010}, port};
		check(play.input(1 - port, true) == 0);
		play.observe(true);
		check(play.position == 0, "the other port cannot consume a word");
		check(play.input(port, false) == 0);
		play.observe(true);
		check(play.position == 0, "menu and reset claims cannot consume a word");
		check(play.input(port, true) == 0x2000);
		play.observe(false);
		check(play.position == 0, "a paused frame cannot consume a word");
		for (const auto word : {0x2000, 0, 0x2010})
		{
			check(play.input(port, true) == word);
			check(play.input(port, true) == word, "repeated reads share a frame");
			play.observe(true);
		}
		check(!play.active());
		play.stop(); check(play.position == 0 && !play.pending);
	}
}

PBD_TEST(PbdPracticeLesson, ChannelRefusesBadSetupAndMenuWords)
{
	using namespace pbd::practice::lesson;
	using pbd::test::check;
	const std::string prefix = R"({"schema":"pbd-practice-lesson-command-1","id":"one","generation":"1-1-0","kind":"play","port":1,"setup":{"characters":[9,0],"stage":37,"side":"right","stance":"crouching","hitCondition":"counter"},"words":)";
	const auto document = [&](const std::vector<std::uint16_t>& words) { return prefix + pbd::json::quoted(pbd::practice_store::encode_words(words)) + "}"; };
	const auto valid = parse(document({1, 2, 4, 8, 0x2010}));
	check(valid.value && valid.value->port == 1 && valid.value->words.size() == 5);
	for (const auto word : {0x0100, 0x0800, 0x0900, 0x0910}) check(parse(document({static_cast<std::uint16_t>(word)})).error == "invalid_recording");
	check(parse(document({0, 0})).error == "empty_recording");
	check(!parse(prefix + "\"AQ\"}").value, "odd byte recording refuses");
	check(!parse(prefix + "\"AAAA=\"}").value, "padded recording refuses");
	auto unknown = document({0x2000});
	unknown.replace(unknown.find("\"stage\":37"), 10, "\"stage\":35");
	check(parse(unknown).error == "unsupported_stage");
}

PBD_TEST(PbdPracticeLesson, RejectedReplacementRetainsActiveCompletion)
{
	using namespace pbd::practice::lesson;
	using pbd::test::check;
	channel state;
	state.ack = "first"; state.outcome = "playing"; state.play.words = {0x2000};
	state.refuse("second", "lesson_busy");
	check(state.ack == "first" && state.outcome == "playing" && state.play.active());
	check(state.rejected == "second" && state.rejection == "lesson_busy");
	state.finish("complete");
	check(state.ack == "first" && state.outcome == "complete" && !state.play.active());
}

PBD_TEST(PbdPracticeLesson, DummyPlaybackLeavesThePlayerControllerLive)
{
	using namespace pbd::practice::lesson;
	using pbd::test::check;
	for (unsigned port = 0; port < 2; ++port)
	{
		playback play{{0x2010}, port};
		play.demonstration = false;
		check(!play.override_for(1 - port, true), "the player keeps the physical controller word during the dummy's action");
		check(play.override_for(port, true) == 0x2010);
		play.observe(false);
		play.demonstration = true;
		check(play.override_for(1 - port, true) == 0, "Show claims the other port");
		play.observe(true);
		check(play.position == 0, "claiming the other port cannot consume the target word");
		check(play.override_for(port, true) == 0x2010);
		play.observe(true);
		check(!play.override_for(port, true) && !play.override_for(1 - port, true));
	}
}

PBD_TEST(PbdPracticeLesson, PlayerCaptureKeepsAdvancingFramesAndCanonicalFacing)
{
	using namespace pbd::practice;
	using namespace pbd::practice::lesson;
	using pbd::test::check;
	channel::input_capture capture;
	pbd_lesson_setup setup{{9, 0}, 28, pbd_lesson_side::right};
	capture.begin(setup);
	check(capture.port == 1 && capture.active && capture.started);
	capture.observe(false, 0x8000, pbd::practice::movement::screen_side::right);
	check(capture.words.empty(), "paused and repeated boundaries add no frames");
	capture.observe(true, 0x8000, pbd::practice::movement::screen_side::right);
	capture.observe(true, 0, pbd::practice::movement::screen_side::left);
	capture.observe(true, 0x2910, pbd::practice::movement::screen_side::left);
	check(capture.words == std::vector<std::uint16_t>{0x2000, 0, 0x2010}, "preserve interior neutral and canonical facing; strip menu controls");
	const auto serialized = capture_document(capture, "end", "run");
	const auto document = pbd::json::detail::json_parser(serialized).parse();
	check(document.value && document.value->find("words")->text == pbd::practice_store::encode_words(capture.words));
	for (unsigned frame = 3; frame < 1801; ++frame) capture.observe(true, 0, pbd::practice::movement::screen_side::left);
	check(capture.words.size() == 1800 && capture.limit_reached && !capture.active);
	capture.begin(setup);
	capture.observe(true, 0x2000, {});
	check(!capture.valid && !capture.active && capture.words.empty(), "unknown facing refuses a take instead of guessing");
}

PBD_TEST(PbdPracticeLesson, FailedCaptureWriteRetainsTheTakeForRetry)
{
	using namespace pbd::practice;
	using namespace pbd::practice::lesson;
	using pbd::test::check;
	struct directory
	{
		std::filesystem::path root = std::filesystem::temp_directory_path() / "pbd-lesson-capture-retry";
		directory()
		{
			std::filesystem::remove_all(root);
			std::filesystem::create_directories(root / "capture.json");
		}
		~directory()
		{
			std::error_code ec;
			std::filesystem::remove_all(root, ec);
		}
	} files;
	channel state;
	state.root = files.root;
	state.generation = "1-1-0";
	state.capture.begin({{9, 0}, 28});
	state.capture.observe(true, 0x2000, movement::screen_side::left);
	state.capture.observe(true, 0, movement::screen_side::left);
	state.capture.observe(true, 0x2010, movement::screen_side::left);
	const auto words = state.capture.words;
	state.end_capture("first");
	check(state.ack == "first" && state.outcome == "capture_unavailable" && !state.capture.active && state.capture.started && state.capture.valid);
	check(state.capture.words == words && std::filesystem::is_directory(files.root / "capture.json"));
	check(std::filesystem::remove(files.root / "capture.json"));
	state.end_capture("retry");
	check(state.ack == "retry" && state.outcome == "complete" && !state.capture.started && state.capture.words == words);
	const auto text = pbd::json::detail::read_text(files.root / "capture.json");
	check(text.value);
	const auto saved = pbd::json::detail::json_parser(*text.value).parse();
	check(saved.value && pbd::json::detail::is(saved.value->find("schema"), "pbd-practice-lesson-capture-1") &&
		  pbd::json::detail::is(saved.value->find("requestId"), "retry") && pbd::json::detail::is(saved.value->find("generation"), state.generation));
	check(pbd::practice_store::decode_words(saved.value->find("words")->text) == words);
	check(!std::filesystem::exists(files.root / "capture.json.tmp"));
}

PBD_TEST(PbdPracticeLesson, PlayerCaptureRefusesChangedSetupButSurvivesPause)
{
	using namespace pbd::practice;
	using namespace pbd::practice::lesson;
	using pbd::test::check;
	const pbd_lesson_setup setup{{9, 0}, 28, pbd_lesson_side::right};
	channel::input_capture capture;
	for (unsigned change = 0; change < 8; ++change)
	{
		capture.begin(setup);
		pbd_lesson_game observed{setup, 10, true, true};
		capture.check_setup(observed);
		check(capture.valid && capture.active, "pause retains the same situation");
		if (change == 0) observed.setup.characters[0] = 7;
		if (change == 1) observed.setup.stage = 29;
		if (change == 2) observed.setup.side = pbd_lesson_side::left;
		if (change == 3) observed.setup.stance = pbd_lesson_stance::crouching;
		if (change == 4) observed.setup.hit_condition = pbd_lesson_hit_condition::counter;
		if (change == 5) observed.mode = 2;
		if (change == 6) observed.in_match = false;
		if (change == 7)
		{
			capture.active = false; capture.limit_reached = true;
			observed.setup.characters[1] = 7;
		}
		capture.check_setup(observed);
		check(!capture.valid && !capture.active, "changed metadata cannot be attached to the recorded words");
	}
}

PBD_TEST(PbdPracticeLesson, ExactSideStanceHitConditionAndKnownStages)
{
	using namespace pbd::practice;
	using result = pbd_lesson_check;
	using pbd::test::check;
	pbd_lesson_setup requested{{9, 0}, 37, pbd_lesson_side::right, pbd_lesson_stance::crouching, pbd_lesson_hit_condition::counter};
	pbd_lesson_game game{requested, 10, true, false};
	check(pbd_admit_lesson(requested, game) == result::ready, "Inferno is a known lesson stage");
	game.setup.side = pbd_lesson_side::left;
	check(pbd_admit_lesson(requested, game) == result::wrong_side);
	game.setup.side = requested.side;
	game.setup.stance = pbd_lesson_stance::standing;
	check(pbd_admit_lesson(requested, game) == result::wrong_stance);
	game.setup.stance = requested.stance;
	game.setup.hit_condition = pbd_lesson_hit_condition::normal;
	check(pbd_admit_lesson(requested, game) == result::wrong_hit_condition);
	for (const auto stage : {14u, 16u, 35u, 36u, 38u})
	{
		requested.stage = stage;
		check(pbd_admit_lesson(requested, game) == result::unsupported_stage);
	}
	for (const auto stage : {0u, 13u, 15u})
		check(pbd::stage_rule::loadable(stage), "an original stage is a known lesson stage");
	check(pbd_lesson_play_port(pbd_lesson_side::left, true) == 0);
	check(pbd_lesson_play_port(pbd_lesson_side::right, true) == 1);
	check(pbd_lesson_play_port(pbd_lesson_side::left, false) == 1);
	check(pbd_lesson_play_port(pbd_lesson_side::right, false) == 0);
}
