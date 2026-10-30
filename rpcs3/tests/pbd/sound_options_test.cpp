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
	context.checkpoint_loaded_generation = 0;
	check(make_plan(defaults, context, read).error == "checkpoint_not_loaded");
	context.checkpoint_loaded_generation = 6;
	check(make_plan(defaults, context, read).error == "checkpoint_not_loaded");
	context = good_context;
	context.current_ppu_id++;
	check(make_plan(defaults, context, read).error == "not_owner_boundary");
	context = good_context;
	context.at_input_boundary = false;
	check(make_plan(defaults, context, read).error == "not_owner_boundary");
	context = good_context;
	context.emulation_generation = context.checkpoint_loaded_generation = 0;
	check(!make_plan(defaults, context, read).value);
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
	invalid.se = 99;
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

PBD_TEST(PbdSoundOptions, Checks)
{
	using namespace pbd::sound_options;
	const auto defaults = parse("PBD-SOUND-1\nspeaker=1\nbgm=12\nse=12\nstage=12\nsubtitle=0\n");
	check(defaults.value && defaults.value->speaker == 1 && defaults.value->bgm == 12 && defaults.value->subtitle == 0);
	const auto quiet = parse("PBD-SOUND-1\r\nsubtitle=1\r\nstage=0\r\nse=1\r\nbgm=0\r\nspeaker=0\r\n");
	check(quiet.value && quiet.value->speaker == 0 && quiet.value->stage == 0 && quiet.value->subtitle == 1);
	for (const auto bad : {
			 "PBD-SOUND-2\nspeaker=1\nbgm=12\nse=12\nstage=12\nsubtitle=0\n",
			 "PBD-SOUND-1\nspeaker=1\nbgm=12\nse=12\nstage=12\n",
			 "PBD-SOUND-1\nspeaker=1\nbgm=12\nse=12\nstage=12\nsubtitle=0\nspeaker=0\n",
			 "PBD-SOUND-1\nspeaker=2\nbgm=12\nse=12\nstage=12\nsubtitle=0\n",
			 "PBD-SOUND-1\nspeaker=1\nbgm=13\nse=12\nstage=12\nsubtitle=0\n",
			 "PBD-SOUND-1\nspeaker=1\nbgm=12\nse=13\nstage=12\nsubtitle=0\n",
			 "PBD-SOUND-1\nspeaker=1\nbgm=12\nse=12\nstage=13\nsubtitle=0\n",
			 "PBD-SOUND-1\nspeaker=1\nbgm=12\nse=12\nstage=12\nsubtitle=2\n",
			 "PBD-SOUND-1\nspeaker=-1\nbgm=12\nse=12\nstage=12\nsubtitle=0\n",
			 "PBD-SOUND-1\nspeaker=+1\nbgm=12\nse=12\nstage=12\nsubtitle=0\n",
			 "PBD-SOUND-1\nspeaker= 1\nbgm=12\nse=12\nstage=12\nsubtitle=0\n",
			 "PBD-SOUND-1\nspeaker=4294967296\nbgm=12\nse=12\nstage=12\nsubtitle=0\n",
			 "PBD-SOUND-1\nspeaker=1\nbgm=12\nse=12\nstage=12\nsubtitle=0\naddress=123\n",
		 })
		check(!parse(bad).value);
	check(!parse(std::string(max_file_bytes + 1, 'x')).value);
	std::string nul = "PBD-SOUND-1\nspeaker=1\nbgm=12\nse=12\nstage=12\nsubtitle=0\n";
	nul.insert(24, 1, '\0');
	check(!parse(nul).value);

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
	runtime_context context{"NPUB30009", executable_sha256, 7, 7, 0x1000001, 0x1000001, true, 3, 44};
	const auto plan = make_plan(*quiet.value, context, read);
	check(plan.value && plan.value->emulation_generation == 7 && plan.value->owner_ppu_id == 0x1000001);
	check(plan.value->setters[0].item_argument == 0x106846e4 && plan.value->setters[0].value_argument == 0 && plan.value->setters[0].expected_callback == 0x26b44c);
	check(plan.value->setters[1].item_argument == 0x106846f0 && plan.value->setters[1].value_argument == 0 && plan.value->setters[1].expected_callback == 0x26b4bc);
	check(plan.value->setters[2].item_argument == 0x106846fc && plan.value->setters[2].value_argument == 1 && plan.value->setters[2].expected_callback == 0x26b534);
	check(plan.value->setters[3].item_argument == 0x10684708 && plan.value->setters[3].value_argument == 0 && plan.value->setters[3].expected_callback == 0x26b5ac);
	check(plan.value->setters[4].item_argument == 0x10684714 && plan.value->setters[4].value_argument == 1 && plan.value->setters[4].expected_callback == 0x26b7f8);
	for (const auto& request : plan.value->setters)
		check(request.entry == 0x1e9e8 && request.toc == 0x470540);

	sound_context_checks(*defaults.value, context, read);
	sound_memory_checks(memory, *defaults.value, context, read);
}
