#include "stdafx.h"
#include "pbd_anchor_dump.h"
#include "pbd_env.h"
#include "pbd_memory_dump_native.h"
#include "pbd_replay_switches.h"
#include "pbd_t5dr_memory.h"
#include "Emu/Memory/vm.h"
#include "Utilities/File.h"

#include <chrono>
#include <filesystem>
#include <optional>

LOG_CHANNEL(pbd_anchor_log, "PBD Anchor dump");

namespace pbd::anchor_dump
{
	namespace
	{
		namespace dump = pbd::memory_dump;

		// Enough for every game of a long set dumped by a live run and several replays into one folder.
		constexpr u32 max_dumps_per_game = 999;

		// The game's data block, then the 0x20000000 and 0x40000000 areas, where the guest maps the memory it allocates
		// while running.
		constexpr std::array<dump::range, 3> anchor_ranges{{{t5dr::data_block.begin, t5dr::data_block.end},
			{0x20000000, 0x30000000}, {0x40000000, 0x50000000}}};

		// Paths are read from the wide environment on Windows, where they may hold text outside the ANSI code page.
#ifdef _WIN32
		using setting_char = wchar_t;
		const wchar_t* setting(std::string_view name) noexcept
		{
			return pbd::env::find_wide(name);
		}
#else
		using setting_char = char;
		const char* setting(std::string_view name) noexcept
		{
			return pbd::env::find(name);
		}
#endif

		std::string utf8(const std::filesystem::path& path)
		{
			const auto text = path.u8string();
			return std::string(text.begin(), text.end());
		}

#ifdef PBD_DIAGNOSTIC_BUILD
		// Pastes one range and logs what it wrote. False when the dump could not be read or nothing was written.
		bool paste_range(const fs::file& in, const dump::page_index& pages, dump::range wanted, u32 game, const std::string& path)
		{
			const auto plan = dump::plan(pages, wanted);
			dump::paste_stats stats;
			stats.absent = plan.absent;
			std::array<u8, dump::page_size> pasted{}, held{};
			for (const auto& piece : plan.pieces)
			{
				if (in.read_at(piece.offset, pasted.data(), piece.size) != piece.size)
				{
					pbd_anchor_log.error("Anchor paste refused: %s could not be read at offset %u", path, piece.offset);
					return false;
				}
				if (!vm::try_access(piece.address, held.data(), piece.size, false) ||
					!vm::try_access(piece.address, pasted.data(), piece.size, true))
				{
					stats.unwritable += piece.size;
					continue;
				}
				stats.add(piece.address, {held.data(), piece.size}, {pasted.data(), piece.size});
			}
			const auto where = stats.changed ? fmt::format(", 0x%08x to 0x%08x", stats.first_change, stats.last_change) : std::string();
			const auto text = fmt::format("game %u, 0x%08x-0x%08x from %s: %u bytes written, %u of them changed%s; "
										  "%u bytes not in the dump, %u not writable",
				game, wanted.begin, wanted.end, path, stats.written, stats.changed, where, stats.absent, stats.unwritable);
			if (!stats.written)
			{
				pbd_anchor_log.error("Anchor paste refused, nothing written: %s", text);
				return false;
			}
			pbd_anchor_log.notice("Anchor paste: %s", text);
			return true;
		}

		// Pastes every range `text` requests; false when the request or the dump cannot be read or a range wrote nothing.
		bool paste_request(std::basic_string_view<setting_char> text, u32 game)
		{
			const auto request = dump::parse_paste(text);
			if (!request)
			{
				pbd_anchor_log.error("PBD_TEST_ANCHOR_PASTE refused: expected <dump file>;<begin>-<end>[,<begin>-<end>...] in hex");
				return false;
			}
			const auto path = utf8(std::filesystem::path(request->file));
			const fs::file in(path, fs::read + fs::isfile);
			const auto pages = in ? dump::read_index(in) : std::nullopt;
			if (!pages)
			{
				pbd_anchor_log.error("Anchor paste refused: %s is not a readable DRMD001 dump", path);
				return false;
			}
			pbd_anchor_log.notice("Anchor paste into game %u: %u ranges from %s (%u pages held)", game, request->ranges.size(),
				path, pages->size());
			for (const auto& wanted : request->ranges)
				if (!paste_range(in, *pages, wanted, game, path))
					return false;
			return true;
		}

		// PBD_TEST_ANCHOR_PASTE is a test switch that writes guest memory; everything it does sits behind this one read.
		bool paste(u32 game)
		{
			const setting_char* text = setting("PBD_TEST_ANCHOR_PASTE");
			return !text || !*text || paste_request(text, game);
		}
#endif
	} // namespace

	void write(u32 game, const char* side) noexcept
	{
		try
		{
			const setting_char* folder = setting("PBD_ANCHOR_DUMP");
			if (!folder || !*folder)
				return;
			const auto started = std::chrono::steady_clock::now();
			for (u32 index = 1; index <= max_dumps_per_game; ++index)
			{
				const auto path = utf8(std::filesystem::path(folder) / fmt::format("anchor-%u-%u.bin", game, index));
				if (fs::is_file(path))
					continue;
				std::optional<u32> pages;
				if (const fs::file out(path, fs::write_new + fs::isfile); out)
					pages = dump::write_file(out, anchor_ranges);
				const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - started);
				if (pages)
					pbd_anchor_log.notice("Anchor dump of %s game %u: %u pages written to %s in %u ms", side, game, *pages, path,
						static_cast<u64>(ms.count()));
				else
					pbd_anchor_log.warning("Anchor dump of %s game %u failed: %s could not be written", side, game, path);
				return;
			}
			pbd_anchor_log.warning("Anchor dump of %s game %u skipped: the folder already holds %u of its dumps", side, game,
				max_dumps_per_game);
		}
		catch (...)
		{
			pbd_anchor_log.warning("Anchor dump of %s game %u failed", side, game);
		}
	}

	bool replay_anchor(u32 game) noexcept
	{
#ifdef PBD_DIAGNOSTIC_BUILD
		try
		{
			if (!paste(game))
				return false;
		}
		catch (...)
		{
			pbd_anchor_log.error("Anchor paste into game %u failed", game);
			return false;
		}
#endif
		write(game, "replay");
		return true;
	}

	bool replay_refuses_test_switches() noexcept
	{
		const char* name = replay_switches::first_refused(pbd::env::find);
		if (!name)
			return false;
		pbd_anchor_log.error("Replay refused: %s is a test switch; a test replay also sets %s=1", name,
			replay_switches::opt_in.data());
		return true;
	}
} // namespace pbd::anchor_dump
