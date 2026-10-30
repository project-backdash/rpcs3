#pragma once
#include "util/types.hpp"
#include "Emu/PBD/pbd_stall_metrics.h"
class cpu_thread;
class ppu_thread;
namespace pbd::ordinary_metrics
{
	void hle_begin(cpu_thread&, const char*) noexcept;
	void hle_end(cpu_thread&) noexcept;
	class scope
	{
		pbd::stall_metrics::scope continuous;
		u64 started = 0;
		u64 cycles = 0;
		const char* name;
		u32 kind;

	public:
		scope(u32 kind, const char* name, bool enabled = true, u64 continuous_minimum_ns = 0) noexcept;
		~scope();
	};
	// Bounded, optional operation observations; not a complete writer journal or exclusion. Recorded only when
	// PBD_ORDINARY_OUTPUT names a directory and PBD_ORDINARY_PRODUCERS=1, into ordinary-producers.csv there.
	void producer(const char* site, u64 target, u64 bytes, u64 detail) noexcept;
	// Main PPU only: a checkpoint restore rewound the consumer counter; drop frame records of the undone tick.
	void rewind_frames(u32 counter) noexcept;
	bool requested() noexcept;
	bool capture_allowed() noexcept;
	// One HLE call as the ordinary metrics measure it (BIND_FUNC opens one around every bound function).
	struct hle_scope
	{
		cpu_thread& cpu;
		hle_scope(cpu_thread& value, const char* process_lifetime_function) noexcept
			: cpu(value)
		{
			pbd::ordinary_metrics::hle_begin(value, process_lifetime_function);
		}
		~hle_scope()
		{
			pbd::ordinary_metrics::hle_end(cpu);
		}
		hle_scope(const hle_scope&) = delete;
		hle_scope& operator=(const hle_scope&) = delete;
	};
} // namespace pbd::ordinary_metrics
