#include "pbd_test.h"

#include "Emu/PBD/pbd_practice_store.h"
#include "Emu/PBD/pbd_practice_memory.h"
#include "Emu/PBD/pbd_dummy_move_channel.h"
#include "Emu/PBD/pbd_dummy_move_input.h"
#include "practice_store_model.h"

#include <future>

using pbd::test::check;

PBD_TEST(PbdPractice, MoveReactionChoices)
{
	using namespace pbd::practice::dummy_move;
	const auto lookup = [](u32 id) -> std::optional<variant_info>
	{
		if (id == 7) return variant_info{1, 1};
		if (id == 12) return variant_info{2, 2};
		return {};
	};
	scheduler player;
	settings configured;
	const auto unchanged = player.rng;
	check(!player.applied_contact(100, contact::guard, configured, lookup) && player.rng == unchanged,
		"Off consumes no random values and has no pending move");
	configured.after_guard = {source::moves, 0, {7, 12}};
	bool first = false, second = false;
	for (u32 frame = 101; frame < 201; ++frame)
	{
		check(player.applied_contact(frame, contact::guard, configured, lookup), "each new block chooses a move");
		const auto selected = *player.pending;
		check(selected.id == 7 || selected.id == 12, "selection stays within the selected rows");
		check(selected.variant == (selected.id == 12 ? 1u : 0u), "native categories two and four use variant one");
		first |= selected.id == 7; second |= selected.id == 12;
		const auto state = player.rng;
		check(!player.applied_contact(frame, contact::guard, configured, lookup) && player.rng == state,
			"repeated callbacks for the same contact do not choose twice");
	}
	check(first && second, "both selected moves occur in repeated reactions");
	configured.after_hit = {source::recording, 3};
	check(player.applied_contact(201, contact::hit, configured, lookup) && player.pending->slot == 3,
		"after being hit independently uses its recorded slot");
	configured.after_guard.ids.push_back(20);
	const auto state = player.rng;
	check(!player.applied_contact(300, contact::guard, configured, lookup) && player.rng == state,
		"a stale character selection is refused before random selection");
}

PBD_TEST(PbdPractice, MoveReactionCountAndAfter)
{
	using namespace pbd::practice::dummy_move;
	using pbd::practice::contact_counted;
	const auto lookup = [](u32 id) -> std::optional<variant_info>
	{
		return id == 7 ? std::optional<variant_info>{variant_info{1, 1}} : std::nullopt;
	};
	const reaction move{source::moves, 0, {7}};
	const settings configured{move, move};
	// One contact that the dummy recovers from at `earliest`; whether the reaction was armed for it.
	scheduler player;
	const auto contact_at = [&](u32 counter, contact kind, const gate& when, u32 earliest)
	{
		const bool armed = player.applied_contact(counter, kind, configured, lookup, when);
		player.recovered(earliest);
		player.advance(earliest, when);
		check(!player.series_open(), "the series ends once the dummy can act again");
		return armed;
	};

	const gate two_hits{contact_counted::hits, 2};
	check(!contact_at(100, contact::guard, two_hits, 112) && player.seen == 0, "a block does not count toward Hits");
	check(!contact_at(200, contact::hit, two_hits, 215) && player.seen == 1, "the first hit of two does not react");
	check(contact_at(300, contact::hit, two_hits, 315) && player.seen == 0, "the second hit reacts and the count starts again");
	check(!contact_at(400, contact::hit, two_hits, 415) && player.seen == 1, "so the next hit is the first of two again");

	player.reset();
	const gate three_blocks{contact_counted::blocks, 3};
	check(!contact_at(100, contact::guard, three_blocks, 112) && !contact_at(200, contact::guard, three_blocks, 212) && player.seen == 2,
		"two blocks of three");
	check(!contact_at(300, contact::hit, three_blocks, 320) && player.seen == 2, "a hit between them does not count toward Blocks");
	check(contact_at(400, contact::guard, three_blocks, 412) && player.seen == 0, "the third block reacts");

	// Every impact inside a string counts; an excluded contact cannot cancel a response already due.
	player.reset();
	const gate one_hit{contact_counted::hits, 1};
	check(!player.applied_contact(100, contact::guard, configured, lookup, one_hit), "a blocked first hit is not a counted hit");
	player.recovered(112);
	check(player.applied_contact(108, contact::hit, configured, lookup, one_hit) && player.pending,
		"the first hit counts even while still recovering from the block");
	player.reset();
	const gate one_block{contact_counted::blocks, 1};
	check(player.applied_contact(100, contact::guard, configured, lookup, one_block), "a block arms a Blocks reaction");
	check(player.applied_contact(108, contact::hit, configured, lookup, one_block) && player.pending,
		"a hit later in the same string keeps the counted block's reaction due");
	player.reset();
	const gate two_either{contact_counted::either, 2};
	check(!player.applied_contact(100, contact::guard, configured, lookup, two_either) &&
			  player.applied_contact(110, contact::guard, configured, lookup, two_either),
		"the second block of a string reaches After 2");
	player.advance(110 + series_gap_frames + 1, two_either);
	check(!player.series_open() && player.seen == 0, "recovery after a due response starts the count again");
	check(!player.applied_contact(400, contact::guard, configured, lookup, two_either), "the next block is the first of two again");
	check(player.applied_contact(410, contact::hit, configured, lookup, two_either), "a hit can supply the second counted contact");

	player.reset();
	player.applied_contact(100, contact::guard, configured, lookup, two_either);
	player.recovered(90);
	player.advance(105, two_either);
	check(player.series_open(), "a recovery read before the contact belongs to the stun it ended");
	player.reset();
	check(!player.series_open() && player.seen == 0 && !player.pending, "Restart clears a partial count and its recovery wait");
	check(!player.applied_contact(100, contact::guard, configured, lookup, two_either), "the same frame after Restart is the first new contact");
	check(player.applied_contact(110, contact::guard, configured, lookup, two_either), "the new second contact arms a response");
	player.reset();
	check(!player.pending && player.seen == 0, "Restart also clears an already armed response");
	check(gate{} == gate{contact_counted::either, 1}, "the defaults react after every contact");
}

