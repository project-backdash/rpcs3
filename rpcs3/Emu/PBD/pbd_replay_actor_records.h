#pragma once

#include "pbd_replay_bank_offset.h"
#include "pbd_replay_carry_types.h"
#include "pbd_t5dr_memory.h"
#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

// How the fighters' records go over both actors at the match initializer, apart from the guest memory they are read from
// and written to (pbd_replay_carry.cpp apply_actor_records).
namespace pbd::replay_carry
{
	enum class records_refusal
	{
		none,
		context,     // not the owner at the match initializer, or the actors are not the ones the game uses
		bank_moved,  // a side's records name another COM bank or style than the one its slot holds now
		model_moved, // a side's records name another model object than the one this boot loaded for it
		pick,        // a side's records name another fighter or pick-table row than the one this boot loaded
		image,       // a side's COM image lacks its header, or its clip pairs put its end outside the slot
		bone_root,   // the bone-root object this boot made for a side does not link back to that side's actor
		bank_word,   // a COM-bank word names neither side's bank
		link,        // a link names another object than the other actor, the actor's own contacts or its side flag
		history,     // a move or reaction pointer is misaligned or leaves no whole record (for the cursor, a halfword)
		             // in either loaded image's payload; only the cursor may point into the executable table instead
		clip,        // a clip-slot word differs from the pair this boot's image holds for that slot
		extension,   // a mode-extension word is set
		readback     // the bytes could not be written or did not read back as written
	};
	// The reason a replay is refused with, for each refusal.
	inline const char* refusal_reason(records_refusal refusal) noexcept
	{
		switch (refusal)
		{
		case records_refusal::bank_moved: return "native_match_actor_records_bank";
		case records_refusal::model_moved: return "native_match_actor_records_model";
		case records_refusal::pick: return "native_match_actor_records_pick";
		case records_refusal::image: return "native_match_actor_records_image";
		case records_refusal::bone_root: return "native_match_actor_records_bone_root";
		case records_refusal::bank_word: return "native_match_actor_records_bank_word";
		case records_refusal::link: return "native_match_actor_records_link";
		case records_refusal::history: return "native_match_actor_records_history";
		case records_refusal::clip: return "native_match_actor_records_clip";
		case records_refusal::extension: return "native_match_actor_records_extension";
		case records_refusal::readback: return "native_match_actor_records_readback";
		default: return "native_match_actor_records_context";
		}
	}
	// The big-endian word of `size` bytes at `offset` in a side's actor.
	inline std::uint32_t records_word(
		const actor_records& records, std::uint32_t side, std::uint32_t offset, std::uint32_t size = 4) noexcept
	{
		std::uint32_t out = 0;
		for (std::uint32_t at = 0; at < size; ++at)
			out = out << 8 | std::uint32_t{records.bytes[replay_memory::actor_stride * side + offset + at]};
		return out;
	}
	// The words of a side's actor that name what this boot loaded for it: the COM bank and style its COM-slot words
	// point into, its model object, and the fighter and pick-table row they were loaded for.
	struct records_placement
	{
		std::uint32_t bank = 0, style = 0, model = 0, fighter = 0, row = 0;
	};
	inline records_placement placement_of(const actor_records& records, std::uint32_t side) noexcept
	{
		namespace mem = replay_memory;
		return {records_word(records, side, mem::actor_com_bank), records_word(records, side, mem::actor_style, 2),
			records_word(records, side, mem::actor_model), records_word(records, side, mem::actor_fighter, 2),
			records_word(records, side, mem::actor_row, 2)};
	}
	using clip_pair_words = std::array<std::uint32_t, 2 * replay_memory::com_clip_pair_count>;
	// What this boot loaded for one side, read beside the actors (apply_actor_records).
	struct loaded_side
	{
		std::uint32_t image_end = 0;       // where its COM image ends (image_end below)
		clip_pair_words clip_pairs{};      // the image's +0x240..+0x26c as loaded
		std::uint32_t bone_root_actor = 0; // word +0x10 of the object at held +0x8c0
	};
	// A loaded COM image ends 8 bytes past the highest A word of its clip pairs: the pairs name 8-byte entries, and the
	// last of them closes the image.
	inline std::uint32_t image_end(const clip_pair_words& pairs) noexcept
	{
		std::uint32_t last = 0;
		for (std::size_t at = 0; at < pairs.size(); at += 2)
			last = std::max(last, pairs[at]);
		return last + 8;
	}
	struct records_write
	{
		records_refusal refusal = records_refusal::none;
		// With a refusal: the side and the word of its actor it names, and what the recording holds there. `image` names
		// the COM bank word with the image's end instead, and `bone_root` the bone-root word with the actor it links to.
		std::uint32_t side = 0, offset = 0, value = 0;
		actor_records image;       // without one: what both actors are to hold
		std::uint32_t changed = 0; // and how many of its bytes differ from what they hold now
	};

