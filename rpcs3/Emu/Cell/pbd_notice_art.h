#pragma once

// The pictures the Ranked notices draw that come from the game itself: the placement plate cut from the game's Beginner stone,
// the round-win lamps lit, the "Ranking Match" pill recoloured gold, the A NEW CHALLENGER word art and the character select's
// flame backdrop. None of them is in this program or its installer: the launcher makes them at setup time from the player's
// own copy of the game, read-only, into a folder it names in PBD_NOTICE_ART (<player data>/notice-art), and writes version.txt
// last. The update latch reads them for the overlays, which never read files: once at the start of the process, then again
// for whatever is still missing when the launcher finished the folder later. A picture that is missing,
// from another recipe, a link, outside the folder, not a PNG or not its size reads as missing, and its notice falls back to
// lettering alone (or to nothing). The same read takes every rank's plate from the launcher's rank badges (PBD_RANK_BADGES).
// Standard library only, so the checks are testable.

#include "pbd_game_overlay.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

namespace pbd::notice_art
{
	using u32 = std::uint32_t;
	using bytes = std::shared_ptr<const std::vector<std::uint8_t>>;

	// The recipe and the game files it was made from: the HUD and Select archive entries the launcher's rank badge and portrait
	// extraction pin. A folder made by another recipe or from another copy of the game is not read.
	inline constexpr std::string_view recipe_version =
		"1:0e5e1f6595d857228b50c078789e4d6b18b78ac65f42b5c5439374e2e7d7ee0f:6215e8e32b8101df6ff6b19093c749c8a65c6ff47f4e6d511abd1c32fd665184";
	inline constexpr std::uintmax_t max_picture_bytes = 4 * 1024 * 1024, max_version_bytes = 256;

	// The pictures, each at the size the overlays draw 1:1 at 1080p (2x for the plate and the lamps).
	enum class picture : u32
	{
		placement_stone,
		placement_match,
		new_challenger,
		select_flames,
		// lamps-<total>-<lit>[-flash], for a total of 4 or 5 sets: lit 0..total, and 1..total with the flash.
		lamps_first
	};
	inline constexpr u32 single_count = static_cast<u32>(picture::lamps_first);
	// 5 lamps: lit 0..5 (6) and flashed 1..5 (5); 4 lamps: lit 0..4 (5) and flashed 1..4 (4).
	inline constexpr u32 lamp_count = 6 + 5 + 5 + 4, picture_count = single_count + lamp_count;

	struct entry
	{
		std::string name;
		u32 width = 0, height = 0;
		bool rgb = false; // the backdrop has no alpha
	};

	// The picture of `total` lamps with `lit` of them lit; with `flash` the last lit one carries the game's flash. None for a
	// total other than 4 or 5 (those screens show the plate and what is left only), more lamps lit than the total, or a flash
	// with none lit.
	inline std::optional<u32> lamps(u32 total, u32 lit, bool flash) noexcept
	{
		if ((total != 4 && total != 5) || lit > total || (flash && !lit))
			return std::nullopt;
		const u32 base = single_count + (total == 5 ? 0u : 11u);
		return base + (flash ? total + lit : lit);
	}

	inline const std::array<entry, picture_count>& entries()
	{
		static const auto list = []
		{
			std::array<entry, picture_count> out{};
			out[static_cast<u32>(picture::placement_stone)] = {"placement-stone.png", 492, 192, false};
			out[static_cast<u32>(picture::placement_match)] = {"placement-match.png", 1000, 400, false};
			out[static_cast<u32>(picture::new_challenger)] = {"new-challenger.png", 1240, 168, false};
			out[static_cast<u32>(picture::select_flames)] = {"select-flames.png", 1280, 720, true};
			for (const u32 total : {5u, 4u})
				for (u32 lit = 0; lit <= total; ++lit)
					for (const bool flash : {false, true})
						if (const auto id = lamps(total, lit, flash))
							out[*id] = {"lamps-" + std::to_string(total) + "-" + std::to_string(lit) + (flash ? "-flash" : "") + ".png", 696, 320, false};
			return out;
		}();
		return list;
	}

