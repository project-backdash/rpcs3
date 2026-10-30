#include "Emu/PBD/pbd_replay_actor_records.h"

#include "pbd_test.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <set>
#include <string_view>
#include <utility>

namespace
{
	namespace carry = pbd::replay_carry;
	namespace mem = pbd::replay_memory;
	using carry::records_refusal;
	using pbd::test::check;

	// A side's pick as a game's match initializer held it: style, fighter and pick-table row, where its COM image ends,
	// the image's clip pairs, and the clip-slot words +0x828, +0x82c, +0x83c ... +0x890 the loader copied from them.
	struct pick
	{
		std::uint32_t style, fighter, row, end;
		carry::clip_pair_words pairs, slots;
	};
	// Game 2 of a recorded set: style 8 on the left against style 23 on the right.
	constexpr pick style8{8, 8, 0x15, 0x216a4574,
		{0x21695d40, 0x21734780, 0x21695d48, 0x21848f84, 0x216966d4, 0x2187a378, 0x216a455c, 0x218911c4, 0x216a4564,
			0x218911cc, 0x216a456c, 0x218911d4},
		{0x216966d4, 0x2187a378, 0x21695d48, 0x21848f84, 0x21695d48, 0x21848f84, 0x216a455c, 0x218911c4, 0x216a4564,
			0x218911cc, 0x216a456c, 0x218911d4}};
	constexpr pick style23{23, 0x18, 0x3c, 0x21718d4c,
		{0x2170a7c0, 0x21a1e380, 0x2170a7c8, 0x21b08ecc, 0x2170b160, 0x21b4bf24, 0x21718d34, 0x21b67af8, 0x21718d3c,
			0x21b67b00, 0x21718d44, 0x21b67b08},
		{0x2170b160, 0x21b4bf24, 0x2170a7c8, 0x21b08ecc, 0x2170a7c8, 0x21b08ecc, 0x21718d34, 0x21b67af8, 0x21718d3c,
			0x21b67b00, 0x21718d44, 0x21b67b08}};
	// Game 3 of the same set, after both players changed character: style 31 on the left, style 6 on the right.
	constexpr pick style31{31, 0x20, 0x4f, 0x21699fb8,
		{0x216858e8, 0x21734780, 0x216883a8, 0x2182f920, 0x21688d48, 0x21865dd4, 0x21699fa0, 0x21877a88, 0x21699fa8,
			0x21877a90, 0x21699fb0, 0x21877a98},
		{0x21688d48, 0x21865dd4, 0x216883a8, 0x2182f920, 0x216883a8, 0x2182f920, 0x21699fa0, 0x21877a88, 0x21699fa8,
			0x21877a90, 0x21699fb0, 0x21877a98}};
	constexpr pick style6{6, 6, 0xf, 0x21734736,
		{0x2172b5f2, 0x21a1e380, 0x2172b5fa, 0x21b228d8, 0x2172bf8e, 0x21b5a314, 0x2173471e, 0x21b7f1e8, 0x21734726,
			0x21b7f1f0, 0x2173472e, 0x21b7f1f8},
		{0x2172bf8e, 0x21b5a314, 0x2172b5fa, 0x21b228d8, 0x2172b5fa, 0x21b228d8, 0x2173471e, 0x21b7f1e8, 0x21734726,
			0x21b7f1f0, 0x2173472e, 0x21b7f1f8}};
	constexpr std::uint32_t bases[] = {0x2162df00, 0x216b8180}, models[] = {0x41b5d590, 0x41e86b10};
	constexpr std::array<std::uint32_t, 12> slot_words = {
		0x828, 0x82c, 0x83c, 0x840, 0x850, 0x854, 0x864, 0x868, 0x878, 0x87c, 0x88c, 0x890};

