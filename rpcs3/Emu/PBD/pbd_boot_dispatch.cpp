#include "stdafx.h"
#include "pbd_boot_request_state.h"

namespace pbd::boot_request::detail
{
	std::string environment(const char* name)
	{
#ifdef _WIN32
		const auto value = pbd::env::find_wide(name);
		if (value)
			return std::string(value, value + std::wcslen(value));
#else
		if (const auto value = pbd::env::find(name))
			return value;
#endif
		return {};
	}
	u32 word(u32 address)
	{
		be_t<u32> value{};
		return vm::try_access(address, &value, sizeof(value), false) ? u32{value} : ~0u;
	}
	u32 byte(u32 address)
	{
		u8 value{};
		return vm::try_access(address, &value, 1, false) ? value : ~0u;
	}
	u32 halfword(u32 address)
	{
		be_t<u16> value{};
		return vm::try_access(address, &value, sizeof(value), false) ? u16{value} : ~0u;
	}
	u32 movie_child()
	{
		// Read the two generation-checked handles used by 0x56f20 / 0x9bf3c.
		// Reading does not apply the native handle helper's invalid-handle clearing side effect.
		const u32 manager = word(0x4691f0), task = word(manager + 0xfc);
		if (!manager || manager == ~0u || !task || task == ~0u || word(task + 12) != word(manager + 0x100))
			return 0;
		const u32 child = word(task + 0x60);
		return child && child != ~0u && word(child + 12) == word(task + 0x64) ? child : 0;
	}
	std::string digest(u32 address, u32 bytes)
	{
		std::vector<u8> data(bytes);
		if (!vm::try_access(address, data.data(), bytes, false))
			return "unreadable";
		std::array<u8, 32> hash{};
		if (mbedtls_sha256_ret(data.data(), bytes, hash.data(), 0))
			return "digest_error";
		std::string result;
		for (auto b : hash)
			result += fmt::format("%02x", b);
		return result;
	}
	u64 mix(u64 hash, u32 value)
	{
		for (u32 shift : {24u, 16u, 8u, 0u})
			hash = (hash ^ ((value >> shift) & 255)) * prime;
		return hash;
	}
	using restore_scope = pbd::guest_call::restore_scope<scratch_bytes>;

	boot_observation& state()
	{
		static boot_observation instance;
		return instance;
	}

	struct native_call
	{
		ppu_thread& ppu;
		boot_observation& s;
		u64 top;
		decltype(ppu.prio.load()) priority = ppu.prio.load();
		u32 cpu_state_before = static_cast<u32>(ppu.state.load());
		decltype(ppu.debugger_mode.load()) debugger = ppu.debugger_mode.load();
		const decltype(Emu.GetEmulationIdentifier()) generation = Emu.GetEmulationIdentifier();
		u32 last_call = 0;
		bool call(u32 entry, u32 r3, u32 r4)
		{
			last_call = entry;
			ppu.gpr[1] = top - linkage_bytes;
			ppu.gpr[3] = r3;
			ppu.gpr[4] = r4;
			ppu.loaded_from_savestate = false;
			ppu.fast_call(entry, pbd::t5dr::toc);
			{
				std::lock_guard lock(s.mutex);
				const auto after = ppu.prio.load();
				s.write(fmt::format("{\"type\":\"native_call_return\",\"ms\":%u,\"frame\":%u,\"entry\":%u,\"result\":%u,\"cpu_state_before\":%u,\"cpu_state\":%u,\"priority_before\":%u,\"priority_after\":%u,\"actual_priority_before\":%d,\"actual_priority_after\":%d,\"enqueue_order_before\":%d,\"enqueue_order_after\":%d,\"debugger_preserved\":%s,\"generation_preserved\":%s}\n",
					s.elapsed(), word(0x1008f07c), entry, static_cast<u32>(ppu.gpr[3]), cpu_state_before, static_cast<u32>(ppu.state.load()), priority.all, after.all,
					static_cast<s64>(priority.prio), static_cast<s64>(after.prio), static_cast<s64>(priority.order), static_cast<s64>(after.order),
					ppu.debugger_mode.load() == debugger ? "true" : "false", Emu.GetEmulationIdentifier() == generation ? "true" : "false"));
			}
			const auto flags = ppu.state.load();
			const bool flags_ok = pbd::pbd_owner_flags_allowed(flags, true, ppu.ack_suspend);
			if (flags && flags_ok)
			{
				std::lock_guard lock(s.mutex);
				s.write(fmt::format("{\"type\":\"owner_flags_deferred\",\"entry\":%u,\"cpuFlags\":%u,\"schedulerPreemption\":%s}\n",
					entry, static_cast<u32>(flags), ppu.ack_suspend ? "true" : "false"));
			}
			return flags_ok && ppu.prio.load().all == priority.all && ppu.debugger_mode.load() == debugger &&
			       Emu.GetEmulationIdentifier() == generation;
		}

