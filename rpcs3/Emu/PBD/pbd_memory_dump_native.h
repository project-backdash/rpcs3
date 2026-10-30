#pragma once

#include "pbd_memory_dump.h"
#include "util/types.hpp"
#include "Utilities/File.h"

#include <optional>
#include <span>

// DRMD001 dumps of this emulator's guest memory, in files opened through the emulator's file functions.
namespace pbd::memory_dump
{
	// Writes the dump of `ranges` to `out`, from its start; the page count, or nothing when a write failed.
	std::optional<u32> write_file(const fs::file& out, std::span<const range> ranges);

	// Each held page's file offset, read from the record addresses; nothing when `in` is not a whole DRMD001 dump.
	std::optional<page_index> read_index(const fs::file& in);
} // namespace pbd::memory_dump