PBD_TEST(PbdPractice, AppliedContactsCountAcrossStrings)
{
	using namespace pbd::practice::dummy_move;
	contact_batch contacts;
	contacts.applied(100, true);
	contacts.applied(100, true);
	check(contacts.take() == std::array<u32, 2>{0, 1}, "a repeated block callback is counted once");
	contacts.applied(100, true);
	check(contacts.take() == std::array<u32, 2>{}, "a repeated boundary cannot count that block again");
	contacts.applied(108, true);
	contacts.applied(116, false);
	check(contacts.take() == std::array<u32, 2>{1, 1}, "later hits and blocks of the same string count separately");
	check(contacts.take() == std::array<u32, 2>{}, "contacts are consumed once per boundary");
	contacts = {};
	contacts.applied(100, true);
	check(contacts.take()[1] == 1, "a restart can reuse the old frame counter");

	using namespace pbd::practice;
	reaction_counter counter;
	counter.applied(false, contact_counted::blocks, 2);
	check(counter.seen == 1 && !counter.ready(), "one block waits for the second");
	counter.applied(true, contact_counted::blocks, 2);
	check(counter.seen == 1 && !counter.due, "a hit between the blocks is excluded");
	counter.applied(false, contact_counted::blocks, 2);
	check(counter.due && counter.seen == 2, "the second block of the string makes the reaction due");
	counter.applied(true, contact_counted::blocks, 2);
	check(counter.due && counter.seen == 2, "another hit cannot clear a due reaction");
	check(counter.ready() && !counter.due && counter.seen == 0, "the recovery frame consumes it and resets the count");
	check(!counter.ready(), "recovery never repeats the reaction");
}

PBD_TEST(PbdPractice, ContactRecoveryReading)
{
	using namespace pbd::practice::dummy_move;
	contact_batch contacts;
	contacts.terminal(90, 1, 20);
	check(!contacts.earliest && !contacts.free_next(90), "a reading before any contact says nothing");
	contacts.applied(100, true);
	contacts.terminal(100, 1, 30);
	check(!contacts.earliest, "a reading on the contact's own frame belongs to the move the contact interrupted");
	// The block reaction runs from cursor 1 on frame 101; its standing end command's window opens on cursor 19, frame 119.
	for (u32 frame = 101; frame < 118; ++frame)
	{
		contacts.terminal(frame, frame - 100, 19);
		check(contacts.earliest == 119u && !contacts.free_next(frame), "block stun");
	}
	contacts.terminal(118, 18, 19);
	check(contacts.free_next(118), "the word chosen after frame 118 plays on frame 119, the first one the dummy can act on");
	check(!contacts.free_next(119), "a reading the frame that ran last did not renew says nothing");
	contacts.terminal(125, 25, 19);
	check(contacts.earliest == 125u && contacts.free_next(125), "inside the window the dummy can act on every frame");
	check(contacts.take() == std::array<u32, 2>{0, 1} && contacts.free_next(125), "taking the contacts keeps the reading");
	contacts.applied(126, false);
	check(!contacts.earliest && !contacts.free_next(125), "the next contact clears it");
	contacts = {};
	contacts.terminal(127, 1, 2);
	check(!contacts.earliest, "a Restart clears the contact the reading needs");
}

PBD_TEST(PbdPractice, MoveReactionInputs)
{
	using namespace pbd::practice::dummy_move;
	pbd::move_list::command command;
	command.samples = {{0, 0x0100, 0}, {0x2000, 0x0201, 0}, {0x2010, 0x0301, 0}, {0, 0x1000, 0}};
	const auto words = command_words(command);
	check(words && words->size() == 21 && (*words)[0] == 0x2000 && (*words)[2] == 0x2010,
		"the stock duration bytes expand exactly and demonstration leading neutral is removed");
	input_player player;
	check(player.arm(*words, 1), "a validated command can target the dummy port");
	player.recovery(100, 10, 20);
	player.boundary(100, true, true);
	check(!player.input(1), "the motion waits until the stock recovery buffer can receive it");
	player.boundary(102, true, true);
	check(player.input(1) == 0x2000 && !player.input(0), "directional setup leaves the live player's port alone");
	player.boundary(103, true, true);
	check(player.input(1) == 0x2000, "a two-frame direction stays held for two advancing frames");
	player.boundary(104, true, true);
	check(player.input(1) == 0x2010, "the attack follows its native directional setup");
	player.boundary(105, true, true);
	check(player.input(1) == 0x2010 && player.position == 2, "the first attack stays buffered until the game accepts it");
	player.boundary(106, false, false);
	check(player.input(1) == 0 && player.position == 2, "pause blanks input and consumes no command frame");
	player.boundary(106, false, true);
	check(player.input(1) == 0x2010, "resume keeps the buffered input");
	player.accepted = true;
	player.boundary(107, true, true);
	check(player.position == 3 && player.input(1) == 0x2010, "accepted playback advances at native update cadence");
	player.stop();
	check(!player.input(1), "completion releases the dummy input route");
	command.samples[0][1] = 0x0102;
	check(!command_words(command), "unknown native sample flags are refused");
	command.samples[0] = {0x0800, 0x0100, 0};
	check(!command_words(command), "a game menu input can never become a dummy command");
}

PBD_TEST(PbdPractice, MoveReactionChannel)
{
	using namespace pbd::practice::dummy_move;
	const picker_catalog catalog{"test-9-26", 9, 26, {{7, "Example move", "1,2", true, 1}}};
	channel link;
	const settings selected{{source::moves, 0, {7}}, {source::off}};
	check(link.apply({"first", catalog.generation, 9, selected}, catalog) && current_settings == selected,
		"a native acknowledgement applies exactly the current character's selection");
	check(!link.apply({"stale", "older-generation", 9, {}}, catalog) && link.error == "fighters_changed" &&
		current_settings == selected, "a stale selection never changes the current reactions");
	check(!link.apply({"wrong-character", catalog.generation, 13, {}}, catalog) && current_settings == selected,
		"the character is checked independently of the generation");
	auto unavailable = selected;
	unavailable.after_guard.ids.push_back(8);
	check(!link.apply({"missing", catalog.generation, 9, unavailable}, catalog) && link.error == "move_unavailable" &&
		current_settings == selected, "an unavailable selected move refuses the entire setting");
	const auto status = pbd::json::detail::json_parser(link.status(catalog)).parse();
	check(status.value && status.value->find("moves")->items.size() == 1 && status.value->find("ack")->text == "missing",
		"the status carries the current catalog and the matching native acknowledgement");
	current_settings = {};
}

PBD_TEST(PbdPractice, MoveReactionPersistence)
{
	using namespace pbd::practice_store;
	using namespace pbd::practice::dummy_move;
	profile saved;
	saved.dummies[9].reactions = {{source::moves, 0, {7, 12}}, {source::recording, 4}};
	saved.dummies[13].reactions = {{source::off}, {source::moves, 0, {21}}};
	const auto decoded = parse_profile(serialize(saved, "test"));
	check(decoded.value && decoded.value->dummies == saved.dummies, "reaction settings survive per-character profile reload");
	check(!read_settings(nullptr), "an older profile has no reaction settings");
	const auto invalid = pbd::json::detail::json_parser(
		R"({"afterGuard":{"source":"moves","ids":[7,7]},"afterHit":{"source":"off"}})").parse();
	check(invalid.value && !read_settings(&*invalid.value), "duplicate selected rows are refused");
}

