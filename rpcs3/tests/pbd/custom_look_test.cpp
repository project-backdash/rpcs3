#include "pbd_test.h"
#include "Emu/PBD/pbd_custom_save.h"

#include "Emu/PBD/pbd_custom_look.h"
#include "Emu/PBD/pbd_custom_editor.h"
#include "Emu/PBD/pbd_custom_photo.h"
#include "Emu/PBD/pbd_custom_wire.h"
#include "Emu/PBD/pbd_practice_start.h"
#include "Emu/PBD/pbd_set_channel_model.h"
#include "Emu/pbd_session_mode.h"
#include <stb_image.h>

using pbd::test::check;

PBD_TEST(PbdCustomEditor, CustomizeOwnsItsControllerOnlyInAnOfflineRequest)
{
	using pbd::session_mode::customize_requested;
	check(customize_requested("1", "0", "1", "customize-request.json"));
	check(customize_requested("1", nullptr, "1", "customize-request.json"));
	check(!customize_requested("1", "1", "1", "customize-request.json"));
	check(!customize_requested(nullptr, "0", "1", "customize-request.json"));
	check(!customize_requested("1", "0", "0", "customize-request.json"));
	check(!customize_requested("1", "0", "1", ""));
	check(!customize_requested("1", "0", "1", nullptr));
}

PBD_TEST(PbdCustomEditor, RankOpacityIsLimitedToTheSelectSpriteUpdate)
{
	using namespace pbd::custom_editor;
	check(select_rank_sprite(0x105b18, 0x1139b4));
	check(!select_rank_sprite(0x105b18, 0x1176bc));
	check(!select_rank_sprite(0x1059cc, 0x1139b4));
	check(std::find(profile_draws.begin(), profile_draws.end(), 0x1059cc) == profile_draws.end());
	check(std::find(profile_draws.begin(), profile_draws.end(), 0x1500c8) == profile_draws.end());
}

PBD_TEST(PbdCustomEditor, EditingHasNoDeadlineAndExitCapturesOnce)
{
	using namespace pbd::custom_editor;
	flow entry;
	observation value{true, true, true, 3, 4, 3, 44, 3, 3, 0};
	check(entry.step(value) == action::enter);
	value.selector = value.next_selector = 6;
	value.shop = 2;
	check(entry.step(value) == action::open_items);
	value.shop = 4;
	for (u32 frame = 0; frame < 72000; ++frame)
		check(entry.step(value) == action::wait);
	check(entry.step(value, 0x0900) == action::capture);
	entry.resume();
	check(entry.step(value, 0x0900) == action::wait);
	check(entry.step(value, 0) == action::wait);
	check(entry.step(value, 0x0800) == action::capture);
}

PBD_TEST(PbdCustomSave, RequiresAReleasedPadAndACompletedPhoto)
{
	using namespace pbd::custom_editor;
	save_panel panel;
	check(!panel.begin(0, {}) && !panel.open());
	check(!panel.begin(11, {}) && !panel.open());
	check(panel.begin(3, {}) && panel.slot() == 3);
	check(panel.step(0x40) == save_action::none && panel.open());
	panel.step(0);
	check(panel.step(0x40) == save_action::none && panel.open());
	panel.photo_ready(true);
	check(panel.step(0x40) == save_action::none && panel.open());
	panel.step(0);
	check(panel.step(0x40) == save_action::save && !panel.open() && panel.slot() == 3);
	panel.step(0);
	check(panel.step(0x40) == save_action::none);
}

PBD_TEST(PbdCustomSave, OccupiedSlotsRequireASeparateExplicitReplacement)
{
	using namespace pbd::custom_editor;
	save_panel panel;
	std::array<bool, 10> occupied{};
	occupied[2] = true;
	check(panel.begin(3, occupied));
	panel.photo_ready(true);
	panel.step(0);
	check(panel.step(0x40) == save_action::none && panel.overwrite() && !panel.replace_selected());
	panel.step(0);
	check(panel.step(0x40) == save_action::none && !panel.overwrite() && panel.open());
	panel.step(0);
	panel.step(0x40);
	panel.step(0);
	panel.step(0x2000);
	check(panel.replace_selected());
	panel.step(0);
	check(panel.step(0x20) == save_action::none && !panel.overwrite() && panel.open());
	panel.step(0);
	panel.step(0x40);
	panel.step(0);
	panel.step(0x8000);
	panel.step(0);
	check(panel.step(0x40) == save_action::save && !panel.open() && panel.slot() == 3);
}

