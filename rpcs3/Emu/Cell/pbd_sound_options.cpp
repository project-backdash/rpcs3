#include "stdafx.h"
#include "pbd_sound_options.h"
#include "../PBD/pbd_env.h"
#include "../PBD/pbd_guest_call.h"
#include "PPUThread.h"
#include "Crypto/sha256.h"
#include "Emu/System.h"
#include "Emu/Memory/vm.h"

#include <algorithm>
#include <array>
#include <cstdlib>
#include <cstring>
#include <stdexcept>
#include <utility>

extern bool g_pbd_restored_timebase_valid;

namespace pbd::sound_options
{
	namespace
	{
		constexpr u32 scratch_bytes = 0x1000, linkage_bytes = 0x80;

		std::string ascii_environment(const char* key)
		{
			std::string out;
#ifdef _WIN32
			if (const auto text = pbd::env::find_wide(key))
				for (auto cursor = text; *cursor; ++cursor)
				{
					if (*cursor > 0x7f || out.size() >= max_file_bytes)
						throw std::invalid_argument("sound environment");
					out.push_back(static_cast<char>(*cursor));
				}
#else
			if (const auto text = pbd::env::find(key))
			{
				if (std::strlen(text) > max_file_bytes)
					throw std::invalid_argument("sound environment");
				out = text;
			}
#endif
			return out;
		}

		bool digest_text(std::string_view text) noexcept
		{
			return text.size() == 64 && text.find_first_not_of("0123456789abcdef") == text.npos;
		}

		std::optional<u32> read_word(u32 address) noexcept
		{
			be_t<u32> value{};
			if (!vm::try_access(address, &value, sizeof(value), false))
				return {};
			return value;
		}

		bool callback_graph_matches()
		{
			std::array<u8, 0x130> bytes{};
			for (const auto& span : callback_code)
			{
				if (span.bytes > bytes.size() || !vm::try_access(span.address, bytes.data(), span.bytes, false))
					return false;
				std::array<u8, 32> digest{};
				if (mbedtls_sha256_ret(bytes.data(), span.bytes, digest.data(), 0))
					return false;
				std::string text;
				for (const auto byte : digest)
					text += fmt::format("%02x", byte);
				if (text != span.sha256)
					return false;
			}
			return true;
		}

		// Guest calls use an isolated linkage area below the original SP. The entire reachable graph fits within 0x800 bytes.
		// Preserve a full page so the callback's dead stack stores cannot change a checkpoint's owner-stack image.
		using restore_scope = pbd::guest_call::restore_scope<scratch_bytes>;

		struct call_scope
		{
			bool& active;
			explicit call_scope(bool& value) : active(value)
			{
				active = true;
			}
			~call_scope()
			{
				active = false;
			}
		};

		bool settings_read_back(const auto& setters)
		{
			for (u32 index = 0; index < bindings.size(); ++index)
			{
				u8 saved = 0;
				if (!vm::try_access(bindings[index].saved_byte, &saved, 1, false) || saved != setters[index].value_argument)
					return false;
			}
			return true;
		}
		bool context_preserved(ppu_thread& ppu, const restore_scope& restore)
		{
			std::array<u8, scratch_bytes> stack_after{};
			return restore.stack_restored && restore.context.registers_match(ppu) &&
			       vm::try_access(restore.stack_address, stack_after.data(), stack_after.size(), false) && stack_after == restore.stack;
		}

		bool owner_stack_available(const ppu_thread& ppu, u64 top)
		{
			return !(ppu.gpr[1] > 0xffffffffu || top < u64{ppu.stack_addr} + scratch_bytes ||
					 top > u64{ppu.stack_addr} + ppu.stack_size || !vm::check_addr(static_cast<u32>(top - scratch_bytes), vm::page_writable, scratch_bytes));
		}

	} // namespace

	void runtime_application::configure_environment(std::uint64_t emulation_generation) noexcept
	{
		generation = emulation_generation;
		try
		{
			const auto flag = ascii_environment("PBD_SOUND_OPTIONS_APPLY");
			requested = !flag.empty();
			if (!requested)
				return;
			if (flag != "1")
			{
				setup_error = "apply_flag_invalid";
				return;
			}
			const auto text = ascii_environment("PBD_SOUND_OPTIONS");
			const auto parsed = parse(text);
			if (!parsed.value)
			{
				setup_error = parsed.error;
				return;
			}
			checkpoint_sha256 = ascii_environment("PBD_SOUND_CHECKPOINT_SHA256");
			if (!digest_text(checkpoint_sha256))
			{
				setup_error = "checkpoint_digest_required";
				return;
			}
			desired = *parsed.value;
		}
		catch (...)
		{
			requested = true;
			setup_error = "launch_settings_invalid";
		}
	}