		void dispatch(bool& control_ok, bool teardown, u32 child, u32 item)
		{
			// Optional one-shot control that reproduces the cold refusal; never a readiness wait.
			// DR's actual 0x9bf3c wait remains inside 0x5b0f0..0x5b0fc, after
			// the full title decision has requested cleanup. Do not gate that decision on it.
			if (teardown)
			{
				control_ok = call(0x9b47c, child, 0) && word(child + 0x64) == 1;
			}
			else if (s.getter_diagnostic)
				control_ok = call(0x9bf3c, 0, 0);
			if (control_ok && !teardown)
			{
				{
					std::lock_guard lock(s.mutex);
					s.sample(ppu, "request_before");
				}
				control_ok = call(0x1e9e8, item, s.target == "vs" ? 2 : 1);
				if (control_ok && byte(0x10684965) != (s.target == "vs" ? 2u : 1u))
					control_ok = false;
				if (control_ok)
				{
					const u32 record = static_cast<u32>(top - 0x80);
					const std::array<be_t<u32>, 2> payload{be_t<u32>{s.target == "vs" ? 2u : 5u}, be_t<u32>{0x01000000u}};
					control_ok = vm::try_access(record, const_cast<be_t<u32>*>(payload.data()), sizeof(payload), true) &&
					             call(0x56f20, record, 0);
					s.applied = control_ok;
				}
			}
		}
	};
	bool boot_observation::finish_request(ppu_thread& ppu, bool teardown, u32 child, u32 callback)
	{
		std::lock_guard lock(mutex);
		if (teardown)
		{
			teardown_child = child;
			teardown_handle_generation = word(child + 12);
			teardown_media_generation = media_generation;
			teardown_requested = true;
			write(fmt::format("{\"type\":\"native_movie_teardown_requested\",\"ms\":%u,\"frame\":%u,\"entry\":%u,\"child\":%u,\"handle_generation\":%u,\"media_generation\":%u,\"worker\":%u}\n",
				elapsed(), word(0x1008f07c), 0x9b47c, child, teardown_handle_generation, teardown_media_generation, media_worker));
			sample(ppu, "teardown_after");
			return true;
		}
		write(fmt::format("{\"type\":\"request_applied\",\"ms\":%u,\"entry\":%u,\"item\":%u,\"initiating_mask\":1,\"owner_preserved\":true,\"stack_guard_unchanged\":true,\"rounds_callback\":%u}\n",
			elapsed(), 0x56f20, target == "vs" ? 2 : 5, word(callback)));
		sample(ppu, "request_after");
		return true;
	}