PBD_TEST(PbdCustomSave, MovingTheSlotPreservesTheOldLookWithoutAnOverwritePrompt)
{
	using namespace pbd::custom_editor;
	save_panel panel;
	std::array<bool, 10> occupied{};
	occupied[9] = true;
	panel.begin(10, occupied);
	panel.photo_ready(true);
	panel.step(0);
	panel.step(0x2000);
	check(panel.slot() == 1);
	panel.step(0);
	panel.step(0x8000);
	check(panel.slot() == 10);
	panel.step(0);
	panel.step(0x8000);
	check(panel.slot() == 9);
	panel.step(0);
	check(panel.step(0x40) == save_action::save && !panel.overwrite() && panel.slot() == 9);
}

PBD_TEST(PbdCustomSave, PhotoFailureStillAllowsEditingOrDiscarding)
{
	using namespace pbd::custom_editor;
	save_panel panel;
	panel.begin(1, {});
	panel.step(0);
	panel.step(0x4000);
	check(panel.row() == 1 && !panel.can_save());
	panel.step(0);
	panel.step(0x2000);
	check(panel.slot() == 1);
	panel.step(0);
	check(panel.step(0x40) == save_action::keep_editing && !panel.open());
	panel.begin(1, {});
	panel.step(0);
	panel.step(0x1000);
	check(panel.row() == 2);
	panel.step(0);
	check(panel.step(0x40) == save_action::discard && !panel.open());
	panel.begin(1, {});
	panel.step(0);
	check(panel.step(0x20) == save_action::keep_editing && !panel.open());
}

PBD_TEST(PbdCustomSave, LosingThePhotoCancelsPendingReplacement)
{
	using namespace pbd::custom_editor;
	save_panel panel;
	std::array<bool, 10> occupied{};
	occupied.fill(true);
	panel.begin(1, occupied);
	panel.photo_ready(true);
	panel.step(0);
	panel.step(0x40);
	panel.step(0);
	panel.step(0x2000);
	check(panel.overwrite() && panel.replace_selected());
	panel.photo_ready(false);
	panel.step(0);
	check(!panel.overwrite() && !panel.can_save() && panel.step(0x40) == save_action::none && panel.open());
}

PBD_TEST(PbdCustomPhoto, EncodesTheCapturedPixelsAsAPortablePng)
{
	using namespace pbd::custom_photo;
	frame value{1, 2, 3, 2, 2, true, {255, 0, 0, 255, 0, 255, 0, 255, 0, 0, 255, 255, 255, 255, 255, 255}};
	const auto png = encode_png(value);
	check(png.size() > 8);
	int width = 0, height = 0, channels = 0;
	auto* decoded = stbi_load_from_memory(png.data(), static_cast<int>(png.size()), &width, &height, &channels, 4);
	check(decoded && width == 2 && height == 2 && channels == 4);
	const bool identical = std::equal(value.rgba.begin(), value.rgba.end(), decoded);
	stbi_image_free(decoded);
	check(identical);
	value.width = 3;
	check(encode_png(value).empty());
	value.width = 2;
	value.valid = false;
	check(encode_png(value).empty());
}

PBD_TEST(PbdCustomPhoto, PracticePortraitContainsThePlayersSideInsteadOfTheCentre)
{
	using namespace pbd::custom_photo;
	frame source{1, 2, 3, 120, 90, true};
	source.rgba.resize(120 * 90 * 4);
	for (u32 y = 0; y < 90; ++y)
		for (u32 x = 0; x < 120; ++x)
		{
			const auto at = (y * 120 + x) * 4;
			source.rgba[at] = x < 80 ? 255 : 0;
			source.rgba[at + 2] = x < 80 ? 0 : 255;
			source.rgba[at + 3] = 255;
		}
	const auto player = portrait(source, true);
	check(player.valid && player.width == 320 && player.height == 480 && player.generation == 2);
	for (std::size_t at = 0; at < player.rgba.size(); at += 4)
		check(player.rgba[at] == 255 && player.rgba[at + 2] == 0);
	const auto centre = portrait(source);
	check(centre.rgba[centre.rgba.size() - 2] == 255);
	source.valid = false;
	check(!portrait(source, true).valid);
}

