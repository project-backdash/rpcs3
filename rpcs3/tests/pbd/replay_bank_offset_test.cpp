#include "Emu/PBD/pbd_replay_bank_offset.h"
#include "Emu/PBD/pbd_replay_resource_reference.h"
#include "Emu/PBD/pbd_replay_result_reference.h"

#include "pbd_test.h"

using namespace pbd::replay_bank_offset;
namespace rr = pbd::replay_resource;

namespace
{
	// The two COM slots and the catalogs the loader published in recorded games. Asuka (style 23) was measured at the
	// left slot; the right-slot Asuka catalogs are the same offsets from the right slot's base.
	constexpr std::uint32_t left_slot = 0x2162df00, right_slot = 0x216b8180;
	constexpr rr::catalog asuka_left_samples{0x2163f544, 1166, 0x2163fe60};
	constexpr rr::catalog steve_left_records{0x2163d47c, 223, 0x21641a2c};
	constexpr rr::catalog asuka_right_samples{right_slot + 0x11644, 1166, right_slot + 0x11f60};
	constexpr rr::catalog dragunov_right_samples{0x216cace0, 1190, 0x216cb62c};

	constexpr banks seen_from_left(std::uint16_t left_style, std::uint16_t right_style)
	{
		return {{right_slot, right_style, right_slot + max_bank_span}, {left_slot, left_style, left_slot + max_bank_span}};
	}
	constexpr banks seen_from_right(std::uint16_t left_style, std::uint16_t right_style)
	{
		return {{left_slot, left_style, left_slot + max_bank_span}, {right_slot, right_style, right_slot + max_bank_span}};
	}
	bool same(std::optional<reference> found, reference expected)
	{
		return found && found->base == expected.base && found->byte_offset == expected.byte_offset &&
		       found->bank_style == expected.bank_style;
	}
} // namespace

// The right actor's cursor 0x2163f6ec was Asuka's sample 212 in game 1. In game 2 the left slot holds Steve, where the
// same address happens to fall on record 110 byte 16; the bank offset names it in both images.
PBD_TEST(ReplayBankOffset, LeftoverCursorKeepsItsSlotOffsetWhenTheOpponentChanges)
{
	const std::uint32_t cursor = 0x2163f6ec;
	const auto game1 = rr::identify(rr::section_kind::samples16, asuka_left_samples, 23, cursor);
	pbd::test::check(game1 && game1->index == 212 && game1->byte_offset == 0, "Asuka sample 212 not found");
	const auto game2 = rr::identify(rr::section_kind::records80, steve_left_records, 16, cursor);
	pbd::test::check(game2 && game2->index == 110 && game2->byte_offset == 16, "Steve record 110 byte 16 not found");

	const auto offset = identify(cursor, seen_from_right(16, 31), cursor_alignment);
	pbd::test::check(same(offset, {base_kind::opposing, 0x117ec, 16}), "Opposing slot offset not chosen");
	pbd::test::check(resolve(*offset, seen_from_right(16, 31), cursor_alignment) == cursor, "Offset did not rebuild the word");
	pbd::test::check(!resolve(*offset, seen_from_right(23, 31), cursor_alignment), "Another loaded style was accepted");
	banks moved = seen_from_right(16, 31);
	moved.opposing.base += 0x80;
	pbd::test::check(resolve(*offset, moved, cursor_alignment) == cursor + 0x80, "The reference carried an address");
}

// Both opponents change for game 2: the left cursor 0x216c993c is game 1's Asuka sample 188 at the right slot, which
// now holds style 2 with no record or sample at that address.
PBD_TEST(ReplayBankOffset, CursorIntoAReplacedImageIsCarriedAsAnOffset)
{
	const std::uint32_t cursor = 0x216c993c;
	const auto game1 = rr::identify(rr::section_kind::samples16, asuka_right_samples, 23, cursor);
	pbd::test::check(game1 && game1->index == 188 && game1->byte_offset == 0, "Asuka sample 188 not found");
	const auto offset = identify(cursor, seen_from_left(23, 2), cursor_alignment);
	pbd::test::check(same(offset, {base_kind::opposing, 0x117bc, 2}), "Right slot offset not chosen");
	pbd::test::check(resolve(*offset, seen_from_left(23, 2), cursor_alignment) == cursor, "Offset did not rebuild the word");
	pbd::test::check(!resolve(*offset, seen_from_left(23, 23), cursor_alignment), "A different loaded style was accepted");
	pbd::test::check(!resolve(*offset, {{0, 0}, {left_slot, 23}}, cursor_alignment), "An unreadable slot was accepted");
}

PBD_TEST(ReplayBankOffset, CursorAtTheEndOfTheSamplesIsAccepted)
{
	const std::uint32_t end = dragunov_right_samples.next;
	pbd::test::check(!rr::identify(rr::section_kind::samples16, dragunov_right_samples, 31, end), "End became a sample");
	const auto offset = identify(end, seen_from_left(16, 31), cursor_alignment);
	pbd::test::check(same(offset, {base_kind::opposing, end - right_slot, 31}), "Samples end not carried");
	pbd::test::check(resolve(*offset, seen_from_left(16, 31), cursor_alignment) == end, "Samples end not rebuilt");
}

PBD_TEST(ReplayBankOffset, CursorInTheExecutableTableIgnoresTheBanks)
{
	for (const std::uint32_t cursor : {0x1000103cu, 0x1000104cu, executable_table})
	{
		const auto offset = identify(cursor, seen_from_left(16, 31), cursor_alignment);
		pbd::test::check(same(offset, {base_kind::executable, cursor - executable_table, 0}), "Table word not named");
		pbd::test::check(resolve(*offset, {}, cursor_alignment) == cursor, "Table word needs a bank");
	}
	pbd::test::check(!identify(executable_table + executable_table_bytes, {}, cursor_alignment), "Word past the table");
	pbd::test::check(!valid({base_kind::executable, 0x20, 0}, cursor_alignment), "Offset past the table is valid");
	pbd::test::check(!valid({base_kind::executable, 0xc, 31}, cursor_alignment), "Table word with a style is valid");
}

