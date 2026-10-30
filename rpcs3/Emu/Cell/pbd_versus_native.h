#pragma once

#include "util/types.hpp"

#include <optional>

class ppu_thread;

namespace pbd::versus
{
	// What offline play's Versus gives a port at one of the game's reads: its own word, less `hidden` and with `added`, or `word`
	// in its place.
	struct port_word
	{
		std::optional<u16> word;
		u16 hidden = 0, added = 0;
	};

	// Offline cellPad reads share one director, sampled at most once per 10 ms; other reads reuse its output.
	// Unsupported titles and ports return no override.
	port_word at_read(u32 port) noexcept;
	// Consumes a local direct-load request only at the authenticated main frame boundary.
	bool frame_boundary(ppu_thread& owner, u32 address) noexcept;
} // namespace pbd::versus