PBD_TEST(PbdCustomPhoto, PortraitRequestsRequireAnExplicitCustomPracticeOpening)
{
	const std::string start = R"({"schema":"pbd-practice-start-1","id":"portrait","characterId":"nina_williams","characterRow":13,"costume":0,"stage":0)";
	const std::string look = R"(,"custom":{"items":[65535,65535,65535,65535],"colours":[255,255,255]})";
	const auto parsed = pbd::practice::start::parse(start + look + R"(,"portrait":true})");
	check(parsed.value && parsed.value->portrait && parsed.value->custom);
	check(!pbd::practice::start::parse(start + R"(,"portrait":true})").value);
	check(!pbd::practice::start::parse(start + look + R"(,"portrait":"true"})").value);
	check(!pbd::practice::start::parse(start + look + R"(,"portrait":null})").value);
	const auto ordinary = pbd::practice::start::parse(start + look + "}");
	check(ordinary.value && !ordinary.value->portrait);
}

PBD_TEST(PbdCustomPhoto, CapturesOneFutureGuestFrameAndPreservesItsIdentity)
{
	using namespace pbd::custom_photo;
	channel capture;
	const auto ticket = capture.request(7, 20);
	check(ticket && !capture.request(7, 20));
	check(!capture.wants_frame(8, 21) && !capture.wants_frame(7, 20));
	check(capture.wants_frame(7, 21) == ticket);
	const std::vector<u8> pixels{10, 20, 30, 0, 40, 50, 60, 0};
	capture.deliver(ticket, 8, 21, pixels, 2, 1, true, true);
	check(!capture.take(ticket));
	capture.deliver(ticket, 7, 21, pixels, 2, 1, true, true);
	check(!capture.wants_frame(7, 22));
	const auto result = capture.take(ticket);
	check(result && result->valid && result->generation == 7 && result->flip == 21 && result->width == 2 && result->height == 1);
	check(result->rgba == std::vector<u8>({30, 20, 10, 255, 60, 50, 40, 255}));
	check(!capture.take(ticket));
	const auto next = capture.request(8, 0);
	check(next > ticket);
	capture.cancel(ticket);
	check(capture.wants_frame(8, 1) == next);
	capture.cancel(next);
	check(!capture.wants_frame(8, 1));
}

PBD_TEST(PbdCustomPhoto, FirstUseNeverHoldsInputAndAbandonsAMovingOpening)
{
	using pbd::custom_photo::entry_pose;
	entry_pose pose;
	check(!pose.observe(10, 17, 17) && !pose.holding());
	pose.arm_practice();
	check(!pose.observe(10, 2, 44) && !pose.holding());
	for (unsigned frame = 0; frame < entry_pose::maximum_opening_boundaries; ++frame)
		check(pose.observe(10, 17, 17) && !pose.holding());
	check(!pose.observe(10, 17, 17) && !pose.holding());
	pose.arm_practice();
	check(pose.observe(10, 17, 17));
	pbd::custom_photo::input_serial.fetch_add(1);
	check(!pose.observe(10, 17, 17) && !pose.holding());
	pose.arm_practice();
	pose.finish();
	check(!pose.observe(10, 17, 17));
	for (unsigned phase = 0; phase < 40; ++phase)
	{
		check(pose.observe(4, 17, phase) == (phase == 4));
		check(!pose.holding());
	}
	check(!pose.observe(4, 3, 4));
}

