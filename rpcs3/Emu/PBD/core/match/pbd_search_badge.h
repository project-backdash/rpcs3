#pragma once

// Matchmaking search status during Practice. The launcher replaces search.pbd beside menu.pbd; the update latch polls
// it and publishes a snapshot for the RSX overlay.

#include <algorithm>
#include <charconv>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

namespace pbd::search_badge
{
	inline constexpr std::size_t max_file_bytes = 256;

	enum class search_mode : std::uint8_t
	{
		quick,
		ranked
	};

	// A Ranked search for a fighter still placing: the rated sets it has done of the total that places it (1..10).
	inline constexpr std::uint32_t max_placement_total = 10;
	struct placing
	{
		std::uint32_t done = 0, total = 0;
		bool operator==(const placing&) const = default;
	};

	struct search
	{
		std::uint64_t generation = 0;
		bool active = false;
		search_mode mode = search_mode::quick;
		std::uint64_t started_ms = 0; // when the search began, Unix ms on this PC's clock
		std::optional<placing> placement;
		bool operator==(const search&) const = default;
	};

	struct parse_result
	{
		std::optional<search> value;
		std::string error;
	};

	namespace detail
	{
		inline bool parse_u64(std::string_view text, std::uint64_t& out) noexcept
		{
			const auto [tail, ec] = std::from_chars(text.data(), text.data() + text.size(), out);
			return !text.empty() && ec == std::errc{} && tail == text.data() + text.size();
		}

		inline std::string_view take_line(std::string_view& text) noexcept
		{
			const auto end = text.find('\n');
			auto line = text.substr(0, end);
			if (!line.empty() && line.back() == '\r')
				line.remove_suffix(1);
			text = end == std::string_view::npos ? std::string_view{} : text.substr(end + 1);
			return line;
		}
	} // namespace detail

	// placing=<done>|<total>: done at most the total, a total of 1..10.
	inline std::optional<placing> parse_placing(std::string_view value) noexcept
	{
		const auto bar = value.find('|');
		std::uint64_t done = 0, total = 0;
		if (bar == std::string_view::npos || !detail::parse_u64(value.substr(0, bar), done) || !detail::parse_u64(value.substr(bar + 1), total) ||
			!total || total > max_placement_total || done > total)
			return {};
		return placing{static_cast<std::uint32_t>(done), static_cast<std::uint32_t>(total)};
	}

	// PBD-SEARCH-1: generation=<n>, active=<0|1>, and while active mode=<ranked|quick> and started=<Unix ms>; a Ranked search for
	// a placing fighter adds placing=<done>|<total> (ranked-notices-1). Unknown or duplicate keys fail closed, like PBD-MENU-1, so
	// launcher and game versions cannot disagree silently.
	inline parse_result parse(std::string_view text)
	{
		if (text.size() > max_file_bytes)
			return {{}, "file_too_large"};
		if (detail::take_line(text) != "PBD-SEARCH-1")
			return {{}, "bad_header"};
		search out;
		bool generation = false, active = false, mode = false, started = false, placement = false;
		while (!text.empty())
		{
			const auto line = detail::take_line(text);
			if (line.empty())
				continue;
			const auto eq = line.find('=');
			const auto key = line.substr(0, eq), value = eq == std::string_view::npos ? std::string_view{} : line.substr(eq + 1);
			bool good = false;
			if (key == "generation")
				good = !std::exchange(generation, true) && detail::parse_u64(value, out.generation) && out.generation;
			else if (key == "active")
				good = !std::exchange(active, true) && (value == "0" || value == "1");
			else if (key == "mode")
				good = !std::exchange(mode, true) && (value == "ranked" || value == "quick");
			else if (key == "started")
				good = !std::exchange(started, true) && detail::parse_u64(value, out.started_ms) && out.started_ms;
			else if (key == "placing")
				good = !std::exchange(placement, true) && (out.placement = parse_placing(value)).has_value();
			if (!good)
				return {{}, "bad_line"};
			if (key == "active")
				out.active = value == "1";
			if (key == "mode")
				out.mode = value == "ranked" ? search_mode::ranked : search_mode::quick;
		}
		if (!generation || !active)
			return {{}, "missing_required"};
		// A running search names its mode and start; an ended one names neither. Only a running Ranked search names a placement.
		if (out.active ? !(mode && started) : (mode || started))
			return {{}, "bad_search"};
		if (out.placement && !(out.active && out.mode == search_mode::ranked))
			return {{}, "bad_search"};
		return {out, {}};
	}

