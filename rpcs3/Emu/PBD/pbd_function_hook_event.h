#pragma once

#include "pbd_function_hook_state.h"

namespace pbd::fn_hook::detail
{
	struct random_event
	{
		random_state& c;
		ppu_thread& ppu;
		u32 address;
		bool replay, skip;
		u32 task, task_slot;

		void inspect_mt();
		void inspect_keyed_replay();
		void draw_diagnostics();
		void random_audit();
		void seed_mobs();
		void seed_stage();
		void reseed_match();
		void random_fence();
		void repair_random();
		void keyed_draw(u32 caller, u32 r13);
		bool read_audit_tls(u32 r13, std::array<u8, 137>& out);
		void close_audit(const random_state::audit_frame& f, bool matched, u32 r13);
		void restore_random_fence(u32 base);
	};

	struct hook_event
	{
		config& c;
		ppu_thread& ppu;
		u32 address;
		bool replay = false, skip = false, can_skip = false;
		u32 task = 0, task_slot = 0;
		std::optional<hook_action> admit();
		void trace_render();
		std::optional<hook_action> owner_entry();
		void player_draw_gate();
		void effects_draw_gate();
		void camera_gate();
		void skip_unless();
		std::optional<hook_action> character_policy();
		std::optional<hook_action> vs_gate();
		void task_dispatch();
		void skip_guard();
		void wait_resources();
		void session_exit_gate();
		void clear_exit_gate();
		void early_flush();
		void record_event();
		hook_action run();
	};
} // namespace pbd::fn_hook::detail
