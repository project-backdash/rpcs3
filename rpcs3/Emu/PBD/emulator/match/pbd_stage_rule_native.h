#pragma once

#include <cstdint>

class ppu_thread;

namespace pbd::stage_rule
{
	// Whether apply() has set the bit in this emulation generation.
	bool applied(std::uint64_t generation) noexcept;
	// Sets the original stages' bit (pbd_stage_rule.h) on the game's main thread, once per emulation generation. False,
	// with nothing written, when the game or its save is not the one this rule reads.
	bool apply(ppu_thread& owner, std::uint64_t generation) noexcept;
} // namespace pbd::stage_rule
