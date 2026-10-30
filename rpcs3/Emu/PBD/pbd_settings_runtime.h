#pragma once

#include "pbd_settings_view.h"

namespace pbd::settings_menu
{
	// Empty for the two rows that stay in the game.
	inline constexpr std::string_view navigate_schema = "pbd-navigate-1";

	inline std::string navigate_json(entry e)
	{
		const auto target = entry_targets[std::min(static_cast<std::uint32_t>(e), entry_count - 1)];
		if (target.empty())
			return {};
		return "{\"schema\":" + pbd::json::quoted(navigate_schema) + ",\"target\":" + pbd::json::quoted(target) + "}\n";
	}

	inline std::string changes_json(const model& m)
	{
		const auto& a = m.start;
		const auto& b = m.current;
		std::vector<std::string> video, audio;
		const bool offered = std::find(resolution_scales.begin(), resolution_scales.end(), b.resolution) != resolution_scales.end();
		if (b.resolution != a.resolution && offered)
			video.push_back("\"resolutionScale\":" + std::to_string(b.resolution));
		if (b.fullscreen != a.fullscreen)
			video.push_back(
				"\"windowMode\":" + pbd::json::quoted(b.fullscreen ? (m.borderless ? "borderless" : "fullscreen") : "windowed"));
		if (b.vsync != a.vsync)
			video.push_back(std::string("\"vsync\":") + (b.vsync ? "true" : "false"));
		if (!b.display.empty() && b.display != a.display)
			video.push_back("\"gameDisplay\":" + pbd::json::quoted(b.display));
		if (b.volume != a.volume)
			audio.push_back("\"masterVolume\":" + std::to_string(std::clamp(b.volume, 0, 100)));
		if (!b.audio_output.empty() && !detail::same_device(b.audio_output, a.audio_output))
			audio.push_back("\"outputDevice\":" + pbd::json::quoted(b.audio_output));
		const auto join = [](const std::vector<std::string>& fields)
		{
			std::string text;
			for (const auto& field : fields)
				text += (text.empty() ? "" : ",") + field;
			return "{" + text + "}";
		};
		std::string patch;
		if (!video.empty())
			patch += "\"video\":" + join(video);
		if (!audio.empty())
			patch += std::string(patch.empty() ? "" : ",") + "\"audio\":" + join(audio);
		std::vector<std::string> mapping;
		for (unsigned i = 0; i < b.input.mapping.size(); ++i)
			if (b.input.mapping[i] != a.input.mapping[i])
				mapping.push_back(pbd::json::quoted(pbd::controllers::settings::button_slots[i]) + ":" + pbd::json::quoted(b.input.mapping[i]));
		if (!mapping.empty())
			patch += std::string(patch.empty() ? "" : ",") + "\"controls\":{\"buttonMapping\":" + join(mapping) + "}";
		const std::string controller = b.input.device != a.input.device ? ",\"controllerLastUsed\":" + pbd::json::quoted(b.input.device) : "";
		return "{\"schema\":" + pbd::json::quoted(changes_schema) + ",\"patch\":{" + patch + "}" + controller + "}\n";
	}

	namespace detail
	{
		// Paths are read wide on Windows: the narrow environment is in the system code page, not UTF-8.
		inline std::optional<std::filesystem::path> env_path(const char* name)
		{
#ifdef _WIN32
			const wchar_t* value = pbd::env::find_wide(name);
#else
			const char* value = pbd::env::find(name);
#endif
			if (!value || !*value)
				return std::nullopt;
			return std::filesystem::path(value);
		}

		inline bool env_is(const char* name, std::string_view expected)
		{
			const char* value = pbd::env::find(name);
			return value && std::string_view(value) == expected;
		}

		inline bool env_set(const char* name)
		{
			const char* value = pbd::env::find(name);
			return value && *value;
		}
	} // namespace detail

	// Where the changes go: PBD_GAME_SETTINGS_CHANGES when the launcher names the file; otherwise game-settings-changes.json in
	// the run's output folder (PBD_TICK_PROBE_OUTPUT, set for Practice and replays) or next to its stop file (spectating).
	inline std::optional<std::filesystem::path> changes_path()
	{
		if (auto file = detail::env_path("PBD_GAME_SETTINGS_CHANGES"))
			return file;
		if (auto folder = detail::env_path("PBD_TICK_PROBE_OUTPUT"))
			return *folder / changes_file_name;
		if (auto stop = detail::env_path("PBD_STOP_FILE"); stop && stop->has_parent_path())
			return stop->parent_path() / changes_file_name;
		return std::nullopt;
	}

