#include "stdafx.h"
#include "pbd_replay_carry.h"
#include "pbd_t5dr_memory.h"
#include "pbd_replay_carry_guard.h"
#include "Emu/Cell/PPUThread.h"
#include "Emu/Cell/pbd_practice_native.h"
#include "Crypto/sha256.h"
#include "Emu/System.h"
#include "Emu/Memory/vm.h"

#include <string>
#include <vector>

LOG_CHANNEL(pbd_carry_log, "PBD_REPLAY_CARRY");

namespace pbd::replay_carry
{
	namespace mem = pbd::replay_memory;
	namespace
	{
		bool supported() noexcept
		{
			return Emu.GetTitleID() == mem::title_id && Emu.GetExecutableHash() == mem::executable_hash;
		}
		u32 word(u32 address) noexcept
		{
			be_t<u32> value{};
			return vm::try_access(address, &value, sizeof(value), false) ? u32{value} : ~0u;
		}
		bool body_matches(const mem::body_signature& signature)
		{
			std::vector<u8> data(signature.bytes);
			std::array<u8, 32> hash{};
			if (!vm::try_access(signature.address, data.data(), signature.bytes, false) ||
				mbedtls_sha256_ret(data.data(), signature.bytes, hash.data(), 0))
				return false;
			std::string actual;
			for (const u8 value : hash)
				actual += fmt::format("%02x", value);
			return actual == signature.sha256;
		}
		bool vs_history_context(ppu_thread& ppu, u64 generation)
		{
			// Mode4's post-battle dispatch50c7c[4] ->50d44 ->5eed4 receives this VS flow object.
			// 5ef10..20 increments +34;5ef60..80 increments the winning port's halfword at +48+4*port.
			// These are game history, not round score, health, input-clock or allocator state.
			return supported() && generation == static_cast<u64>(Emu.GetEmulationIdentifier()) &&
			       get_current_cpu_thread() == &ppu && ppu.id == mem::main_thread && ppu.gpr[2] == mem::toc &&
			       word(mem::flow_mode) == 4 && word(mem::flow_toc_slot) == mem::flow_mode &&
			       body_matches(mem::history_update) && body_matches(mem::history_initialize);
		}
	} // namespace
	bool vector_access(u32 address, std::array<std::uint32_t, 3>& bits, bool write)
	{
		for (u32 axis = 0; axis < bits.size(); ++axis)
		{
			be_t<u32> word = bits[axis];
			if (!vm::try_access(address + 4 * axis, &word, sizeof(word), write))
				return false;
			if (!write)
				bits[axis] = word;
		}
		return true;
	}
	bool actor_context(ppu_thread& ppu, u64 generation, std::span<const mem::body_signature> signatures)
	{
		if (!vs_history_context(ppu, generation) || word(mem::actors_toc_slot) != mem::actors)
			return false;
		for (const auto& signature : signatures)
			if (!body_matches(signature))
				return false;
		return true;
	}
	bool read_vs_match_history(ppu_thread& ppu, u64 generation, vs_match_history& out) noexcept
	{
		try
		{
			if (!vs_history_context(ppu, generation))
				return false;
			be_t<u32> completed{};
			std::array<be_t<u16>, 2> wins{};
			if (!vm::try_access(mem::completed_games, &completed, sizeof(completed), false) ||
				!vm::try_access(mem::left_wins, &wins[0], sizeof(wins[0]), false) ||
				!vm::try_access(mem::right_wins, &wins[1], sizeof(wins[1]), false))
				return false;
			out = {u32{completed}, {u16{wins[0]}, u16{wins[1]}}};
			return true;
		}
		catch (...)
		{
			return false;
		}
	}
	bool read_previous_game_flow(ppu_thread& ppu, u64 generation, previous_game_flow& out) noexcept
	{
		try
		{
			if (!vs_history_context(ppu, generation))
				return false;
			previous_game_flow sample;
			for (std::size_t i = 0; i < sample.words.size(); ++i)
			{
				be_t<u32> value{};
				if (!vm::try_access(mem::previous_game_flow[i], &value, sizeof(value), false))
					return false;
				sample.words[i] = value;
			}
			out = sample;
			return true;
		}
		catch (...)
		{
			return false;
		}
	}
	namespace
	{
		bool actor_planar_context(ppu_thread& ppu, u64 generation)
		{
			// The native transform path stores signed quantized planar terms that survive match initialization.
			return vs_history_context(ppu, generation) && word(mem::actors_toc_slot) == mem::actors &&
			       body_matches(mem::planar_x) && body_matches(mem::planar_z);
		}
	} // namespace
	bool read_actor_planar_terms(ppu_thread& ppu, u64 generation, actor_planar_terms& out) noexcept
	{
		try
		{
			if (!actor_planar_context(ppu, generation))
				return false;
			actor_planar_terms sample;
			for (u32 side = 0; side < 2; ++side)
				for (u32 term = 0; term < 2; ++term)
					sample.bits[side][term] =
						word(mem::actors + mem::actor_stride * side + mem::actor_planar + 4 * term);
			out = sample;
			return true;
		}
		catch (...)
		{
			return false;
		}
	}
	bool apply_actor_planar_terms(ppu_thread& ppu, u64 generation, const actor_planar_terms& saved) noexcept
	{
		try
		{
			if (!owner_ready(ppu, generation) || ppu.cia != mem::match_initialize || ppu.gpr[3] != 2 ||
				!actor_planar_context(ppu, generation))
				return false;
			for (u32 side = 0; side < 2; ++side)
				for (u32 term = 0; term < 2; ++term)
				{
					const u32 address = mem::actors + mem::actor_stride * side + mem::actor_planar + 4 * term;
					be_t<u32> value = saved.bits[side][term];
					if (!vm::try_access(address, &value, sizeof(value), true) ||
						word(address) != saved.bits[side][term])
						return false;
				}
			return true;
		}
		catch (...)
		{
			return false;
		}
	}
	namespace
	{
		bool actor_transform_context(ppu_thread& ppu, u64 generation)
		{
			// The native transform sum survives initialization and is stored as exact float bits by player port.
			return vs_history_context(ppu, generation) && word(mem::actors_toc_slot) == mem::actors &&
			       body_matches(mem::transform_products) && body_matches(mem::transform_sum);
		}
	} // namespace
	bool read_actor_transform_sum(ppu_thread& ppu, u64 generation, actor_transform_sum& out) noexcept
	{
		try
		{
			if (!actor_transform_context(ppu, generation))
				return false;
			actor_transform_sum sample;
			for (u32 side = 0; side < 2; ++side)
			{
				be_t<u32> value{};
				if (!vm::try_access(mem::actors + mem::actor_stride * side + mem::actor_transform_sum, &value,
						sizeof(value), false))
					return false;
				sample.bits[side] = value;
			}
			out = sample;
			return true;
		}
		catch (...)
		{
			return false;
		}
	}
	bool apply_actor_transform_sum(ppu_thread& ppu, u64 generation, const actor_transform_sum& saved) noexcept
	{
		try
		{
			actor_transform_sum observed;
			if (!owner_ready(ppu, generation) || ppu.cia != mem::match_initialize || ppu.gpr[3] != 2 ||
				!actor_transform_context(ppu, generation))
				return false;
			for (u32 side = 0; side < 2; ++side)
			{
				be_t<u32> value = saved.bits[side];
				if (!vm::try_access(
						mem::actors + mem::actor_stride * side + mem::actor_transform_sum, &value, sizeof(value), true))
					return false;
			}
			return read_actor_transform_sum(ppu, generation, observed) && observed.bits == saved.bits;
		}
		catch (...)
		{
			return false;
		}
	}
	namespace
	{
		bool actor_descriptor_context(ppu_thread& ppu, u64 generation)
		{
			// f09e4 copies the native descriptor's halfword+8 into actor+17a, by player port.
			// Match initialization preserves this descriptor code; adjacent+178 is a separate field.
			return vs_history_context(ppu, generation) && word(mem::actors_toc_slot) == mem::actors &&
			       body_matches(mem::descriptor_code);
		}
	} // namespace
	bool read_actor_descriptor_code(ppu_thread& ppu, u64 generation, actor_descriptor_code& out) noexcept
	{
		try
		{
			if (!actor_descriptor_context(ppu, generation))
				return false;
			actor_descriptor_code sample;
			for (u32 side = 0; side < 2; ++side)
			{
				be_t<u16> code{};
				if (!vm::try_access(mem::actors + mem::actor_stride * side + mem::actor_descriptor_code, &code,
						sizeof(code), false))
					return false;
				sample.codes[side] = code;
			}
			out = sample;
			return true;
		}
		catch (...)
		{
			return false;
		}
	}
	bool apply_actor_descriptor_code(ppu_thread& ppu, u64 generation, const actor_descriptor_code& saved) noexcept
	{
		try
		{
			actor_descriptor_code observed;
			if (!owner_ready(ppu, generation) || ppu.cia != mem::match_initialize || ppu.gpr[3] != 2 ||
				!actor_descriptor_context(ppu, generation))
				return false;
			for (u32 side = 0; side < 2; ++side)
			{
				be_t<u16> code = saved.codes[side];
				if (!vm::try_access(
						mem::actors + mem::actor_stride * side + mem::actor_descriptor_code, &code, sizeof(code), true))
					return false;
			}
			return read_actor_descriptor_code(ppu, generation, observed) && observed.codes == saved.codes;
		}
		catch (...)
		{
			return false;
		}
	}
	namespace
	{
		bool actor_descriptor_deltas_context(ppu_thread& ppu, u64 generation)
		{
			// f3398 computes extsh(actor+8c - descriptor+1c - 8000); f33fc/f3404 store that pair.
			// The adjacent +2bc resource cursor is deliberately outside this typed scalar contract.
			return vs_history_context(ppu, generation) && word(mem::actors_toc_slot) == mem::actors &&
			       body_matches(mem::descriptor_delta_compute) && body_matches(mem::descriptor_delta_store);
		}
	} // namespace
	bool read_actor_descriptor_deltas(ppu_thread& ppu, u64 generation, actor_descriptor_deltas& out) noexcept
	{
		try
		{
			if (!actor_descriptor_deltas_context(ppu, generation))
				return false;
			actor_descriptor_deltas sample;
			for (u32 side = 0; side < 2; ++side)
				for (u32 term = 0; term < 2; ++term)
				{
					be_t<u16> value{};
					if (!vm::try_access(
							mem::actors + mem::actor_stride * side + mem::actor_descriptor_deltas + 2 * term, &value,
							sizeof(value), false))
						return false;
					sample.bits[side][term] = value;
				}
			out = sample;
			return true;
		}
		catch (...)
		{
			return false;
		}
	}
	bool apply_actor_descriptor_deltas(ppu_thread& ppu, u64 generation, const actor_descriptor_deltas& saved) noexcept
	{
		try
		{
			actor_descriptor_deltas observed;
			if (!owner_ready(ppu, generation) || ppu.cia != mem::match_initialize || ppu.gpr[3] != 2 ||
				!actor_descriptor_deltas_context(ppu, generation))
				return false;
			for (u32 side = 0; side < 2; ++side)
				for (u32 term = 0; term < 2; ++term)
				{
					be_t<u16> value = saved.bits[side][term];
					if (!vm::try_access(
							mem::actors + mem::actor_stride * side + mem::actor_descriptor_deltas + 2 * term, &value,
							sizeof(value), true))
						return false;
				}
			return read_actor_descriptor_deltas(ppu, generation, observed) && observed.bits == saved.bits;
		}
		catch (...)
		{
			return false;
		}
	}
	namespace
	{
		bool actor_motion_context(ppu_thread& ppu, u64 generation)
		{
			// e8dac computes -(actor position - native target)/step count; f1914 computes another motion path.
			// Both store float32 X/Z steps at actor+2f8/+2fc. Capture the actual bits, never recompute old positions.
			return vs_history_context(ppu, generation) && word(mem::actors_toc_slot) == mem::actors &&
			       body_matches(mem::target_motion) && body_matches(mem::reaction_motion);
		}
	} // namespace
	bool read_actor_motion_step(ppu_thread& ppu, u64 generation, actor_motion_step& out) noexcept
	{
		try
		{
			if (!actor_motion_context(ppu, generation))
				return false;
			actor_motion_step sample;
			for (u32 side = 0; side < 2; ++side)
				for (u32 term = 0; term < 2; ++term)
				{
					be_t<u32> value{};
					if (!vm::try_access(mem::actors + mem::actor_stride * side + mem::actor_motion_step + 4 * term,
							&value, sizeof(value), false))
						return false;
					sample.bits[side][term] = value;
				}
			out = sample;
			return true;
		}
		catch (...)
		{
			return false;
		}
	}
	bool apply_actor_motion_step(ppu_thread& ppu, u64 generation, const actor_motion_step& saved) noexcept
	{
		try
		{
			if (!owner_ready(ppu, generation) || ppu.cia != mem::match_initialize || ppu.gpr[3] != 2 ||
				!actor_motion_context(ppu, generation))
				return false;
			for (u32 side = 0; side < 2; ++side)
				for (u32 term = 0; term < 2; ++term)
				{
					be_t<u32> value = saved.bits[side][term];
					if (!vm::try_access(mem::actors + mem::actor_stride * side + mem::actor_motion_step + 4 * term,
							&value, sizeof(value), true))
						return false;
				}
			actor_motion_step observed;
			return read_actor_motion_step(ppu, generation, observed) && observed.bits == saved.bits;
		}
		catch (...)
		{
			return false;
		}
	}
	bool read_descriptor_derived_flag(ppu_thread& ppu, u64 generation, u8& out) noexcept
	{
		try
		{
			// Capture the actual inherited byte. The current descriptor's byte can differ in game1.
			if (!vs_history_context(ppu, generation) || word(mem::descriptor_toc_slot) != mem::descriptor_object ||
				word(mem::descriptor_flag_toc_slot) != mem::descriptor_flag ||
				!body_matches(mem::descriptor_flag_derive))
				return false;
			return vm::try_access(mem::descriptor_flag, &out, sizeof(out), false);
		}
		catch (...)
		{
			return false;
		}
	}
	bool apply_descriptor_derived_flag(ppu_thread& ppu, u64 generation, u8 saved) noexcept
	{
		try
		{
			u8 before = 0, observed = 0;
			return owner_ready(ppu, generation) && ppu.cia == mem::match_initialize && ppu.gpr[3] == 2 &&
			       read_descriptor_derived_flag(ppu, generation, before) &&
			       vm::try_access(mem::descriptor_flag, &saved, sizeof(saved), true) &&
			       read_descriptor_derived_flag(ppu, generation, observed) && observed == saved;
		}
		catch (...)
		{
			return false;
		}
	}
	bool refresh_descriptor_flag(ppu_thread& ppu, u64 generation, descriptor_flag& out) noexcept
	{
		try
		{
			// 154b0..c0 derives this byte from the currently loaded native descriptor, not saved replay bytes.
			//  Kind2 does not call that producer. Its consumers include28c590/297b90/2a4b6c.
			if (!owner_ready(ppu, generation) || ppu.cia != mem::match_initialize || ppu.gpr[3] != 2 ||
				word(mem::descriptor_toc_slot) != mem::descriptor_object ||
				word(mem::descriptor_flag_toc_slot) != mem::descriptor_flag ||
				!body_matches(mem::descriptor_flag_derive))
				return false;
			const u32 descriptor = word(mem::descriptor_pointer);
			if (!descriptor || descriptor > 0xffffffdfu)
				return false;
			u8 value = 0, before = 0, observed = 0;
			if (!vm::try_access(descriptor + mem::descriptor_flag_offset, &value, 1, false) ||
				!vm::try_access(mem::descriptor_flag, &before, 1, false) ||
				!vm::try_access(mem::descriptor_flag, &value, 1, true) ||
				!vm::try_access(mem::descriptor_flag, &observed, 1, false) || observed != value)
				return false;
			out = {descriptor, before, value};
			return true;
		}
		catch (...)
		{
			return false;
		}
	}
	bool apply_vs_match_history(ppu_thread& ppu, u64 generation, const vs_match_history& saved) noexcept
	{
		try
		{
			if (!owner_ready(ppu, generation) || !vs_history_context(ppu, generation))
				return false;
			be_t<u32> completed = saved.completed_games;
			std::array<be_t<u16>, 2> wins{saved.wins[0], saved.wins[1]};
			vs_match_history observed;
			return vm::try_access(mem::completed_games, &completed, sizeof(completed), true) &&
			       vm::try_access(mem::left_wins, &wins[0], sizeof(wins[0]), true) &&
			       vm::try_access(mem::right_wins, &wins[1], sizeof(wins[1]), true) &&
			       read_vs_match_history(ppu, generation, observed) &&
			       observed.completed_games == saved.completed_games && observed.wins == saved.wins;
		}
		catch (...)
		{
			return false;
		}
	}
	bool apply_vs_flow(ppu_thread& ppu, u64 generation, const game_values& saved) noexcept
	{
		if (saved.history && !apply_vs_match_history(ppu, generation, *saved.history))
			return false;
		if (!saved.flow)
			return true;
		try
		{
			if (!owner_ready(ppu, generation) || !vs_history_context(ppu, generation))
				return false;
			for (std::size_t i = 0; i < saved.flow->words.size(); ++i)
			{
				be_t<u32> value = saved.flow->words[i];
				if (!vm::try_access(mem::previous_game_flow[i], &value, sizeof(value), true))
					return false;
			}
			previous_game_flow observed;
			return read_previous_game_flow(ppu, generation, observed) && observed.words == saved.flow->words;
		}
		catch (...)
		{
			return false;
		}
	}
	bool read_actor_records(ppu_thread& ppu, u64 generation, actor_records& out) noexcept
	{
		try
		{
			actor_records sample;
			if (!actor_context(ppu, generation, {}) ||
				!vm::try_access(mem::actors, sample.bytes.data(), mem::actor_records_bytes, false))
				return false;
			out = sample;
			return true;
		}
		catch (...)
		{
			return false;
		}
	}
	namespace
	{
		// What this boot loaded for a side: its COM image's clip pairs and end, and the actor its bone-root object links
		// back to. An image without the COM header keeps an end of 0, which the plan refuses once placement has passed.
		loaded_side read_loaded_side(const actor_records& held, u32 side)
		{
			loaded_side out;
			const u32 base = placement_of(held, side).bank;
			std::array<be_t<u32>, 2 * mem::com_clip_pair_count> pairs{};
			if (word(base) == mem::com_version && word(base + 4) == mem::com_magic &&
				vm::try_access(base + mem::com_clip_pairs, pairs.data(), sizeof(pairs), false))
			{
				std::copy(pairs.begin(), pairs.end(), out.clip_pairs.begin());
				out.image_end = image_end(out.clip_pairs);
			}
			out.bone_root_actor = word(records_word(held, side, mem::actor_bone_root) + mem::bone_root_actor);
			return out;
		}
		void log_refusal(const actor_records& saved, const actor_records& held, const std::array<loaded_side, 2>& loaded,
			const records_write& write)
		{
			const u32 side = write.side, base = placement_of(held, side).bank, opposing = placement_of(held, 1 - side).bank;
			const auto recorded = placement_of(saved, side), now = placement_of(held, side);
			switch (write.refusal)
			{
			case records_refusal::bank_moved:
			case records_refusal::model_moved:
				pbd_carry_log.error("Side %u records name bank 0x%08x style %u model 0x%08x; it holds bank 0x%08x style %u "
									"model 0x%08x",
					side, recorded.bank, recorded.style, recorded.model, now.bank, now.style, now.model);
				return;
			case records_refusal::pick:
				pbd_carry_log.error("Side %u records name fighter %u row 0x%x; it holds fighter %u row 0x%x", side,
					recorded.fighter, recorded.row, now.fighter, now.row);
				return;
			case records_refusal::image:
				pbd_carry_log.error("Side %u COM image 0x%08x (header 0x%08x 0x%08x) ends at 0x%08x, outside 0x%08x..0x%08x",
					side, base, word(base), word(base + 4), write.value, base + mem::com_payload_offset,
					base + replay_bank_offset::max_bank_span);
				return;
			case records_refusal::bone_root:
				pbd_carry_log.error("Side %u bone-root object 0x%08x names actor 0x%08x, not 0x%08x", side,
					records_word(held, side, mem::actor_bone_root), write.value, mem::actors + mem::actor_stride * side);
				return;
			default:
				pbd_carry_log.error("Side %u records word +0x%03x holds 0x%08x (%s); this boot loaded image 0x%08x..0x%08x, "
									"opposing 0x%08x..0x%08x",
					side, write.offset, write.value, refusal_reason(write.refusal), base, loaded[side].image_end, opposing,
					loaded[1 - side].image_end);
			}
		}
	} // namespace
	static records_refusal pbd_write_actor_records(ppu_thread& ppu, u64 generation, const actor_records& saved, u32& changed) noexcept
	{
		try
		{
			actor_records held;
			if (!read_actor_records(ppu, generation, held))
				return records_refusal::context;
			const std::array<loaded_side, 2> loaded{read_loaded_side(held, 0), read_loaded_side(held, 1)};
			auto write = plan_actor_records(saved, held, loaded);
			if (write.refusal != records_refusal::none)
			{
				log_refusal(saved, held, loaded, write);
				return write.refusal;
			}
			if (!vm::try_access(mem::actors, write.image.bytes.data(), mem::actor_records_bytes, true) ||
				!read_actor_records(ppu, generation, held) || held != write.image)
				return records_refusal::readback;
			changed = write.changed;
			return records_refusal::none;
		}
		catch (...)
		{
			return records_refusal::context;
		}
	}
	records_refusal apply_actor_records(ppu_thread& ppu, u64 generation, const actor_records& saved, u32& changed) noexcept
	{
		if (!owner_ready(ppu, generation) || ppu.cia != mem::match_initialize || ppu.gpr[3] != 2)
			return records_refusal::context;
		return pbd_write_actor_records(ppu, generation, saved, changed);
	}
	records_refusal pbd_apply_round_actor_records(ppu_thread& ppu, u64 generation, const actor_records& saved, u32& changed) noexcept
	{
		if (!owner_ready(ppu, generation) || !pbd::replay_native::can_initialize_round(ppu, generation))
			return records_refusal::context;
		return pbd_write_actor_records(ppu, generation, saved, changed);
	}
} // namespace pbd::replay_carry