PBD_TEST(PbdCustomPhoto, HiddenLoadingKeepsItsOpeningUntilVisibleButNeverCapturesHiddenFrames)
{
	using pbd::custom_photo::entry_pose;
	entry_pose pose;
	check(!pose.observe(10, 2, 44, false));
	for (unsigned frame = 0; frame < 1200; ++frame)
		check(!pose.observe(10, 17, 17, false) && !pose.holding());
	check(pose.observe(10, 17, 17));
	// Two dispatches per presented frame still leave enough time for the opening fade.
	for (unsigned frame = 0; frame < 250; ++frame)
		check(pose.observe(10, 17, 17));
	pose.observe(10, 2, 44, false);
	pbd::custom_photo::input_serial.fetch_add(1);
	check(!pose.observe(10, 17, 17, false));
	check(!pose.observe(10, 17, 17));
	check(!pose.observe(4, 17, 4, false));
	check(pose.observe(4, 17, 4));
}

PBD_TEST(PbdCustomPhoto, RejectsOversizedIncompleteAndUnfencedFrames)
{
	using namespace pbd::custom_photo;
	for (unsigned bad = 0; bad < 4; ++bad)
	{
		channel capture;
		const auto ticket = capture.request(1, 0);
		const std::array<u8, 4> pixel{};
		capture.deliver(ticket, 1, 1, pixel, bad == 0 ? 0 : bad == 1 ? 4097 :
														bad == 2     ? 2 :
																	   1,
			1, false, bad != 3);
		const auto result = capture.take(ticket);
		check(result && !result->valid && result->rgba.empty());
	}
}

PBD_TEST(PbdCustomEditor, OnlyIsolatedOfflineSelectCanEnter)
{
	using namespace pbd::custom_editor;
	const observation ready{true, true, true, 3, 4, 3, 44, 3, 3, 0};
	check(admissible(ready));
	for (unsigned bad = 0; bad < 7; ++bad)
	{
		auto value = ready;
		if (bad == 0)
			value.verified = false;
		if (bad == 1)
			value.offline = false;
		if (bad == 2)
			value.user = 2;
		if (bad == 3)
			value.mode = 10;
		if (bad == 4)
			value.scene = 17;
		if (bad == 5)
			value.pending = 2;
		if (bad == 6)
			value.user = 1;
		check(flow{}.step(value) == action::refuse);
	}
	auto waiting = ready;
	waiting.parent_ready = false;
	flow entry;
	check(entry.step(waiting) == action::wait);
	check(entry.step(ready) == action::enter);
}

PBD_TEST(PbdCustomEditor, SaveIsolationExpiresWithItsEmulationAndWaitingIsBounded)
{
	using namespace pbd::custom_editor;
	save_generation.store(42);
	save_blocked.store(true);
	check(blocks_saves(42) && !blocks_saves(43));
	save_blocked.store(false);
	check(!blocks_saves(42));
	flow entry;
	const observation waiting{true, true, false, 3, 4, 3, 44, 3, 3, 0};
	for (u32 frame = 0; frame < 3600; ++frame)
		check(entry.step(waiting) == action::wait);
	check(entry.step(waiting) == action::refuse);
	check(exercise_word(0) == 0 && exercise_word(29) == 0 && exercise_word(32) == 0 && exercise_word(152) == 0);
}

PBD_TEST(PbdCustomEditor, OpensItemsOnceAndInterceptsEveryExitBeforeGameStart)
{
	using namespace pbd::custom_editor;
	for (unsigned exit = 0; exit < 4; ++exit)
	{
		flow entry;
		observation value{true, true, true, 3, 4, 3, 44, 3, 3, 0};
		check(entry.step(value) == action::enter);
		value.selector = value.next_selector = 5;
		check(entry.step(value) == action::wait);
		value.selector = value.next_selector = 6;
		value.shop = 1;
		check(entry.step(value) == action::wait);
		value.shop = 2;
		check(entry.step(value) == action::open_items);
		value.shop = 4;
		check(entry.step(value) == action::wait);
		value.shop = 5;
		check(entry.step(value) == action::wait);
		if (exit == 0)
			value.shop = 2;
		if (exit == 1)
			value.shop = 3;
		if (exit == 2)
			value.shop = 6;
		if (exit == 3)
			value.next_selector = 7;
		check(entry.step(value) == action::capture);
		check(entry.step(value) == action::wait);
	}
	flow premature;
	observation value{true, true, true, 3, 4, 3, 44, 3, 3, 0};
	check(premature.step(value) == action::enter);
	value.selector = value.next_selector = 6;
	value.shop = 6;
	check(premature.step(value) == action::refuse);
}

