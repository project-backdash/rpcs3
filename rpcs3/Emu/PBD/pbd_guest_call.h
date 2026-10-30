#pragma once

#include "../Cell/PPUThread.h"
#include "Emu/Memory/vm.h"
#include <array>
#include <cstddef>
#include <cstring>
#include <memory>
#include <string>

extern thread_local std::string (*g_tls_log_prefix)();

// A hook that calls game code on the main thread puts the thread back exactly as the game left it: guest_context
// saves the registers and the thread's bookkeeping, and restore_scope also saves the scratch stack below the caller's
// stack pointer that the call uses. Asynchronous CPU control, priority and debugger flags are not saved; they stay
// live rather than being restored from a stale copy.
namespace pbd::guest_call
{
	struct guest_context
	{
		std::array<u64, 32> gpr{};
		std::array<f64, 32> fpr{};
		std::array<v128, 32> vr{};
		std::array<u8, 32> cr{}, fpscr{};
		std::array<std::byte, 128> rdata{};
		std::array<u64, 8> syscall_args{};
		decltype(ppu_thread::xer) xer{};
		decltype(ppu_thread::hv_ctx) hv_ctx{};
		decltype(ppu_thread::call_history) call_history;
		decltype(ppu_thread::syscall_history) syscall_history;
		decltype(ppu_thread::hle_func_calls_with_toc_info) hle_func_calls;
		u64 lr, ctr, rtime, block_hash, exec_bytes, start_time, end_time;
		u64 last_ftsc, last_ftime, last_fail, last_succ, res_notify_time;
		u32 cia, vrsave, jm_mask, raddr, res_cached, res_notify, res_notify_postpone_streak, last_faddr;
		s32 cancel_sleep;
		v128 sat;
		bool nj, use_full_rdata, loaded_from_savestate, interrupt_thread_executing, stop_flag_removal_protection;
		const char *current_function, *last_function, *current_module;
		std::string (*log_prefix)();
		std::shared_ptr<utils::serial> optional_savestate_state;