namespace
{
	using namespace pbd::practice_store;
	using pbd::practice::movement::preset;

	std::vector<u16> expand(const store_model::take& take)
	{
		std::vector<u16> words;
		for (const auto& step : take.steps)
			words.insert(words.end(), step.frames, step.word);
		return words;
	}
	profile full_profile()
	{
		profile value;
		value.mode = pbd::practice::training_mode::defense;
		value.view.command_history = {false, true};
		value.view.frame_data = false;
		value.view.movement_streaks = false;
		value.restart = restart_position::right_wall;
		value.options = game_options{10, 1, 0, 2, 1};
		auto& kazuya = value.dummies[13];
		kazuya.offense = {preset::wavedash, 8, {pbd::practice::after_contact::action, 2}, pbd::practice::contact_counted::blocks, 3};
		kazuya.defense = {preset::backdash_cancel, 2, pbd::practice::movement::wavedash_end::back,
			pbd::practice::movement::dash_end::stop_first};
		value.dummies[0].offense.movement = preset::crouch;
		return value;
	}
	std::string changed(std::string text, std::string_view from, std::string_view to)
	{
		const auto at = text.find(from);
		check(at != std::string::npos, "fixture text");
		return text.replace(at, from.size(), to);
	}
	// A fresh directory under the system's temporary one, removed again by the destructor.
	struct scratch
	{
		std::filesystem::path root;
		scratch()
		{
			const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
			root = std::filesystem::temp_directory_path() / ("pbd-practice-store-test-" + std::to_string(stamp));
			check(std::filesystem::create_directory(root));
		}
		~scratch()
		{
			std::error_code ec;
			if (root.filename().string().starts_with("pbd-practice-store-test-"))
				std::filesystem::remove_all(root, ec);
		}
		std::size_t count(std::string_view prefix) const
		{
			std::size_t found = 0;
			for (const auto& entry : std::filesystem::recursive_directory_iterator(root))
				found += entry.path().filename().string().starts_with(prefix) ? 1 : 0;
			return found;
		}
	};
} // namespace

PBD_TEST(PbdPracticeStore, WordsEncoding)
{
	const std::vector<u16> words{0x0000, 0x4000, 0xc0ff, 0x1234, 0xffff};
	const auto text = encode_words(words);
	check(text.find_first_not_of("ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_") == std::string::npos);
	check(decode_words(text) == words && decode_words("") == std::vector<u16>{} && encode_words({}).empty());
	for (std::size_t size = 0; size < 7; ++size)
	{
		const std::vector<u16> some(words.begin(), words.begin() + std::min(size, words.size()));
		check(decode_words(encode_words(some)) == some);
	}
	check(!decode_words(text + "="));
	check(!decode_words(text.substr(0, text.size() - 1) + "+"));
	// One byte is not a word, a lone character is not a byte, and padding bits must be zero.
	check(!decode_words("AA") && !decode_words("A") && !decode_words("AAB"));
	check(decode_words("AAA") == std::vector<u16>{0});
}

PBD_TEST(PbdPracticeStore, ProfileRoundTrip)
{
	const auto value = full_profile();
	const auto text = serialize(value, "2026-01-01T00:00:00.000Z");
	const auto document = pbd::json::detail::json_parser(text).parse();
	check(document.value && pbd::json::detail::is(document.value->find("schema"), "pbd-practice-profile-1") && text.back() == '\n');
	const auto parsed = parse_profile(text);
	check(parsed.value && *parsed.value == value);
	// A fresh profile has no game options, and reads back the same.
	const auto fresh = parse_profile(serialize(profile{}, "x"));
	check(fresh.value && *fresh.value == profile{} && !fresh.value->options);
	// Restart Position names every choice but Remembered.
	for (u32 i = 0; i < restart_names.size(); ++i)
	{
		auto other = value;
		other.restart = static_cast<restart_position>(i);
		check(parse_profile(serialize(other, "x")).value->restart == other.restart);
	}
}

PBD_TEST(PbdPracticeStore, WakeupChoiceBelongsToEachDummy)
{
	using namespace pbd::practice;
	for (u32 option = 0; option < pbd_wakeup_option_count; ++option)
	{
		profile value;
		value.dummies[0].wakeup = static_cast<pbd_wakeup_option>(option);
		value.dummies[13].wakeup = pbd_wakeup_option::stay_down;
		const auto parsed = parse_profile(serialize(value, "x"));
		check(parsed.value && *parsed.value == value, "wake-up belongs to the dummy character across launches");
	}
	const auto older = parse_profile(R"({"schema":"pbd-practice-profile-1","dummies":{"0":{"offense":{}}}})");
	check(older.value && older.value->dummies.at(0).wakeup == pbd_wakeup_option::stand_up, "older settings default to Stand Up");
	for (const auto invalid : {"-1", "15", "999999", "null", "true", "\"3\""})
	{
		const auto text = std::string(R"({"schema":"pbd-practice-profile-1","dummies":{"0":{"wakeup":)") + invalid + "}}}";
		const auto parsed = parse_profile(text);
		check(parsed.value && parsed.value->dummies.at(0).wakeup == pbd_wakeup_option::stand_up, "invalid wake-up values use Stand Up");
	}
}

PBD_TEST(PbdPracticeStore, GuardChoiceAndRandomCounterHitAreRemembered)
{
	using pbd::practice::pbd_guard_option;
	for (u32 option = 0; option < pbd::practice::pbd_guard_option_count; ++option)
		for (const bool random : {false, true})
		{
			auto value = full_profile();
			value.guard = static_cast<pbd_guard_option>(option);
			value.counter_hit_random = random;
			const auto parsed = parse_profile(serialize(value, "x"));
			check(parsed.value && *parsed.value == value, "the guard choice and Counter Hit's Random come back next launch");
		}
	const auto older = parse_profile(R"({"schema":"pbd-practice-profile-1"})");
	check(older.value && older.value->guard == pbd_guard_option::game && !older.value->counter_hit_random,
		"a profile from before them keeps the game's own options");
	for (const auto invalid : {"-1", "3", "999999", "null", "true", "\"1\"", "1.5"})
	{
		const auto parsed = parse_profile(std::string(R"({"schema":"pbd-practice-profile-1","dummyGuard":)") + invalid + "}");
		check(parsed.value && parsed.value->guard == pbd_guard_option::game, "an invalid guard choice is the game's own");
	}
	for (const auto invalid : {"1", "0", "null", "\"true\"", "[]"})
	{
		const auto parsed = parse_profile(std::string(R"({"schema":"pbd-practice-profile-1","counterHitRandom":)") + invalid + "}");
		check(parsed.value && !parsed.value->counter_hit_random, "only true turns Counter Hit's Random on");
	}
}

