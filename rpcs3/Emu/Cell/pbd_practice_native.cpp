#include "stdafx.h"
#include "../PBD/pbd_native_call.h"

namespace pbd::practice::native
{

	thread_local bool calling = false;
	u32 word(u32 address) noexcept
	{
		be_t<u32> value{};
		return vm::try_access(address, &value, sizeof(value), false) ? u32{value} : ~0u;
	}
	u32 halfword(u32 address) noexcept
	{
		be_t<u16> value{};
		return vm::try_access(address, &value, sizeof(value), false) ? u16{value} : ~0u;
	}
	u32 byte(u32 address) noexcept
	{
		u8 value{};
		return vm::try_access(address, &value, 1, false) ? value : ~0u;
	}
	bool supported() noexcept
	{
		return Emu.GetTitleID() == pbd::t5dr::title_id && Emu.GetExecutableHash() == pbd::t5dr::executable_hash;
	}
	bool body_matches(u32 address, u32 bytes, const char* expected)
	{
		std::vector<u8> data(bytes);
		std::array<u8, 32> hash{};
		if (!vm::try_access(address, data.data(), bytes, false) || mbedtls_sha256_ret(data.data(), bytes, hash.data(), 0))
			return false;
		std::string actual;
		for (const u8 value : hash)
			actual += fmt::format("%02x", value);
		return actual == expected;
	}
	using restore_scope = pbd::guest_call::restore_scope<scratch_bytes>;

	bool preserved_call(ppu_thread& ppu, u32 entry, u32 r3, u32 r4,
		u32 staged_address, const std::array<be_t<u32>, 8>* staged_settings, u32* result_r3)
	{
		const u64 top = ppu.gpr[1] & ~u64{15};
		if (ppu.gpr[1] > 0xffffffffu || top < u64{ppu.stack_addr} + scratch_bytes ||
			top > u64{ppu.stack_addr} + ppu.stack_size ||
			!vm::check_addr(static_cast<u32>(top - scratch_bytes), vm::page_writable, scratch_bytes))
			return false;
		restore_scope restore(ppu, static_cast<u32>(top - scratch_bytes));
		if (!restore.save_stack())
			return false;
		const auto priority = ppu.prio.load();
		const auto debugger = ppu.debugger_mode.load();
		const u64 generation = static_cast<u64>(Emu.GetEmulationIdentifier());
		// Build the complete restoration scope before changing native option scratch memory. An unavailable stack or
		// failed architectural-state allocation must leave the pending native choices untouched.
		if (staged_settings)
		{
			std::array<be_t<u32>, 8> observed = *staged_settings;
			if (!staged_address || !vm::check_addr(staged_address, vm::page_readable | vm::page_writable, sizeof(observed)) ||
				!vm::try_access(staged_address, observed.data(), sizeof(observed), true) ||
				!vm::try_access(staged_address, observed.data(), sizeof(observed), false) || observed != *staged_settings)
				return false;
		}
		bool returned = false;
		u32 native_result = 0;
		calling = true;
		try
		{
			ppu.gpr[1] = top - linkage_bytes;
			ppu.gpr[3] = r3;
			ppu.gpr[4] = r4;
			ppu.loaded_from_savestate = false;
			ppu.fast_call(entry, pbd::t5dr::toc);
			native_result = static_cast<u32>(ppu.gpr[3]);
			// exec_task leaves the callee early only when check_state sees a stopped state: stop (thread not running),
			// exit or req_exit (the thread or emulator is shutting down) or again (a savestate interrupted the call). Its
			// normal end is the return stub's ret, which fast_call clears. Every other flag is asynchronous work another
			// thread may raise at any instant, including just after the callee returned: pause and suspend (suspend_all
			// and LV2 preemption), memory (a VM range lock), wait, signal, notify, yield, preempt, pending (scheduler),
			// temp (self-set around suspend_all), and the debugger or diagnostic-park requests. The guest services those at
			// its next check_state; they do not mean the call failed to complete, so they must not fail it.
			returned = !::is_stopped(ppu.state.load()) && ppu.prio.load().all == priority.all &&
			           ppu.debugger_mode.load() == debugger && static_cast<u64>(Emu.GetEmulationIdentifier()) == generation;
		}
		catch (...)
		{
			returned = false;
		}
		std::array<u8, 256> guard{};
		const bool low_guard = vm::try_access(restore.stack_address, guard.data(), guard.size(), false) &&
		                       !std::memcmp(guard.data(), restore.stack.data(), guard.size());
		restore.restore();
		std::array<u8, scratch_bytes> readback{};
		const bool stack_equal = restore.stack_restored && vm::try_access(restore.stack_address, readback.data(), readback.size(), false) &&
		                         readback == restore.stack;
		calling = false;
		const bool preserved = returned && low_guard && stack_equal && restore.context.registers_match(ppu);
		if (preserved && result_r3)
			*result_r3 = native_result;
		return preserved;
	}