	// The folder's recipe: version.txt holds recipe_version, with or without a line end.
	inline bool version_matches(const std::filesystem::path& directory)
	{
		std::error_code ec;
		const auto path = directory / "version.txt";
		const auto size = std::filesystem::file_size(path, ec);
		if (ec || size > max_version_bytes || !std::filesystem::is_regular_file(std::filesystem::symlink_status(path, ec)))
			return false;
		std::ifstream file(path, std::ios::binary);
		std::string text(static_cast<std::size_t>(size), '\0');
		if (!file || (size && !file.read(text.data(), static_cast<std::streamsize>(size))))
			return false;
		while (!text.empty() && (text.back() == '\n' || text.back() == '\r'))
			text.pop_back();
		return text == recipe_version;
	}

	// A PNG's 8-bit colour type and size from its header: signature, then IHDR's width, height, bit depth and colour type.
	inline bool header_matches(const std::vector<std::uint8_t>& data, const entry& expected) noexcept
	{
		static constexpr std::array<std::uint8_t, 16> start{0x89, 'P', 'N', 'G', '\r', '\n', 0x1a, '\n', 0, 0, 0, 13, 'I', 'H', 'D', 'R'};
		if (data.size() < 33 || !std::equal(start.begin(), start.end(), data.begin()))
			return false;
		const auto word = [&](std::size_t at)
		{
			return u32{data[at]} << 24 | u32{data[at + 1]} << 16 | u32{data[at + 2]} << 8 | u32{data[at + 3]};
		};
		const std::uint8_t depth = data[24], colour = data[25];
		const bool type = colour == 6 || (expected.rgb && colour == 2);
		return word(16) == expected.width && word(20) == expected.height && depth == 8 && type;
	}

	// One picture's bytes from `directory`: a regular file (not a link) of at most 4 MiB inside the folder, whose header names
	// the picture's size. Null for anything else.
	inline bytes read_picture(const std::filesystem::path& directory, u32 id)
	{
		if (id >= picture_count || directory.empty() || !directory.is_absolute())
			return {};
		const auto& expected = entries()[id];
		std::error_code ec;
		const auto path = directory / expected.name;
		if (!std::filesystem::is_regular_file(std::filesystem::symlink_status(path, ec)) || ec)
			return {};
		const auto folder = std::filesystem::canonical(directory, ec);
		const auto file_path = ec ? std::filesystem::path{} : std::filesystem::canonical(path, ec);
		if (ec || file_path.parent_path() != folder)
			return {};
		const auto size = std::filesystem::file_size(file_path, ec);
		if (ec || size < 33 || size > max_picture_bytes)
			return {};
		std::vector<std::uint8_t> data(static_cast<std::size_t>(size));
		std::ifstream file(file_path, std::ios::binary);
		if (!file || !file.read(reinterpret_cast<char*>(data.data()), static_cast<std::streamsize>(size)) || !header_matches(data, expected))
			return {};
		return std::make_shared<const std::vector<std::uint8_t>>(std::move(data));
	}

	// Every picture of the folder; all null when the folder is not this recipe's.
	struct folder
	{
		std::array<bytes, picture_count> pictures{};
		bool recipe = false; // version.txt matched
		u32 found = 0;
	};
	inline folder read_folder(const std::filesystem::path& directory)
	{
		folder out;
		if (directory.empty() || !directory.is_absolute() || !version_matches(directory))
			return out;
		out.recipe = true;
		for (u32 id = 0; id < picture_count; ++id)
			if ((out.pictures[id] = read_picture(directory, id)))
				out.found++;
		return out;
	}

	// PBD_NOTICE_ART: the launcher's folder of notice pictures; empty when it names none.
	inline std::filesystem::path directory()
	{
#ifdef _WIN32
		const wchar_t* value = pbd::env::find_wide("PBD_NOTICE_ART");
		return value && *value ? std::filesystem::path(value) : std::filesystem::path{};
#else
		const char* value = pbd::env::find("PBD_NOTICE_ART");
		return value && *value ? std::filesystem::path(value) : std::filesystem::path{};
#endif
	}

	// Takes from `read` the pictures `held` lacks; whether it took any. A picture already held is kept.
	inline bool take_missing(folder& held, const folder& read) noexcept
	{
		bool taken = false;
		for (u32 id = 0; id < picture_count; ++id)
			if (!held.pictures[id] && read.pictures[id])
			{
				held.pictures[id] = read.pictures[id];
				held.found++;
				taken = true;
			}
		held.recipe = held.recipe || read.recipe;
		return taken;
	}

