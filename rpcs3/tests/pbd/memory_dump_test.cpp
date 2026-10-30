#include "Emu/PBD/pbd_memory_dump.h"

#include "pbd_test.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

using namespace pbd::memory_dump;
using pbd::test::check;

namespace
{
	// A guest that maps two pages, 0x10099000 holding 1 and 0x1009a000 holding 2, and not the page after them.
	bool read_guest(std::uint32_t address, std::span<std::uint8_t, page_size> page)
	{
		if (address != 0x10099000 && address != 0x1009a000)
			return false;
		std::fill(page.begin(), page.end(), static_cast<std::uint8_t>(address == 0x10099000 ? 1 : 2));
		return true;
	}

	// The writer's sink on a byte vector, as the native sink is on a file.
	struct buffer
	{
		std::vector<std::uint8_t> bytes;
		bool fail = false;

		bool put(std::span<const std::uint8_t> data)
		{
			bytes.insert(bytes.end(), data.begin(), data.end());
			return !fail;
		}

		bool put_at(std::uint64_t offset, std::span<const std::uint8_t> data)
		{
			if (offset + data.size() > bytes.size())
				return false;
			std::copy(data.begin(), data.end(), bytes.begin() + static_cast<std::ptrdiff_t>(offset));
			return true;
		}
	};

	// The guest's three pages, dumped by the writer.
	std::vector<std::uint8_t> guest_dump()
	{
		buffer out;
		const std::array<range, 1> pages{{{0x10099000, 0x1009c000}}};
		check(pbd::memory_dump::write(pages, read_guest, out) == 2u, "The writer did not count the two mapped pages");
		return out.bytes;
	}

	// The page addresses read back as the paste reads them, from the first 4 bytes of each record.
	std::vector<std::uint32_t> addresses_of(const std::vector<std::uint8_t>& file, std::uint32_t count)
	{
		std::vector<std::uint32_t> out;
		for (std::uint32_t i = 0; i < count; ++i)
			out.push_back(little_u32(file.data() + record_offset(i)));
		return out;
	}
} // namespace

PBD_TEST(PbdMemoryDump, TheWriterPutsTheCountAndEachMappedPageAfterItsAddress)
{
	const auto file = guest_dump();
	const std::vector<std::uint8_t> header{'D', 'R', 'M', 'D', '0', '0', '1', 0, 2, 0, 0, 0};
	check(file.size() == header_size + 2 * record_size, "The dump does not hold exactly the two mapped pages");
	check(std::equal(header.begin(), header.end(), file.begin()), "The header is not the magic and a count of 2");
	const std::vector<std::uint8_t> first{0x00, 0x90, 0x09, 0x10}, second{0x00, 0xa0, 0x09, 0x10};
	check(std::equal(first.begin(), first.end(), file.data() + record_offset(0)), "The first address is not 0x10099000");
	check(std::equal(second.begin(), second.end(), file.data() + record_offset(1)), "The second address is not 0x1009a000");
	check(file[record_offset(0) + 4] == 1 && file[record_offset(1) - 1] == 1, "The first page's bytes are misplaced");
	check(file[record_offset(1) + 4] == 2 && file.back() == 2, "The second page's bytes are misplaced");
}

PBD_TEST(PbdMemoryDump, TheReaderAcceptsWhatTheWriterWrote)
{
	const auto file = guest_dump();
	const auto count = page_count(file, file.size());
	check(count == 2u, "The writer's header did not read back");
	const auto pages = index_pages(addresses_of(file, *count));
	check(pages && pages->size() == 2, "The writer's pages were not indexed");
	check(pages->at(0x10099000) == record_offset(0) + 4, "The first page's bytes are not after its address");
	check(pages->at(0x1009a000) == record_offset(1) + 4, "The second page's bytes are not one record later");
}

PBD_TEST(PbdMemoryDump, ARangeStartsAtThePageThatHoldsItsBeginning)
{
	buffer out;
	const std::array<range, 1> inside{{{0x1009a010, 0x1009a020}}};
	check(pbd::memory_dump::write(inside, read_guest, out) == 1u, "The page holding the range was not written");
	check(little_u32(out.bytes.data() + record_offset(0)) == 0x1009a000, "The record's address is not the page's start");
}

PBD_TEST(PbdMemoryDump, AFailedWriteIsReportedAsNoDump)
{
	buffer out;
	out.fail = true;
	const std::array<range, 1> pages{{{0x10099000, 0x1009c000}}};
	check(!pbd::memory_dump::write(pages, read_guest, out), "A dump whose bytes were not written reported a count");
}