	bool in_native_call() noexcept
	{
		return calling;
	}
	bool boundary_readable(const ppu_thread& ppu) noexcept
	{
		return supported() && !calling && ppu.id == pbd::t5dr::main_ppu_id && ppu.cia == pbd::t5dr::boundary_pc &&
		       ppu.gpr[2] == pbd::t5dr::toc && !ppu.current_function && !::is_stopped(ppu.state.load());
	}
	bool boundary_ready(const ppu_thread& ppu) noexcept
	{
		return boundary_readable(ppu) && !ppu.state.load();
	}
	owner_view inspect(bool owned) noexcept
	{
		owner_view view;
		view.generation = static_cast<u64>(Emu.GetEmulationIdentifier());
		if (!supported() || word(pbd::t5dr::game_mode_address) != 10 || word(0x10052a50) != 0x1008ebf4 ||
			word(0x1008ebf4) != 0 || word(0x4687bc) != 0x10092e98 ||
			word(0x468e5c) != 0x10091ab0 || word(0x10091ab0) != 0x10090040 || word(0x10091ab4) != 0x10090910 ||
			word(0x1009009c) != 0x10090910 || word(0x1009096c) != 0x10090040 ||
			halfword(0x1009007c) != 0 || halfword(0x1009094c) != 1)
			return view;
		const u32 which = byte(0x10090200);
		if (which == ~0u)
			return view;
		view.practice = 0x1008ebf4;
		view.initiating = which ? 0x10090910 : 0x10090040;
		view.opponent = which ? 0x10090040 : 0x10090910;
		view.port = halfword(view.initiating + 0x3c);
		const u32 blocked = word(0x10092e98), paused = word(0x10092ea4);
		view.pause_acknowledged = blocked != ~0u && paused != ~0u && (blocked || paused);
		view.owned = owned;
		return view;
	}