	// What a played game leaves in a side's actor: the move records +0xc0, +0xc4, +0x320, +0x7e4, +0x818 and +0x81c,
	// the reaction cursor and result, and the bone-root object the recording's boot made.
	struct played
	{
		std::array<std::uint32_t, 6> moves;
		std::uint32_t cursor, result, bone_root;
	};
	constexpr std::array<std::uint32_t, 6> move_words = {0xc0, 0xc4, 0x320, 0x7e4, 0x818, 0x81c};
	constexpr played game2_left{
		{0x2167df44, 0x2167df44, 0, 0x2167df44, 0x2167a1d0, 0x2168c17c}, 0x216c97c6, 0x216c484c, 0x10659270};
	constexpr played game2_right{
		{0x21704234, 0x21704234, 0, 0x21704234, 0x216fa360, 0x216f8e98}, 0x216c989c, 0x21642a50, 0x10634070};
	constexpr played game3_left{
		{0x2167a5ac, 0x2167a5ac, 0, 0x21704364, 0x2167d7d8, 0x2167d824}, 0x1000104c, 0x216c39ec, 0x10679370};
	constexpr played game3_right{
		{0x21704318, 0x21704318, 0, 0x21704318, 0x216f8db4, 0x216f6638}, 0x1000104c, 0x216420f0, 0x1067d9f0};

	std::uint32_t actor(std::uint32_t side)
	{
		return mem::actors + mem::actor_stride * side;
	}
	void put(carry::actor_records& records, std::uint32_t side, std::uint32_t offset, std::uint32_t value,
		std::uint32_t size = 4)
	{
		for (std::uint32_t at = 0; at < size; ++at)
			records.bytes[mem::actor_stride * side + offset + at] = static_cast<std::uint8_t>(value >> 8 * (size - 1 - at));
	}
	// The words a fresh boot and a recording both hold for a pick: side, placement, pick, COM-bank words, clip slots.
	void place(carry::actor_records& records, std::uint32_t side, const pick& chosen)
	{
		put(records, side, mem::actor_side, side, 2);
		put(records, side, mem::actor_style, chosen.style, 2);
		put(records, side, mem::actor_fighter, chosen.fighter, 2);
		put(records, side, mem::actor_row, chosen.row, 2);
		put(records, side, mem::actor_com_bank, bases[side]);
		put(records, side, mem::actor_model, models[side]);
		put(records, side, mem::actor_bank_54, bases[side]);
		put(records, side, mem::actor_current_bank, bases[side]);
		for (std::size_t word = 0; word < slot_words.size(); ++word)
			put(records, side, slot_words[word], chosen.slots[word]);
	}
	struct boot
	{
		carry::actor_records held;
		std::array<carry::loaded_side, 2> loaded;
	};
	// A fresh boot at the match initializer of a game with these picks: its actors over zeros with the bone-root objects
	// it made, and the images and bone roots it loaded.
	boot fresh(const pick& left, const pick& right)
	{
		constexpr std::uint32_t bone_roots[] = {0x10630ef0, 0x10633a70};
		const pick* picks[] = {&left, &right};
		boot out;
		for (std::uint32_t side = 0; side < 2; ++side)
		{
			place(out.held, side, *picks[side]);
			put(out.held, side, mem::actor_bone_root, bone_roots[side]);
			out.loaded[side] = {picks[side]->end, picks[side]->pairs, actor(side)};
		}
		return out;
	}
	// The same game as a played set left it at that initializer, with 0x5a in every scalar byte: links to the other
	// actor, both contact owners the actor itself, the first contact selected, its own side flag, and the given moves.
	carry::actor_records recorded(const pick& left, const pick& right, const played& left_words, const played& right_words)
	{
		const pick* picks[] = {&left, &right};
		const played* words[] = {&left_words, &right_words};
		carry::actor_records out;
		out.bytes.fill(0x5a);
		for (std::uint32_t side = 0; side < 2; ++side)
		{
			place(out, side, *picks[side]);
			put(out, side, mem::actor_bank_58, bases[side]);
			for (const std::uint32_t link : {0x5c, 0x60, 0x64})
				put(out, side, link, actor(1 - side));
			put(out, side, 0x530, actor(side));
			put(out, side, 0x580, actor(side));
			put(out, side, 0x5d0, actor(side) + 0x530);
			put(out, side, 0x8a0, 0x1041c170 + 4 * side);
			for (std::size_t move = 0; move < move_words.size(); ++move)
				put(out, side, move_words[move], words[side]->moves[move]);
			put(out, side, 0x2bc, words[side]->cursor);
			for (const std::uint32_t unset : {0x300, 0x8a8, 0x8ac})
				put(out, side, unset, 0);
			put(out, side, 0x324, words[side]->result);
			put(out, side, mem::actor_bone_root, words[side]->bone_root);
		}
		return out;
	}
	carry::actor_records game2()
	{
		return recorded(style8, style23, game2_left, game2_right);
	}
	struct word_case
	{
		std::uint32_t side, offset, value;
	};
	// The plan of game 2's recording over its fresh boot, with one word changed.
	carry::records_write with_word(word_case changed)
	{
		auto saved = game2();
		put(saved, changed.side, changed.offset, changed.value);
		const auto boot = fresh(style8, style23);
		return carry::plan_actor_records(saved, boot.held, boot.loaded);
	}
	bool refused(const carry::records_write& write, records_refusal refusal, word_case at)
	{
		return write.refusal == refusal && write.side == at.side && write.offset == at.offset && write.value == at.value;
	}
	std::uint32_t differing(const carry::actor_records& a, const carry::actor_records& b)
	{
		std::uint32_t count = 0;
		for (std::size_t at = 0; at < a.bytes.size(); ++at)
			if (a.bytes[at] != b.bytes[at])
				++count;
		return count;
	}
	bool bone_root_byte(std::size_t at)
	{
		const auto offset = at % mem::actor_stride;
		return offset >= mem::actor_bone_root && offset < mem::actor_bone_root + 4;
	}
} // namespace

