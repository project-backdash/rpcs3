#pragma once

#include "util/types.hpp"
#include "Emu/Memory/vm.h"

#include <array>
#include <stdexcept>

class ppu_thread;

namespace pbd::json
{
	struct json_value;
}

// The guest state a fresh-game replay restores at a match or round start: the context of the game's random generators,
// read from the guest or from the replay file, and the checked guest reads and writes the restore is made of.
namespace pbd::replay_restore
{
	// The big-endian word at `address`; throws when its page cannot be read.
	inline u32 word(u32 address)
	{
		be_t<u32> value{};
		if (!vm::try_access(address, &value, sizeof(value), false))
			throw std::runtime_error("native_read_unavailable");
		return value;
	}

	// Writes `value` at `address` and reads it back; false when either access failed or the guest holds another value.
	template <typename T>
	bool store(u32 address, const T& value)
	{
		T copy = value, readback{};
		return vm::try_access(address, &copy, sizeof(copy), true) &&
		       vm::try_access(address, &readback, sizeof(readback), false) && readback == value;
	}

	inline bool store_word(u32 address, u32 value)
	{
		return store(address, be_t<u32>{value});
	}

	// Whether each Mokujin on the stage has the style the game's style table holds for round `ordinal`.
	bool round_styles_bound(u32 ordinal) noexcept;

	// The generators a match or round start reseeds or reads: libc rand()'s seed and table, the flow LCG words, the
	// Mersenne Twister, the network generator, and Mokujin's style and eligibility tables.
	struct generators
	{
		std::array<u8, 140> libc{};
		std::array<be_t<u32>, 2> flow{};
		std::array<be_t<u32>, 624> mt{};
		std::array<be_t<u32>, 3> network{};
		std::array<u8, 18> styles{};
		std::array<u8, 8> eligibility{};
		u32 mt_index = 0;

		// Read from the owner thread's guest state; throws when a part cannot be read or the twister changed meanwhile.
		explicit generators(ppu_thread& owner);
		// Read from a replay file's generator context; throws when a field is missing or out of range.
		explicit generators(const pbd::json::json_value& value);
		// Writes every part except the style table back to the guest, each write read back.
		bool apply(ppu_thread& owner) const;
		// Whether the guest's style table already equals the saved one.
		bool style_readback() const;
	};
} // namespace pbd::replay_restore
