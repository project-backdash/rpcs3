#pragma once

#include "util/types.hpp"

class ppu_thread;
class cpu_thread;

// Runs requested title decisions on the main PPU without substituting scene fields or guest continuations.
// Disabled unless a boot target or witness path is supplied; witnesses are bounded and read-only.
namespace pbd::boot_request
{
	bool event(ppu_thread& ppu, u32 address) noexcept;
	// Observe completed native operations; never changes allocator or worker control.
	void memory_allocated(cpu_thread& cpu, u32 address, u32 bytes) noexcept;
	void memory_freed(cpu_thread& cpu, u32 address, u32 bytes) noexcept;
	void thread_exited(ppu_thread& ppu) noexcept;
} // namespace pbd::boot_request
