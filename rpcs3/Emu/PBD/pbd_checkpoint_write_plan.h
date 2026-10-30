#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <utility>
#include <vector>

// The checkpoint writer's exclusions and ordered copies. The callback is the guest-memory boundary, also used by tests.
namespace pbd::checkpoint_memory
{
	using u8 = std::uint8_t;
	using u16 = std::uint16_t;
	using u32 = std::uint32_t;
	using u64 = std::uint64_t;
	using usz = std::size_t;
	inline constexpr std::array<std::pair<u32, u32>, 7> default_exclusions{{{0x104ccdf8, 0x104cce40},
		{0x1062474c, 0x10624750}, {0x106d4554, 0x106d4608}, {0x107520b0, 0x107520e0}, {0x107521d4, 0x107521e0},
		{0x40011700, 0x400119c0}, {0x4002d320, 0x4002d4e8}}};

	inline bool default_excluded(u32 address) noexcept
	{
		for (const auto& [begin, end] : default_exclusions)
			if (address >= begin && address < end)
				return true;
		return false;
	}

	class write_plan
	{

		struct write_span
		{
			u32 page;
			u16 offset, length;
		};
		struct write_run
		{
			u32 address;
			u64 image_offset;
			u32 length;
		};
		std::vector<u8> exclude_mask;
		std::vector<std::pair<u64, u64>> exclude_ranges;
		std::vector<write_span> restore_spans;
		std::vector<write_run> restore_runs;

	public:
		bool excluded_at(usz page, u32 offset, u32 address) const noexcept
		{
			const usz slot = page * 4096 + offset;
			return slot < exclude_mask.size() ? exclude_mask[slot] != 0 : default_excluded(address);
		}

	public:
		void reset(const std::vector<u32>& pages)
		{
			exclude_mask.assign(pages.size() * 4096ull, 0);
			exclude_ranges.clear();
			restore_spans.clear();
			restore_runs.clear();
		}
		void exclude(u64 begin, u64 end, const std::vector<u32>& pages)
		{
			if (begin < end)
				exclude_ranges.emplace_back(begin, end);
			for (usz i = 0; i < pages.size(); ++i)
			{
				const u64 lo = std::max<u64>(begin, pages[i]), hi = std::min<u64>(end, u64{pages[i]} + 4096);
				for (u64 a = lo; a < hi; ++a)
					exclude_mask[i * 4096 + a - pages[i]] = 1;
			}
		}
		void prepare(const std::vector<u32>& pages)
		{
			std::sort(exclude_ranges.begin(), exclude_ranges.end());
			for (u32 i = 0; i < pages.size(); ++i)
				for (u32 o = 0; o < 4096;)
				{
					if (exclude_mask[i * 4096ull + o])
					{
						++o;
						continue;
					}
					const u32 first = o;
					while (o < 4096 && !exclude_mask[i * 4096ull + o])
						++o;
					restore_spans.push_back({i, static_cast<u16>(first), static_cast<u16>(o - first)});
				}
			for (const auto& span : restore_spans)
			{
				const u32 address = pages[span.page] + span.offset;
				const u64 offset = span.page * 4096ull + span.offset;
				if (!restore_runs.empty() && u64{restore_runs.back().address} + restore_runs.back().length == address &&
					restore_runs.back().image_offset + restore_runs.back().length == offset && restore_runs.back().length < 0x100000)
					restore_runs.back().length += span.length;
				else
					restore_runs.push_back({address, offset, span.length});
			}
		}

		template <typename Write>
		bool write_pages_except(const std::vector<u32>& pages, std::vector<u8>& image, u64& written, Write&& write) const noexcept
		{
			if (image.size() != pages.size() * 4096ull)
				return false;
			if (exclude_mask.size() == pages.size() * 4096ull)
			{
				for (const auto& r : restore_runs)
				{
					if (!write(r.address, std::span<const u8>{image.data() + r.image_offset, r.length}))
						return false;
					written += r.length;
				}
				return true;
			}
			for (usz i = 0; i < pages.size(); i++)
			{
				for (u32 o = 0; o < 4096;)
				{
					if (excluded_at(i, o, pages[i] + o))
					{
						++o;
						continue;
					}
					const u32 s = o;
					while (o < 4096 && !excluded_at(i, o, pages[i] + o))
						++o;
					if (!write(pages[i] + s, std::span<const u8>{image.data() + i * 4096 + s, o - s}))
						return false;
					written += o - s;
				}
			}
			return true;
		}

		// Writes one 4 KiB page from image, skipping declared exclusion spans (the rolling undo honours the same
		// never-restored spans as the static restore write). excluded accumulates the skipped byte count.
		template <typename Write>
		bool write_page_except_ranges(u32 page, const u8* image, u64& excluded, Write&& write) const noexcept
		{
			u32 o = 0;
			if (!exclude_ranges.empty())
			{
				for (const auto& [begin, end] : exclude_ranges)
				{
					if (end <= page || begin >= page + 4096ull)
						continue;
					const u32 lo = static_cast<u32>(std::max<u64>(begin, page) - page), hi = static_cast<u32>(std::min<u64>(end, page + 4096ull) - page);
					if (lo > o && !write(page + o, std::span<const u8>{image + o, lo - o}))
						return false;
					if (hi > o)
						excluded += hi - std::max(lo, o);
					o = std::max(o, hi);
				}
			}
			return o >= 4096 || write(page + o, std::span<const u8>{image + o, 4096 - o});
		}
	};
} // namespace pbd::checkpoint_memory
