#pragma once

#include "../PBD/pbd_game_overlay_parser.h"

namespace pbd::game_overlay
{
	inline parse_result read(const std::filesystem::path& path)
	{
		std::error_code ec;
		const auto size = std::filesystem::file_size(path, ec);
		if (ec)
			return {{}, "unavailable"};
		if (size > max_file_bytes)
			return {{}, "file_too_large"};
		std::ifstream file(path, std::ios::binary);
		if (!file)
			return {{}, "unavailable"};
		std::string text(static_cast<std::size_t>(size), '\0');
		if (size && !file.read(text.data(), static_cast<std::streamsize>(size)))
			return {{}, "read_failed"};
		return parse(text);
	}

	// Read <directory>/rank_NN.png on the update thread; rendering receives only the bytes. Missing or oversized files,
	// bad PNG signatures and non-absolute directories leave the panel with its rank text alone.
	inline constexpr std::uintmax_t max_rank_badge_bytes = 2 * 1024 * 1024;
	inline std::shared_ptr<const std::vector<std::uint8_t>> read_rank_badge(const std::filesystem::path& directory, std::uint32_t rank)
	{
		static constexpr std::array<char, 8> png{'\x89', 'P', 'N', 'G', '\r', '\n', '\x1a', '\n'};
		if (directory.empty() || !directory.is_absolute() || rank > max_rank_index)
			return {};
		const auto path = directory / ("rank_" + std::string(rank < 10 ? "0" : "") + std::to_string(rank) + ".png");
		std::error_code ec;
		const auto size = std::filesystem::file_size(path, ec);
		if (ec || size <= png.size() || size > max_rank_badge_bytes)
			return {};
		std::vector<std::uint8_t> bytes(static_cast<std::size_t>(size));
		std::ifstream file(path, std::ios::binary);
		if (!file || !file.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(size)) ||
			!std::equal(png.begin(), png.end(), bytes.begin(), [](char a, std::uint8_t b)
				{
					return static_cast<std::uint8_t>(a) == b;
				}))
			return {};
		return std::make_shared<const std::vector<std::uint8_t>>(std::move(bytes));
	}

	// PBD_RANK_BADGES: the launcher's folder of rank badges (<profile>/rank-badges); empty when it names none.
	inline std::filesystem::path rank_badge_directory()
	{
#ifdef _WIN32
		const wchar_t* value = pbd::env::find_wide("PBD_RANK_BADGES");
		return value && *value ? std::filesystem::path(value) : std::filesystem::path{};
#else
		const char* value = pbd::env::find("PBD_RANK_BADGES");
		return value && *value ? std::filesystem::path(value) : std::filesystem::path{};
#endif
	}

	inline std::string event_text(std::uint64_t generation, std::uint64_t serial, std::string_view action, std::uint32_t selected)
	{
		return "PBD-EVENT-1\ngeneration=" + std::to_string(generation) + "\nserial=" + std::to_string(serial) +
		       "\naction=" + std::string(action) + "\nselected=" + std::to_string(selected) + "\n";
	}

	struct shared_state
	{
		std::mutex mutex;
		snapshot current;
		std::uint64_t back_generation = 0;
	};
	inline std::atomic<bool> pointer_captured{false};

	inline shared_state& state()
	{
		static shared_state value;
		return value;
	}

	inline void publish(const snapshot& value)
	{
		auto& shared = state();
		std::lock_guard lock(shared.mutex);
		if (!value.visible || value.generation != shared.current.generation)
			shared.back_generation = 0;
		shared.current = value;
	}

	inline snapshot load()
	{
		auto& shared = state();
		std::lock_guard lock(shared.mutex);
		return shared.current;
	}
	inline bool request_back()
	{
		auto& shared = state();
		std::lock_guard lock(shared.mutex);
		if (!shared.current.visible || shared.current.practice || shared.current.back.empty())
			return false;
		shared.back_generation = shared.current.generation;
		return true;
	}
	inline bool take_back(std::uint64_t generation)
	{
		auto& shared = state();
		std::lock_guard lock(shared.mutex);
		const bool answer = generation && shared.back_generation == generation;
		shared.back_generation = 0;
		return answer;
	}

	inline bool requested() noexcept
	{
#ifdef _WIN32
		const wchar_t* value = pbd::env::find_wide("PBD_MENU_CHANNEL");
		return value && *value;
#else
		return false;
#endif
	}
} // namespace pbd::game_overlay
