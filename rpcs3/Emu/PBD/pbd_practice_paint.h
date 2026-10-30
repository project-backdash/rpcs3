#pragma once

#include "util/types.hpp"

class ppu_thread;

namespace pbd::practice::paint
{
	void claim_paint(bool owned, u64 generation) noexcept;
	bool suppress_paint(ppu_thread& ppu, u32 address) noexcept;
	bool screen_returned() noexcept;
} // namespace pbd::practice::paint