	bool apply(ppu_thread& ppu, boot_observation& s)
	{
		const bool teardown = s.media_retire && !s.teardown_requested;
		if (!teardown)
			s.attempted = true;
		const auto refuse = [&](const char* reason)
		{
			std::lock_guard lock(s.mutex);
			s.write(fmt::format("{\"type\":\"request_refused\",\"ms\":%u,\"reason\":\"%s\",\"cpuFlags\":%u,\"schedulerPreemption\":%s}\n",
				s.elapsed(), reason, static_cast<u32>(ppu.state.load()), ppu.ack_suspend ? "true" : "false"));
			s.failed = true;
			return false;
		};
		if (Emu.GetTitleID() != pbd::t5dr::title_id || Emu.GetExecutableHash() != pbd::t5dr::executable_hash ||
			ppu.id != pbd::t5dr::main_ppu_id || ppu.gpr[2] != pbd::t5dr::toc || ppu.current_function)
			return refuse("unsupported_or_unsafe_owner");
		const char* owner_refusal = "boot_owner_scheduler";
		if (!pbd::replay_native::service_owner(ppu, static_cast<u64>(Emu.GetEmulationIdentifier()), &owner_refusal))
			return refuse(owner_refusal);
		const pbd::replay_native::owner_guard admitted(ppu);
		if (digest(0x56f20, 0x3dc) != "02244922c6128a1e233814c1af5d152e7b8bd77a6a46954bb1c6bc900dbf154e" ||
			digest(0x1e9e8, 0x90) != "81a254db75cfc221b5b07db3a285f9a3ca27a7c64a7e459d3aa5f376719afc72" ||
			digest(0x1f99bc, 0xc0) != "a2bcf3b309c1cb20f40571c764e45258207b89cd91c993aed306bedbe31d7655" ||
			digest(0x9b47c, 12) != "a1102c60c2ecf5d7cf680a870b7ce31884f6664673b3de4ff2993aa729276c36")
			return refuse("native_code_identity_mismatch");
		const u32 child = movie_child();
		if (s.media_retire && (!child || (!teardown && (child != s.teardown_child || word(child + 12) != s.teardown_handle_generation ||
														   s.media_generation != s.teardown_media_generation || !s.media_free_observed || !s.media_exit_observed || !s.media_allocations.empty() ||
														   word(0x10532a38 + 0x2ac) != 0 || byte(0x10532a38 + 0x2bc) != 1))))
			return refuse("movie_retirement_or_owner_handle_not_preserved");
		const auto item = 0x106846a8u, table = word(item), callback = word(table + 12);
		if (word(item + 8) != 0x10684965 || word(callback + 4) != pbd::t5dr::toc ||
			word(callback) < 0x10000 || word(callback) >= 0x400000)
			return refuse("rounds_item_not_initialized");
		const u64 top = ppu.gpr[1] & ~u64{15};
		if (ppu.gpr[1] > 0xffffffffu || top < u64{ppu.stack_addr} + scratch_bytes ||
			top > u64{ppu.stack_addr} + ppu.stack_size || !vm::check_addr(static_cast<u32>(top - scratch_bytes), vm::page_writable, scratch_bytes))
			return refuse("owner_stack_unavailable");
		restore_scope restore(ppu, static_cast<u32>(top - scratch_bytes));
		if (!restore.save_stack())
			return refuse("owner_stack_unreadable");
		native_call calls{ppu, s, top};
		s.executing = true;
		bool control_ok = true;
		bool dispatch_exception = false;
		try
		{
			calls.dispatch(control_ok, teardown, child, item);
		}
		catch (...)
		{
			control_ok = false;
			dispatch_exception = true;
		}
		std::array<u8, 256> low_guard{};
		const bool guard = vm::try_access(restore.stack_address, low_guard.data(), low_guard.size(), false) &&
		                   !std::memcmp(low_guard.data(), restore.stack.data(), low_guard.size());
		restore.restore();
		const bool preserved = restore.stack_restored && restore.context.registers_match(ppu);
		s.executing = false;
		{
			std::lock_guard lock(s.mutex);
			s.write(fmt::format("{\"type\":\"native_call_preservation\",\"entry\":%u,\"low_stack_guard\":%s,\"stack_restored\":%s,\"registers_restored\":%s,\"control_unchanged\":%s,\"dispatch_exception\":%s,\"cpu_state\":%u}\n",
				calls.last_call, guard ? "true" : "false", restore.stack_restored ? "true" : "false", preserved ? "true" : "false",
				control_ok ? "true" : "false", dispatch_exception ? "true" : "false", static_cast<u32>(ppu.state.load())));
		}
		if (!guard)
			return refuse("scratch_low_guard_changed");
		if (!restore.stack_restored)
			return refuse("scratch_stack_restore_failed");
		if (!preserved)
			return refuse("owner_register_restore_failed");
		if (dispatch_exception)
			return refuse("native_callback_threw");
		if (!control_ok)
			return refuse("native_callback_control_or_setter_readback_changed");
		return s.finish_request(ppu, teardown, child, callback);
	}
} // namespace pbd::boot_request::detail