PBD_TEST(PbdMemoryDump, ARequestNamesTheDumpAndItsRanges)
{
	const auto request = parse_paste(std::string_view("C:/runs/a;b/anchor-2-1.bin;0x100996a0-0x10099ef0,10099f10-10099F14"));
	check(request, "A well-formed request was refused");
	check(request->file == "C:/runs/a;b/anchor-2-1.bin", "The dump's name did not end at the last ';'");
	check(request->ranges.size() == 2, "The request did not hold its two ranges");
	check(request->ranges[0].begin == 0x100996a0 && request->ranges[0].end == 0x10099ef0, "The camera block was misread");
	check(request->ranges[1].begin == 0x10099f10 && request->ranges[1].end == 0x10099f14, "A range without 0x was misread");
	const auto wide = parse_paste(std::wstring_view(L"anchor-2-1.bin;20000000-30000000"));
	check(wide && wide->file == L"anchor-2-1.bin" && wide->ranges.size() == 1 && wide->ranges[0].end == 0x30000000,
		"A request from the wide environment was misread");
}

PBD_TEST(PbdMemoryDump, AMalformedRequestIsRefused)
{
	for (const std::string_view text : {"anchor.bin", ";10-20", "anchor.bin;", "anchor.bin;20-10", "anchor.bin;10-10",
			 "anchor.bin;10", "anchor.bin;-20", "anchor.bin;0x-20", "anchor.bin;10-20,", "anchor.bin;1g-20",
			 "anchor.bin;10-123456789"})
		check(!parse_paste(text), std::string("A malformed request was read: ") + std::string(text));
	std::string many = "anchor.bin;10-20";
	for (std::size_t i = 1; i <= max_ranges; ++i)
		many += ",10-20";
	check(!parse_paste(std::string_view(many)), "A request over the range limit was read");
}

PBD_TEST(PbdMemoryDump, TheHeaderMustMatchTheFileLength)
{
	const auto file = guest_dump();
	check(page_count(file, file.size()) == 2u, "A whole dump of two pages was not read");
	check(!page_count(file, file.size() - 1), "A truncated dump was read");
	check(!page_count(std::span(file).first(8), file.size()), "A header without its count was read");
	auto renamed = file;
	renamed[6] = '2';
	check(!page_count(renamed, renamed.size()), "A dump with another magic was read");
}

PBD_TEST(PbdMemoryDump, PagesAreIndexedByTheirRecords)
{
	const auto pages = index_pages(std::vector<std::uint32_t>{0x10099000, 0x20000000});
	check(pages && pages->size() == 2, "Two pages were not indexed");
	check(pages->at(0x10099000) == header_size + 4, "The first page's bytes are not after its address");
	check(pages->at(0x20000000) == header_size + record_size + 4, "The second page's bytes are not one record later");
	check(!index_pages(std::vector<std::uint32_t>{0x10099004}), "An unaligned page was indexed");
	check(!index_pages(std::vector<std::uint32_t>{0x10099000, 0x10099000}), "A repeated page was indexed");
}

PBD_TEST(PbdMemoryDump, ARangeIsCopiedPageByPageAndMissingPagesAreCounted)
{
	const auto pages = index_pages(std::vector<std::uint32_t>{0x10099000});
	check(pages, "The page was not indexed");
	// The camera block, 0x850 bytes, lies inside one page.
	const auto camera = plan(*pages, {0x100996a0, 0x10099ef0});
	check(camera.pieces.size() == 1 && camera.absent == 0, "The camera block was not one copy");
	check(camera.pieces[0].address == 0x100996a0 && camera.pieces[0].size == 0x850, "The camera copy has the wrong extent");
	check(camera.pieces[0].offset == header_size + 4 + 0x6a0, "The camera copy reads the wrong bytes");
	const auto across = plan(*pages, {0x10099f00, 0x1009a010});
	check(across.pieces.size() == 1 && across.pieces[0].size == 0x100, "A range was not cut at the page end");
	check(across.absent == 0x10, "The bytes on the page the dump lacks were not counted");
	const auto none = plan(*pages, {0x20000000, 0x20000004});
	check(none.pieces.empty() && none.absent == 4, "A range the dump lacks was copied");
}

PBD_TEST(PbdMemoryDump, APasteReportsWhatItChanged)
{
	const auto file = guest_dump();
	const auto count = page_count(file, file.size());
	check(count, "The test dump did not read back");
	const auto pages = index_pages(addresses_of(file, *count));
	check(pages, "The test dump's pages were not indexed");
	// The guest holds 1 everywhere: the dump's first page matches it, its second page holds 2.
	constexpr std::uint32_t guest_base = 0x10099000;
	std::vector<std::uint8_t> guest(2 * page_size, 1);
	paste_stats stats;
	for (const auto& piece : plan(*pages, {0x10099ff0, 0x1009a008}).pieces)
	{
		const std::span<std::uint8_t> held(guest.data() + (piece.address - guest_base), piece.size);
		const std::vector<std::uint8_t> before(held.begin(), held.end());
		std::copy_n(file.data() + piece.offset, piece.size, held.begin());
		stats.add(piece.address, before, held);
	}
	check(stats.written == 0x18, "The paste did not write the whole range");
	check(stats.changed == 8, "Only the second page's bytes should have changed");
	check(stats.first_change == 0x1009a000 && stats.last_change == 0x1009a007, "The changed bytes were placed wrongly");
}