	// Where a navigate request goes: only where the launcher names the file. Without one the menu has no top level, and Esc opens
	// Settings directly.
	inline std::optional<std::filesystem::path> navigate_path()
	{
		return detail::env_path("PBD_NAVIGATE_REQUEST");
	}

	// The launcher's process ID as PBD_LAUNCHER_PID gives it (decimal digits only). Nothing for zero, for anything that is not a
	// process ID, and for 0xFFFFFFFF: that value is ASFW_ANY, which would let every process take the foreground, not the launcher.
	inline std::optional<std::uint32_t> parse_launcher_process_id(std::string_view text)
	{
		if (text.empty() || text.size() > 10)
			return std::nullopt;
		std::uint64_t value = 0;
		for (const char c : text)
		{
			if (c < '0' || c > '9')
				return std::nullopt;
			value = value * 10 + static_cast<std::uint64_t>(c - '0');
		}
		if (value == 0 || value >= 0xFFFFFFFFull)
			return std::nullopt;
		return static_cast<std::uint32_t>(value);
	}

	// The launcher that started this run names its own process, so the game window, while in front, lets that process (and no
	// other) take the foreground: the lobby's call for this player's turn brings the launcher over the game (pbd::game_window::settings_menu::tick).
	// Nothing when the run was started without the launcher.
	inline std::optional<std::uint32_t> launcher_process_id()
	{
		static const std::optional<std::uint32_t> value = []() -> std::optional<std::uint32_t>
		{
			const char* text = pbd::env::find("PBD_LAUNCHER_PID");
			if (!text)
				return std::nullopt;
			return parse_launcher_process_id(text);
		}();
		return value;
	}

	// An online set, Practice, a replay (recorded inputs, or a replay game file), spectating or offline play.
	inline bool available()
	{
		static const bool value = []
		{
			using namespace detail;
			if (env_set("PBD_PEER_ROLE") || env_set("PBD_SET_CHANNEL"))
				return true;
			return env_is("PBD_PRACTICE_REWIND", "1") || env_set("PBD_SESSION_REPLAY") || env_set("PBD_REPLAY_V2_GAME") ||
			       env_set("PBD_SPECTATOR_CHANNEL") || pbd::session_mode::offline_play();
		}();
		return value;
	}

	inline std::string_view context()
	{
		if (detail::env_set("PBD_PEER_ROLE") || detail::env_set("PBD_SET_CHANNEL"))
			return "Online set";
		if (pbd::session_mode::offline_play())
			return "Offline";
		if (detail::env_is("PBD_PRACTICE_REWIND", "1"))
			return "Practice";
		if (detail::env_set("PBD_SPECTATOR_CHANNEL"))
			return "Spectating";
		return "Replay";
	}

	// Raised while the menu is open and kept until every button that was down when it closed is released, so the press that
	// closed it never reaches the game or the menu under it.
	inline std::atomic<bool> claim{false};
	inline bool claimed() noexcept
	{
		return claim.load(std::memory_order_acquire);
	}

	// Whether the claim blanks a game port where the update latch injects the ports' words: only in Practice, whose ports carry
	// this PC's own controller (the dummy's playback keeps feeding P2 while it plays). Everywhere else the ports carry recorded
	// input: a replay's recorded fighters, and a watcher's two ports, which replay the watched game and are checked against the
	// source's applied words frame by frame. Blanking those while the menu is open would desync the replay or fault the watch; a
	// replay's taken fighter already reads a neutral word at its source (the replay controls and the direct replay read nothing
	// while claimed), so no port outside Practice needs the claim here.
	inline bool blanks_port(bool claimed, bool practice, std::uint32_t port, bool dummy_playing) noexcept
	{
		return claimed && practice && (port == 0 || !dummy_playing);
	}

	// The menu is on screen.
	inline std::atomic<bool> showing{false};
	inline bool shown() noexcept
	{
		return showing.load(std::memory_order_acquire);
	}