PBD_TEST(ReplayActorRecords, ReadsEachSidesPlacementAsBigEndianWords)
{
	const auto records = fresh(style8, style23).held;
	const auto left = carry::placement_of(records, 0), right = carry::placement_of(records, 1);
	check(left.bank == 0x2162df00 && left.style == 8 && left.model == 0x41b5d590 && left.fighter == 8 && left.row == 0x15,
		"Left placement misread");
	check(right.bank == 0x216b8180 && right.style == 23 && right.model == 0x41e86b10 && right.fighter == 0x18 &&
			  right.row == 0x3c,
		"Right placement misread");
}

PBD_TEST(ReplayActorRecords, AnImageEndsEightBytesPastItsHighestClip)
{
	for (const auto* chosen : {&style8, &style23, &style31, &style6})
		check(carry::image_end(chosen->pairs) == chosen->end, "Image end misread");
}

PBD_TEST(ReplayActorRecords, WritesAPlayedGameOverItsFreshBootButKeepsEachBoneRoot)
{
	// The recording's game 1 left links, moves and another bone-root pool order than the replay's fresh boot holds.
	const auto saved = game2();
	const auto boot = fresh(style8, style23);
	const auto write = carry::plan_actor_records(saved, boot.held, boot.loaded);
	check(write.refusal == records_refusal::none, "A played game 2 refused");
	bool taken_right = true;
	for (std::size_t at = 0; at < write.image.bytes.size(); ++at)
		taken_right = taken_right && write.image.bytes[at] == (bone_root_byte(at) ? boot.held : saved).bytes[at];
	check(taken_right, "Byte taken from the wrong side");
	check(carry::records_word(write.image, 0, mem::actor_bone_root) == 0x10630ef0 &&
			  carry::records_word(write.image, 1, mem::actor_bone_root) == 0x10633a70,
		"A recorded bone-root address was written");
	check(write.changed == differing(write.image, boot.held), "Changed bytes miscounted");
}

PBD_TEST(ReplayActorRecords, IdenticalActorsChangeNothing)
{
	const auto boot = fresh(style8, style23);
	const auto write = carry::plan_actor_records(boot.held, boot.held, boot.loaded);
	check(write.refusal == records_refusal::none && write.image == boot.held && write.changed == 0, "Identical actors changed");
	auto bone_root_only = boot.held;
	put(bone_root_only, 1, mem::actor_bone_root, 0x10634070);
	const auto kept = carry::plan_actor_records(bone_root_only, boot.held, boot.loaded);
	check(kept.refusal == records_refusal::none && kept.image == boot.held && kept.changed == 0,
		"A recorded bone-root address was written");
	const auto saved = game2();
	const auto same = carry::plan_actor_records(saved, saved, boot.loaded);
	check(same.refusal == records_refusal::none && same.changed == 0, "A played game changed over itself");
	auto one_move = boot.held;
	put(one_move, 0, 0xc0, 0x2167df44);
	const auto moved = carry::plan_actor_records(one_move, boot.held, boot.loaded);
	check(moved.refusal == records_refusal::none && moved.changed == 4, "One move record miscounted");
}