	bool commit_options(ppu_thread& ppu, const request& command, const owner_view& current, result& out)
	{
		constexpr std::array<u32, 7> offsets{0x6c, 0x30, 0x3c, 0x14, 0x18, 0x74, 0x64};
		constexpr std::array<u32, 7> scratch_indices{0, 2, 3, 4, 5, 6, 7};
		constexpr std::array<u32, 5> maximum{10, 1, 1, 2, 1};
		// A restore carries all five menu options packed in its value; a commit changes the one option it names.
		const bool restore = command.kind == operation::option_restore;
		const auto restored = pbd::practice::unpack_game_options(command.value);
		static_assert(pbd::practice::game_option_count == maximum.size());
		const u32 selected = static_cast<u32>(command.choice);
		if (restore)
		{
			for (u32 i = 0; i < maximum.size(); ++i)
				if (restored[i] > maximum[i])
					return false;
			if (pbd::practice::pack_game_options(restored) != command.value)
				return false;
		}
		else if (selected >= maximum.size() || command.value > maximum[selected])
			return false;
		std::array<u32, 7> desired{};
		std::array<be_t<u32>, 8> scratch{};
		if (!vm::try_access(current.practice + 0x3fc, scratch.data(), sizeof(scratch), false))
			return false;
		for (u32 i = 0; i < desired.size(); ++i)
		{
			desired[i] = word(current.practice + offsets[i]);
			if (desired[i] == ~0u || (i < maximum.size() && desired[i] > maximum[i]))
				return false;
		}
		if (word(current.practice + 0x70) != desired[0])
			return false;
		const u32 action_before = desired[0];
		if (restore)
			std::copy(restored.begin(), restored.end(), desired.begin());
		else
			desired[selected] = command.value;
		for (u32 i = 0; i < desired.size(); ++i)
			scratch[scratch_indices[i]] = desired[i];
		// The Dummy Action has a second scratch copy, written only when the action is chosen. A restore that changes
		// the action writes it the same way; one that keeps the action leaves it as a commit of another option does.
		if (restore ? desired[0] != action_before : selected == 0)
			scratch[1] = desired[0];
		out.entry = 0x456a4;
		if (!body_matches(out.entry, 0x120, "06771bf6a6f47cffd79b2e12b6aea64502bfc1c3824218be76b867a6cf8dc13e"))
			return false;
		out.preserved = preserved_call(ppu, out.entry, current.initiating, current.opponent,
			current.practice + 0x3fc, &scratch);
		const auto after = inspect(true);
		out.readback = out.preserved && after.practice == current.practice && after.generation == current.generation &&
		               after.initiating == current.initiating && after.opponent == current.opponent && word(current.practice + 0x70) == desired[0];
		for (u32 i = 0; i < desired.size(); ++i)
			out.readback &= word(current.practice + offsets[i]) == desired[i];
		return true;
	}

	result execute(ppu_thread& ppu, const request& command, const owner_view& owner) noexcept
	{
		result out;
		try
		{
			const auto current = inspect(owner.owned);
			if (!boundary_ready(ppu) || !owner.owned || !current.pause_acknowledged ||
				!current.practice || current.generation != owner.generation || command.generation != owner.generation ||
				current.initiating != owner.initiating || current.opponent != owner.opponent || current.port != owner.port)
				return out;
			if (command.kind == operation::sound || command.kind == operation::stage_load)
				return pbd::practice::commands::execute(ppu, command, current, {body_matches, [](ppu_thread& thread, pbd::practice::commands::stock_operation kind, u32 value)
																				   {
																					   using call = pbd::practice::commands::stock_operation;
																					   if (kind == call::sound)
																						   return preserved_call(thread, 0xac9b8, value, 0);
																					   if (kind == call::select_stage)
																						   return preserved_call(thread, 0x1328c, value, 0);
																					   return preserved_call(thread, 0x25da54, 14, 2);
																				   }});
			if (command.kind == operation::option_commit || command.kind == operation::option_restore)
			{
				if (!commit_options(ppu, command, current, out))
					return out;
			}
			else if (command.kind == operation::command_list)
			{
				out.entry = 0x1ce80;
				if (!body_matches(out.entry, 0xac, "4058eb0c9f60314ff7ee0466fb8701cd79f300544dc383e1b8381a14c83b8a45"))
					return out;
				out.preserved = preserved_call(ppu, out.entry, current.port, 0);
				out.readback = out.preserved && word(0x10092eb8) == 1 && halfword(0x10092ec8) == current.port;
			}
			else if (command.kind == operation::character_select)
			{
				out.entry = 0x4e9dc;
				if (!body_matches(out.entry, 0x7fc, "0a185c0079f5b67191abd835dae800a73ab7436e2d260b160cd6b62095dd17c1"))
					return out;
				out.preserved = preserved_call(ppu, out.entry, 10, 1u << current.port);
				// Actual character/stage arrival is observed later; a callback return alone cannot prove that transition.
				out.readback = out.preserved;
			}
			out.accepted = out.preserved && out.readback;
		}
		catch (...)
		{
			calling = false;
		}
		return out;
	}
} // namespace pbd::practice::native
