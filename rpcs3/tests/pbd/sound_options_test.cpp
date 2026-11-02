#include "pbd_test.h"

#include "Emu/Cell/pbd_sound_options.h"

#include <map>
#include <string>

using pbd::test::check;

static void sound_context_checks(const pbd::sound_options::settings& defaults, pbd::sound_options::runtime_context& context, const auto& read)
{
	using namespace pbd::sound_options;
	const auto good_context = context;
	context.title_id = "NPUB00000";
	check(make_plan(defaults, context, read).error == "unsupported_game");
	context = good_context;
	context.loaded_executable_sha256 = "changed";
	check(make_plan(defaults, context, read).error == "unsupported_game");
	context = good_context;
	context.emulation_generation = 0;
	check(make_plan(defaults, context, read).error == "no_emulation");
	context = good_context;
	context.current_ppu_id++;
	check(make_plan(defaults, context, read).error == "not_owner_boundary");
	context = good_context;
	context.at_input_boundary = false;
	check(make_plan(defaults, context, read).error == "not_owner_boundary");
	context = good_context;
	context.current_ppu_id = context.owner_ppu_id = 0;
	check(!make_plan(defaults, context, read).value);
	context = good_context;
	context.scene = 16;
	check(make_plan(defaults, context, read).error == "clean_select_scene_required");
	context = good_context;
	context.selector = 43;
	check(make_plan(defaults, context, read).error == "clean_select_scene_required");
	context = good_context;
	settings invalid = defaults;
	invalid.se = 15;
	check(make_plan(invalid, context, read).error == "out_of_range");
}

static void sound_memory_checks(std::map<std::uint32_t, std::uint32_t>& memory, const pbd::sound_options::settings& defaults, const pbd::sound_options::runtime_context& context, const auto& read)
{
	using namespace pbd::sound_options;
	const auto good_memory = memory;
	memory[setter_opd + 4]++;
	check(make_plan(defaults, context, read).error == "setter_signature_mismatch");
	for (const auto& word : instructions)
	{
		memory = good_memory;
		memory[word.address] ^= 1;
		check(make_plan(defaults, context, read).error == "callback_signature_mismatch");
	}
	for (const auto& item : bindings)
	{
		for (const auto address : {item.item, item.item + 8, item.vtable + 12, item.callback_opd, item.callback_opd + 4})
		{
			memory = good_memory;
			memory[address]++;
			check(make_plan(defaults, context, read).error == "preference_layout_mismatch");
		}
	}
	memory = good_memory;
	memory.erase(0x26b564);
	check(!make_plan(defaults, context, read).value);
}

PBD_TEST(PbdSoundOptions, ParsesTheLauncherValue)
{
	using namespace pbd::sound_options;
	const auto defaults = parse("speaker=1;bgm=12;se=12;stage=12");
	check(defaults.value && defaults.value->speaker == 1 && defaults.value->bgm == 12 && defaults.value->se == 12 &&
		  defaults.value->stage == 12);
	const auto loud = parse("stage=14;se=13;bgm=0;speaker=0");
	check(loud.value && loud.value->speaker == 0 && loud.value->bgm == 0 && loud.value->se == 13 && loud.value->stage == 14,
		"any key order, and the game's menu goes to 14");
	check(valid(settings{}) && valid(*loud.value));
	check(!valid(settings{2, 12, 12, 12}) && !valid(settings{1, 15, 12, 12}) && !valid(settings{1, 12, 15, 12}) &&
		  !valid(settings{1, 12, 12, 15}));
	for (const auto bad : {
			 "",
			 "speaker=1;bgm=12;se=12",
			 "speaker=1;bgm=12;se=12;stage=12;speaker=0",
			 "speaker=1;bgm=12;se=12;stage=12;subtitle=0",
			 "speaker=1;bgm=12;se=12;stage=12;",
			 ";speaker=1;bgm=12;se=12;stage=12",
			 "speaker=1;;bgm=12;se=12;stage=12",
			 "speaker=1,bgm=12,se=12,stage=12",
			 "speaker=1\nbgm=12\nse=12\nstage=12",
			 "PBD-SOUND-1\nspeaker=1\nbgm=12\nse=12\nstage=12\nsubtitle=0\n",
			 "speaker=2;bgm=12;se=12;stage=12",
			 "speaker=1;bgm=15;se=12;stage=12",
			 "speaker=1;bgm=12;se=15;stage=12",
			 "speaker=1;bgm=12;se=12;stage=15",
			 "speaker=-1;bgm=12;se=12;stage=12",
			 "speaker=+1;bgm=12;se=12;stage=12",
			 "speaker= 1;bgm=12;se=12;stage=12",
			 "speaker=1 ;bgm=12;se=12;stage=12",
			 "speaker=;bgm=12;se=12;stage=12",
			 "=1;bgm=12;se=12;stage=12",
			 "speaker=4294967296;bgm=12;se=12;stage=12",
			 "SPEAKER=1;bgm=12;se=12;stage=12",
		 })
		check(!parse(bad).value);
	check(parse("speaker=1;bgm=12;se=12;stage=12;subtitle=0").error == "unknown_key");
	check(parse("speaker=1;bgm=12;se=12").error == "missing_key");
	check(parse("speaker=1;bgm=12;se=12;stage=15").error == "out_of_range");
	check(parse(std::string(max_text_bytes + 1, '1')).error == "too_long");
	std::string nul = "speaker=1;bgm=12;se=12;stage=12";
	nul.insert(14, 1, '\0');
	check(!parse(nul).value);
}