PBD_TEST(ReplayActorRecords, CarriesStaleMoveRecordsAfterACharacterChange)
{
	// Game 3's right side changed to style 6; its +0x818 lies in the new image but on no record boundary.
	const auto stale = fresh(style8, style6);
	const auto write = carry::plan_actor_records(recorded(style8, style6, game2_left, game3_right), stale.held, stale.loaded);
	check(write.refusal == records_refusal::none && carry::records_word(write.image, 1, 0x818) == 0x216f8db4,
		"A stale move record refused or changed");
	// The whole of game 3, whose left +0x7e4 names a record in the right side's image.
	const auto third = fresh(style31, style6);
	check(carry::plan_actor_records(recorded(style31, style6, game3_left, game3_right), third.held, third.loaded).refusal ==
			  records_refusal::none,
		"Game 3 refused");
}

PBD_TEST(ReplayActorRecords, AcceptsEveryValueAWordMayHold)
{
	const word_case cases[] = {
		{0, 0xc0, 0x21704364},            // a move record in the opposing image
		{0, 0xc0, 0x216a4574 - 0x50},     // the last whole record before the image's end
		{0, 0x2bc, 0x1000104c},           // the cursor in the executable table
		{1, 0x2bc, 0x21718d4c - 2},       // the cursor on the image's last halfword
		{0, 0xbc, 0x216b8180},            // the current bank switched to the opponent's
		{0, 0x58, 0},                     // a bank word a fresh actor holds
		{0, 0x5d0, actor(0) + 0x580},     // the second contact selected
		{0, 0x300, 0x2162df00 + 0x20000}, // a reaction entry in the own image
		{0, 0x878, 0x216a4564},           // slot 4's own default
	};
	for (const auto& accepted : cases)
		check(with_word(accepted).refusal == records_refusal::none, "An allowed value refused");
	auto boot = fresh(style8, style23);
	boot.loaded[1].image_end = 0x216b8180 + 0x8a280;
	check(carry::plan_actor_records(game2(), boot.held, boot.loaded).refusal == records_refusal::none,
		"An image ending at the slot's end refused");
}

PBD_TEST(ReplayActorRecords, RefusesAClipSlotThatIsNotTheLoadedPair)
{
	const word_case cases[] = {
		{0, 0x878, 0x76bf9ab8}, // unmapped
		{0, 0x878, 0x216a455c}, // another clip of the same image
		{0, 0x878, 0x21718d3c}, // the other side's default
		{0, 0x87c, 0},
	};
	for (const auto& clip : cases)
		check(refused(with_word(clip), records_refusal::clip, clip), "A foreign clip word accepted");
	// Every slot word moved by 8, on each side, is refused at its own offset.
	for (std::uint32_t side = 0; side < 2; ++side)
		for (std::size_t word = 0; word < slot_words.size(); ++word)
		{
			const word_case moved{side, slot_words[word], (side ? style23 : style8).slots[word] + 8};
			check(refused(with_word(moved), records_refusal::clip, moved), "A moved slot word accepted");
		}
}

PBD_TEST(ReplayActorRecords, RefusesThePreviousStylesClipPair)
{
	// Game 3's left side loaded style 31, but slot 4 still holds style 8's pair from game 2.
	const auto boot = fresh(style31, style23);
	auto saved = recorded(style31, style23, game3_left, game2_right);
	check(carry::plan_actor_records(saved, boot.held, boot.loaded).refusal == records_refusal::none, "Game 3 refused");
	put(saved, 0, 0x878, 0x216a4564);
	put(saved, 0, 0x87c, 0x218911cc);
	check(refused(carry::plan_actor_records(saved, boot.held, boot.loaded), records_refusal::clip, {0, 0x878, 0x216a4564}),
		"A previous style's clip pair accepted");
}