	inline parse_result read(const std::filesystem::path& path)
	{
		std::error_code ec;
		const auto size = std::filesystem::file_size(path, ec);
		if (ec)
			return {{}, "unavailable"};
		if (size > max_file_bytes)
			return {{}, "file_too_large"};
		std::ifstream file(path, std::ios::binary);
		std::string text(static_cast<std::size_t>(size), '\0');
		if (!file || (size && !file.read(text.data(), static_cast<std::streamsize>(size))))
			return {{}, "unavailable"};
		return parse(text);
	}

	inline std::string_view mode_name(search_mode mode) noexcept
	{
		return mode == search_mode::ranked ? "Ranked" : "Quick Match";
	}

	// The time searched as m:ss; a clock set back reads 0:00, and the minutes stop at 99.
	inline std::string elapsed(std::uint64_t started_ms, std::uint64_t now_ms)
	{
		const std::uint64_t seconds = std::min<std::uint64_t>(now_ms > started_ms ? (now_ms - started_ms) / 1000 : 0, 99 * 60 + 59);
		const auto ss = seconds % 60;
		return std::to_string(seconds / 60) + (ss < 10 ? ":0" : ":") + std::to_string(ss);
	}

	// The search line's place on the 1280x720 canvas: in play, one prompt-style line right-aligned under the dummy's life
	// bar on the dummy's side (P2's, the right), above P2's command history (its first row is centred at 166) and clear of the
	// rank plates and P1's history; over Practice's pause menu, left-aligned above the menu panel (top 112).
	struct place
	{
		float x = 0.f, centre = 0.f;
		bool right_aligned = false;
	};
	inline constexpr place placement(bool menu_open) noexcept
	{
		if (menu_open)
			return {56.f, 112.f - 22.f, false};
		return {1228.f, 135.f, true};
	}

	// Drawn while a search runs and no launcher panel is on screen: the match-found prompt takes its place. Practice's own pause
	// menu is not a launcher panel.
	inline constexpr bool shows(const search& value, bool launcher_panel) noexcept
	{
		return value.active && !launcher_panel;
	}

	struct shared_state
	{
		std::mutex mutex;
		search current;
	};

	inline shared_state& state()
	{
		static shared_state value;
		return value;
	}

	inline void publish(const search& value)
	{
		auto& shared = state();
		std::lock_guard lock(shared.mutex);
		shared.current = value;
	}

	inline search load()
	{
		auto& shared = state();
		std::lock_guard lock(shared.mutex);
		return shared.current;
	}

	// The latch's reader. search.pbd is replaced by rename, which Windows refuses while the file is open, so it is read at most
	// four times a second and only when its write time changed; an older generation never replaces a newer one.
	struct reader
	{
		std::filesystem::path file;
		std::chrono::steady_clock::time_point next{};
		std::filesystem::file_time_type written{};
		bool written_valid = false;
		search shown{};
		std::string last_error;
	};

	// Returns a refusal the first time it happens, for the log; empty otherwise.
	inline std::string poll(reader& from, std::chrono::steady_clock::time_point now)
	{
		if (from.file.empty() || now < from.next)
			return {};
		from.next = now + std::chrono::milliseconds(250);
		std::error_code ec;
		const auto written = std::filesystem::last_write_time(from.file, ec);
		if (!ec && from.written_valid && written == from.written)
			return {};
		auto parsed = read(from.file);
		if (!parsed.value)
		{
			const bool fresh = parsed.error != "unavailable" && parsed.error != from.last_error;
			from.last_error = parsed.error;
			return fresh ? parsed.error : std::string{};
		}
		from.written_valid = !ec;
		from.written = written;
		from.last_error.clear();
		if (parsed.value->generation > from.shown.generation)
		{
			from.shown = *parsed.value;
			publish(from.shown);
		}
		return {};
	}
} // namespace pbd::search_badge