	// The process's pictures: read by the latch (load, then retry), and only handed to the overlays (get). An install whose
	// setup predates the notices gets its folder in the background after the launcher starts, while a game held ready for a
	// match may already be running, so what the first read missed is looked for again (retry) until everything is in.
	inline constexpr std::chrono::seconds retry_every{10};
	struct shared_state
	{
		std::mutex mutex;
		folder pictures;
		// Every rank's plate from the launcher's rank badges (PBD_RANK_BADGES), for the plates beside the players' names.
		std::array<bytes, pbd::game_overlay::max_rank_index + 1> rank_badges{};
		bool loaded = false, complete = false;
		std::chrono::steady_clock::time_point next_retry{};
	};
	inline bool all_in(const shared_state& shared) noexcept
	{
		return shared.pictures.found == picture_count &&
		       std::all_of(shared.rank_badges.begin(), shared.rank_badges.end(), [](const bytes& badge)
				   {
					   return static_cast<bool>(badge);
				   });
	}
	inline shared_state& state()
	{
		static shared_state value;
		return value;
	}
	// Reads the folder the first time it is called; returns what it read then (for the log), or nothing on later calls.
	inline std::optional<folder> load()
	{
		auto& shared = state();
		{
			std::lock_guard lock(shared.mutex);
			if (shared.loaded)
				return std::nullopt;
			shared.loaded = true;
		}
		auto read = read_folder(directory());
		std::array<bytes, pbd::game_overlay::max_rank_index + 1> badges{};
		const auto badge_folder = pbd::game_overlay::rank_badge_directory();
		for (u32 rank = 0; rank < badges.size(); ++rank)
			badges[rank] = pbd::game_overlay::read_rank_badge(badge_folder, rank);
		std::lock_guard lock(shared.mutex);
		shared.pictures = read;
		shared.rank_badges = badges;
		shared.complete = all_in(shared);
		return read;
	}
	// Looks again, at most every retry_every, for the pictures and plates the reads so far missed (the latch calls it when the
	// launcher hands this game a panel or a set's VS cards); nothing is read once everything is in. Returns the folder when this
	// look found more (for the log), or nothing.
	inline std::optional<folder> retry(std::chrono::steady_clock::time_point now)
	{
		auto& shared = state();
		bool pictures_missing = false;
		std::array<bool, pbd::game_overlay::max_rank_index + 1> badge_missing{};
		{
			std::lock_guard lock(shared.mutex);
			if (!shared.loaded || shared.complete || now < shared.next_retry)
				return std::nullopt;
			shared.next_retry = now + retry_every;
			pictures_missing = shared.pictures.found < picture_count;
			for (u32 rank = 0; rank < badge_missing.size(); ++rank)
				badge_missing[rank] = !shared.rank_badges[rank];
		}
		const auto read = pictures_missing ? read_folder(directory()) : folder{};
		std::array<bytes, pbd::game_overlay::max_rank_index + 1> badges{};
		const auto badge_folder = pbd::game_overlay::rank_badge_directory();
		for (u32 rank = 0; rank < badges.size(); ++rank)
			if (badge_missing[rank])
				badges[rank] = pbd::game_overlay::read_rank_badge(badge_folder, rank);
		std::lock_guard lock(shared.mutex);
		bool found = take_missing(shared.pictures, read);
		for (u32 rank = 0; rank < badges.size(); ++rank)
			if (!shared.rank_badges[rank] && badges[rank])
			{
				shared.rank_badges[rank] = badges[rank];
				found = true;
			}
		shared.complete = all_in(shared);
		return found ? std::optional<folder>(shared.pictures) : std::nullopt;
	}
	inline bytes get(u32 id)
	{
		auto& shared = state();
		std::lock_guard lock(shared.mutex);
		return id < picture_count ? shared.pictures.pictures[id] : bytes{};
	}
	inline bytes get(picture id)
	{
		return get(static_cast<u32>(id));
	}
	inline bytes rank_badge(u32 rank)
	{
		auto& shared = state();
		std::lock_guard lock(shared.mutex);
		return rank < shared.rank_badges.size() ? shared.rank_badges[rank] : bytes{};
	}
} // namespace pbd::notice_art