PBD_TEST(PbdPracticeStore, ThrowEscapeBelongsToEachDummy)
{
	using namespace pbd::practice;
	for (u32 option = 0; option < throw_escape_option_count; ++option)
	{
		profile value;
		value.dummies[0].throw_escape = static_cast<throw_escape_option>(option);
		value.dummies[13].throw_escape = throw_escape_option::random;
		const auto parsed = parse_profile(serialize(value, "x"));
		check(parsed.value && *parsed.value == value, "Throw Escape belongs to the dummy character across launches");
	}
	const auto older = parse_profile(R"({"schema":"pbd-practice-profile-1","dummies":{"0":{"wakeup":2}}})");
	check(older.value && older.value->dummies.at(0).throw_escape == throw_escape_option::none &&
			  older.value->dummies.at(0).wakeup == pbd_wakeup_option::back_roll,
		"older settings default to None and keep their Wake-up");
	for (const auto invalid : {"-1", "3", "999999", "null", "true", "\"1\""})
	{
		const auto text = std::string(R"({"schema":"pbd-practice-profile-1","dummies":{"0":{"throwEscape":)") + invalid + "}}}";
		const auto parsed = parse_profile(text);
		check(parsed.value && parsed.value->dummies.at(0).throw_escape == throw_escape_option::none, "invalid values use None");
	}
}

PBD_TEST(PbdPracticeStore, OlderProfileKeepsSettings)
{
	scratch dir;
	auto expected = full_profile();
	const auto text = changed(serialize(expected, "x"), "\"trainingMode\":",
		"\"lastPick\":{\"playerRow\":37,\"dummyRow\":69,\"stage\":17},\"trainingMode\":");
	expected.pick = last_pick{37, 69, 17};
	check(commit_text(profile_path(dir.root), text));
	const auto loaded = load_profile(dir.root);
	check(loaded.state == load_state::loaded && loaded.value == expected);
	check(parse_profile(serialize(loaded.value, "x")).value == expected);
	for (const auto& invalid : {R"({"playerRow":85,"dummyRow":69,"stage":17})", R"({"playerRow":37,"dummyRow":-1,"stage":17})",
		R"({"playerRow":37,"dummyRow":69,"stage":256})", R"({"playerRow":37,"stage":17})"})
	{
		const auto rejected = parse_profile(changed(text, R"({"playerRow":37,"dummyRow":69,"stage":17})", invalid));
		check(rejected.value && !rejected.value->pick && rejected.value->options == expected.options,
			"an invalid remembered selection cannot discard the remaining settings");
	}
}

PBD_TEST(PbdPracticeStore, ProfileClamping)
{
	const auto text = serialize(full_profile(), "x");
	const auto read = [](const std::string& value)
	{
		const auto parsed = parse_profile(value);
		check(parsed.value, "profile read");
		return *parsed.value;
	};
	check(read(changed(text, "\"wavedashSpeed\":8", "\"wavedashSpeed\":99")).dummies[13].offense.wavedash_speed == 8);
	check(read(changed(text, "\"wavedashSpeed\":2", "\"wavedashSpeed\":0")).dummies[13].defense.wavedash_speed == 2);
	check(read(changed(text, "\"movement\":1,", "\"movement\":50,")).dummies[13].offense.movement == preset::jump_back);
	check(read(changed(text, "\"reactionKind\":2", "\"reactionKind\":77")).dummies[13].offense.reaction.kind == pbd::practice::after_contact::moves);
	check(read(changed(text, "\"reactionKind\":2", "\"reactionKind\":\"two\"")).dummies[13].offense.reaction.kind == pbd::practice::after_contact::none);
	check(read(changed(text, "\"reactionSlot\":2", "\"reactionSlot\":12")).dummies[13].offense.reaction.slot == 7);
	check(read(changed(text, "\"counted\":0", "\"counted\":9")).dummies[13].offense.counted == pbd::practice::contact_counted::either);
	check(read(changed(text, "\"times\":3", "\"times\":0")).dummies[13].offense.times == 1);
	check(read(changed(text, "\"times\":3", "\"times\":-2")).dummies[13].offense.times == 1);
	check(read(changed(text, "\"wavedashEnd\":2", "\"wavedashEnd\":7")).dummies[13].defense.wavedash_end ==
		  pbd::practice::movement::wavedash_end::back);
	check(read(changed(text, "\"dummyAction\":10", "\"dummyAction\":15")).options == game_options{10, 1, 0, 2, 1});
	check(read(changed(text, "\"attackData\":2", "\"attackData\":7")).options == game_options{10, 1, 0, 2, 1});
	check(read(changed(text, "\"attackData\":2", "\"attackData\":0")).options == game_options{10, 1, 0, 1, 1},
		"old hidden-HUD values restore the normal game's HUD without changing any other Practice option");
	// A partial set of game options cannot be restored at once, so none is kept.
	check(!read(changed(text, "\"hitAnalysis\":1", "\"hitHistory\":1")).options);
	check(read(changed(text, "\"right_wall\"", "\"remembered\"")).restart == restart_position::centre);
	check(read(changed(text, "\"defense\",\"display\"", "\"cpu\",\"display\"")).mode == pbd::practice::training_mode::offense);
	check(read(changed(text, "\"p1History\":false", "\"p1History\":0")).view.command_history[0]);
	// Dummy keys are base characters in decimal.
	for (const auto key : {"35", "abc", "013", "-1", ""})
		check(!read(changed(text, "\"13\":", std::string("\"") + key + "\":")).dummies.contains(13));
	check(read(changed(text, "\"13\":", "\"34\":")).dummies.contains(34));
	check(read(changed(text, "\"13\":{", "\"12\":7,\"13\":{")).dummies.size() == 2);
	// Unreadable documents and other schemas are refused.
	check(parse_profile("{").error.size() && parse_profile("[]").error == "bad_schema");
	check(parse_profile(changed(text, "pbd-practice-profile-1", "pbd-practice-profile-2")).error == "bad_schema");
	check(parse_profile(changed(text, "\"schema\":", "\"schema\":1,\"x\":")).error == "bad_schema");
}