	application_result refuse(std::string_view reason, u32 completed = 0, bool restored = false)
	{
		return application_result{application_status::refused, reason, restored, completed};
	}

	application_result runtime_application::apply_at_owner_boundary(ppu_thread& ppu, std::uint32_t pc, bool first_boundary) noexcept
	{
		if (!requested)
			return {};
		if (attempted || pc != pbd::t5dr::boundary_pc || ppu.id != pbd::t5dr::main_ppu_id)
			return {application_status::waiting, {}};
		attempted = true;

		if (!setup_error.empty())
			return refuse(setup_error);
		if (!desired || !first_boundary || executing || ppu.cia != pbd::t5dr::boundary_pc || ppu.current_function || ppu.state.load())
			return refuse("unsafe_owner_boundary");
		const auto boot = Emu.PBDGetCheckpointBootIdentity();
		if (!boot || boot->emulation_generation != generation || generation != static_cast<u64>(Emu.GetEmulationIdentifier()) ||
			boot->sha256 != checkpoint_sha256 || !g_pbd_restored_timebase_valid)
			return refuse("checkpoint_provenance_mismatch");
		if (Emu.GetExecutableHash() != pbd::t5dr::executable_hash)
			return refuse("unsupported_executable");
		const auto scene = read_word(pbd::t5dr::scene_address), selector = read_word(pbd::t5dr::subscene_address);
		if (!scene || !selector)
			return refuse("scene_unreadable");
		u32 completed = 0;
		try
		{
			const runtime_context context{Emu.GetTitleID(), executable_sha256, generation, boot->emulation_generation,
				ppu.id, pbd::t5dr::main_ppu_id, true, *scene, *selector};
			const auto plan = make_plan(*desired, context, read_word);
			if (!plan.value)
				return refuse(plan.error);
			if (!callback_graph_matches())
				return refuse("callback_graph_mismatch");
			const u64 top = ppu.gpr[1] & ~u64{15};
			if (!owner_stack_available(ppu, top))
				return refuse("owner_stack_unavailable");
			const auto old_priority = ppu.prio.load().all;
			const auto old_debugger = ppu.debugger_mode.load();
			restore_scope restore(ppu, static_cast<u32>(top - scratch_bytes));
			if (!restore.save_stack())
				return refuse("owner_stack_unreadable");
			call_scope call(executing);
			try
			{
				for (const auto& setter : plan.value->setters)
				{
					if (ppu.state.load() || generation != static_cast<u64>(Emu.GetEmulationIdentifier()))
						break;
					ppu.gpr[1] = top - linkage_bytes;
					ppu.gpr[3] = setter.item_argument;
					ppu.gpr[4] = setter.value_argument;
					// A loaded thread's first-call LR rule must not turn this injected call into its outer continuation.
					ppu.loaded_from_savestate = false;
					ppu.fast_call(setter.entry, setter.toc);
					if (ppu.state.load() || ppu.prio.load().all != old_priority || ppu.debugger_mode.load() != old_debugger)
						break;
					++completed;
				}
			}
			catch (...)
			{
				restore.restore();
				return refuse("setter_dispatch_failed", completed, restore.stack_restored && restore.context.registers_match(ppu));
			}
			restore.restore();
			const bool preserved = context_preserved(ppu, restore);
			if (!preserved)
				return refuse("owner_context_restore_failed", completed);
			// Concurrent stop/debug/scheduler flags belong to their producers; never overwrite them with an earlier snapshot.
			if (completed != plan.value->setters.size() || ppu.state.load() || ppu.prio.load().all != old_priority ||
				ppu.debugger_mode.load() != old_debugger || generation != static_cast<u64>(Emu.GetEmulationIdentifier()))
				return refuse("owner_control_state_changed", completed, true);
			if (!settings_read_back(plan.value->setters))
				return refuse("setting_readback_mismatch", completed, true);
			return {application_status::applied, {}, true, completed};
		}
		catch (...)
		{
			return refuse("sound_application_failed", completed);
		}
	}
} // namespace pbd::sound_options
