#pragma once
#include "../PBD/pbd_env.h"
#include "util/types.hpp"
#include "../PBD/pbd_t5dr_memory.h"
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <vector>

// Optional owner-PPU diagnostics, enabled by PBD_OWNER_TIMELINE=1, on the same steady clock as fn-probe.csv.
//
//   - kind 6/7: command-buffer wait begins/ends (a = segment offset, b = requested count / busy-wait iterations);
//   - kind 10/11: armed-page write-fault capture begins/ends (a = fault address, b = 0 / capture succeeded).
//
// Reports publish owner-timeline.csv alongside fn-probe.csv. Subtract fn-probe-summary.json's origin_steady_ns from
// timeline timestamps to compare them with fn-probe.csv, whose `cyc` column also records the calling thread's host cycles.
namespace pbd::owner_timeline
{
	struct entry
	{
		u32 kind;
		u32 a;
		u64 ns; // steady_clock, ns since its epoch (fn-probe-summary.json carries origin_steady_ns for alignment)
		u64 b;
		u64 c;
	};

	inline bool read_enabled()
	{
		const char* v = pbd::env::find("PBD_OWNER_TIMELINE");
		return v && v[0] == '1';
	}

	inline const bool enabled = read_enabled();
	inline std::vector<entry> ring(enabled ? 4'000'000 : 0);
	inline std::atomic<u64> used{0};
	inline std::atomic<u64> dropped{0};

	inline u64 now_ns()
	{
		return static_cast<u64>(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count());
	}

	// The calling thread's host cycles (QueryThreadCycleTime), or zero outside Windows; defined in pbd_write_watch.cpp.
	u64 thread_cycles();

	inline void record(u32 kind, u32 a, u64 b, u64 c = 0)
	{
		const u64 i = used.fetch_add(1, std::memory_order_relaxed);
		if (i >= ring.size())
		{
			dropped.fetch_add(1, std::memory_order_relaxed);
			return;
		}
		ring[i] = entry{kind, a, now_ns(), b, c};
	}

	constexpr u32 owner_id = pbd::t5dr::main_ppu_id;
} // namespace pbd::owner_timeline