PBD_TEST(PbdPracticeStore, LiveOptionsStaySafeAcrossSelections)
{
	const game_options initial{10, 1, 0, 0, 1};
	const game_options expected{10, 1, 0, 1, 1};
	scratch dir;
	memory store;
	store.root = dir.root;
	profile settings;
	settings.options = initial;
	for (int selection = 0; selection < 2; ++selection)
	{
		store.autosave(settings, std::nullopt, "x");
		check(store.saved.options == expected, "the live cache cannot restore the hidden-HUD value");
		settings = store.saved;
	}
	settings.options = game_options{10, 1, 0, 2, 1};
	store.autosave(settings, std::nullopt, "x");
	check(store.saved.options == settings.options, "valid options and all other settings remain unchanged");
}

PBD_TEST(PbdPracticeStore, RecordingsRoundTrip)
{
	auto value = empty_recordings(13);
	value.interval = 1;
	for (u32 i = 0; i < slot_count; ++i)
	{
		const auto& take = store_model::takes[i];
		auto& out = value.slots[i];
		out.words = expand(take);
		out.leading_neutral = take.leading;
		out.trailing_neutral = take.trailing;
		out.stop_chord = take.stop_chord;
		out.frequency = i + 1;
		out.name = "Take " + std::to_string(i + 1);
	}
	value.slots[3].enabled = false;
	value.slots[5].trim = false;
	const auto text = serialize(value, "2026-01-01T00:00:00.000Z");
	const auto parsed = parse_recordings(text, 13);
	check(parsed.value && *parsed.value == value);
	check(parse_recordings(text, 12).error == "bad_character");
	check(parse_recordings(changed(text, "pbd-practice-recordings-1", "pbd-practice-recordings-9"), 13).error == "bad_schema");
	// Eight full takes stay near 40 KB, well inside the reader's limits.
	auto full = empty_recordings(0);
	for (auto& each : full.slots)
		each.words.assign(max_take_words, 0xc0ff);
	const auto large = serialize(full, "x");
	check(large.size() < 41000 && parse_recordings(large, 0).value == full);
}

PBD_TEST(PbdPracticeStore, RecordingsClamping)
{
	auto value = empty_recordings(5);
	value.slots[0].words.assign(10, 0x4000);
	value.slots[0].leading_neutral = 2;
	value.slots[0].trailing_neutral = 3;
	value.slots[0].stop_chord = 4;
	const auto text = serialize(value, "x");
	const auto read = [](const std::string& source)
	{
		const auto parsed = parse_recordings(source, 5);
		check(parsed.value, "recordings read");
		return *parsed.value;
	};
	check(read(text) == value);
	// Counts are cut to fit the take: the stop chord first, then the leading and trailing neutral frames.
	const auto longer_chord = changed(text, "\"stopChord\":4", "\"stopChord\":8");
	const auto fitted = read(changed(longer_chord, "\"trailingNeutral\":3", "\"trailingNeutral\":9"));
	check(fitted.slots[0].stop_chord == 8 && fitted.slots[0].leading_neutral == 2 && fitted.slots[0].trailing_neutral == 0);
	check(read(changed(text, "\"stopChord\":4", "\"stopChord\":40")).slots[0].stop_chord == 10);
	check(read(changed(text, "\"frequency\":1,", "\"frequency\":12,")).slots[0].frequency == 10);
	check(read(changed(text, "\"interval\":3", "\"interval\":9")).interval == 3);
	check(read(changed(text, "\"interval\":1}", "\"interval\":7}")).slots[0].interval == 3);
	// Words that do not decode leave the slot empty; a longer take is cut at the recording limit.
	const auto bad = read(changed(text, "\"words\":\"", "\"words\":\"*"));
	check(bad.slots[0].words.empty() && bad.slots[0].stop_chord == 0 && bad.slots[0].leading_neutral == 0);
	auto longer = value;
	longer.slots[0].words.assign(max_take_words + 7, 0x1000);
	check(read(serialize(longer, "x")).slots[0].words.size() == max_take_words);
	// A name must be plain text of 1 to 32 bytes.
	check(read(changed(text, "\"name\":\"Slot 1\"", "\"name\":\"\"")).slots[0].name == "Slot 1");
	check(read(changed(text, "\"name\":\"Slot 1\"", "\"name\":\"a\\nb\"")).slots[0].name == "Slot 1");
	check(read(changed(text, "\"name\":\"Slot 1\"", "\"name\":\"Hellsweep punish\"")).slots[0].name == "Hellsweep punish");
	// Missing slots are empty, and only eight are read.
	const auto short_list = read(changed(text, "\"slots\":[", "\"slots\":[{},"));
	check(short_list.slots[0].words.empty() && short_list.slots[1].words.size() == 10);
}

PBD_TEST(PbdPracticeStore, ReactionNeedsARecordedSlot)
{
	auto slots = empty_recordings(1);
	slots.slots[2].words = {0x4000};
	using pbd::practice::after_contact;
	offense_rows rows;
	rows.reaction = {after_contact::action, 2}; // Dummy Action 3
	drop_empty_reaction(rows, slots);
	check(rows.reaction == pbd::practice::contact_reaction{after_contact::action, 2});
	rows.reaction = {after_contact::action, 3}; // Dummy Action 4, empty
	drop_empty_reaction(rows, slots);
	check(rows.reaction == pbd::practice::contact_reaction{});
	rows.reaction = {after_contact::movement, 0, pbd::practice::movement::reaction_input::fuzzy_duck}; // an input needs no slot
	drop_empty_reaction(rows, slots);
	check(rows.reaction.kind == after_contact::movement && rows.reaction.input == pbd::practice::movement::reaction_input::fuzzy_duck);
}