		explicit guest_context(const ppu_thread& ppu)
			: xer(ppu.xer), hv_ctx(ppu.hv_ctx), call_history(ppu.call_history), syscall_history(ppu.syscall_history),
			  hle_func_calls(ppu.hle_func_calls_with_toc_info), lr(ppu.lr), ctr(ppu.ctr), rtime(ppu.rtime), block_hash(ppu.block_hash),
			  exec_bytes(ppu.exec_bytes), start_time(ppu.start_time), end_time(ppu.end_time), last_ftsc(ppu.last_ftsc),
			  last_ftime(ppu.last_ftime), last_fail(ppu.last_fail), last_succ(ppu.last_succ), res_notify_time(ppu.res_notify_time),
			  cia(ppu.cia), vrsave(ppu.vrsave), jm_mask(ppu.jm_mask), raddr(ppu.raddr), res_cached(ppu.res_cached),
			  res_notify(ppu.res_notify), res_notify_postpone_streak(ppu.res_notify_postpone_streak), last_faddr(ppu.last_faddr),
			  cancel_sleep(ppu.cancel_sleep), sat(ppu.sat), nj(ppu.nj), use_full_rdata(ppu.use_full_rdata),
			  loaded_from_savestate(ppu.loaded_from_savestate), interrupt_thread_executing(ppu.interrupt_thread_executing),
			  stop_flag_removal_protection(ppu.stop_flag_removal_protection), current_function(ppu.current_function),
			  last_function(ppu.last_function), current_module(ppu.current_module), log_prefix(g_tls_log_prefix),
			  optional_savestate_state(ppu.optional_savestate_state)
		{
			std::memcpy(gpr.data(), ppu.gpr, sizeof(ppu.gpr));
			std::memcpy(fpr.data(), ppu.fpr, sizeof(ppu.fpr));
			std::memcpy(vr.data(), ppu.vr, sizeof(ppu.vr));
			std::memcpy(cr.data(), ppu.cr.bits, sizeof(ppu.cr.bits));
			std::memcpy(fpscr.data(), ppu.fpscr.bits.bits, sizeof(ppu.fpscr.bits.bits));
			std::memcpy(rdata.data(), ppu.rdata, sizeof(ppu.rdata));
			std::memcpy(syscall_args.data(), ppu.syscall_args, sizeof(ppu.syscall_args));
		}
		void restore(ppu_thread& ppu) noexcept
		{
			std::memcpy(ppu.gpr, gpr.data(), sizeof(ppu.gpr));
			std::memcpy(ppu.fpr, fpr.data(), sizeof(ppu.fpr));
			std::memcpy(ppu.vr, vr.data(), sizeof(ppu.vr));
			std::memcpy(ppu.cr.bits, cr.data(), sizeof(ppu.cr.bits));
			std::memcpy(ppu.fpscr.bits.bits, fpscr.data(), sizeof(ppu.fpscr.bits.bits));
			std::memcpy(ppu.rdata, rdata.data(), sizeof(ppu.rdata));
			std::memcpy(ppu.syscall_args, syscall_args.data(), sizeof(ppu.syscall_args));
			ppu.xer = xer;
			ppu.hv_ctx = hv_ctx;
			ppu.lr = lr;
			ppu.ctr = ctr;
			ppu.rtime = rtime;
			ppu.block_hash = block_hash;
			ppu.exec_bytes = exec_bytes;
			ppu.start_time = start_time;
			ppu.end_time = end_time;
			ppu.last_ftsc = last_ftsc;
			ppu.last_ftime = last_ftime;
			ppu.last_fail = last_fail;
			ppu.last_succ = last_succ;
			ppu.res_notify_time = res_notify_time;
			ppu.cia = cia;
			ppu.vrsave = vrsave;
			ppu.jm_mask = jm_mask;
			ppu.raddr = raddr;
			ppu.res_cached = res_cached;
			ppu.res_notify = res_notify;
			ppu.res_notify_postpone_streak = res_notify_postpone_streak;
			ppu.last_faddr = last_faddr;
			ppu.cancel_sleep = cancel_sleep;
			ppu.sat = sat;
			ppu.nj = nj;
			ppu.use_full_rdata = use_full_rdata;
			ppu.loaded_from_savestate = loaded_from_savestate;
			ppu.interrupt_thread_executing = interrupt_thread_executing;
			ppu.stop_flag_removal_protection = stop_flag_removal_protection;
			ppu.current_function = current_function;
			ppu.last_function = last_function;
			ppu.current_module = current_module;
			g_tls_log_prefix = log_prefix;
			ppu.optional_savestate_state = optional_savestate_state;
			ppu.call_history.data.swap(call_history.data);
			ppu.call_history.index = call_history.index;
			ppu.call_history.last_r1 = call_history.last_r1;
			ppu.call_history.last_r2 = call_history.last_r2;
			ppu.syscall_history.data.swap(syscall_history.data);
			ppu.syscall_history.index = syscall_history.index;
			ppu.syscall_history.count_debug_arguments = syscall_history.count_debug_arguments;
			ppu.hle_func_calls_with_toc_info.swap(hle_func_calls);
		}
		bool registers_match(const ppu_thread& ppu) const noexcept
		{
			return !std::memcmp(gpr.data(), ppu.gpr, sizeof(ppu.gpr)) && !std::memcmp(fpr.data(), ppu.fpr, sizeof(ppu.fpr)) &&
			       !std::memcmp(vr.data(), ppu.vr, sizeof(ppu.vr)) && !std::memcmp(cr.data(), ppu.cr.bits, sizeof(ppu.cr.bits)) &&
			       !std::memcmp(fpscr.data(), ppu.fpscr.bits.bits, sizeof(ppu.fpscr.bits.bits)) &&
			       !std::memcmp(rdata.data(), ppu.rdata, sizeof(ppu.rdata)) && !std::memcmp(&sat, &ppu.sat, sizeof(sat)) &&
			       ppu.cia == cia && ppu.lr == lr && ppu.ctr == ctr && ppu.vrsave == vrsave && ppu.jm_mask == jm_mask &&
			       ppu.xer.so == xer.so && ppu.xer.ov == xer.ov && ppu.xer.ca == xer.ca && ppu.xer.cnt == xer.cnt &&
			       ppu.nj == nj && ppu.raddr == raddr && ppu.rtime == rtime && ppu.use_full_rdata == use_full_rdata &&
			       ppu.res_cached == res_cached && ppu.res_notify == res_notify && ppu.res_notify_time == res_notify_time &&
			       ppu.res_notify_postpone_streak == res_notify_postpone_streak && ppu.block_hash == block_hash &&
			       ppu.exec_bytes == exec_bytes && ppu.start_time == start_time && ppu.end_time == end_time && ppu.cancel_sleep == cancel_sleep &&
			       ppu.last_ftsc == last_ftsc && ppu.last_ftime == last_ftime && ppu.last_faddr == last_faddr &&
			       ppu.last_fail == last_fail && ppu.last_succ == last_succ && ppu.stop_flag_removal_protection == stop_flag_removal_protection &&
			       !std::memcmp(syscall_args.data(), ppu.syscall_args, sizeof(ppu.syscall_args)) &&
			       ppu.optional_savestate_state == optional_savestate_state && g_tls_log_prefix == log_prefix &&
			       !std::memcmp(&hv_ctx, &ppu.hv_ctx, sizeof(hv_ctx)) &&
			       ppu.call_history.index == call_history.index && ppu.call_history.last_r1 == call_history.last_r1 &&
			       ppu.call_history.last_r2 == call_history.last_r2 && ppu.syscall_history.index == syscall_history.index &&
			       ppu.syscall_history.count_debug_arguments == syscall_history.count_debug_arguments &&
			       ppu.loaded_from_savestate == loaded_from_savestate && ppu.interrupt_thread_executing == interrupt_thread_executing &&
			       ppu.current_function == current_function && ppu.last_function == last_function && ppu.current_module == current_module;
		}
	};

	template <u32 scratch_bytes>
	struct restore_scope
	{
		ppu_thread& ppu;
		guest_context context;
		u32 stack_address;
		std::array<u8, scratch_bytes> stack{};
		bool armed = false, restored = false, stack_restored = false;
		restore_scope(ppu_thread& value, u32 address) : ppu(value), context(value), stack_address(address) {}
		bool save_stack() noexcept
		{
			armed = vm::try_access(stack_address, stack.data(), stack.size(), false);
			return armed;
		}
		void restore() noexcept
		{
			if (restored)
				return;
			if (armed)
				stack_restored = vm::try_access(stack_address, stack.data(), stack.size(), true);
			context.restore(ppu);
			restored = true;
		}
		~restore_scope()
		{
			restore();
		}
	};
} // namespace pbd::guest_call