PBD_TEST(PbdSoundOptions, PlansTheFourSetters)
{
	using namespace pbd::sound_options;
	std::map<std::uint32_t, std::uint32_t> memory{{setter_opd, setter_entry}, {setter_opd + 4, pbd::t5dr::toc}};
	for (const auto& word : instructions)
		memory[word.address] = word.value;
	for (const auto& item : bindings)
	{
		memory[item.item] = item.vtable;
		memory[item.item + 8] = item.saved_byte;
		memory[item.vtable + 12] = item.callback_opd;
		memory[item.callback_opd] = item.callback_entry;
		memory[item.callback_opd + 4] = pbd::t5dr::toc;
	}
	const auto read = [&](std::uint32_t address) -> std::optional<std::uint32_t>
	{
		const auto found = memory.find(address);
		return found == memory.end() ? std::nullopt : std::optional<std::uint32_t>{found->second};
	};
	runtime_context context{"NPUB30009", executable_sha256, 7, 0x1000001, 0x1000001, true, 3, 44};
	const auto loud = parse("speaker=0;bgm=0;se=13;stage=14");
	check(loud.value);
	const auto plan = make_plan(*loud.value, context, read);
	check(plan.value && plan.value->emulation_generation == 7 && plan.value->owner_ppu_id == 0x1000001);
	check(plan.value->setters.size() == 4, "the subtitle preference is never called");
	check(plan.value->setters[0].item_argument == 0x106846e4 && plan.value->setters[0].value_argument == 0 && plan.value->setters[0].expected_callback == 0x26b44c);
	check(plan.value->setters[1].item_argument == 0x106846f0 && plan.value->setters[1].value_argument == 0 && plan.value->setters[1].expected_callback == 0x26b4bc);
	check(plan.value->setters[2].item_argument == 0x106846fc && plan.value->setters[2].value_argument == 13 && plan.value->setters[2].expected_callback == 0x26b534);
	check(plan.value->setters[3].item_argument == 0x10684708 && plan.value->setters[3].value_argument == 14 && plan.value->setters[3].expected_callback == 0x26b5ac);
	for (const auto& request : plan.value->setters)
		check(request.entry == 0x1e9e8 && request.toc == 0x470540);
	for (const auto& item : bindings)
		check(item.saved_byte >= 0x1068496a && item.saved_byte < 0x1068496e, "the saved bytes the setup digest leaves out");

	sound_context_checks(settings{}, context, read);
	sound_memory_checks(memory, settings{}, context, read);
}

PBD_TEST(PbdSoundOptions, ARefusalKeepsPlayingUnlessTheOwnerThreadWasNotPutBack)
{
	using namespace pbd::sound_options;
	check(play_continues({application_status::applied, {}, true, 4}));
	check(play_continues({application_status::waiting, {}, false, 0}));
	check(play_continues({application_status::refused, "clean_select_scene_required", true, 0}));
	check(play_continues({application_status::refused, "setting_readback_mismatch", true, 4}));
	check(!play_continues({application_status::refused, "owner_context_restore_failed", false, 2}));
}