namespace
{
	std::string custom_table_text(const std::vector<std::uint8_t>& data)
	{
		constexpr char digits[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
		std::string out;
		for (std::size_t i = 0; i < data.size(); i += 3)
		{
			const auto word = (std::uint32_t{data[i]} << 16) | (std::uint32_t{data[i + 1]} << 8) | data[i + 2];
			for (int shift = 18; shift >= 0; shift -= 6)
				out += digits[(word >> shift) & 63];
		}
		return out;
	}
} // namespace

PBD_TEST(PbdCustomWire, PackedItemsAndColoursMatchTheLauncher)
{
	using namespace pbd::custom_wire;
	const look wanted{77, {61, 67, 66, 50}, {-1, 3, 11}};
	const auto encoded = pack(wanted);
	check(hex(encoded) == "4d003d004300420032ff030b");
	check(unpack(encoded) == wanted);
	const auto entry = drawn_json({10, encoded});
	check(read_drawn(&entry, 77) == drawn{10, encoded});
	check(!read_drawn(&entry, 78));
	for (const auto invalid : {"4D003d004300420032ff030b", "4d003d004300420032ff030", "4d0000004300420032ff030b", "4d8000004300420032ff030b", "4d003d004300420032ff0c0b"})
	{
		const auto bad = json::object({{"slot", json::number(1)}, {"bytes", json::string(invalid)}});
		check(!read_drawn(&bad, 77));
	}
	for (const u32 slot : {0u, 11u})
	{
		const auto bad = drawn_json({slot, encoded});
		check(!read_drawn(&bad, 77));
	}
	const auto absent = pack(look{77, {127, no_item, no_item, no_item}});
	check(unpack(absent) && shown(*unpack(absent)) == look{77});
}

PBD_TEST(PbdCustomWire, TablesAreBoundedCanonicalAndUnambiguous)
{
	using namespace pbd::custom_wire;
	std::vector<u8> data(block_bytes, 0xff);
	const auto encoded = pack({77, {61, 67, 66, 50}, {-1, 3, 11}});
	std::copy(encoded.begin(), encoded.end(), data.begin() + look_bytes * 9);
	const auto text = custom_table_text(data);
	check(read_table(text) == data);
	const auto request = json::object({{"slot", json::number(10)}, {"table", json::string(text)}});
	const auto picked = read_pick(&request);
	check(picked && resolve(*picked, 77) == drawn{10, encoded} && !resolve(*picked, 78));
	check(!resolve(pick{1, data}, 77));
	check(!read_table(text + "=") && !read_table(" " + text) && !read_table(text.substr(1)) && !read_table(""));
	check(!read_table(std::string(160, '/')));
	auto bad = text;
	bad[0] = '-';
	check(!read_table(bad));
	const auto first = data;
	data.insert(data.end(), first.begin(), first.end());
	const auto duplicate = read_table(custom_table_text(data));
	check(duplicate && !resolve(pick{10, *duplicate}, 77));
	data.resize(maximum_bytes, 0xff);
	for (std::size_t at = 0; at < data.size(); at += block_bytes)
		std::copy(encoded.begin(), encoded.end(), data.begin() + at);
	check(read_table(custom_table_text(data)));
	data.resize(maximum_bytes + block_bytes, 0xff);
	check(!read_table(custom_table_text(data)));
}

namespace
{
	using namespace pbd::custom_look;

	// Lili's first costume (row 77): heads 61 62 63 65 68 72 73 76, faces 67 71 75, uppers 66 69 77, lowers 50 64 70.
	constexpr u32 lili_first = 77, lili_second = 78;

	std::array<u8, blocks_size> saved_blocks()
	{
		std::array<u8, blocks_size> out{};
		for (u32 i = 0; i < blocks_size; ++i)
			out[i] = static_cast<u8>(i * 7 + 3);
		return out;
	}

