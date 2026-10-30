#include "pbd_test.h"

#include "Emu/PBD/pbd_undo_restore.h"
#include "Emu/PBD/pbd_checkpoint_write_plan.h"
#include "Emu/PBD/pbd_replay_capture.h"

#include <algorithm>
#include <array>
#include <random>
#include <string_view>
#include <vector>

using pbd::test::check;
using pbd::undo_restore::plan;

namespace
{
	struct restore_fault
	{
		std::string_view failure;
		struct
		{
			bool started = false;
		} restored;
		bool terminal = false, host_restored = false, writer_rewound = false, inputs_resumed = false, bridge_started = false;
		std::vector<std::uint8_t> memory = std::vector<std::uint8_t>(8192, 0x55);
		bool admit_anchor()
		{
			return failure != "anchor";
		}
		bool plan_journal()
		{
			return failure != "journal";
		}
		bool wait_restore_ready()
		{
			return failure != "busy";
		}
		bool restore_memory()
		{
			if (failure == "voice")
				return false;
			restored.started = true;
			const std::vector<std::uint32_t> pages{0x1000, 0x2000};
			std::vector<std::uint8_t> saved(8192, 0xa7);
			pbd::checkpoint_memory::write_plan writes;
			writes.reset(pages);
			writes.prepare(pages);
			std::uint64_t written = 0;
			const bool ok = writes.write_pages_except(pages, saved, written,
				[&](std::uint32_t address, std::span<const std::uint8_t> bytes)
				{
					const std::size_t offset = address - 0x1000;
					const auto count = failure == "partial_write" ?
				                           std::min(bytes.size(), offset < 4096 ? 4096 - offset : 0) :
				                           bytes.size();
					std::copy_n(bytes.begin(), count, memory.begin() + offset);
					return count == bytes.size();
				});
			if (!ok)
				return false;
			if (failure == "readback")
				memory.back() ^= 1;
			return memory == saved;
		}
		void audit_restore() {}
		bool restore_host_state()
		{
			host_restored = true;
			return failure != "host";
		}
		bool rewind_record_writer()
		{
			writer_rewound = true;
			return failure != "writer_timeout";
		}
		bool discard_future_history()
		{
			return failure != "history";
		}
		void resume_input_capture()
		{
			inputs_resumed = true;
		}
		void start_restore_bridge()
		{
			bridge_started = true;
		}
		void stop_failed_restore()
		{
			terminal = true;
		}
	};
} // namespace

PBD_TEST(PbdUndoRestore, RefusalsBeforeAnyGuestWriteKeepTheCurrentBoundaryUsable)
{
	for (const auto failure : {"anchor", "journal", "busy", "voice"})
	{
		restore_fault steps{failure};
		check(!pbd::undo_restore::restore_anchor(steps));
		check(!steps.restored.started && !steps.terminal);
		check(!steps.host_restored && !steps.writer_rewound && !steps.inputs_resumed && !steps.bridge_started);
		check(std::all_of(steps.memory.begin(), steps.memory.end(), [](auto byte)
			{
				return byte == 0x55;
			}));
	}
}

PBD_TEST(PbdUndoRestore, PartialWritesReadbackAndWriterFailuresCannotResumeGuestInputs)
{
	for (const auto failure : {"partial_write", "readback", "host", "writer_timeout", "history"})
	{
		restore_fault steps{failure};
		check(!pbd::undo_restore::restore_anchor(steps));
		check(steps.restored.started && steps.terminal);
		check(!steps.inputs_resumed && !steps.bridge_started);
		check(steps.memory.front() == 0xa7);
		check(steps.memory.back() == (steps.failure == "partial_write" ? 0x55 : steps.failure == "readback" ? 0xa6 :
																											  0xa7));
		check(steps.host_restored == (steps.failure != "partial_write" && steps.failure != "readback"));
		check(steps.writer_rewound == (steps.failure == "writer_timeout" || steps.failure == "history"));
	}
	// The same transaction serves rewind, Retry and Return; only a complete restore admits its bridge.
	restore_fault steps{};
	check(pbd::undo_restore::restore_anchor(steps));
	check(!steps.terminal && steps.host_restored && steps.writer_rewound && steps.inputs_resumed && steps.bridge_started);
}

