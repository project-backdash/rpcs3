#pragma once
#include "pbd_stall_metrics.h"

namespace pbd::session_runtime
{
	inline u64 pbd_fault_thread_tag() noexcept
	{
		static thread_local u8 tag;
		return reinterpret_cast<u64>(&tag);
	}

	// Capture lock: the fault handler, the boundary re-arm and the vm remap notices. Re-entry by the holder is allowed
	// (a fault raised while the holder itself writes guest memory), so it cannot self-deadlock; nothing is awaited
	// under it except VirtualProtect and memcpy.
	struct pbd_fault_guard
	{
		std::atomic<u64>& lock;
		u64 me;
		bool reentered;
		explicit pbd_fault_guard(std::atomic<u64>& l) noexcept
			: lock(l), me(pbd_fault_thread_tag()), reentered(l.load(std::memory_order_relaxed) == me)
		{
			if (reentered)
				return;
			pbd::stall_metrics::scope measure_wait("fault_capture_lock_wait", 0, 0, true, 1'000'000, pbd::stall_metrics::sampling::elapsed);
			for (u64 expected = 0; !lock.compare_exchange_weak(expected, me, std::memory_order_acquire); expected = 0)
				std::this_thread::yield();
		}
		~pbd_fault_guard() noexcept
		{
			if (!reentered)
				lock.store(0, std::memory_order_release);
		}
	};

	// Split memory-bound checkpoint copies across the boundary caller and PBD_TICK_CAPTURE_THREADS helpers.
	// The caller waits for all shards before resuming guest execution. Helpers spin briefly between jobs,
	// then sleep on the generation counter.
	struct pbd_capture_pool
	{
		struct job
		{
			u32 address;
			u64 offset;
			u32 bytes;
		};
		std::vector<std::vector<job>> shards; // shards[0] is the caller's
		std::vector<std::thread> threads;
		std::atomic<u32> generation{0};
		std::atomic<u32> done{0};
		std::atomic<bool> failed{false}, stop{false};
		std::atomic<u8*> destination{nullptr};

		bool copy(const std::vector<job>& list, u8* dst) const noexcept
		{
			pbd::stall_metrics::scope measure_copy("checkpoint_shard_copy");
			bool ok = true;
			for (const auto& j : list)
				ok = vm::try_read_stream(j.address, dst + j.offset, j.bytes) && ok;
			return ok;
		}

		void start(const std::vector<job>& jobs, u32 helpers)
		{
			shards.assign(helpers + 1, {});
			u64 total = 0;
			for (const auto& j : jobs)
				total += j.bytes;
			u64 filled = 0;
			for (const auto& j : jobs)
			{
				const usz k = std::min<usz>(helpers, static_cast<usz>(filled * (helpers + 1) / std::max<u64>(total, 1)));
				shards[k].push_back(j);
				filled += j.bytes;
			}
			for (u32 k = 1; k <= helpers; k++)
				threads.emplace_back([this, k]()
					{
						u32 seen = 0;
						for (;;)
						{
							u32 g = generation.load(std::memory_order_acquire);
							for (u32 spin = 0; g == seen && spin < 60000 && !stop.load(std::memory_order_relaxed); spin++)
							{
								_mm_pause();
								g = generation.load(std::memory_order_acquire);
							}
							if (g == seen && !stop.load(std::memory_order_relaxed))
							{
								generation.wait(seen);
								continue;
							}
							if (stop.load(std::memory_order_relaxed))
								return;
							seen = g;
							if (!copy(shards[k], destination.load(std::memory_order_acquire)))
								failed.store(true, std::memory_order_release);
							done.fetch_add(1, std::memory_order_acq_rel);
						}
					});
		}

		bool run(u8* dst) noexcept
		{
			const u32 helpers = static_cast<u32>(threads.size());
			failed.store(false, std::memory_order_release);
			done.store(0, std::memory_order_release);
			destination.store(dst, std::memory_order_release);
			generation.fetch_add(1, std::memory_order_acq_rel);
			generation.notify_all();
			bool ok = copy(shards[0], dst);
			pbd::stall_metrics::scope measure_wait("checkpoint_helpers_wait");
			for (u32 spin = 0; done.load(std::memory_order_acquire) < helpers; spin++)
			{
				if (spin < 200000)
					_mm_pause();
				else
					std::this_thread::yield();
			}
			return ok && !failed.load(std::memory_order_acquire);
		}

		~pbd_capture_pool()
		{
			stop.store(true, std::memory_order_release);
			generation.fetch_add(1, std::memory_order_acq_rel);
			generation.notify_all();
			for (auto& t : threads)
				if (t.joinable())
					t.join();
		}
	};

	inline bool pbd_fault_candidate_address(u64 page) noexcept
	{
		// Main memory, user blocks and the stack. Video memory (RSX local mirror) and the SPU areas are never armed.
		return page >= 0x10000 && page < 0xE0000000ull && !(page >= 0xC0000000ull && page < 0xD0000000ull);
	}
} // namespace pbd::session_runtime
