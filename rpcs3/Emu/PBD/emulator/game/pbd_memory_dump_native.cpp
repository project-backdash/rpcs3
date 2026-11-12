#include "stdafx.h"
#include "pbd_memory_dump_native.h"
#include "Emu/Memory/vm.h"

#include <vector>

namespace pbd::memory_dump
{
	namespace
	{
		// The writer's sink on a file: appends at the file's position; put_at, the dump's last write, seeks first.
		struct file_sink
		{
			const fs::file& file;

			bool put(std::span<const u8> bytes) const
			{
				return file.write(bytes.data(), bytes.size()) == bytes.size();
			}

			bool put_at(u64 offset, std::span<const u8> bytes) const
			{
				return file.seek(static_cast<s64>(offset)) == offset && put(bytes);
			}
		};
	} // namespace

	std::optional<u32> write_file(const fs::file& out, std::span<const range> ranges)
	{
		file_sink sink{out};
		const auto read = [](u32 address, std::span<u8, page_size> page)
		{
			return vm::check_addr(address, vm::page_readable) && vm::try_access(address, page.data(), page_size, false);
		};
		return write(ranges, read, sink);
	}

	std::optional<page_index> read_index(const fs::file& in)
	{
		std::array<u8, header_size> header{};
		if (in.read_at(0, header.data(), header.size()) != header.size())
			return std::nullopt;
		const auto count = page_count(header, in.size());
		if (!count)
			return std::nullopt;
		std::vector<u32> addresses(*count);
		std::array<u8, 4> address{};
		for (u32 i = 0; i < *count; ++i)
		{
			if (in.read_at(record_offset(i), address.data(), address.size()) != address.size())
				return std::nullopt;
			addresses[i] = little_u32(address.data());
		}
		return index_pages(addresses);
	}
} // namespace pbd::memory_dump