PBD_TEST(PbdCheckpointMemory, PublicCapturePreservesLiveVoiceHandlesInStaticAndUndoRestores)
{
	const std::vector<std::uint32_t> pages{0x106d4000, 0x4000a000};
	pbd::checkpoint_memory::write_plan restore;
	restore.reset(pages);
	for (const auto& [begin, end] : pbd::checkpoint_memory::default_exclusions)
		restore.exclude(begin, end, pages);
	for (const auto& [begin, end] : pbd::replay_capture::pbd_exclusions)
		restore.exclude(begin, end, pages);
	restore.prepare(pages);
	std::vector<std::uint8_t> snapshot(8192, 0x11), memory(8192, 0x77);
	const auto write = [&](std::uint32_t address, std::span<const std::uint8_t> bytes)
	{
		const auto slot = address >= pages[1] ? 1u : 0u;
		const auto offset = address - pages[slot];
		check(offset + bytes.size() <= 4096);
		std::copy(bytes.begin(), bytes.end(), memory.begin() + slot * 4096 + offset);
		return true;
	};
	const auto check_live = [&]
	{
		check(memory[0x608] == 0x77 && memory[0x60f] == 0x77);
		check(memory[4096 + 0x7d4] == 0x77 && memory.back() == 0x77);
		check(memory[0x610] == 0x11 && memory[4096 + 0x7d3] == 0x11);
	};
	std::uint64_t written = 0, excluded = 0;
	check(restore.write_pages_except(pages, snapshot, written, write));
	check_live();
	memory.assign(8192, 0x77);
	for (std::size_t slot = 0; slot < pages.size(); ++slot)
		check(restore.write_page_except_ranges(pages[slot], snapshot.data() + slot * 4096, excluded, write));
	check_live();
}

PBD_TEST(PbdUndoRestore, KeepsTheOldestPreimageAndTheOrderOfFinalWrites)
{
	plan restore;
	const std::array<std::uint32_t, 4> first{0x1000, 0x2000, 0x1000, 0xfffff000};
	const std::array<std::uint32_t, 3> later{0x2000, 0x3000, 0x1000};
	check(restore.append(8, first));
	check(restore.append(9, later));
	const auto copies = restore.copies();
	check(restore.candidates() == 7 && copies.size() == 4);
	check(copies[0].journal == 8 && copies[0].slot == 0 && copies[0].address == 0x1000);
	check(copies[1].journal == 8 && copies[1].slot == 1 && copies[1].address == 0x2000);
	check(copies[2].journal == 8 && copies[2].slot == 3 && copies[2].address == 0xfffff000);
	check(copies[3].journal == 9 && copies[3].slot == 1 && copies[3].address == 0x3000);
	// Reuse and ring wrap must not retain the previous correction's page marks.
	restore.clear();
	check(restore.append(0, later));
	check(restore.candidates() == 3 && restore.copies().size() == 3);
	check(restore.copies()[0].journal == 0);
	const std::array<std::uint32_t, 1> malformed{0x1001};
	check(!restore.append(1, malformed));
	check(restore.candidates() == 3 && restore.copies().size() == 3);
	restore.clear();
	check(restore.append(8, first, false) && restore.append(9, later, false));
	check(restore.copies().size() == 7);
}

PBD_TEST(PbdUndoRestore, FullAndUniqueRewindsProduceTheSameBytesWithExclusions)
{
	using page = std::array<std::uint8_t, 4096>;
	struct journal
	{
		std::vector<std::uint32_t> pages;
		std::vector<page> bytes;
	};
	std::mt19937 random(0x52455744);
	plan restore;
	for (std::uint32_t trial = 0; trial < 96; ++trial)
	{
		// Different footprints, empty intervals and repeated writes within a remapped page are all retained.
		std::array<journal, 8> history;
		for (std::uint32_t tick = 0; tick < history.size(); ++tick)
		{
			auto& e = history[tick];
			const auto count = random() % 32;
			for (std::uint32_t slot = 0; slot < count; ++slot)
			{
				e.pages.push_back((1 + random() % 16) * 4096);
				e.bytes.emplace_back();
				for (auto& byte : e.bytes.back())
					byte = static_cast<std::uint8_t>(random());
			}
		}
		std::array<page, 16> full{}, unique{};
		for (auto& bytes : full)
			bytes.fill(0xc7);
		unique = full;
		const auto write = [](auto& image, std::uint32_t address, const page& bytes)
		{
			auto& target = image[address / 4096 - 1];
			for (std::size_t offset = 0; offset < bytes.size(); ++offset)
			{
				// Excluded device/foreign spans stay live even when a page has several captured versions.
				if ((offset >= 17 && offset < 32) || (offset >= 271 && offset < 275) || offset >= 4090)
					continue;
				target[offset] = bytes[offset];
			}
		};
		std::vector<std::uint32_t> final_write_order;
		for (std::size_t tick = history.size(); tick-- > 0;)
		{
			const auto& e = history[tick];
			for (std::size_t slot = e.pages.size(); slot-- > 0;)
			{
				write(full, e.pages[slot], e.bytes[slot]);
				std::erase(final_write_order, e.pages[slot]);
				final_write_order.push_back(e.pages[slot]);
			}
		}
		restore.clear();
		for (std::uint32_t tick = 0; tick < history.size(); ++tick)
			check(restore.append(tick, history[tick].pages));
		std::vector<std::uint32_t> actual_order;
		const auto copies = restore.copies();
		for (auto copy = copies.rbegin(); copy != copies.rend(); ++copy)
		{
			write(unique, copy->address, history[copy->journal].bytes[copy->slot]);
			actual_order.push_back(copy->address);
		}
		check(full == unique);
		check(actual_order == final_write_order);
		check(copies.size() <= restore.candidates());
		for (const auto& bytes : unique)
			check(bytes[17] == 0xc7 && bytes[274] == 0xc7 && bytes[4095] == 0xc7);
	}
}