	// A pointer press made in the menu owns its release too, so the release never becomes a mouse-bound game input.
	inline std::atomic<bool> pointer_captured{false};
	inline bool pointer_owned() noexcept
	{
		return shown() || pointer_captured.load(std::memory_order_acquire);
	}

	// Practice's Settings row asks the window to open the menu; the window polls for it.
	inline std::atomic<bool> open_requested{false};
	inline void request_open() noexcept
	{
		open_requested.store(true, std::memory_order_release);
	}
	inline bool take_open_request() noexcept
	{
		return open_requested.exchange(false, std::memory_order_acq_rel);
	}

	// A pad's PS / Home / Guide button opens the top level as Esc does (pbd_controller_backend.cpp), the way the consoles
	// open their own menu, so a pad player reaches Start recording and Return to Launcher without the keyboard.
	inline std::atomic<bool> top_open_requested{false};
	inline void request_top_open() noexcept
	{
		top_open_requested.store(true, std::memory_order_release);
	}
	inline bool take_top_open_request() noexcept
	{
		return top_open_requested.exchange(false, std::memory_order_acq_rel);
	}

	// Offline play's Button setup (pbd_player_seats.h): the Controls page alone, on that controller's own layout.
	namespace detail
	{
		inline std::mutex controls_mutex;
		inline std::string controls_device;
	} // namespace detail
	inline std::atomic<bool> controls_open_requested{false};
	inline void request_controls_open(std::string device)
	{
		{
			std::lock_guard lock(detail::controls_mutex);
			detail::controls_device = std::move(device);
		}
		controls_open_requested.store(true, std::memory_order_release);
	}
	inline std::optional<std::string> take_controls_open_request()
	{
		if (!controls_open_requested.exchange(false, std::memory_order_acq_rel))
			return std::nullopt;
		std::lock_guard lock(detail::controls_mutex);
		return detail::controls_device;
	}

	// Return to Launcher or Exit Game chosen in the replay's own Options menu. The window answers it as if the row had been
	// chosen here (pbd::game_window::settings_menu::choose_leave), so the launcher ends the run exactly as it does for this menu. False where this
	// menu cannot ask the launcher (no request file): the caller then ends the replay itself.
	inline std::atomic<std::uint32_t> leave_requested{0};
	inline bool request_leave(entry e)
	{
		if ((e != entry::launcher && e != entry::quit) || !available() || !navigate_path())
			return false;
		leave_requested.store(static_cast<std::uint32_t>(e) + 1, std::memory_order_release);
		return true;
	}
	inline std::optional<entry> take_leave_request() noexcept
	{
		const auto value = leave_requested.exchange(0, std::memory_order_acq_rel);
		return value ? std::optional<entry>(static_cast<entry>(value - 1)) : std::nullopt;
	}

	// Esc over Practice's fight: Practice pauses as START does and opens its own menu on the Settings category under this one,
	// so closing Settings leaves the game paused where the player came from.
	inline std::atomic<bool> practice_pause_requested{false};
	inline void request_practice_pause() noexcept
	{
		practice_pause_requested.store(true, std::memory_order_release);
	}
	inline bool take_practice_pause() noexcept
	{
		return practice_pause_requested.exchange(false, std::memory_order_acq_rel);
	}

	// Closing a menu that Esc opened on its top level (Return to Game): the Practice pause menu Esc opened under it resumes the
	// fight, as START there would.
	inline std::atomic<bool> practice_resume_requested{false};
	inline void request_practice_resume() noexcept
	{
		practice_resume_requested.store(true, std::memory_order_release);
	}
	inline bool take_practice_resume() noexcept
	{
		return practice_resume_requested.exchange(false, std::memory_order_acq_rel);
	}

	// Steady-clock milliseconds of the overlay's last composed frame. While the game presents nothing (a paused replay, a held
	// boundary) the window asks for UI-only flips so the menu still appears and moves; while the game presents, it asks for none.
	inline std::atomic<std::uint64_t> composed_ms{0};

	struct shared_state
	{
		std::mutex mutex;
		view current;
	};

	inline shared_state& state()
	{
		static shared_state value;
		return value;
	}

	inline void publish(view value)
	{
		auto& shared = state();
		std::lock_guard lock(shared.mutex);
		shared.current = std::move(value);
	}

	inline view load()
	{
		auto& shared = state();
		std::lock_guard lock(shared.mutex);
		return shared.current;
	}
} // namespace pbd::settings_menu