PBD_TEST(ReplayActorRecords, RefusesHistoryThatLeavesNoWholeRecordInEitherImage)
{
	const word_case cases[] = {
		{0, 0xc0, 0x216a4574 + 0x100},  // past the own image's end
		{0, 0xc0, 0x21734780},          // the motion archive
		{0, 0xc0, 0x2167df44 + 2},      // off a word
		{0, 0x818, 0x2162df00 + 0x180}, // the image header
		{0, 0xc0, 0x216a4574 - 0x4c},   // a record that would run past the end
		{0, 0x2bc, 0x10001050},         // just past the executable table
		{0, 0x2bc, 0x1000102e},         // just before the executable table
		{0, 0x2bc, 0x1000104d},         // off a halfword in the executable table
		{0, 0x2bc, 0x216c97c7},         // off a halfword in an image
		{1, 0x2bc, 0x21718d4c},         // just past the image's end
		{1, 0x324, 0x76bf9ab8},
		{0, 0x320, 0x21720000}, // past the opposing image's end
		{0, 0x300, 0x216b0000}, // between the two images
	};
	for (const auto& history : cases)
		check(refused(with_word(history), records_refusal::history, history),
			"A history pointer misaligned or without a whole record in either image accepted");
}

PBD_TEST(ReplayActorRecords, RefusesBankWordsLinksAndExtensionsThatNameAnythingElse)
{
	const std::pair<word_case, records_refusal> cases[] = {
		{{0, 0x54, 0x2162df00 + 4}, records_refusal::bank_word},
		{{0, 0x5c, actor(0)}, records_refusal::link},
		{{1, 0x64, 0x425ae8c0}, records_refusal::link},
		{{0, 0x530, actor(1)}, records_refusal::link},
		{{0, 0x5d0, actor(1) + 0x530}, records_refusal::link},
		{{0, 0x8a0, 0x1041c174}, records_refusal::link},
		{{1, 0x8a8, 0x10650000}, records_refusal::extension},
	};
	for (const auto& [word, refusal] : cases)
		check(refused(with_word(word), refusal, word), "A foreign word accepted");
}

PBD_TEST(ReplayActorRecords, RefusesEveryPointerWordThatLeavesItsRule)
{
	// Each pointer word of the field table holds an unmapped address, on either side. The offsets are spelled out
	// rather than taken from replay_memory's lists, so a word dropped from one of those lists still fails here.
	using enum records_refusal;
	const std::pair<std::uint32_t, records_refusal> words[] = {{0x54, bank_word}, {0x58, bank_word}, {0xbc, bank_word},
		{0x5c, link}, {0x60, link}, {0x64, link}, {0x530, link}, {0x580, link}, {0x5d0, link}, {0x8a0, link},
		{0xc0, history}, {0xc4, history}, {0x320, history}, {0x7e4, history}, {0x818, history}, {0x81c, history},
		{0x300, history}, {0x324, history}, {0x2bc, history}, {0x8a8, extension}, {0x8ac, extension}};
	for (std::uint32_t side = 0; side < 2; ++side)
		for (const auto& [offset, refusal] : words)
		{
			const word_case bad{side, offset, 0x76bf9ab8};
			check(refused(with_word(bad), refusal, bad), "A pointer word outside its rule accepted");
		}
}

PBD_TEST(ReplayActorRecords, RefusesAnImageOrBoneRootThatDoesNotFitItsSide)
{
	const auto saved = game2();
	const auto plan_with = [&](std::uint32_t side, carry::loaded_side loaded)
	{
		auto boot = fresh(style8, style23);
		boot.loaded[side] = loaded;
		return carry::plan_actor_records(saved, boot.held, boot.loaded);
	};
	check(refused(plan_with(1, {style23.end, style23.pairs, 0}), records_refusal::bone_root, {1, mem::actor_bone_root, 0}),
		"A dangling bone root accepted");
	check(refused(plan_with(0, {0x2162df00 + 0x270, style8.pairs, actor(0)}), records_refusal::image,
			  {0, mem::actor_com_bank, 0x2162df00 + 0x270}),
		"An image ending in its header accepted");
	check(refused(plan_with(1, {0x216b8180 + 0x8a284, style23.pairs, actor(1)}), records_refusal::image,
			  {1, mem::actor_com_bank, 0x216b8180 + 0x8a284}),
		"An image ending past its slot accepted");
	check(refused(plan_with(0, {0, {}, actor(0)}), records_refusal::image, {0, mem::actor_com_bank, 0}),
		"An image without its header accepted");
}

