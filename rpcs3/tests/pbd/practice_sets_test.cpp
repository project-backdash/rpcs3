#include "pbd_test.h"

#include "Emu/PBD/pbd_practice_sets.h"

using pbd::test::check;
using namespace pbd::practice_store;

namespace
{
	constexpr auto saved_at = "2000-01-02T03:04:05.006Z";
	struct directory
	{
		std::filesystem::path root = std::filesystem::temp_directory_path() /
		                             ("pbd-practice-sets-test-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
		~directory()
		{
			std::error_code ec;
			std::filesystem::remove_all(root, ec);
		}
	};
	struct release_gate
	{
		std::promise<void>& release;
		bool pending = true;
		void finish()
		{
			if (std::exchange(pending, false))
				release.set_value();
		}
		~release_gate()
		{
			finish();
		}
	};
	set_content three_actions(u32 character = 13)
	{
		set_content out{empty_recordings(character), {}};
		out.actions.interval = 2;
		out.playback.movement = pbd::practice::movement::preset::sidewalk_left;
		for (u32 i = 0; i < 3; ++i)
		{
			auto& each = out.actions.slots[i];
			each.words = {0, 0x8000, 0x8040, 0x4000, 0};
			each.leading_neutral = each.trailing_neutral = 1;
			each.frequency = i + 1;
			each.interval = i;
			each.trim = i != 1;
			each.enabled = i != 2;
		}
		return out;
	}
} // namespace

PBD_TEST(PbdPracticeSets, SaveLoadOverwriteAndTenPositions)
{
	directory dir;
	writer files;
	recording_sets sets;
	const auto first = three_actions();
	check(sets.submit(files, dir.root, 1, first, saved_at));
	check(sets.pending(13));
	check(!sets.find(13, 1));
	check(!sets.submit(files, dir.root, 2, first, saved_at));
	files.wait_idle();
	const auto result = sets.poll();
	check(result.size() == 1 && result[0].saved && result[0].position == 1);
	check(!sets.pending(13));
	check(sets.find(13, 1)->content == first);
	check(action_count(first) == 3);
	check(load_set(dir.root, 13, 1).value.content == first);
	check(set_path(dir.root, 13, 1) == dir.root / "recordings" / "13" / "set-2.json");
	auto replacement = first;
	replacement.actions.slots[0].words[1] = 0x2000;
	check(sets.submit(files, dir.root, 1, replacement, "2001-02-03T04:05:06Z"));
	files.wait_idle();
	check(sets.poll()[0].saved);
	check(load_set(dir.root, 13, 1).value.content == replacement);
	check(load_set(dir.root, 13, 1).value.saved_at == "2001-02-03T04:05:06Z");
	for (u32 position = 0; position < recording_set_count; ++position)
	{
		check(sets.submit(files, dir.root, position, first, saved_at));
		files.wait_idle();
		check(sets.poll()[0].saved);
	}
	recording_sets fresh;
	fresh.read(dir.root);
	for (u32 position = 0; position < recording_set_count; ++position)
		check(fresh.find(13, position)->content == first);
}

PBD_TEST(PbdPracticeSets, FailedSaveKeepsOldSetAndUnsavedChanges)
{
	directory dir;
	writer files;
	recording_sets sets;
	const auto first = three_actions();
	check(sets.submit(files, dir.root, 1, first, saved_at));
	files.wait_idle();
	check(sets.poll()[0].saved);
	auto edited = first;
	edited.actions.slots[0].enabled = false;
	writer failing([](const auto&, const auto&)
		{
			return false;
		});
	check(sets.submit(failing, dir.root, 1, edited, saved_at));
	failing.wait_idle();
	check(!sets.poll()[0].saved);
	check(sets.find(13, 1)->content == first);
	check(sets.changed(edited));
	check(load_set(dir.root, 13, 1).value.content == first);
}

PBD_TEST(PbdPracticeSets, NamesSurviveRelaunchOverwriteAndFailedWrites)
{
	directory dir;
	writer files;
	recording_sets sets;
	auto content = three_actions();
	content.actions.slots[7].moves = {11, 12};
	content.actions.slots[7].frequency = 4;
	check(action_count(content) == 4);
	check(sets.submit(files, dir.root, 3, content, saved_at, "Low parry drills 2"));
	files.wait_idle();
	check(sets.poll()[0].saved);
	check(set_name(sets.find(13, 3), 3) == "Low parry drills 2");
	check(set_name(nullptr, 0) == "Set 1");
	recording_sets reopened;
	reopened.read(dir.root);
	check(reopened.find(13, 3)->name == "Low parry drills 2");
	check(reopened.select(13, 3) == content);
	check(!reopened.find(12, 3));
	writer failing([](const auto&, const auto&)
		{
			return false;
		});
	check(reopened.submit(failing, dir.root, 3, content, saved_at, "Replacement"));
	failing.wait_idle();
	check(!reopened.poll()[0].saved);
	check(set_name(reopened.find(13, 3), 3) == "Low parry drills 2");
	check(load_set(dir.root, 13, 3).value.name == "Low parry drills 2");
	check(reopened.submit(files, dir.root, 3, content, saved_at, "Punish drills"));
	files.wait_idle();
	check(reopened.poll()[0].saved);
	check(load_set(dir.root, 13, 3).value.name == "Punish drills");
}

PBD_TEST(PbdPracticeSets, NamesAreOptionalForOldSetsAndValidatedForNewOnes)
{
	const auto content = three_actions();
	const auto old = parse_set(serialize_set({content, saved_at}), 13);
	check(old.value && old.value->name.empty());
	check(set_name(&*old.value, 9) == "Set 10");
	for (const std::string name : {std::string("Punish \"low\""), std::string("D\xC3\xA9"
																			  "fense"),
			 std::string(32, 'x')})
	{
		const auto parsed = parse_set(serialize_set({content, saved_at, name}), 13);
		check(parsed.value && parsed.value->name == name && parsed.value->content == content);
	}
	directory dir;
	writer files;
	recording_sets sets;
	for (const auto& name : std::vector<std::string>{" ", " leading", "trailing ", "bad\nname", "bad\tname", "\xC0\xAF", std::string(33, 'x')})
	{
		check(!valid_set_name(name));
		check(!sets.submit(files, dir.root, 0, content, saved_at, name));
	}
	auto malformed = serialize_set({content, saved_at, "Valid"});
	malformed.replace(malformed.find("\"name\":\"Valid\""), 14, "\"name\":\" \"");
	check(parse_set(malformed, 13).error == "bad_set_name");
	check(!std::filesystem::exists(set_path(dir.root, 13, 0)) && !sets.find(13, 0));
}

PBD_TEST(PbdPracticeSets, NameEntryOffersOnlyWorkingOptionsAndCanBackOut)
{
	recording_name_editor editor{13, 3};
	using result = recording_name_editor::result;
	check(editor.choices().size() == 12 && !editor.choices()[0].enabled && !editor.choices()[8].enabled);
	check(editor.choose(0) == result::editing);
	check(editor.choose(1) == result::editing && editor.group == 1 && editor.focus == 1);
	check(editor.choices().size() == 11);
	check(editor.choose(4) == result::editing && editor.text == "D");
	check(editor.back() && editor.focus == 1 && editor.group == 0);
	check(editor.choose(4) == result::editing && editor.group == 4);
	check(editor.choose(5) == result::editing && editor.text == "De");
	check(editor.choose(6) == result::editing && editor.text == "Def");
	check(editor.choose(0) == result::editing && editor.group == 0);
	check(editor.choose(8) == result::editing && editor.text == "Def " && editor.name() == "Def");
	check(editor.choose(7) == result::editing && editor.group == 7);
	check(editor.choose(3) == result::editing && editor.text == "Def 2");
	check(editor.back());
	check(editor.choose(0) == result::save);
	check(editor.choose(9) == result::editing && editor.text == "Def ");
	check(editor.choose(10) == result::editing && editor.text.empty());
	check(!editor.back());
	check(editor.choose(11) == result::cancel);
	editor.text = "D\xC3\xA9";
	check(editor.choose(9) == result::editing && editor.text == "D");
	editor.text.assign(max_set_name_bytes, 'x');
	check(!editor.choices()[8].enabled);
	editor.choose(1);
	check(!editor.choices()[1].enabled);
	editor.choose(1);
	check(editor.text.size() == max_set_name_bytes);
	check(editor.choices().size() <= pbd::practice::max_options);
}

PBD_TEST(PbdPracticeSets, ChangesCompareWithSavedOrLoadedContentPerCharacter)
{
	directory dir;
	writer files;
	recording_sets sets;
	auto live = three_actions();
	check(sets.changed(live));
	check(!sets.changed({empty_recordings(12), {}}));
	check(sets.submit(files, dir.root, 1, live, saved_at));
	files.wait_idle();
	check(sets.poll()[0].saved);
	check(!sets.changed(live));
	live.actions.slots[0].frequency++;
	check(sets.changed(live));
	const auto loaded = sets.select(13, 1);
	check(loaded && !sets.changed(*loaded));
	check(loaded->actions.slots[0].frequency == 1);
	check(!sets.select(12, 1));
	check(sets.changed(three_actions(12)));
	recording_sets reopened;
	reopened.read(dir.root);
	reopened.begin_character(*loaded);
	check(!reopened.changed(*loaded));
	check(reopened.changed(live));
	const set_content cleared{empty_recordings(13), {}};
	check(reopened.changed(cleared));
	reopened.begin_character(cleared);
	check(reopened.changed(cleared));
}

PBD_TEST(PbdPracticeSets, UnsavedInitialContentStaysDirtyAfterClearingOrChangingCharacters)
{
	recording_sets sets;
	const set_content empty{empty_recordings(12), {}};
	const auto unsaved = three_actions(13);
	sets.begin_character(empty);
	check(!sets.changed(empty));
	check(sets.changed(three_actions(12)));
	sets.begin_character(unsaved);
	check(sets.changed(unsaved));
	check(sets.changed({empty_recordings(13), {}}));
	sets.begin_character({empty_recordings(13), {}});
	check(sets.changed(unsaved));
	check(!sets.changed(empty));
}

PBD_TEST(PbdPracticeSets, PendingWritesPreserveTheOldSetAndLaterEdits)
{
	directory dir;
	writer initial;
	recording_sets sets;
	const auto original = three_actions();
	check(sets.submit(initial, dir.root, 1, original, saved_at));
	initial.wait_idle();
	check(sets.poll()[0].saved);
	std::promise<void> started, release;
	auto released = release.get_future().share();
	bool first = true;
	writer blocked([&](const auto& path, const auto& text)
		{
			if (std::exchange(first, false))
				started.set_value();
			released.wait();
			return commit_text(path, text);
		});
	release_gate gate{release};
	auto captured = original;
	captured.actions.slots[0].frequency = 5;
	check(sets.submit(blocked, dir.root, 1, captured, saved_at));
	started.get_future().wait();
	check(sets.poll().empty());
	check(sets.find(13, 1)->content == original);
	check(!sets.select(13, 1));
	check(!sets.submit(blocked, dir.root, 2, original, saved_at));
	check(sets.submit(blocked, dir.root, 1, three_actions(14), saved_at));
	auto edited = captured;
	edited.actions.slots[0].words[1] = 0x2000;
	check(sets.changed(edited));
	gate.finish();
	blocked.wait_idle();
	const auto completed = sets.poll();
	check(completed.size() == 2 && completed[0].saved && completed[1].saved);
	check(sets.find(13, 1)->content == captured);
	check(sets.find(14, 1)->content == three_actions(14));
	check(!sets.changed(captured) && sets.changed(edited));
	check(load_set(dir.root, 13, 1).value.content == captured);
}

PBD_TEST(PbdPracticeSets, UnknownSchemaAndOversizedFilesAreKeptAside)
{
	directory dir;
	const auto path = set_path(dir.root, 13, 1);
	check(commit_text(path, "{\"schema\":\"pbd-practice-recordings-999\"}"));
	const auto unknown = load_set(dir.root, 13, 1);
	check(unknown.state == load_state::set_aside && unknown.error == "bad_schema");
	check(!std::filesystem::exists(path));
	check(commit_text(path, std::string(max_set_bytes + 1, ' ')));
	const auto oversized = load_set(dir.root, 13, 1);
	check(oversized.state == load_state::set_aside && oversized.error == "file_too_large");
	u32 aside = 0;
	for (const auto& entry : std::filesystem::directory_iterator(path.parent_path()))
		aside += entry.path().filename().string().find("set-2.unreadable-") == 0;
	check(aside == 2);
}

PBD_TEST(PbdPracticeSets, RefusesInvalidPositionsDatesAndWrites)
{
	directory dir;
	writer files;
	recording_sets sets;
	const auto valid = three_actions();
	check(!sets.submit(files, dir.root, 10, valid, saved_at));
	check(!sets.submit(files, dir.root, 1, three_actions(base_characters), saved_at));
	check(!sets.submit(files, dir.root, 1, valid, "untrusted\ntext"));
	check(load_set(dir.root, 13, 10).error == "bad_set_position");
	check(!valid_saved_at("2000-01-02T03:04:05.abcZ"));
	check(!valid_saved_at("2000-02-30T03:04:05Z"));
	check(!valid_saved_at("2000-01-02T24:04:05Z"));
	auto bad = valid;
	bad.actions.slots[0].words.resize(max_take_words + 1);
	check(!sets.submit(files, dir.root, 1, bad, saved_at));
	bad = valid;
	bad.actions.slots[0].name = "bad\nname";
	check(!valid_content(bad));
	bad = valid;
	bad.actions.slots[0].frequency = pbd::practice::max_frequency + 1;
	check(!valid_content(bad));
	bad = valid;
	bad.actions.slots[0].stop_chord = 6;
	check(!valid_content(bad));
	using namespace pbd::practice::movement;
	for (const defense_rows invalid : {defense_rows{static_cast<preset>(preset_count)},
			 defense_rows{preset::none, min_wavedash_speed - 1}, defense_rows{preset::none, max_wavedash_speed + 1},
			 defense_rows{preset::none, default_wavedash_speed, static_cast<wavedash_end>(wavedash_end_count)},
			 defense_rows{preset::none, default_wavedash_speed, wavedash_end::back, static_cast<dash_end>(dash_end_count)}})
	{
		bad = valid;
		bad.playback = invalid;
		check(!valid_content(bad));
	}
	// A failed rename must remain a failure even when removing its temporary file succeeds.
	std::filesystem::create_directories(dir.root / "occupied.json");
	check(!commit_text(dir.root / "occupied.json", "{}"));
	check(!std::filesystem::exists(dir.root / "recordings") && !sets.find(13, 1));
}

PBD_TEST(PbdPracticeSets, FullLengthSequencesKeepSideRelativeWordsAndPlayback)
{
	auto live = three_actions();
	for (auto& each : live.actions.slots)
		each.words.resize(max_take_words, 0x8040);
	check(valid_content(live));
	const auto text = serialize_set({live, saved_at});
	check(text.size() < max_set_bytes);
	const auto parsed = parse_set(text, 13);
	check(parsed.value && parsed.value->content == live);
	check(parse_recordings(text, 13).value == live.actions);
	check(!parse_set(text, 12).value);
}

PBD_TEST(PbdPracticeSets, SavedSetBytesKeepTheAutosaveSchema)
{
	constexpr std::string_view golden =
		"{\"schema\":\"pbd-practice-recordings-1\",\"character\":13,\"savedAt\":\"2000-01-02T03:04:05Z\","
		"\"playback\":{\"interval\":3},\"slots\":["
		"{\"words\":\"\",\"leadingNeutral\":0,\"trailingNeutral\":0,\"stopChord\":0,\"trim\":true,\"enabled\":true,"
		"\"name\":\"Slot 1\",\"frequency\":1,\"interval\":1},"
		"{\"words\":\"\",\"leadingNeutral\":0,\"trailingNeutral\":0,\"stopChord\":0,\"trim\":true,\"enabled\":true,"
		"\"name\":\"Slot 2\",\"frequency\":1,\"interval\":1},"
		"{\"words\":\"\",\"leadingNeutral\":0,\"trailingNeutral\":0,\"stopChord\":0,\"trim\":true,\"enabled\":true,"
		"\"name\":\"Slot 3\",\"frequency\":1,\"interval\":1},"
		"{\"words\":\"\",\"leadingNeutral\":0,\"trailingNeutral\":0,\"stopChord\":0,\"trim\":true,\"enabled\":true,"
		"\"name\":\"Slot 4\",\"frequency\":1,\"interval\":1},"
		"{\"words\":\"\",\"leadingNeutral\":0,\"trailingNeutral\":0,\"stopChord\":0,\"trim\":true,\"enabled\":true,"
		"\"name\":\"Slot 5\",\"frequency\":1,\"interval\":1},"
		"{\"words\":\"\",\"leadingNeutral\":0,\"trailingNeutral\":0,\"stopChord\":0,\"trim\":true,\"enabled\":true,"
		"\"name\":\"Slot 6\",\"frequency\":1,\"interval\":1},"
		"{\"words\":\"\",\"leadingNeutral\":0,\"trailingNeutral\":0,\"stopChord\":0,\"trim\":true,\"enabled\":true,"
		"\"name\":\"Slot 7\",\"frequency\":1,\"interval\":1},"
		"{\"words\":\"\",\"leadingNeutral\":0,\"trailingNeutral\":0,\"stopChord\":0,\"trim\":true,\"enabled\":true,"
		"\"name\":\"Slot 8\",\"frequency\":1,\"interval\":1}],"
		"\"defense\":{\"movement\":0,\"wavedashSpeed\":3,\"wavedashEnd\":0,\"dashEnd\":0}}\n";
	const set_content empty{empty_recordings(13), {}};
	check(serialize_set({empty, "2000-01-02T03:04:05Z"}) == golden);
	check(parse_set(golden, 13).value->content == empty);
	check(parse_recordings(golden, 13).value == empty.actions);
}