PBD_TEST(PbdPracticeStore, OlderActionAfterAHitOrBlockKeepsItsChoice)
{
	using pbd::practice::after_contact;
	using pbd::practice::contact_reaction;
	using pbd::practice::movement::reaction_input;
	const auto offense = [](std::string fields)
	{
		const auto parsed = parse_profile(R"({"schema":"pbd-practice-profile-1","dummies":{"13":{"offense":{)" + fields + "}}}}");
		check(parsed.value, "profile read");
		return parsed.value->dummies.at(13).offense;
	};
	// Before reactionKind the row was one option: 0 None, 1 the Movement row's movement, 2 to 9 Dummy Action 1 to 8.
	check(offense(R"("afterContact":0)").reaction == contact_reaction{}, "None stays None");
	check(offense(R"("afterContact":4)").reaction == contact_reaction{after_contact::action, 2}, "Dummy Action 3 stays Dummy Action 3");
	check(offense(R"("afterContact":9)").reaction == contact_reaction{after_contact::action, 7}, "Dummy Action 8 stays Dummy Action 8");
	check(offense(R"("movement":6,"afterContact":1)").reaction == contact_reaction{after_contact::movement, 0, reaction_input::sidestep_right},
		"a sidestep after contacts becomes the Sidestep Right input");
	check(offense(R"("movement":9,"afterContact":1)").reaction == contact_reaction{after_contact::movement, 0, reaction_input::crouch},
		"a crouch after contacts becomes the Crouch input");
	check(offense(R"("movement":4,"afterContact":1)").reaction == contact_reaction{after_contact::movement, 0, reaction_input::backdash},
		"a backdash after contacts becomes the Backdash input");
	check(offense(R"("movement":1,"afterContact":1)").reaction == contact_reaction{} && offense(R"("movement":1,"afterContact":1)").movement == preset::wavedash,
		"a technique has no input to become, so the row is None and the technique plays on its own");
	check(offense(R"("afterContact":1,"reactionKind":0)").reaction == contact_reaction{}, "the new fields win over the old one");
	// The new fields round-trip.
	profile value;
	value.dummies[3].offense.reaction = {after_contact::movement, 0, reaction_input::fuzzy_duck};
	value.dummies[4].offense.reaction = {after_contact::action, 5};
	value.dummies[5].offense.reaction = {after_contact::moves};
	check(parse_profile(serialize(value, "x")).value == value, "every kind of reaction survives a reload");
}

PBD_TEST(PbdPracticeStore, SlotMovesFromTheMoveList)
{
	auto value = empty_recordings(7);
	value.slots[1].moves = {12, 40};
	value.slots[2].words = {0x4000};
	value.slots[2].moves = {3};
	const auto text = serialize(value, "x");
	const auto parsed = parse_recordings(text, 7);
	check(parsed.value && *parsed.value == value, "a slot's moves and the recording kept beside them survive a reload");
	const auto document = pbd::json::detail::json_parser(text).parse();
	check(document.value);
	const auto* slots = document.value->find("slots");
	check(slots && slots->kind == pbd::json::json_kind::array && slots->items.size() == value.slots.size());
	for (std::size_t i = 0; i < value.slots.size(); ++i)
		check((slots->items[i].find("moves") != nullptr) == !value.slots[i].moves.empty(), "only a slot with moves writes them");
	const auto broken = parse_recordings(changed(text, "[12,40]", "[12,12]"), 7);
	check(broken.value && broken.value->slots[1].moves.empty() && broken.value->slots[2].moves == std::vector<u32>{3},
		"a broken list of moves is dropped, and the slot keeps its recording");
	// A slot that has moves but no recording can be Offense's Recording reaction.
	using pbd::practice::after_contact;
	offense_rows rows;
	rows.reaction = {after_contact::action, 1};
	drop_empty_reaction(rows, value);
	check(rows.reaction == pbd::practice::contact_reaction{after_contact::action, 1}, "a slot with moves is not empty");
}
PBD_TEST(PbdPracticeStore, FilesAreSetAsideWhenUnreadable)
{
	scratch dir;
	check(load_profile(dir.root).state == load_state::absent && load_recordings(dir.root, 3).state == load_state::absent);
	check(commit_text(profile_path(dir.root), serialize(full_profile(), "x")));
	check(commit_text(recordings_path(dir.root, 3), serialize(empty_recordings(3), "x")));
	check(std::filesystem::exists(dir.root / "recordings" / "3" / "autosave.json") && dir.count("profile.json.tmp") == 0);
	const auto profile_read = load_profile(dir.root);
	check(profile_read.state == load_state::loaded && profile_read.value == full_profile());
	check(load_recordings(dir.root, 3).state == load_state::loaded);
	// Another character's file, a broken file and an oversized one are renamed aside, and Practice starts fresh.
	check(commit_text(recordings_path(dir.root, 4), serialize(empty_recordings(3), "x")));
	const auto other = load_recordings(dir.root, 4);
	check(other.state == load_state::set_aside && other.error == "bad_character" && other.value == empty_recordings(4));
	check(!std::filesystem::exists(recordings_path(dir.root, 4)) && dir.count("autosave.unreadable-") == 1);
	check(commit_text(profile_path(dir.root), "{\"schema\":"));
	const auto broken = load_profile(dir.root);
	check(broken.state == load_state::set_aside && broken.value == profile{} && dir.count("profile.unreadable-") == 1);
	check(commit_text(profile_path(dir.root), std::string(pbd::json::max_file_bytes + 1, ' ')));
	check(load_profile(dir.root).error == "file_too_large" && dir.count("profile.unreadable-") == 2);
	check(load_profile(dir.root).state == load_state::absent);
}

PBD_TEST(PbdPracticeStore, AutosaveRetriesFailedSettingsAndThePreviousDummy)
{
	scratch dir;
	std::map<std::filesystem::path, unsigned> attempts;
	memory store([&](const std::filesystem::path& file, const std::string& text)
		{
			return ++attempts[file] > 1 && commit_text(file, text);
		});
	store.root = dir.root;
	store.library[12] = empty_recordings(12);
	const auto settings = full_profile();
	auto take = empty_recordings(3);
	take.slots[0].words = {0, 0x4000, 0x4040, 0};
	store.autosave(settings, take, "x");
	store.files.wait_idle();
	check(load_profile(dir.root).state == load_state::absent && load_recordings(dir.root, 3).state == load_state::absent,
		"the first commits fail");
	check(store.saved == settings && store.library.at(3) == take, "failed writes retain the live settings and take");
	store.autosave(settings, store.library.at(12), "y");
	store.files.wait_idle();
	check(attempts.at(profile_path(dir.root)) == 2 && attempts.at(recordings_path(dir.root, 3)) == 2,
		"unchanged settings and the previous dummy retry on the next save check");
	const auto reopened = load_profile(dir.root);
	const auto recordings = load_recordings(dir.root, 3);
	check(reopened.state == load_state::loaded && reopened.value == settings &&
			  recordings.state == load_state::loaded && recordings.value == take,
		"reopening restores both successful retries");
}