	u16 item_at(const std::array<u8, blocks_size>& blocks, u32 costume, u32 slot)
	{
		const u32 at = costume * block_size + equipped_offset + slot * 2;
		return static_cast<u16>(blocks[at] << 8 | blocks[at + 1]);
	}
} // namespace

PBD_TEST(PbdCustomLook, TheCatalogueCoversTheRowTable)
{
	using pbd::custom_catalogue::rows;
	check(rows.size() == 86);
	u32 dressable = 0;
	for (const auto& row : rows)
		dressable += row.dressable;
	// 34 fighters with two dressed costumes (Jinpachi has none), and Xiaoyu's and Asuka's third.
	check(dressable == 70);
	check(rows[lili_first].fighter == 31 && rows[lili_first].costume == 0 && rows[lili_first].dressable);
	check(rows[lili_second].fighter == 31 && rows[lili_second].costume == 1 && rows[lili_second].dressable);
	check(rows[18].fighter == 7 && rows[18].costume == 2 && rows[18].dressable && !rows[19].dressable);
	check(rows[61].fighter == 24 && rows[61].costume == 2 && rows[61].dressable && !rows[62].dressable);
	check(!rows[81].dressable && !rows[82].dressable && !rows[85].dressable && !rows[4].dressable);
	check(rows[lili_first].colours == std::array<u8, 3>{12, 12, 12});
}

PBD_TEST(PbdCustomLook, TheBlocksAreWhereTheGameKeepsThem)
{
	check(record_address(0) == 0x106c9934 && record_address(1) == 0x106c9b28);
	// The save's blocks: file 0x820 + fighter * 0x108, with the save loaded at 0x10684960.
	check(save_blocks_address(0) == 0x10685180 && save_blocks_address(31) == 0x10685180 + 31 * 0x108);
	check(blocks_size == 0x108 && equipped_offset + item_slots * 2 <= colour_offset && colour_offset + colour_parts < block_size);
	// The three blocks end before the PLAYER DATA numbers the records also hold.
	check(blocks_size <= pbd::game_profile::wins_offset);
}

PBD_TEST(PbdCustomLook, ItemsGoToTheirPartsSlot)
{
	const auto* row = catalogue_row(lili_first);
	check(row && part_of(*row, 61) == 0u && part_of(*row, 67) == 1u && part_of(*row, 66) == 2u && part_of(*row, 50) == 3u);
	check(!part_of(*row, 60) && !part_of(*row, 1) && !part_of(*row, 127) && !part_of(*row, 200));
	// In any slot order, each item lands in its part's slot.
	const auto normal = normalise({lili_first, {50, 61, 66, 67}, {own_colour, 3, 11}});
	check(normal && normal->items == std::array<u16, 4>{61, 67, 66, 50} && normal->colours == std::array<s8, 3>{own_colour, 3, 11});
	// The loader's other spellings of none become 0xffff.
	const auto sparse = normalise({lili_first, {0, 0x8000, 64, 0xfffe}, {own_colour, own_colour, own_colour}});
	check(sparse && sparse->items == std::array<u16, 4>{no_item, no_item, no_item, 64});
}

PBD_TEST(PbdCustomLook, LooksTheGameCouldNotShowAreRefused)
{
	// An item of another costume, two items for one part, a colour past its list.
	check(!normalise({lili_first, {60, no_item, no_item, no_item}}));
	check(normalise({lili_second, {60, no_item, no_item, no_item}}));
	check(!normalise({lili_first, {61, 62, no_item, no_item}}));
	check(!normalise({lili_first, {no_item, no_item, no_item, no_item}, {12, own_colour, own_colour}}));
	check(!normalise({lili_first, {no_item, no_item, no_item, no_item}, {-2, own_colour, own_colour}}));
	// Items or colours on a costume the game never dresses, or a row outside the table.
	check(!normalise({4, {no_item, no_item, no_item, no_item}, {0, own_colour, own_colour}}));
	check(!normalise({19, {3, no_item, no_item, no_item}}));
	check(!normalise({86, {}}));
	// The defaults are a look on every row of the table, dressed or not.
	check(normalise(look{4}) == look{4} && !normalise(look{84}) && !normalise(look{85}));
	// What a player shows: the look, or the row's defaults.
	check(shown({lili_first, {61, 62, no_item, no_item}}) == look{lili_first});
	check(shown({lili_first, {61, no_item, no_item, no_item}}).items[0] == 61);
}

PBD_TEST(PbdCustomLook, TheRecordKeepsTheSavedBlocksWithTheLookOnItsCostume)
{
	const auto saved = saved_blocks();
	const auto dressed = record_blocks(saved, shown({lili_second, {51, 56, 52, 50}, {0, own_colour, 5}}));
	for (u32 costume = 0; costume < block_count; ++costume)
	{
		// Owned items and owned-colour masks stay as saved, and so does the byte after the colours.
		for (u32 i = 0; i < equipped_offset; ++i)
			check(dressed[costume * block_size + i] == saved[costume * block_size + i]);
		for (u32 i = equipped_offset + item_slots * 2; i < colour_offset; ++i)
			check(dressed[costume * block_size + i] == saved[costume * block_size + i]);
		check(dressed[costume * block_size + block_size - 1] == saved[costume * block_size + block_size - 1]);
	}
	// The chosen costume (row 78 is costume 1) wears the look; the others their defaults.
	check(item_at(dressed, 1, 0) == 51 && item_at(dressed, 1, 1) == 56 && item_at(dressed, 1, 2) == 52 && item_at(dressed, 1, 3) == 50);
	check(dressed[block_size + colour_offset] == 0 && dressed[block_size + colour_offset + 1] == 0xff && dressed[block_size + colour_offset + 2] == 5);
	for (const u32 costume : {0u, 2u})
	{
		for (u32 slot = 0; slot < item_slots; ++slot)
			check(item_at(dressed, costume, slot) == no_item);
		for (u32 part = 0; part < colour_parts; ++part)
			check(dressed[costume * block_size + colour_offset + part] == 0xff);
	}
	// A default look clears every costume; a row the game never dresses touches only the equipped items and colours.
	const auto plain = record_blocks(saved, look{lili_first});
	for (u32 costume = 0; costume < block_count; ++costume)
		check(item_at(plain, costume, 0) == no_item && plain[costume * block_size + colour_offset] == 0xff);
	check(record_blocks(saved, look{81}) == record_blocks(saved, look{lili_first}));
	check(record_blocks(saved, {lili_first, {61, 62, no_item, no_item}}) == plain);
}

PBD_TEST(PbdCustomLook, ShopInventoryContainsOnlyTheFightersOwnCatalogue)
{
	u32 edited = 0;
	for (u32 selected = 0; selected < 84; ++selected)
	{
		const auto& row = pbd::custom_catalogue::rows[selected];
		const auto blocks = shop_blocks(look{selected});
		check(blocks.has_value() == row.dressable);
		if (!blocks)
			continue;
		++edited;
		check(read_record(*blocks, selected) == look{selected});
		for (const auto& costume : pbd::custom_catalogue::rows)
		{
			if (costume.fighter != row.fighter || !costume.dressable)
				continue;
			const auto* block = blocks->data() + costume.costume * block_size;
			std::array<bool, 128> seen{};
			for (u32 slot = 0; slot < 32; ++slot)
			{
				const u16 item = static_cast<u16>(u16{block[slot * 2]} << 8 | block[slot * 2 + 1]);
				if (!item)
					continue;
				check(part_of(costume, item) && !seen[item]);
				seen[item] = true;
			}
			for (u16 item = 1; item < 128; ++item)
				check(seen[item] == part_of(costume, item).has_value());
			for (u32 part = 0; part < colour_parts; ++part)
			{
				u32 mask = 0;
				for (u32 byte = 0; byte < 4; ++byte)
					mask = mask << 8 | block[0x48 + part * 4 + byte];
				check(mask == (u32{1} << costume.colours[part]) - 1);
			}
		}
	}
	check(edited == 70 && !shop_blocks(look{84}) && !shop_blocks(look{85}));
}

PBD_TEST(PbdCustomLook, ShopReadbackValidatesEquippedItemsAndColours)
{
	const look wanted{lili_second, {51, 56, 52, 50}, {0, own_colour, 11}};
	auto blocks = shop_blocks(wanted);
	check(blocks && read_record(*blocks, lili_second) == wanted);
	check(!read_record(*blocks, 19) && !read_record(*blocks, 84) && !read_record(*blocks, 86));
	check(!part_of(*catalogue_row(lili_second), 127));
	(*blocks)[block_size + equipped_offset + 1] = 127;
	check(!read_record(*blocks, lili_second));
	blocks = shop_blocks(wanted);
	(*blocks)[block_size + colour_offset] = 12;
	check(!read_record(*blocks, lili_second));
	check(!shop_blocks({lili_first, {61, 62, no_item, no_item}}));
}

PBD_TEST(PbdCustomLook, PracticeStartCarriesAnOptionalLook)
{
	using pbd::practice::start::parse;
	const std::string head = R"({"schema":"pbd-practice-start-1","id":"one","characterId":"lili","characterRow":77,"costume":0,"stage":23)";
	const auto none = parse(head + "}");
	check(none.value && !none.value->custom);
	const auto with = parse(head + R"(,"custom":{"items":[61,67,66,65535],"colours":[255,3,11]}})");
	check(with.value && with.value->custom);
	check(with.value->custom->items == std::array<u16, 4>{61, 67, 66, no_item});
	check(with.value->custom->colours == std::array<s8, 3>{own_colour, 3, 11});
	const auto oversize = parse(head + R"(,"custom":")" + std::string(20000, 'x') + "\"}");
	check(oversize.value && oversize.value->custom == look{77});
	// The look's shape is checked: four items, three colours, nothing else, each in range.
	for (const char* custom : {R"({"items":[61,67,66],"colours":[255,3,11]})", R"({"items":[61,67,66,1,2],"colours":[255,3,11]})",
			 R"({"items":[61,67,66,65536],"colours":[255,3,11]})", R"({"items":[61,67,66,1],"colours":[256,3,11]})",
			 R"({"items":[61,67,66,1],"colours":[255,3]})", R"({"items":[61,67,66,1],"colours":[255,3,11],"name":"x"})",
			 R"([61,67,66,1])", R"({"items":[61,67,66,-1],"colours":[255,3,11]})"})
	{
		const auto invalid = parse(head + R"(,"custom":)" + custom + "}");
		check(invalid.value && invalid.value->custom && *invalid.value->custom == look{77}, custom);
	}
}