	namespace detail
	{
		namespace mem = replay_memory;
		namespace bank_offset = replay_bank_offset;

		// How a pointer word is checked before it is copied exactly. Zero always passes: a fresh actor holds it.
		enum class pointer_rule
		{
			bank,        // the side's own COM base or the opposing one; throws switch the current bank to the opponent's
			other_actor, // the other actor, at its fixed address
			self,        // the actor itself: a hit contact's owner
			contact,     // one of the actor's own two hit contacts
			side_flag,   // the side's own word of the side-flag table
			record,      // a word in either loaded image's payload with a whole record before the image's end
			cursor,      // a halfword in either loaded image's payload or in the executable table
			zero         // nothing: a VS game start holds no mode extension
		};
		struct pointer_words
		{
			pointer_rule rule;
			std::span<const std::uint32_t> offsets;
		};
		struct image_bounds
		{
			std::uint32_t base = 0, end = 0;
		};
		struct side_bounds
		{
			std::uint32_t side = 0, self = 0, other = 0;
			image_bounds own, opposing;
		};
		inline bool in_payload(std::uint32_t value, image_bounds image, std::uint32_t alignment, std::uint32_t reach) noexcept
		{
			return value % alignment == 0 && value >= std::uint64_t{image.base} + mem::com_payload_offset &&
			       std::uint64_t{value} + reach <= image.end;
		}
		inline bool in_executable_table(std::uint32_t value) noexcept
		{
			return value % bank_offset::cursor_alignment == 0 && value >= bank_offset::executable_table &&
			       std::uint64_t{value} + bank_offset::cursor_alignment <=
			           bank_offset::executable_table + bank_offset::executable_table_bytes;
		}
		inline bool allowed(pointer_rule rule, std::uint32_t value, const side_bounds& at) noexcept
		{
			const auto in_either = [&](std::uint32_t alignment, std::uint32_t reach)
			{
				return in_payload(value, at.own, alignment, reach) || in_payload(value, at.opposing, alignment, reach);
			};
			const std::uint32_t first_contact = at.self + mem::actor_hit_contacts;
			switch (rule)
			{
			case pointer_rule::bank: return !value || value == at.own.base || value == at.opposing.base;
			case pointer_rule::other_actor: return !value || value == at.other;
			case pointer_rule::self: return !value || value == at.self;
			case pointer_rule::contact:
				return !value || value == first_contact || value == first_contact + mem::hit_contact_bytes;
			case pointer_rule::side_flag: return !value || value == mem::side_flags + 4 * at.side;
			case pointer_rule::record: return !value || in_either(bank_offset::result_alignment, mem::com_record_span);
			case pointer_rule::cursor:
				return !value || in_either(bank_offset::cursor_alignment, bank_offset::cursor_alignment) ||
				       in_executable_table(value);
			default: return !value;
			}
		}
		inline records_refusal refusal_of(pointer_rule rule) noexcept
		{
			switch (rule)
			{
			case pointer_rule::bank: return records_refusal::bank_word;
			case pointer_rule::record:
			case pointer_rule::cursor: return records_refusal::history;
			case pointer_rule::zero: return records_refusal::extension;
			default: return records_refusal::link;
			}
		}
		inline bool refuse(records_write& out, records_refusal refusal, std::uint32_t side, std::uint32_t offset,
			std::uint32_t value) noexcept
		{
			out.refusal = refusal;
			out.side = side;
			out.offset = offset;
			out.value = value;
			return true;
		}
		// Placement and pick: a side whose records name another bank, style, model, fighter or row than it holds now.
		inline bool refuse_placement(
			const actor_records& saved, const actor_records& held, std::uint32_t side, records_write& out) noexcept
		{
			const auto recorded = placement_of(saved, side), now = placement_of(held, side);
			if (recorded.bank != now.bank)
				return refuse(out, records_refusal::bank_moved, side, mem::actor_com_bank, recorded.bank);
			if (recorded.style != now.style)
				return refuse(out, records_refusal::bank_moved, side, mem::actor_style, recorded.style);
			if (recorded.model != now.model)
				return refuse(out, records_refusal::model_moved, side, mem::actor_model, recorded.model);
			if (recorded.fighter != now.fighter)
				return refuse(out, records_refusal::pick, side, mem::actor_fighter, recorded.fighter);
			if (recorded.row != now.row)
				return refuse(out, records_refusal::pick, side, mem::actor_row, recorded.row);
			return false;
		}
		// What this boot loaded for a side: an image that ends inside its slot, and a bone-root object of its own.
		inline bool refuse_loaded(
			const actor_records& held, std::uint32_t side, const loaded_side& loaded, records_write& out) noexcept
		{
			const std::uint64_t base = placement_of(held, side).bank;
			if (loaded.image_end < base + mem::com_payload_offset || loaded.image_end > base + bank_offset::max_bank_span)
				return refuse(out, records_refusal::image, side, mem::actor_com_bank, loaded.image_end);
			if (loaded.bone_root_actor != mem::actors + mem::actor_stride * side)
				return refuse(out, records_refusal::bone_root, side, mem::actor_bone_root, loaded.bone_root_actor);
			return false;
		}
		// Each clip slot holds, word for word, the pair of this boot's list that the loader copies into it. Any other
		// value is a pair of an earlier image or of the other side's, whose memory this boot may have refilled.
		inline bool refuse_clips(
			const actor_records& saved, std::uint32_t side, const loaded_side& loaded, records_write& out) noexcept
		{
			for (std::uint32_t slot = 0; slot < mem::clip_slot_pairs.size(); ++slot)
				for (std::uint32_t word = 0; word < 2; ++word)
				{
					const auto offset = mem::actor_clip_slots + mem::clip_slot_bytes * slot + 4 * word;
					const auto value = records_word(saved, side, offset);
					if (value != loaded.clip_pairs[2 * mem::clip_slot_pairs[slot] + word])
						return refuse(out, records_refusal::clip, side, offset, value);
				}
			return false;
		}
		inline bool refuse_words(const actor_records& saved, const actor_records& held, std::uint32_t side,
			const std::array<loaded_side, 2>& loaded, records_write& out) noexcept
		{
			const side_bounds at{side, mem::actors + mem::actor_stride * side, mem::actors + mem::actor_stride * (1 - side),
				{placement_of(held, side).bank, loaded[side].image_end},
				{placement_of(held, 1 - side).bank, loaded[1 - side].image_end}};
			constexpr std::uint32_t banks[] = {mem::actor_bank_54, mem::actor_bank_58, mem::actor_current_bank},
									others[] = {mem::actor_opponent, mem::actor_interaction, mem::actor_partner},
									owners[] = {mem::actor_hit_contacts, mem::actor_hit_contacts + mem::hit_contact_bytes},
									selected[] = {mem::actor_selected_contact}, flag[] = {mem::actor_side_flag},
									reactions[] = {mem::actor_reaction_entry, mem::actor_reaction_result},
									cursor[] = {mem::actor_reaction_cursor};
			const pointer_words checks[] = {{pointer_rule::bank, banks}, {pointer_rule::other_actor, others},
				{pointer_rule::self, owners}, {pointer_rule::contact, selected}, {pointer_rule::side_flag, flag},
				{pointer_rule::record, mem::actor_move_records}, {pointer_rule::record, reactions},
				{pointer_rule::cursor, cursor}, {pointer_rule::zero, mem::actor_mode_extension}};
			for (const auto& [rule, offsets] : checks)
				for (const auto offset : offsets)
					if (const auto value = records_word(saved, side, offset); !allowed(rule, value, at))
						return refuse(out, refusal_of(rule), side, offset, value);
			return refuse_clips(saved, side, loaded[side], out);
		}
	} // namespace detail