PBD_TEST(ReplayBankOffset, OwnSlotIsTriedAfterTheOpposingSlot)
{
	// Seen from the left actor the opposing slot is the higher one, so a word in the left slot lies below it.
	const auto left = identify(0x2163f6ec, seen_from_left(16, 31), cursor_alignment);
	pbd::test::check(same(left, {base_kind::own, 0x117ec, 16}), "Left slot word not named as own");
	// Seen from the right actor the opposing slot is lower and ends where the right slot begins.
	const auto right = identify(0x216cacf8, seen_from_right(16, 31), cursor_alignment);
	pbd::test::check(same(right, {base_kind::own, 0x216cacf8 - right_slot, 31}), "Right slot word not named as own");
	pbd::test::check(span({left_slot, 16, left_slot + max_bank_span}, {right_slot, 31, right_slot + max_bank_span}) ==
		right_slot - left_slot, "Lower slot overlaps the higher");
	pbd::test::check(span({right_slot, 31, right_slot + max_bank_span}, {left_slot, 16, left_slot + max_bank_span}) ==
		max_bank_span, "Higher slot span changed");
}

PBD_TEST(ReplayBankOffset, LoadedImageEndExcludesTheMotionArchive)
{
	// The right style-6 image ends at 0x21734736; the left motion archive begins at 0x21734740.
	const banks loaded{{right_slot, 6, 0x21734736}, {left_slot, 8, 0x216a4574}};
	pbd::test::check(identify(0x21734734, loaded, cursor_alignment).has_value(), "Last image halfword refused");
	pbd::test::check(!identify(0x21734736, loaded, cursor_alignment), "Image end became a cursor");
	pbd::test::check(!identify(0x21734734, loaded, result_alignment), "Result crosses the image end");
	pbd::test::check(!identify(0x21734740, loaded, cursor_alignment), "Motion archive became a bank offset");
	pbd::test::check(!resolve({base_kind::opposing, 0x21734740 - right_slot, 6}, loaded, result_alignment),
		"Saved motion-archive offset rebuilt");
	pbd::test::check(!resolve({base_kind::opposing, 0, 6}, {{right_slot, 6}, loaded.own}, cursor_alignment),
		"Unmeasured image end accepted");
	pbd::test::check(span({right_slot, 6, right_slot + max_bank_span + 2}, loaded.own) == 0,
		"Oversized image accepted");
}

PBD_TEST(ReplayBankOffset, WordOutsideEveryBaseIsRefused)
{
	const auto loaded = seen_from_left(23, 2);
	for (const std::uint32_t address : {0x10090040u, left_slot - 2, right_slot + max_bank_span, 0xfffffffeu, 0u})
		pbd::test::check(!identify(address, loaded, cursor_alignment), "A word outside the banks was named");
	pbd::test::check(!identify(0x216c993d, loaded, cursor_alignment), "An odd cursor was named");
	pbd::test::check(!identify(0x216c993e, loaded, result_alignment), "A result off its word alignment was named");
	pbd::test::check(identify(0x216c993c, loaded, result_alignment).has_value(), "An aligned result was refused");
	pbd::test::check(valid({base_kind::own, max_bank_span - 2, 0}, cursor_alignment), "Last slot halfword refused");
	pbd::test::check(!valid({base_kind::own, max_bank_span, 0}, cursor_alignment), "Offset past the slot is valid");
	pbd::test::check(!valid({base_kind::opposing, max_bank_span - 2, 0}, result_alignment), "Unaligned result valid");
	pbd::test::check(!valid({static_cast<base_kind>(7), 0, 0}, cursor_alignment), "Unknown base is valid");
	pbd::test::check(!valid({base_kind::opposing, 0, 0}, 8), "Unknown alignment is valid");
}

// The retained result 0x21639d5c of an earlier recording is a payload prefix word; kind 4 rebuilds the same word.
PBD_TEST(ReplayBankOffset, ResultBankOffsetRebuildsTheRetainedWord)
{
	namespace result = pbd::replay_result;
	const auto loaded = seen_from_right(16, 31);
	const auto offset = identify(0x21639d5c, loaded, result_alignment);
	pbd::test::check(same(offset, {base_kind::opposing, 0xbe5c, 16}), "Result offset not chosen");
	const pbd::replay_carry::reaction_result_ref saved{4, offset->byte_offset, offset->bank_style, offset->base};
	pbd::test::check(result::resolve_bank_offset(saved, loaded) == 0x21639d5cu, "Result word not rebuilt");
	pbd::test::check(!result::resolve_bank_offset({3, 0xbbe8, 16}, loaded), "A prefix reference resolved as kind 4");
	pbd::test::check(!result::resolve_bank_offset({4, 0xbe5e, 16, base_kind::opposing}, loaded), "Unaligned result");
	pbd::test::check(!result::resolve_prefix({3, 0xbbe8, 16, base_kind::own}, left_slot, steve_left_records, 16),
		"A prefix reference with another base resolved");
}

PBD_TEST(ReplayBankOffset, BaseNamesRoundTrip)
{
	for (const auto base : {base_kind::opposing, base_kind::own, base_kind::executable})
		pbd::test::check(parse_base(name(base)) == base, "Base name did not round trip");
	pbd::test::check(!parse_base("address"), "Unknown base name parsed");
	pbd::test::check(!parse_base(""), "Empty base name parsed");
}
