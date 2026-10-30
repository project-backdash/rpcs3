#include "pbd_test.h"

#include "Emu/PBD/pbd_stage_rule.h"
#include "Emu/Cell/pbd_setup_digest.h"
#include "Emu/Cell/pbd_stage_names.h"

using pbd::test::check;

PBD_TEST(PbdStageRule, LoadableStages)
{
	using namespace pbd::stage_rule;
	for (std::uint32_t id = 0; id <= 0x0d; ++id)
		check(original(id) && loadable(id), "Tekken 5 originals 0x00-0x0d");
	check(original(0x0f) && loadable(0x0f), "Final Stage");
	for (std::uint32_t id = 0x11; id <= 0x22; ++id)
		check(!original(id) && loadable(id), "Dark Resurrection stages");
	check(!original(0x25) && loadable(0x25), "Inferno");
	for (const std::uint32_t id : {0x0eu, 0x10u, 0x23u, 0x24u, 0x26u, 0xffu, 0xffffffffu})
		check(!original(id) && !loadable(id), "ids with no stage behind them");
	// The display names and the loadable stages are the same set, so every named stage loads and none is unnamed.
	std::uint32_t named = 0;
	for (std::uint32_t id = 0; id < 0x100; ++id)
	{
		check(loadable(id) == !pbd::stage_names::name(id).empty(), "names and loadable stages agree");
		named += loadable(id) ? 1 : 0;
	}
	check(named == 34 && pbd::stage_names::names.size() == 34, "thirty-four stages");
}

PBD_TEST(PbdStageRule, UnlockByte)
{
	using namespace pbd::stage_rule;
	// The byte is offset 0x4a of the save image, inside what the session start digest covers.
	check(unlock_byte == pbd::setup_digest::save_image + 0x4a, "save offset 0x4a");
	check(unlock_byte < pbd::setup_digest::save_image + pbd::setup_digest::save_size, "inside the save image");
	check(unlock_pointer == 0x106848c0 && unlock_setting == 0x106848c0 && save_object_slot == 0x46ee04,
		"the game's own references");
	using pbd::setup_digest::creation;
	for (const auto& span : {creation[0], creation[1], pbd::setup_digest::sound})
		check(0x4a < span.begin || 0x4a >= span.end, "the digest does not skip the byte");

	const observation good{true, save_object, unlock_byte, unlock_byte, 0x00};
	check(unlocked(good) == std::optional<std::uint8_t>{std::uint8_t{0x10}}, "a new save gains only the original stages");
	auto jinpachi = good;
	jinpachi.value = 0x22;
	check(unlocked(jinpachi) == std::optional<std::uint8_t>{std::uint8_t{0x32}}, "a Jinpachi save keeps its bits");
	auto done = good;
	done.value = 0x33;
	check(unlocked(done) == std::optional<std::uint8_t>{std::uint8_t{0x33}}, "a save that has the bit is unchanged");
	for (std::uint32_t value = 0; value < 0x100; ++value)
	{
		auto any = good;
		any.value = static_cast<std::uint8_t>(value);
		const auto next = unlocked(any);
		check(next && *next == (value | 0x10), "only bit 0x10 is set");
	}

	auto patched = good;
	patched.pinned = false;
	check(!unlocked(patched), "another executable is refused");
	auto moved = good;
	moved.save_slot = save_object + 0x10;
	check(!unlocked(moved), "a different save object is refused");
	auto elsewhere = good;
	elsewhere.pointer = unlock_byte + 1;
	check(!unlocked(elsewhere), "a reader pointing elsewhere is refused");
	auto setting = good;
	setting.setting = 0;
	check(!unlocked(setting), "a setting pointing elsewhere is refused");
}
