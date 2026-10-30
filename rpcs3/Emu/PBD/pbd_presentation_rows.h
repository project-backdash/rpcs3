#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>

// How the game's frames reach the screen and its keys reach the game, as the rows of game-presentation.jsonl in the run
// folder. The launcher's run summary keeps the last present row and the last keyboard row, and counts every present row
// after the first as a change of path: a fallback to the game's own window, or a swap chain rebuilt with another mode.
namespace pbd::presentation
{
	inline constexpr std::string_view file_name = "game-presentation.jsonl";

	// Drawn in the launcher's window (hosted) or in the game's own, through a DXGI flip-model swap chain or Vulkan's own,
	// with the VkPresentModeKHR in use (0 immediate, 1 mailbox, 2 FIFO, 3 FIFO relaxed).
	struct present_path
	{
		bool hosted = false;
		bool flip_model = false;
		std::int32_t present_mode = 0;

		bool operator==(const present_path&) const = default;
	};

	inline std::string present_row(const present_path& path)
	{
		const auto flag = [](bool on)
		{
			return on ? "true" : "false";
		};
		return std::string("{\"type\":\"present\",\"hosted\":") + flag(path.hosted) + ",\"flip_model\":" + flag(path.flip_model) +
		       ",\"present_mode\":" + std::to_string(path.present_mode) + "}\n";
	}

	// Keys came by raw input on a thread of their own (raw) or by the game window's own key messages.
	inline std::string keyboard_row(bool raw)
	{
		return std::string("{\"type\":\"keyboard\",\"raw\":") + (raw ? "true" : "false") + "}\n";
	}

	// The folder the session writes peer-transport.jsonl into: PBD_TICK_PROBE_OUTPUT, when PBD_TICK_PROBE_PAGES names the
	// page set beside it and both are absolute (rollback_session::configure_page_set). Otherwise the run has none.
	inline std::filesystem::path run_folder(const std::filesystem::path& pages, const std::filesystem::path& output)
	{
		return pages.is_absolute() && output.is_absolute() ? output : std::filesystem::path();
	}

	// Turns reports into rows, one per change. Reports come in any order: a present row waits for the first swap chain,
	// since only then is the present mode known, and a report of where the game is drawn before it is only remembered.
	class rows
	{
		present_path m_now;
		bool m_presenting = false;
		std::optional<present_path> m_written;
		std::optional<bool> m_raw;

		std::optional<std::string> present_change()
		{
			if (!m_presenting || m_written == m_now)
				return std::nullopt;
			m_written = m_now;
			return present_row(m_now);
		}

	public:
		std::optional<std::string> hosted(bool value)
		{
			m_now.hosted = value;
			return present_change();
		}

		std::optional<std::string> swapchain(bool flip_model, std::int32_t present_mode)
		{
			m_now.flip_model = flip_model;
			m_now.present_mode = present_mode;
			m_presenting = true;
			return present_change();
		}

		std::optional<std::string> keyboard(bool raw)
		{
			if (m_raw == raw)
				return std::nullopt;
			m_raw = raw;
			return keyboard_row(raw);
		}
	};
} // namespace pbd::presentation
