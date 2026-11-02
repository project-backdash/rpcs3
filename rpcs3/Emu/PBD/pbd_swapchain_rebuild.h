#pragma once

#include <cstdint>
#include <utility>

// When the renderer rebuilds a swapchain the driver reports suboptimal (VKPresent.cpp). A driver can call every new
// swapchain suboptimal while the window keeps its size, from the acquire, the present or both, and since each rebuild makes
// the same swapchain again, following every report rebuilds on every flip, with a full GPU sync each time, and never settles.
// A report for an unchanged window is followed a bounded number of times and the swapchain is then kept until the window
// changes or a run of frames passes with no report at all; a single successful acquire or present between reports is not
// such a run. An out-of-date or unavailable swapchain is always rebuilt, since it cannot be presented to.
namespace pbd::swapchain_rebuild
{
	constexpr std::uint32_t same_window_limit = 2;
	// About a second of frames whose acquire and present both succeeded.
	constexpr std::uint32_t clean_frames_to_follow_again = 60;

	class rule
	{
		std::uint32_t m_same_window = 0; // rebuilds asked for by a suboptimal report for an unchanged window, since the last reset
		std::uint32_t m_clean = 0;       // frames in a row whose acquire and present both succeeded
		bool m_frame_clean = true;       // no suboptimal report yet for the frame being shown
		bool m_held_reported = false;

		void reset() noexcept
		{
			m_same_window = 0;
			m_clean = 0;
			m_frame_clean = true;
			m_held_reported = false;
		}

	public:
		// A rebuild starts. `suboptimal_only`: the only reason is a suboptimal report (nothing out of date or unavailable);
		// `same_window`: the window still has the size and monitor the replaced swapchain was made for.
		void rebuilding(bool suboptimal_only, bool same_window) noexcept
		{
			if (suboptimal_only && same_window)
			{
				++m_same_window;
				m_frame_clean = true;
			}
			else
				reset();
		}

		// A suboptimal report from the acquire or the present of the frame being shown. Returns whether it rebuilds the
		// swapchain; a report for a changed window is always followed by the caller.
		bool suboptimal() noexcept
		{
			m_clean = 0;
			m_frame_clean = false;
			return m_same_window < same_window_limit;
		}

		// The frame being shown was presented; `success`: the present returned success. A frame counts as clean only when its
		// acquire made no suboptimal report either, and after a run of clean frames a later report is followed again.
		void presented(bool success) noexcept
		{
			if (!success || !m_frame_clean)
				m_clean = 0;
			else if (m_clean < clean_frames_to_follow_again && ++m_clean == clean_frames_to_follow_again)
			{
				m_same_window = 0;
				m_held_reported = false;
			}
			m_frame_clean = true;
		}

		// True the first time a report is not followed since the last reset, for the one warning that says so.
		bool report_held() noexcept
		{
			return !std::exchange(m_held_reported, true);
		}

		std::uint32_t same_window_rebuilds() const noexcept
		{
			return m_same_window;
		}
	};

	// Whether the `count`th of a repeated event is logged (a result other than success, a swapchain made): the first eight,
	// then every hundredth, so an event the driver repeats on every frame stays visible without filling the log.
	constexpr bool log_status(std::uint64_t count) noexcept
	{
		return count != 0 && (count <= 8 || count % 100 == 0);
	}

	// How a swapchain is made in place of `old`, which is none for the first one (vkutils/swapchain.cpp). `create(exclusive,
	// old)` asks the driver for a swapchain, with the full-screen exclusive request when `exclusive` and `old` as the one it
	// replaces, and returns whether one was made; `destroy(old)` destroys a swapchain and drops the images taken from it.
	// A call with `old` retires it whether or not it succeeds, and a retired swapchain gives no more images and is no longer
	// valid as `old`, so it is destroyed either way. A driver that refuses the full-screen exclusive request is asked once
	// more without it, with the retired swapchain destroyed first. Vulkan refuses a window as in use only while it has a
	// swapchain that is not retired, but a driver or layer that also counts a retired one would refuse the second call, and
	// destroying it first is valid either way. Returns whether a swapchain was made; when not, none is left, and the next
	// attempt starts from nothing.
	template <typename Handle, typename Create, typename Destroy>
	bool create_replacing(Handle old, bool exclusive, Create&& create, Destroy&& destroy)
	{
		bool made = create(exclusive, old);
		if (!made && exclusive)
		{
			if (old)
				destroy(std::exchange(old, Handle{}));
			made = create(false, Handle{});
		}
		if (old)
			destroy(old);
		return made;
	}

	// How the renderer waits out a driver that refuses a swapchain for a moment, as while the game host moves the window
	// (VKPresent.cpp). `attempt()` rebuilds and returns whether rendering can go on, with a new swapchain or with the frame
	// contexts a skipped rebuild kept; `stopped()` ends the attempts when the emulator stops, and `wait()` paces them.
	// Returns whether rendering can go on; when not, the emulator stopped or all `attempts` were refused.
	template <typename Attempt, typename Stopped, typename Wait>
	bool rebuild_with_retries(std::uint32_t attempts, Attempt&& attempt, Stopped&& stopped, Wait&& wait)
	{
		for (std::uint32_t i = 0; i < attempts; ++i)
		{
			if (attempt())
				return true;
			if (stopped())
				return false;
			wait();
		}
		return false;
	}
} // namespace pbd::swapchain_rebuild