PBD_TEST(PbdPracticeStore, WriterKeepsTheLatestText)
{
	std::promise<void> started, release;
	auto release_future = release.get_future().share();
	std::mutex mutex;
	std::vector<std::string> committed;
	bool first = true;
	{
		writer files([&](const std::filesystem::path&, const std::string& text)
			{
				bool wait = false;
				{
					std::lock_guard lock(mutex);
					committed.push_back(text);
					wait = std::exchange(first, false);
				}
				if (wait)
				{
					started.set_value();
					release_future.wait();
				}
				return text != "fail";
			});
		auto initial = files.submit("profile.json", []
			{
				return std::string("first");
			});
		started.get_future().wait();
		// While the first write runs, three more texts for one file leave only the newest.
		std::vector<std::future<bool>> updates;
		for (const auto* text : {"second", "third", "fourth"})
			updates.push_back(files.submit("profile.json", [text]
				{
					return std::string(text);
				}));
		auto failed = files.submit("other.json", []
			{
				return std::string("fail");
			});
		release.set_value();
		files.wait_idle();
		check(initial.get() && !updates[0].get() && !updates[1].get() && updates[2].get() && !failed.get(),
			"only committed texts succeed; superseded texts and failed commits report failure");
	}
	check(committed.size() == 3 && committed[0] == "first");
	check(std::count(committed.begin(), committed.end(), "fourth") == 1 && std::count(committed.begin(), committed.end(), "second") == 0);
	// The real commit, and the writer's destructor finishing what is pending.
	scratch dir;
	{
		writer files;
		files.submit(profile_path(dir.root), []
			{
				return serialize(full_profile(), "x");
			});
	}
	check(load_profile(dir.root).value == full_profile());
}

// Every dummy's saved slots are read at the start.
PBD_TEST(PbdPracticeStore, Library)
{
	scratch dir;
	check(commit_text(recordings_path(dir.root, 3), serialize(empty_recordings(3), "x")));
	auto twelve = empty_recordings(12);
	twelve.slots[1].words = {0x4000, 0x4000};
	check(commit_text(recordings_path(dir.root, 12), serialize(twelve, "x")));
	check(commit_text(recordings_path(dir.root, 7), serialize(empty_recordings(8), "x")));
	std::filesystem::create_directories(dir.root / "recordings" / "abc");
	std::filesystem::create_directories(dir.root / "recordings" / "035");
	const auto found = load_library(dir.root);
	check(found.dummies.size() == 2 && found.dummies.at(12) == twelve && found.dummies.at(3) == empty_recordings(3));
	check(found.set_aside.size() == 1 && found.set_aside[0] == "7: bad_character");
	check(load_library(dir.root / "missing").dummies.empty());
}

namespace
{
	using pbd::json::json_value;
	json_value json_input(std::string_view source)
	{
		auto parsed = pbd::json::detail::json_parser(source).parse();
		check(parsed.value);
		return std::move(*parsed.value);
	}
	profile profile_field(std::string field, json_value value)
	{
		auto parsed = parse_profile(pbd::json::detail::serialize(
			json_value::object({{"schema", json_value::string(std::string(profile_schema))}, {std::move(field), std::move(value)}})));
		check(parsed.value, parsed.error);
		return std::move(*parsed.value);
	}
	void replace_field(json_value& object, std::string name, json_value value)
	{
		for (auto& [key, item] : object.fields)
		{
			if (key == name)
			{
				item = std::move(value);
				return;
			}
		}
		object.fields.emplace_back(std::move(name), std::move(value));
	}
	std::array<bool, 6> display_flags(const display& value)
	{
		return {value.command_history[0], value.command_history[1], value.attack_info,
			value.frame_data, value.control_hints, value.movement_streaks};
	}
	recordings recording_fields(json_value playback, json_value slots)
	{
		auto parsed = parse_recordings(
			pbd::json::detail::serialize(json_value::object({{"schema", json_value::string(std::string(recordings_schema))},
				{"character", detail::code(13)}, {"playback", std::move(playback)}, {"slots", std::move(slots)}})),
			13);
		check(parsed.value, parsed.error);
		return std::move(*parsed.value);
	}
	slot slot_field(std::string field, json_value value)
	{
		auto item = slot_json(empty_recordings(13).slots[0]);
		replace_field(item, "words", json_value::string(encode_words({0, 0x8040, 0x4000, 0})));
		replace_field(item, std::move(field), std::move(value));
		return recording_fields(json_value::object(), json_value::array({std::move(item)})).slots[0];
	}
} // namespace

PBD_TEST(PbdPracticeStore, EveryDisplayFlagAcceptsOnlyBooleans)
{
	constexpr std::array names{"p1History", "p2History", "attackInfo", "frameData", "controlHints", "movementStreaks"};
	for (std::size_t i = 0; i < names.size(); ++i)
	{
		const auto disabled = display_flags(profile_field("display", json_value::object({{names[i], json_value::boolean(false)}})).view);
		for (std::size_t bit = 0; bit < disabled.size(); ++bit)
			check(disabled[bit] == (bit != i));
		for (const auto invalid : {"null", "0", "1", "\"false\"", "[]", "{}"})
		{
			const auto read = profile_field("display", json_value::object({{names[i], json_input(invalid)}}));
			check(read.view == display{});
		}
	}
	for (const auto invalid : {"null", "0", "false", "[]", "\"display\""})
		check(profile_field("display", json_input(invalid)).view == display{});
}

PBD_TEST(PbdPracticeStore, EveryDummyRowClampsOrDefaultsInvalidValues)
{
	struct row
	{
		const char* group;
		const char* name;
		u32 low, high, fallback;
		u32 (*read)(const dummy_rows&);
	};
	const std::array<row, 11> rows{{
		{"offense", "movement", 0, pbd::practice::movement::preset_count - 1, 0,
			[](const dummy_rows& v)
			{
				return static_cast<u32>(v.offense.movement);
			}},
		{"offense", "wavedashSpeed", 2, 8, 3, [](const dummy_rows& v)
			{
				return v.offense.wavedash_speed;
			}},
		{"offense", "reactionKind", 0, 3, 0, [](const dummy_rows& v)
			{
				return static_cast<u32>(v.offense.reaction.kind);
			}},
		{"offense", "reactionInput", 0, 6, 0, [](const dummy_rows& v)
			{
				return static_cast<u32>(v.offense.reaction.input);
			}},
		{"offense", "reactionSlot", 0, 7, 0, [](const dummy_rows& v)
			{
				return v.offense.reaction.slot;
			}},
		{"offense", "counted", 0, 2, 2, [](const dummy_rows& v)
			{
				return static_cast<u32>(v.offense.counted);
			}},
		{"offense", "times", 1, 5, 1, [](const dummy_rows& v)
			{
				return v.offense.times;
			}},
		{"defense", "movement", 0, pbd::practice::movement::preset_count - 1, 0, [](const dummy_rows& v)
			{
				return static_cast<u32>(v.defense.movement);
			}},
		{"defense", "wavedashSpeed", 2, 8, 3, [](const dummy_rows& v)
			{
				return v.defense.wavedash_speed;
			}},
		{"defense", "wavedashEnd", 0, 2, 0, [](const dummy_rows& v)
			{
				return static_cast<u32>(v.defense.wavedash_end);
			}},
		{"defense", "dashEnd", 0, 1, 0, [](const dummy_rows& v)
			{
				return static_cast<u32>(v.defense.dash_end);
			}},
	}};
	for (const auto& rule : rows)
	{
		const auto read = [&](json_value value)
		{
			const auto profile = profile_field("dummies",
				json_value::object({{"13", json_value::object({{rule.group, json_value::object({{rule.name, std::move(value)}})}})}}));
			return rule.read(profile.dummies.at(13));
		};
		check(read(detail::code(0)) == rule.low);
		check(read(detail::code(rule.high + 1)) == rule.high);
		check(read(json_input("9007199254740991")) == rule.high);
		for (const auto invalid : {"-1", "null", "false", "[]", "{}", "\"3\"", "1.5", "1e3", "9007199254740992"})
			check(read(json_input(invalid)) == rule.fallback);
	}
}

