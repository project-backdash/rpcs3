#pragma once

#include "pbd_t5dr_memory.h"
#include "util/types.hpp"
#include <span>

class ppu_thread;

namespace pbd::replay_carry
{
	bool actor_context(ppu_thread& owner, u64 generation, std::span<const replay_memory::body_signature> signatures);
}
