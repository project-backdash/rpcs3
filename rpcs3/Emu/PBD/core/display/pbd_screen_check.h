#pragma once

#include <array>
#include <atomic>
#include <cstdint>
#include <mutex>
#include <optional>

// The colours a presented frame puts at the on-screen check's sample points. The game host's window thread asks for them
// (rpcs3qt/pbd_game_host_window.h) and compares them with the screen; the renderer copies them from the image it presents,
// overlays included, and reads the copy once that frame has completed on the GPU (VKPresent.cpp), so asking never stalls
// rendering.
namespace pbd::screen_check
{
	constexpr std::uint32_t grid = 4, points = grid * grid;

	// The centres of a 4x4 grid over a surface `width` x `height` pixels: the same places in the window and in the swapchain
	// when both have one size (client_x/client_y map them otherwise).
	constexpr std::uint32_t point_x(std::uint32_t index, std::uint32_t width) noexcept
	{
		return static_cast<std::uint32_t>((2ull * (index % grid) + 1) * width / (2 * grid));
	}
	constexpr std::uint32_t point_y(std::uint32_t index, std::uint32_t height) noexcept
	{
		return static_cast<std::uint32_t>((2ull * (index / grid) + 1) * height / (2 * grid));
	}

	// Where a client area `client` pixels across shows pixel `at` of an image `image` pixels across. A driver can fix the
	// surface at the monitor's size for a smaller window, and nothing reports how the larger image reaches the window: shown
	// at its own size from the client's top left, the pixel stays in place (the last column when past the edge); `stretched`
	// over the client area, its centre keeps its fraction of the image. `image` 0: no image, the client's own point.
	constexpr std::uint32_t image_to_client(std::uint32_t at, std::uint32_t image, std::uint32_t client, bool stretched) noexcept
	{
		if (stretched && image)
			return static_cast<std::uint32_t>((2ull * at + 1) * client / (2ull * image));
		return at < client ? at : (client ? client - 1 : 0);
	}
	// The client point that shows grid point `index` of a frame drawn over `drawn` pixels of an image `image` pixels across;
	// `drawn` 0 for no frame, the client's own grid.
	constexpr std::uint32_t client_x(std::uint32_t index, std::uint32_t drawn, std::uint32_t image, std::uint32_t client, bool stretched) noexcept
	{
		return image_to_client(point_x(index, drawn ? drawn : client), image, client, stretched);
	}
	constexpr std::uint32_t client_y(std::uint32_t index, std::uint32_t drawn, std::uint32_t image, std::uint32_t client, bool stretched) noexcept
	{
		return image_to_client(point_y(index, drawn ? drawn : client), image, client, stretched);
	}

	struct frame_colours
	{
		std::array<std::uint32_t, points> rgb{}; // 0xRRGGBB at each point; black where nothing is drawn
		// The part of the image the renderer draws, from its top left, where the points are placed: the window size it made
		// the swapchain for, within the images.
		std::uint32_t width = 0, height = 0;
		std::uint32_t image_width = 0, image_height = 0; // the swapchain's images, larger when the driver fixes their size
		std::uint64_t request = 0;                       // the request this frame answers
	};

	class exchange
	{
		std::atomic<std::uint64_t> m_requested{0};
		std::atomic<std::uint64_t> m_claimed{0};
		mutable std::mutex m_mutex;
		frame_colours m_answer;

	public:
		// Asks for the colours of a frame presented from now on; the answer carries the returned number or a later one.
		std::uint64_t request() noexcept
		{
			return m_requested.fetch_add(1, std::memory_order_acq_rel) + 1;
		}
		// The renderer thread takes the newest waiting request for the frame it presents now; 0 when none waits.
		std::uint64_t claim() noexcept
		{
			const auto wanted = m_requested.load(std::memory_order_acquire);
			if (wanted == m_claimed.load(std::memory_order_relaxed))
				return 0;
			m_claimed.store(wanted, std::memory_order_relaxed);
			return wanted;
		}
		void answer(const frame_colours& colours) noexcept
		{
			std::lock_guard lock(m_mutex);
			if (colours.request > m_answer.request)
				m_answer = colours;
		}
		std::optional<frame_colours> answered(std::uint64_t request) const noexcept
		{
			std::lock_guard lock(m_mutex);
			if (!request || m_answer.request < request)
				return std::nullopt;
			return m_answer;
		}
	};

	inline exchange& frames() noexcept
	{
		static exchange value;
		return value;
	}
} // namespace pbd::screen_check