PBD_TEST(PbdPracticeStore, AllGameOptionsNeedNumbersAndStayInMenuRanges)
{
	for (std::size_t i = 0; i < game_option_names.size(); ++i)
	{
		for (const auto invalid : {"null", "false", "[]", "{}", "\"1\""})
		{
			auto options = json_value::object();
			for (const auto name : game_option_names)
				detail::put(options, std::string(name), detail::code(0));
			replace_field(options, std::string(game_option_names[i]), json_input(invalid));
			check(!profile_field("gameOptions", options).options);
		}
		for (const auto number : {"-1", "1.5", "1e3", "9007199254740992"})
		{
			auto options = json_value::object();
			for (const auto name : game_option_names)
				detail::put(options, std::string(name), detail::code(0));
			replace_field(options, std::string(game_option_names[i]), json_input(number));
			check(profile_field("gameOptions", options).options == game_options{0, 0, 0, 1, 0});
		}
		auto options = json_value::object();
		for (const auto name : game_option_names)
			detail::put(options, std::string(name), detail::code(0));
		replace_field(options, std::string(game_option_names[i]), json_input("9007199254740991"));
		auto expected = game_options{0, 0, 0, 1, 0};
		expected[i] = pbd::practice::game_option_maximum[i];
		check(profile_field("gameOptions", options).options == expected);
	}
}

PBD_TEST(PbdPracticeStore, SlotFlagsCountsNamesAndPlaybackStaySafe)
{
	for (const auto name : {"trim", "enabled"})
	{
		const auto off = slot_field(name, json_value::boolean(false));
		check(std::string_view(name) == "trim" ? !off.trim && off.enabled : off.trim && !off.enabled);
		for (const auto invalid : {"null", "0", "1", "\"false\"", "[]", "{}"})
		{
			const auto read = slot_field(name, json_input(invalid));
			check(read.trim && read.enabled);
		}
	}
	for (const auto name : {"leadingNeutral", "trailingNeutral", "stopChord", "frequency", "interval"})
	{
		for (const auto invalid : {"null", "-1", "false", "[]", "{}", "\"3\"", "1.5", "1e3", "9007199254740992"})
		{
			const auto read = slot_field(name, json_input(invalid));
			check(read.leading_neutral == 0 && read.trailing_neutral == 0 && read.stop_chord == 0);
			check(read.frequency == 1 && read.interval == 1);
		}
		const auto high = slot_field(name, json_input("9007199254740991"));
		check(high.stop_chord + high.leading_neutral + high.trailing_neutral <= high.words.size());
		check(high.frequency <= 10 && high.interval <= 3);
	}
	for (const auto invalid : {"null", "0", "false", "[]", "{}", "\"\"", "\"bad\\nname\"", "\"bad\\u007fname\""})
		check(slot_field("name", json_input(invalid)).name == "Slot 1");
	check(slot_field("name", json_value::string(std::string(33, 'a'))).name == "Slot 1");
	check(slot_field("name", json_value::string(std::string(32, 'a'))).name == std::string(32, 'a'));
	for (const auto invalid : {"null", "0", "false", "[]", "{}", "\"A\"", "\"AA\"", "\"AAB\"", "\"AAA=\""})
		check(slot_field("words", json_input(invalid)).words.empty());
	for (const auto invalid : {"null", "-1", "false", "[]", "{}", "\"3\"", "1.5", "1e3", "9007199254740992"})
		check(recording_fields(json_value::object({{"interval", json_input(invalid)}}), json_value::array()).interval == default_interval);
	check(recording_fields(json_value::object({{"interval", json_input("9007199254740991")}}), json_value::array()).interval == 3);
}

PBD_TEST(PbdPracticeStore, DocumentBudgetsDuplicateKeysAndUtf8AreEnforced)
{
	const auto profile = serialize(full_profile(), "x");
	const auto recording = serialize(empty_recordings(13), "x");
	const auto refused = [](const std::string& text, bool slots)
	{
		check(slots ? !parse_recordings(text, 13).value : !parse_profile(text).value);
	};
	for (const auto slots : {false, true})
	{
		const auto& valid = slots ? recording : profile;
		const auto closing = valid.find_last_of('}');
		const unsigned cuts = slots ? 64 : 256;
		for (unsigned cut = 0; cut <= cuts; ++cut)
			refused(valid.substr(0, closing * cut / cuts), slots);
		refused(valid + std::string(pbd::json::max_file_bytes, ' '), slots);
		refused(changed(valid, "\"schema\":", "\"schema\":null,\"schema\":"), slots);
		refused(
			changed(valid, "\"savedAt\":\"x\"", "\"savedAt\":\"x\",\"note\":\"" + std::string(1, static_cast<char>(0xff)) + "\""), slots);
		refused(
			changed(valid, "\"savedAt\":\"x\"", "\"savedAt\":\"x\",\"note\":\"" + std::string(max_string_bytes + 1, 'a') + "\""), slots);
		refused(std::string("\xEF\xBB\xBF") + valid, slots);
		refused(valid + "{}", slots);
	}
	refused(changed(profile, "\"times\":3", "\"times\":3,\"times\":1"), false);
	refused(R"({"schema":"pbd-practice-profile-1","trainingMode":"defense","training\u004dode":"offense"})", false);
	refused(changed(recording, "\"frequency\":1", "\"frequency\":1,\"frequency\":2"), true);
}