PBD_TEST(PbdCheckpointMemory, StaticAndUndoWritesPreserveExcludedLiveBytes)
{
	using pbd::checkpoint_memory::write_plan;
	const std::vector<std::uint32_t> pages{0x1000, 0x2000};
	std::vector<std::uint8_t> image(8192, 0xa7), memory(8192, 0x55);
	write_plan restore, other_session;
	restore.reset(pages);
	restore.exclude(0x1800, 0x1a00, pages);
	restore.exclude(0x1880, 0x1900, pages);
	restore.exclude(0x1ff0, 0x2010, pages);
	restore.prepare(pages);
	other_session.reset(pages);
	other_session.exclude(0x1000, 0x3000, pages);
	other_session.prepare(pages);
	const auto write = [&](std::uint32_t address, std::span<const std::uint8_t> bytes)
	{
		check(address >= 0x1000 && address + bytes.size() <= 0x3000);
		std::copy(bytes.begin(), bytes.end(), memory.begin() + address - 0x1000);
		return true;
	};
	const auto excluded = [](std::uint32_t address)
	{
		return (address >= 0x1800 && address < 0x1a00) || (address >= 0x1ff0 && address < 0x2010);
	};
	std::uint64_t written = 0;
	check(restore.write_pages_except(pages, image, written, write));
	check(written == 8192 - 512 - 32);
	for (std::uint32_t offset = 0; offset < memory.size(); ++offset)
		check(memory[offset] == (excluded(0x1000 + offset) ? 0x55 : 0xa7));
	const auto static_result = memory;
	std::fill(memory.begin(), memory.end(), 0x55);
	std::uint64_t skipped = 0;
	for (std::size_t slot = 0; slot < pages.size(); ++slot)
		check(restore.write_page_except_ranges(pages[slot], image.data() + slot * 4096, skipped, write));
	check(memory == static_result && skipped == 512 + 32);
	written = 0;
	check(other_session.write_pages_except(pages, image, written, write) && written == 0);
	check(!restore.write_pages_except(pages, image, written, [](auto, auto)
		{
			return false;
		}));
	image.pop_back();
	check(!restore.write_pages_except(pages, image, written, write));
}

PBD_TEST(PbdCheckpointMemory, AFailedSecondGuestWriteLeavesTheFirstPageRestored)
{
	const std::vector<std::uint32_t> pages{0x1000, 0x3000};
	std::vector<std::uint8_t> image(8192, 0x11), memory(8192, 0x77);
	pbd::checkpoint_memory::write_plan restore;
	restore.reset(pages);
	restore.prepare(pages);
	std::uint64_t written = 0;
	std::uint32_t calls = 0;
	const bool complete = restore.write_pages_except(pages, image, written,
		[&](std::uint32_t address, std::span<const std::uint8_t> bytes)
		{
			++calls;
			if (address == pages[1])
				return false;
			std::copy(bytes.begin(), bytes.end(), memory.begin());
			return true;
		});
	check(!complete && calls == 2 && written == 4096);
	check(memory[0] == 0x11 && memory[4095] == 0x11);
	check(memory[4096] == 0x77 && memory.back() == 0x77);
}