	// The recording's bytes over the actors `held` now, checked against what this boot loaded (`loaded`). A pointer word
	// is copied exactly when it points where this boot holds what the recording held: the actors sit at fixed addresses,
	// and a COM image depends only on its style and the bases it was loaded at, which the placement check pins. Any other
	// value is refused, never relocated, and then nothing is written. Each fighter keeps the bone-root object this boot
	// made for it. Placement and pick go first for both sides, so a character change is refused as such and not as a word
	// that changed with it; then the loaded images and bone roots; then the words.
	inline records_write plan_actor_records(
		const actor_records& saved, const actor_records& held, const std::array<loaded_side, 2>& loaded) noexcept
	{
		records_write out;
		out.image = saved;
		for (std::uint32_t side = 0; side < 2; ++side)
			if (detail::refuse_placement(saved, held, side, out))
				return out;
		for (std::uint32_t side = 0; side < 2; ++side)
			if (detail::refuse_loaded(held, side, loaded[side], out))
				return out;
		for (std::uint32_t side = 0; side < 2; ++side)
			if (detail::refuse_words(saved, held, side, loaded, out))
				return out;
		for (std::uint32_t side = 0; side < 2; ++side)
		{
			const auto bone_root = replay_memory::actor_stride * side + replay_memory::actor_bone_root;
			std::copy_n(held.bytes.begin() + bone_root, 4, out.image.bytes.begin() + bone_root);
		}
		for (std::size_t at = 0; at < out.image.bytes.size(); ++at)
			if (out.image.bytes[at] != held.bytes[at])
				++out.changed;
		return out;
	}
} // namespace pbd::replay_carry
