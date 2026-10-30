#include "stdafx.h"
#include "pbd_native_call.h"

LOG_CHANNEL(pbd_resource_log, "PBD Resource request");

namespace pbd::replay_native
{
	namespace
	{
		thread_local const ppu_thread* admitted_owner = nullptr;
		// Accumulate flags deferred by the outermost owner_guard for prepare_resources diagnostics.
		thread_local u32 deferred_flags = 0;
		thread_local bool deferred_preemption = false;

		bool owner_flags_ok(const ppu_thread& ppu) noexcept
		{
			// Read state before ack_suspend: LV2 publishes its acknowledgement before raising suspend.
			const auto flags = ppu.state.load();
			const bool preemption = ppu.ack_suspend;
			if (!pbd::pbd_owner_flags_allowed(flags, admitted_owner == &ppu, preemption))
				return false;
			deferred_flags |= static_cast<u32>(flags);
			deferred_preemption = deferred_preemption || (flags & cpu_flag::suspend && preemption);
			return true;
		}

		bool owner_context_ok(ppu_thread& ppu, u64 generation, resource_mode mode = resource_mode::versus) noexcept
		{
			return pbd::practice::native::supported() && generation == static_cast<u64>(Emu.GetEmulationIdentifier()) &&
			       get_current_cpu_thread() == &ppu && ppu.id == pbd::t5dr::main_ppu_id && ppu.gpr[2] == pbd::t5dr::toc &&
			       !ppu.current_function && !pbd::practice::native::in_native_call() &&
			       (mode == resource_mode::versus || mode == resource_mode::practice) &&
			       pbd::practice::native::word(pbd::t5dr::game_mode_address) == static_cast<u32>(mode) &&
			       pbd::practice::native::word(pbd::t5dr::toc - 0x7f04) == 0x1008f060 &&
			       pbd::practice::native::word(pbd::t5dr::toc - 0x7498) == 0x1008f060;
		}
		bool owner_ok(ppu_thread& ppu, u64 generation, resource_mode mode = resource_mode::versus) noexcept
		{
			return owner_context_ok(ppu, generation, mode) && owner_flags_ok(ppu);
		}
		bool setters_ok()
		{
			return pbd::practice::native::body_matches(0x5ab44, 20, "046829de70f159efd02c9919ace69aea3a866292a7194dc3154aea028db23b86") &&
			       pbd::practice::native::body_matches(0x1328c, 12, "b29055699a53ed38deb2196eb21f86f7ddd6994b90081ba156131b5ea7796349") &&
			       pbd::practice::native::body_matches(0x25da54, 200, "c43543e2fd2e1d6dacfdf5caa676ea2c4d4d3be30aa0422038079bfc0f480794") &&
			       pbd::practice::native::body_matches(0x53c88, 620, "8ed063bd13630f70ed9da08e8bd06393f8f3113d5352f653b80faa4d8378d1bc");
		}
		bool readback(const choices& selected) noexcept
		{
			return pbd::practice::native::word(0x10093f50) == selected.left &&
			       pbd::practice::native::word(0x10093f54) == selected.right &&
			       pbd::practice::native::word(0x1008f088) == selected.stage;
		}
		bool request_resources(ppu_thread& ppu, u64 generation, const choices& selected, const char** refusal, resource_mode mode) noexcept
		{
			const auto refuse = [&](const char* reason)
			{
				if (refusal)
					*refusal = reason;
				return false;
			};
			try
			{
				if (!can_prepare_resources(ppu, generation, selected, refusal, mode))
					return false;
				// Request game mode 4 with its resource scene 2, retaining the battle type (VS or Practice).
				// Mode 14 means no mode change: on a select checkpoint that would run the selector's scene 2 instead.
				// The game-mode handler installs the actor rows and styles before later scenes allocate their models.
				if (!pbd::practice::native::preserved_call(ppu, 0x5ab44, 0, selected.left))
					return refuse("resource_left_setter_failed");
				if (!pbd::practice::native::preserved_call(ppu, 0x5ab44, 1, selected.right))
					return refuse("resource_right_setter_failed");
				if (!pbd::practice::native::preserved_call(ppu, 0x1328c, selected.stage, 0))
					return refuse("resource_stage_setter_failed");
				if (!readback(selected))
					return refuse("resource_selection_readback");
				if (!pbd::practice::native::preserved_call(ppu, 0x25da54, 4, 2))
					return refuse("resource_flow_setter_failed");
				if (pbd::practice::native::word(0x1008f064) != 4 || pbd::practice::native::word(pbd::t5dr::subscene_address) != 2)
					return refuse("resource_flow_readback");
				return true;
			}
			catch (...)
			{
				return refuse("resource_prepare_exception");
			}
		}
	} // namespace
	owner_guard::owner_guard(const ppu_thread& owner) noexcept
		: m_previous(admitted_owner)
	{
		if (!m_previous)
		{
			deferred_flags = 0;
			deferred_preemption = false;
		}
		admitted_owner = &owner;
	}
	owner_guard::~owner_guard() noexcept
	{
		admitted_owner = m_previous;
	}
	bool service_owner(ppu_thread& ppu, u64 generation, const char** refusal) noexcept
	{
		const auto refuse = [&](const char* reason)
		{
			if (refusal)
				*refusal = reason;
			return false;
		};
		try
		{
			if (generation != static_cast<u64>(Emu.GetEmulationIdentifier()) || get_current_cpu_thread() != &ppu)
				return refuse("native_match_scheduler_owner_changed");
			// Never yield against a partially applied start, even if an existing helper asks to service again.
			if (admitted_owner == &ppu)
				return owner_flags_ok(ppu) || refuse("native_match_cpu_flags");
			// Service scheduler work through the CPU path, including suspensions owned by LV2 preemption.
			// Stop, debug and diagnostic requests remain refusals; this adapter never clears their flags.
			const auto flags = ppu.state.load();
			const auto entry = pbd::pbd_owner_service_entry(flags, ppu.ack_suspend);
			if (entry == pbd::pbd_owner_entry::ready)
				return true;
			if (entry == pbd::pbd_owner_entry::refuse_flags)
				return refuse("native_match_cpu_flags");
			if (entry == pbd::pbd_owner_entry::refuse_suspension)
				return refuse("native_match_suspension_not_scheduler_preemption");
			if (ppu.check_state())
				return refuse("native_match_scheduler_did_not_resume");
			// Work raised after check_state is deferred by the caller's immediate owner_guard.
			// Use the guard's flag policy here so host scheduling cannot change admission on either peer.
			const auto serviced = ppu.state.load();
			if (!pbd::pbd_owner_flags_allowed(serviced, true, ppu.ack_suspend))
				return refuse("native_match_scheduler_did_not_resume");
			if (generation != static_cast<u64>(Emu.GetEmulationIdentifier()) || get_current_cpu_thread() != &ppu)
				return refuse("native_match_scheduler_owner_changed");
			return true;
		}
		catch (...)
		{
			return refuse("native_match_scheduler_exception");
		}
	}
	bool apply_handicap_options(ppu_thread& ppu, u64 generation, u32 left_percent, u32 right_percent) noexcept
	{
		try
		{
			// 12544 consumes mode4's halfwords at game-flow+4a+4*side and computes vitality itself.
			//  The CS commit also writes these fields; this setter supplies those bounded parameters without CS input.
			if (!owner_ok(ppu, generation) || !left_percent || left_percent > 200 || !right_percent || right_percent > 200 ||
				pbd::practice::native::word(pbd::t5dr::toc - 0x7f60) != pbd::t5dr::game_mode_address ||
				pbd::practice::native::word(pbd::t5dr::toc - 0x60c8) != 0x1008ebf6 ||
				!pbd::practice::native::body_matches(0x12544, 0xe0,
					"1c83dd79453450c720700e159350e62c6c14f24f05abee1e053f181fd795fe9c"))
				return false;
			be_t<u16> left = static_cast<u16>(left_percent), right = static_cast<u16>(right_percent);
			be_t<u16> observed_left{}, observed_right{};
			return vm::try_access(0x1008ebf6, &left, 2, true) &&
			       vm::try_access(0x1008ebfa, &right, 2, true) &&
			       vm::try_access(0x1008ebf6, &observed_left, 2, false) &&
			       vm::try_access(0x1008ebfa, &observed_right, 2, false) &&
			       observed_left == left && observed_right == right;
		}
		catch (...)
		{
			return false;
		}
	}
	bool can_prepare_resources(ppu_thread& ppu, u64 generation, const choices& selected, const char** refusal, resource_mode mode) noexcept
	{
		const auto refuse = [&](const char* reason)
		{
			if (refusal)
				*refusal = reason;
			return false;
		};
		try
		{
			if (!owner_context_ok(ppu, generation, mode))
			{
				if (ppu.current_function)
					return refuse("resource_owner_current_function");
				return refuse("resource_owner_context");
			}
			if (ppu.cia != pbd::t5dr::boundary_pc || ppu.lr != pbd::t5dr::boundary_lr)
				return refuse("resource_owner_boundary");
			if (selected.left >= 85 || selected.right >= 85 || !pbd::stage_rule::loadable(selected.stage))
				return refuse("resource_selection_range");
			if (pbd::practice::native::word(pbd::t5dr::scene_address) != 3 ||
				pbd::practice::native::word(pbd::t5dr::subscene_address) != 44)
				return refuse("resource_select_boundary");
			if (mode == resource_mode::practice && (pbd::practice::native::word(0x1008f060) != 6 ||
													   pbd::practice::native::word(0x1008f064) != 14))
				return refuse("resource_practice_boundary");
			if (!setters_ok())
				return refuse("resource_setters_unverified");
			if (!service_owner(ppu, generation, refusal))
				return false;
			if (!owner_ok(ppu, generation, mode))
				return refuse("resource_owner_cpu_flags");
			if (ppu.cia != pbd::t5dr::boundary_pc || ppu.lr != pbd::t5dr::boundary_lr)
				return refuse("resource_owner_boundary");
			return true;
		}
		catch (...)
		{
			return refuse("resource_admission_exception");
		}
	}
	bool prepare_resources(ppu_thread& ppu, u64 generation, const choices& selected, const char** refusal, resource_mode mode) noexcept
	{
		const bool prepared = request_resources(ppu, generation, selected, refusal, mode);
		// Report flags deferred during the guarded request and those still pending at its end.
		const auto left = ppu.state.load();
		const bool preemption = deferred_preemption || (left & cpu_flag::suspend && ppu.ack_suspend);
		if (admitted_owner == &ppu && (deferred_flags || left))
			pbd_resource_log.notice("Resource request %s: owner_guard deferred CPU flags 0x%x, 0x%x left at its end (scheduler preemption %u).",
				prepared ? "made" : (refusal && *refusal ? *refusal : "refused"), deferred_flags, static_cast<u32>(left), preemption ? 1u : 0u);
		return prepared;
	}
	bool can_continue_resource_handoff(ppu_thread& ppu, u64 generation, const choices& selected) noexcept
	{
		try
		{
			// Scene 2 with pending scene 3 leads to model allocation in scene 10; keep the stock transition intact.
			return owner_ok(ppu, generation) && ppu.cia == 0x53ee4 && setters_ok() && readback(selected) &&
			       pbd::practice::native::word(pbd::t5dr::scene_address) == 2 &&
			       pbd::practice::native::word(pbd::t5dr::subscene_address) == 3;
		}
		catch (...)
		{
			return false;
		}
	}
	bool admit_match_initializer(ppu_thread& ppu, u64 generation, const choices& selected, const char** refusal, u32* scheduler_flags,
		bool* scheduler_preemption) noexcept
	{
		const auto refuse = [&](const char* reason)
		{
			if (refusal)
				*refusal = reason;
			return false;
		};
		try
		{
			if (!pbd::practice::native::supported())
				return refuse("native_match_title_or_executable_identity");
			if (generation != static_cast<u64>(Emu.GetEmulationIdentifier()))
				return refuse("native_match_generation");
			if (get_current_cpu_thread() != &ppu || ppu.id != pbd::t5dr::main_ppu_id)
				return refuse("native_match_owner_thread");
			if (ppu.gpr[2] != pbd::t5dr::toc)
				return refuse("native_match_TOC");
			if (ppu.current_function)
				return refuse("native_match_current_function");
			if (pbd::practice::native::in_native_call())
				return refuse("native_match_recursive_call");
			const auto flags = ppu.state.load();
			if (scheduler_flags)
				*scheduler_flags = static_cast<u32>(flags);
			const bool preemption = ppu.ack_suspend;
			if (scheduler_preemption)
				*scheduler_preemption = preemption;
			if (!service_owner(ppu, generation, refusal))
				return false;
			if (pbd::practice::native::word(pbd::t5dr::game_mode_address) != 4)
				return refuse("native_match_mode");
			if (pbd::practice::native::word(pbd::t5dr::toc - 0x7f04) != 0x1008f060 ||
				pbd::practice::native::word(pbd::t5dr::toc - 0x7498) != 0x1008f060)
				return refuse("native_match_flow_pointer");
			if (ppu.cia != 0x159f0 || ppu.gpr[3] != 2)
				return refuse("native_match_boundary_PC_or_kind");
			if (ppu.lr != 0x414fc && ppu.lr != 0x44f60 && ppu.lr != 0x51e70)
				return refuse("native_match_stock_caller");
			if (pbd::practice::native::word(pbd::t5dr::scene_address) != 12)
				return refuse("native_match_resource_scene");
			if (pbd::practice::native::word(pbd::t5dr::subscene_address) != 16)
				return refuse("native_match_resource_pending_scene");
			if (!setters_ok())
				return refuse("native_match_setter_body_identity");
			if (!readback(selected))
				return refuse("native_match_pre_initializer_choice_readback");
			if (!pbd::practice::native::body_matches(0x159f0, 0x1054,
					"b45802ad8d90b3bc1584caf64ee76b678b3ec86468682391cb0d059c61e1e2ad"))
				return refuse("native_match_initializer_body_identity");
			// The kind-2 initializer dereferences actor+0x8a4 at 0x28e0c8, after the stock loader installs the models.
			// Selection/style fields alone do not establish model readiness; check ownership and readability before any write.
			if (pbd::practice::native::word(pbd::t5dr::toc - 0x7ef0) != 0x10091ab0 ||
				pbd::practice::native::word(0x10091ab0) != 0x10090040 ||
				pbd::practice::native::word(0x10091ab4) != 0x10090910)
				return refuse("native_match_actor_ownership");
			for (u32 actor : {0x10090040u, 0x10090910u})
			{
				const u32 model = pbd::practice::native::word(actor + 0x8a4);
				std::array<u8, 0x20> header{};
				if (!model || !vm::try_access(model, header.data(), header.size(), false))
					return refuse("native_match_actor_model_not_prepared");
			}
			// Install the captured generator context at this stock call; the guest runs the initializer and scene transition once.
			return true;
		}
		catch (...)
		{
			return refuse("native_match_entry_exception");
		}
	}
	bool can_initialize_round(ppu_thread& ppu, u64 generation, const char** refusal) noexcept
	{
		const auto refuse = [&](const char* reason)
		{
			if (refusal)
				*refusal = reason;
			return false;
		};
		try
		{
			// Report the first failed prerequisite without servicing the scheduler or mutating guest state.
			if (!pbd::practice::native::supported())
				return refuse("native_round_title_or_executable_identity");
			if (generation != static_cast<u64>(Emu.GetEmulationIdentifier()))
				return refuse("native_round_generation");
			if (get_current_cpu_thread() != &ppu || ppu.id != pbd::t5dr::main_ppu_id)
				return refuse("native_round_owner_thread");
			if (ppu.gpr[2] != pbd::t5dr::toc)
				return refuse("native_round_TOC");
			if (!owner_flags_ok(ppu))
				return refuse("native_round_cpu_flags");
			if (ppu.current_function)
				return refuse("native_round_current_function");
			if (pbd::practice::native::in_native_call())
				return refuse("native_round_recursive_call");
			if (pbd::practice::native::word(pbd::t5dr::game_mode_address) != 4)
				return refuse("native_round_mode");
			if (pbd::practice::native::word(pbd::t5dr::toc - 0x7f04) != 0x1008f060 ||
				pbd::practice::native::word(pbd::t5dr::toc - 0x7498) != 0x1008f060)
				return refuse("native_round_flow_pointer");
			if (ppu.cia != pbd::t5dr::boundary_pc || ppu.lr != pbd::t5dr::boundary_lr)
				return refuse("native_round_boundary_PC_or_caller");
			const u32 scene = pbd::practice::native::word(pbd::t5dr::scene_address);
			if (scene != 16 && scene != 17)
				return refuse("native_round_scene");
			if (pbd::practice::native::word(pbd::t5dr::toc - 0x7ef0) != 0x10091ab0 ||
				pbd::practice::native::word(0x10091ab0) != 0x10090040 ||
				pbd::practice::native::word(0x10091ab4) != 0x10090910)
				return refuse("native_round_actor_ownership");
			if (!pbd::practice::native::body_matches(0x159f0, 0x1054,
					"b45802ad8d90b3bc1584caf64ee76b678b3ec86468682391cb0d059c61e1e2ad"))
				return refuse("native_round_initializer_body_identity");
			for (u32 actor : {0x10090040u, 0x10090910u})
			{
				const u32 model = pbd::practice::native::word(actor + 0x8a4);
				std::array<u8, 0x20> header{};
				if (!model || !vm::try_access(model, header.data(), header.size(), false))
					return refuse("native_round_actor_model_not_prepared");
			}
			return true;
		}
		catch (...)
		{
			return refuse("native_round_context_exception");
		}
	}
	namespace
	{
		bool round_style_plan(ppu_thread& ppu, u64 generation, u32 ordinal, std::array<u16, 2>& wanted,
			std::array<bool, 2>& change)
		{
			if (ordinal >= 32 || !can_initialize_round(ppu, generation) ||
				pbd::practice::native::word(pbd::t5dr::toc - 0x72a8) != 0x10093470)
				return false;
			for (u32 side = 0; side < 2; ++side)
			{
				const u32 actor = side ? 0x10090910 : 0x10090040;
				const auto character = pbd::practice::native::halfword(actor + 0x40);
				const auto style = pbd::practice::native::halfword(actor + 0x3e);
				if (character == ~0u || style == ~0u)
					return false;
				wanted[side] = static_cast<u16>(style);
				if (character != 0x12)
					continue;
				// 59448 returns zero beyond the nine generated rounds; retain the game's actual rule for ties.
				const auto generated = ordinal > 8 ? 0 : pbd::practice::native::byte(0x10093470 + 9 * side + ordinal);
				if (generated >= 36)
					return false; // 5947C's native style-to-character table contains36 entries.
				wanted[side] = static_cast<u16>(generated);
				change[side] = generated != style;
			}
			if (!change[0] && !change[1])
				return true;
			return pbd::practice::native::word(pbd::t5dr::toc - 0x72a4) == 0x10001424 &&
			       pbd::practice::native::word(pbd::t5dr::toc - 0x6f60) == 0x10095c78 &&
			       pbd::practice::native::word(pbd::t5dr::toc - 0x6f50) == 0x10095c74 &&
			       pbd::practice::native::word(0x10095c78) && pbd::practice::native::word(0x10095c78) != ~0u &&
			       pbd::practice::native::word(0x10095c74) && pbd::practice::native::word(0x10095c74) != ~0u &&
			       pbd::practice::native::body_matches(0x59448, 0x2c,
					   "cde12ca5b64e34736f67b84ccb6c9d761b48e71257567ea8e731a04c09dd9b06") &&
			       pbd::practice::native::body_matches(0x70a44, 0x7e8,
					   "305e9eb4d6d849c938a5eedb6f37580f32da39f313db16e3c05c45db12d97217");
		}
	} // namespace
	bool round_spawn_available(ppu_thread& ppu, u64 generation, u32 selector) noexcept
	{
		try
		{
			if (!can_initialize_round(ppu, generation) ||
				pbd::practice::native::word(pbd::t5dr::toc - 0x7df8) != 0x10091cf0 ||
				!pbd::practice::native::body_matches(0xa523c, 0x16c,
					"1ac93d416fd54fffb8df6d5ba1d6a87f9a1e00128b15576ae58983cec103ca1e"))
				return false;
			u32 count = 0;
			// The stock getter owns both loaded-header and resource-manager lookup branches.
			// It returns the extent used by1AB9C's native spawn loop; no host pointer walk or guessed maximum.
			return pbd::practice::native::preserved_call(ppu, 0xa523c, 0, 0, 0, nullptr, &count) &&
			       count && count <= 0x7fffffffu && selector < count;
		}
		catch (...)
		{
			return false;
		}
	}
	bool can_prepare_round_styles(ppu_thread& ppu, u64 generation, u32 ordinal) noexcept
	{
		try
		{
			std::array<u16, 2> wanted{};
			std::array<bool, 2> change{};
			return round_style_plan(ppu, generation, ordinal, wanted, change);
		}
		catch (...)
		{
			return false;
		}
	}
	bool prepare_round_styles(ppu_thread& ppu, u64 generation, u32 ordinal) noexcept
	{
		try
		{
			std::array<u16, 2> wanted{};
			std::array<bool, 2> change{};
			if (!round_style_plan(ppu, generation, ordinal, wanted, change))
				return false;
			if (!change[0] && !change[1])
				return true;
			for (u32 side = 0; side < 2; ++side)
			{
				if (!change[side])
					continue;
				u32 style = ~0u;
				if (!pbd::practice::native::preserved_call(ppu, 0x59448, side, ordinal, 0, nullptr, &style) ||
					style != wanted[side])
					return false;
			}
			for (u32 side = 0; side < 2; ++side)
			{
				if (!change[side])
					continue;
				const u32 actor = side ? 0x10090910 : 0x10090040;
				be_t<u16> style = wanted[side];
				// This is stock425F0's generated selection parameter, followed by its full resource binding call.
				// No style table, recorded style, move pointer or model pointer is installed from the replay file.
				if (!vm::try_access(actor + 0x3e, &style, sizeof(style), true) ||
					pbd::practice::native::halfword(actor + 0x3e) != wanted[side])
					return false;
			}
			if (!pbd::practice::native::preserved_call(ppu, 0x70a44, 0x10090040, 0x10090910))
				return false;
			for (u32 side = 0; side < 2; ++side)
			{
				const u32 actor = side ? 0x10090910 : 0x10090040;
				std::array<u8, 16> header{};
				const u32 moves = pbd::practice::native::word(actor + 0x50);
				if (pbd::practice::native::halfword(actor + 0x3e) != wanted[side] ||
					!moves || !vm::try_access(moves, header.data(), header.size(), false))
					return false;
			}
			return true;
		}
		catch (...)
		{
			return false;
		}
	}
	bool initialize_round(ppu_thread& ppu, u64 generation, u32 kind) noexcept
	{
		try
		{
			if ((kind != 4 && kind != 5 && kind != 6) || !can_initialize_round(ppu, generation))
				return false;
			const u32 ordinal = pbd::practice::native::word(0x1008f0e4);
			if (!pbd::practice::native::preserved_call(ppu, 0x159f0, kind, 0))
				return false;
			// Kind6 cleans up the old round without incrementing its ordinal; kind4/5 enter the next round.
			return kind == 6 ? pbd::practice::native::word(0x1008f0e4) == ordinal :
			                   pbd::practice::native::word(0x1008f0e4) == ordinal + 1 &&
			                       pbd::practice::native::word(0x1009322c) == 4 &&
			                       pbd::practice::native::word(0x10093230) == 4;
		}
		catch (...)
		{
			return false;
		}
	}
} // namespace pbd::replay_native

namespace pbd::replay_carry
{
	bool owner_ready(ppu_thread& ppu, u64 generation) noexcept
	{
		return pbd::replay_native::owner_ok(ppu, generation);
	}
} // namespace pbd::replay_carry