PBD_TEST(PbdCustomLook, OpeningWritesAfterTheOutgoingSelectorAndBeforeModelLoading)
{
	using pbd::practice::start::look_boundary;
	check(!look_boundary(false, false, 10, 2, 44));
	check(!look_boundary(true, false, 10, 3, 44));
	check(!look_boundary(true, false, 10, 3, 2));
	check(look_boundary(true, false, 10, 2, 44));
	check(!look_boundary(true, true, 10, 2, 44));
	check(!look_boundary(true, false, 4, 2, 44));
	check(!look_boundary(true, false, 10, 2, 2));
	for (const u32 scene : {7u, 8u, 9u, 10u, 17u})
		check(!look_boundary(true, false, 10, scene, 44));
}
PBD_TEST(PbdCustomWire, FixedFightersReadOnlyTheirWornLookAndDefaultOnInvalidData)
{
	using namespace pbd::set_channel;
	const std::string head = R"({"memberId":"one","name":"One","character":{"id":"lili","row":77})";
	capabilities caps{"build", "compat", {77, 78}, {23}};
	const auto read = [&](std::string_view extra)
	{
		const auto value = detail::json_parser(head + std::string(extra) + "}").parse();
		check(value.value);
		fighter out;
		check(parse_fighter(&*value.value, out, caps));
		return out;
	};
	check(!read("").look);
	const auto chosen = read(R"(,"look":{"slot":3,"bytes":"4d003d004300420032ff030b"})");
	check(chosen.look && chosen.look->slot == 3 && pbd::custom_wire::unpack(chosen.look->value)->items[3] == 50);
	for (const auto bad : {R"(,"look":{"slot":3,"bytes":"4e003d004300420032ff030b"})", R"(,"look":{"slot":0,"bytes":"4d003d004300420032ff030b"})",
			 R"(,"look":{"slot":3,"table":"AAAA"})", R"(,"look":{"slot":3,"bytes":"4d003d004300420032ff030b","name":"private"})"})
		check(!read(bad).look);
}
