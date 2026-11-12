#pragma once

#include <cstdint>

class ppu_thread;

namespace pbd::character_rule
{
	enum class action
	{
		stock,
		skip,
		refused
	};
	void configure(std::uint64_t generation, bool ban_jinpachi) noexcept;
	bool banned(std::uint64_t generation) noexcept;
	bool normalize(ppu_thread& owner, std::uint64_t generation) noexcept;
	action event(ppu_thread& owner, std::uint32_t address, bool can_skip) noexcept;
} // namespace pbd::character_rule