PBD_TEST(ReplayActorRecords, RefusesAnotherPlacementOrPickAndNamesTheSide)
{
	const std::pair<word_case, records_refusal> words[] = {
		{{1, mem::actor_com_bank, 0x2162df00}, records_refusal::bank_moved},
		{{1, mem::actor_model, 0x41b5d590}, records_refusal::model_moved},
	};
	for (const auto& [word, refusal] : words)
		check(refused(with_word(word), refusal, word), "Another bank or model accepted");
	const std::pair<word_case, records_refusal> halves[] = {
		{{0, mem::actor_style, 31}, records_refusal::bank_moved},
		{{0, mem::actor_row, 0x16}, records_refusal::pick}, // the same fighter and style in another costume
		{{1, mem::actor_fighter, 0x19}, records_refusal::pick},
	};
	const auto boot = fresh(style8, style23);
	for (const auto& [half, refusal] : halves)
	{
		auto saved = game2();
		put(saved, half.side, half.offset, half.value, 2);
		check(refused(carry::plan_actor_records(saved, boot.held, boot.loaded), refusal, half), "Another style or pick accepted");
	}
}

PBD_TEST(ReplayActorRecords, ChecksPlacementForBothSidesBeforeAnythingElse)
{
	auto saved = game2();
	put(saved, 0, 0xc0, 0x216a4574 + 0x100);
	auto boot = fresh(style8, style23);
	check(refused(carry::plan_actor_records(saved, boot.held, boot.loaded), records_refusal::history,
			  {0, 0xc0, 0x216a4574 + 0x100}),
		"The bad word was not refused");
	boot.loaded[1].bone_root_actor = 0;
	check(refused(carry::plan_actor_records(saved, boot.held, boot.loaded), records_refusal::bone_root,
			  {1, mem::actor_bone_root, 0}),
		"A side's words were checked before the other side's bone root");
	put(saved, 1, mem::actor_row, 0x3d, 2);
	check(refused(carry::plan_actor_records(saved, boot.held, boot.loaded), records_refusal::pick,
			  {1, mem::actor_row, 0x3d}),
		"A side's words or bone root were checked before the other side's pick");
	put(saved, 1, mem::actor_style, 6, 2);
	check(refused(carry::plan_actor_records(saved, boot.held, boot.loaded), records_refusal::bank_moved,
			  {1, mem::actor_style, 6}),
		"A side's words were checked before the other side's character change");
}

PBD_TEST(ReplayActorRecords, EachRefusalHasItsOwnReason)
{
	using enum records_refusal;
	const std::pair<records_refusal, std::string_view> reasons[] = {
		{context, "native_match_actor_records_context"},
		{bank_moved, "native_match_actor_records_bank"},
		{model_moved, "native_match_actor_records_model"},
		{pick, "native_match_actor_records_pick"},
		{image, "native_match_actor_records_image"},
		{bone_root, "native_match_actor_records_bone_root"},
		{bank_word, "native_match_actor_records_bank_word"},
		{link, "native_match_actor_records_link"},
		{history, "native_match_actor_records_history"},
		{clip, "native_match_actor_records_clip"},
		{extension, "native_match_actor_records_extension"},
		{readback, "native_match_actor_records_readback"},
	};
	std::set<std::string_view> distinct;
	for (const auto& [refusal, reason] : reasons)
	{
		check(carry::refusal_reason(refusal) == reason, "Refusal reason changed");
		distinct.insert(reason);
	}
	check(distinct.size() == std::size(reasons), "Two refusals share a reason");
}
