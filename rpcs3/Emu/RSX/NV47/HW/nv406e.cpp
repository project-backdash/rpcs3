#include "stdafx.h"
#include "nv406e.h"
#include "nv47_sync.hpp"

#include "Emu/RSX/RSXThread.h"
#include "Emu/RSX/pbd_replay_pacing.h"
#include "Emu/PBD/pbd_replay_viewer_menu.h"
#include "Emu/PBD/pbd_peer_recovery.h"
#include "Emu/Cell/pbd_session_wait.h"

#include "context_accessors.define.h"

namespace rsx
{
	namespace nv406e
	{
		void set_reference(context* ctx, u32 /*reg*/, u32 arg)
		{
			RSX(ctx)->sync();

			// Write ref+get (get will be written again with the same value at command end)
			auto& dma = *vm::_ptr<RsxDmaControl>(RSX(ctx)->dma_address);
			dma.get.release(RSX(ctx)->fifo_ctrl->get_pos());
			dma.ref.store(arg);
		}

		void semaphore_acquire(context* ctx, u32 /*reg*/, u32 arg)
		{
			RSX(ctx)->sync_point_request.release(true);
			const u32 addr = get_address(REGS(ctx)->semaphore_offset_406e(), REGS(ctx)->semaphore_context_dma_406e());

			// Syncronization point, may be associated with memory changes without actually changing addresses
			RSX(ctx)->m_graphics_state |= rsx::pipeline_state::fragment_program_needs_rehash;

			const auto& sema = vm::_ref<RsxSemaphore>(addr);
			const auto& atomic_sema = vm::_ref<atomic_t<RsxSemaphore>>(addr);

			if (sema == arg)
			{
				// Flip semaphore doesnt need wake-up delay
				if (addr != RSX(ctx)->label_addr + 0x10)
				{
					RSX(ctx)->flush_fifo();
					RSX(ctx)->fifo_wake_delay(2);
				}

				return;
			}
			else
			{
				RSX(ctx)->flush_fifo();
			}

			u64 start = get_system_time();
			u64 last_check_val = start;

			// Record only: the frame handshake (label 253, +0xFD0) waits for the guest's NEXT present; while parked here
			// the renderer has consumed everything up to the end of the last present (the bounded wait at window start polls this).
			const bool pbd_handshake = addr == RSX(ctx)->label_addr + 0xFD0;
			if (pbd_handshake)
			{
				::pbd::replay_pacing::handshake_waits.fetch_add(1, std::memory_order_relaxed);
				::pbd::replay_pacing::handshake_parked.store(1, std::memory_order_release);
			}
			const auto pbd_unpark = [&]()
			{
				if (pbd_handshake)
					::pbd::replay_pacing::handshake_parked.store(0, std::memory_order_release);
			};

			while (sema != arg)
			{
				// The frame handshake counts up by one per present. A correction restores and re-executes guest frames while this
				// label keeps its newest value, so an acquire the renderer reaches afterwards can await a value the guest has already
				// written past: waiting for that exact value would stall the renderer for the whole driver recovery timeout (a
				// frozen second on screen, then the same continuation). A handshake a few presents past the awaited value counts as
				// reached.
				if (pbd_handshake)
				{
					const u32 past = u32{sema} - arg;
					if (past >= 1 && past <= 8)
					{
						if (::pbd::replay_pacing::handshake_passed.fetch_add(1, std::memory_order_relaxed) < 16)
							rsx_log.warning("nv406e::semaphore_acquire: frame handshake already passed (expected=0x%x value=0x%x); continuing", arg, u32{sema});
						break;
					}
				}

				if (RSX(ctx)->test_stopped())
				{
					pbd_unpark();
					RSX(ctx)->state += cpu_flag::again;
					return;
				}

				const bool pbd_paused_viewer = pbd_handshake && !RSX(ctx)->in_begin_end &&
				                               ::pbd::replay_viewer::presentation.enabled.load(std::memory_order_relaxed) &&
				                               ::pbd::replay_viewer::presentation.paused.load(std::memory_order_relaxed);
				const bool pbd_network_hold = ::pbd::peer_recovery::refresh_at_handshake(pbd_handshake, RSX(ctx)->in_begin_end,
					::pbd::session_wait::recovering().load(std::memory_order_acquire), ::pbd::session_wait::showing().load(std::memory_order_acquire));
				const bool pbd_controlled_pause = pbd_paused_viewer || pbd_network_hold;
				// A deliberate host pause cannot be a guest semaphore timeout; resume keeps the normal timeout.
				if (pbd_controlled_pause)
					start = last_check_val = get_system_time();

				if (const auto tdr = static_cast<u64>(g_cfg.video.driver_recovery_timeout))
				{
					const u64 current = get_system_time();

					if (current - last_check_val > 20'000)
					{
						// Suspicious amnount of time has passed
						// External pause such as debuggers' pause or operating system sleep may have taken place
						// Ignore it
						start += current - last_check_val;
					}

					last_check_val = current;

					if ((current - start) > tdr)
					{
						// If longer than driver timeout force exit
						rsx_log.error("nv406e::semaphore_acquire has timed out. semaphore_address=0x%X expected=0x%x value=0x%x", addr, arg, u32{sema});
						break;
					}
				}

				if (RSX(ctx)->external_interrupt_lock ||
					(RSX(ctx)->state & (cpu_flag::dbg_global_pause + cpu_flag::exit)) == cpu_flag::dbg_global_pause)
				{
					RSX(ctx)->cpu_wait({});
					continue;
				}

				RSX(ctx)->on_semaphore_acquire_wait();
				// DR's frame handshake is after the last present. A paused viewer must repaint host UI here,
				// without satisfying this semaphore, dispatching another guest update or servicing a guest flip.
				if (pbd_controlled_pause &&
					(RSX(ctx)->async_flip_requested & rsx::thread::flip_request::native_ui) &&
					!(RSX(ctx)->async_flip_requested & rsx::thread::flip_request::emu_requested))
				{
					RSX(ctx)->refresh_native_ui_at_frame_handshake();
				}

				// Wait until the value changes or until 100us pass.
				utils::spin_on_cacheline_once(atomic_sema, sema, 100);
			}

			pbd_unpark();
			RSX(ctx)->fifo_wake_delay();
			RSX(ctx)->performance_counters.idle_time += (get_system_time() - start);
		}

		void semaphore_release(context* ctx, u32 reg, u32 arg)
		{
			const u32 offset = REGS(ctx)->semaphore_offset_406e();

			if (offset % 4)
			{
				rsx_log.warning("NV406E semaphore release is using unaligned semaphore, ignoring. (offset=0x%x)", offset);
				return;
			}

			const u32 ctxt = REGS(ctx)->semaphore_context_dma_406e();

			// By avoiding doing this on flip's semaphore release
			// We allow last gcm's registers reset to occur in case of a crash
			if (const bool is_flip_sema = (offset == 0x10 && ctxt == CELL_GCM_CONTEXT_DMA_SEMAPHORE_R);
				!is_flip_sema)
			{
				RSX(ctx)->sync_point_request.release(true);
			}

			const u32 addr = get_address(offset, ctxt);

			// TODO: Check if possible to write on reservations
			if (RSX(ctx)->label_addr >> 28 != addr >> 28)
			{
				rsx_log.error("NV406E semaphore unexpected address. Please report to the developers. (offset=0x%x, addr=0x%x)", offset, addr);
				RSX(ctx)->recover_fifo();
				return;
			}

			if (addr == RSX(ctx)->device_addr + 0x30 && !arg)
			{
				// HW flip synchronization related, 1 is not written without display queue command (TODO: make it behave as real hw)
				arg = 1;
			}

			util::write_gcm_label<false, true>(ctx, reg, addr, arg);
		}
	} // namespace nv406e
} // namespace rsx
