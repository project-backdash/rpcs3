#pragma once

#include "Emu/PBD/pbd_env.h"
#include "Utilities/File.h"
#include "Utilities/StrFmt.h"
#include "../VulkanAPI.h"

#include <chrono>
#include <condition_variable>
#include <deque>
#include <filesystem>
#include <mutex>
#include <string>
#include <thread>

// PBD_PRESENT_WAIT_TRACE=<absolute path>: records, for every presented frame, when it was handed to the swapchain and when
// the driver reports it on screen (VK_KHR_present_wait), one JSON line each. Without the variable nothing is enabled and the
// present path is unchanged. The gap between the two times is the part of input latency that lies after the emulator: the
// window manager's composition and the display's own refresh.
namespace pbd::present_wait
{
	inline const std::wstring& requested_path()
	{
		static const std::wstring path = []
		{
#ifdef _WIN32
			const auto value = pbd::env::find_wide("PBD_PRESENT_WAIT_TRACE");
			if (value && *value && std::filesystem::path(value).is_absolute())
				return std::wstring(value);
#endif
			return std::wstring();
		}();
		return path;
	}

	inline bool requested()
	{
		return !requested_path().empty();
	}

	inline u64 now_ns()
	{
		return std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
	}

	// One waiter thread per process: it waits on each presented frame in order and writes when the driver showed it. A
	// swapchain is retired (no further waits on it) before it is destroyed, so the waiter never touches a destroyed handle.
	class tracer
	{
		struct frame
		{
			VkSwapchainKHR swapchain;
			u64 id;
			u64 presented_ns;
		};

		std::mutex m_mutex;
		std::condition_variable m_wake;
		std::deque<frame> m_frames;
		VkSwapchainKHR m_in_driver = VK_NULL_HANDLE; // the swapchain the waiter is waiting on, outside the lock
		VkSwapchainKHR m_retiring = VK_NULL_HANDLE;  // the swapchain about to be destroyed
		VkDevice m_device = VK_NULL_HANDLE;
		PFN_vkWaitForPresentKHR m_wait = nullptr;
		fs::file m_output;
		std::thread m_thread;
		bool m_stop = false;
		u64 m_next_id = 0;

		void run()
		{
			std::unique_lock lock(m_mutex);
			while (true)
			{
				m_wake.wait(lock, [this]
					{
						return m_stop || !m_frames.empty();
					});
				if (m_stop)
					return;
				const frame next = m_frames.front();
				m_frames.pop_front();
				m_in_driver = next.swapchain;
				lock.unlock();
				// Bounded waits, so a retire never waits long for this thread to leave the driver.
				VkResult result = VK_TIMEOUT;
				for (int tries = 0; tries < 20 && result == VK_TIMEOUT; tries++)
				{
					result = m_wait(m_device, next.swapchain, next.id, 50'000'000);
					if (result == VK_TIMEOUT)
					{
						std::lock_guard check(m_mutex);
						if (m_stop || m_retiring == next.swapchain)
							break;
					}
				}
				const u64 shown_ns = now_ns();
				lock.lock();
				m_in_driver = VK_NULL_HANDLE;
				m_wake.notify_all();
				if (m_retiring == next.swapchain)
					continue;
				const std::string line = result == VK_SUCCESS ?
				                             fmt::format("{\"id\":%u,\"presented_ns\":%u,\"shown_ns\":%u,\"ms\":%.3f}\n", next.id, next.presented_ns, shown_ns,
												 (shown_ns - next.presented_ns) / 1e6) :
				                             fmt::format("{\"id\":%u,\"presented_ns\":%u,\"result\":%d}\n", next.id, next.presented_ns, static_cast<int>(result));
				m_output.write(line.data(), line.size());
			}
		}

	public:
		static tracer& get()
		{
			static tracer value;
			return value;
		}

		~tracer()
		{
			{
				std::lock_guard lock(m_mutex);
				m_stop = true;
			}
			m_wake.notify_all();
			if (m_thread.joinable())
				m_thread.join();
		}

		// Called once the device exists with present_id and present_wait enabled. False keeps tracing off.
		bool start(VkDevice device)
		{
			std::lock_guard lock(m_mutex);
			if (m_thread.joinable())
				return m_device == device;
			const auto wide = requested_path();
			const auto utf8 = std::filesystem::path(wide).u8string();
			m_wait = reinterpret_cast<PFN_vkWaitForPresentKHR>(vkGetDeviceProcAddr(device, "vkWaitForPresentKHR"));
			if (!m_wait || !m_output.open(std::string(utf8.begin(), utf8.end()), fs::write_new + fs::isfile))
				return false;
			const std::string header = "{\"type\":\"header\",\"schema\":1,\"clock\":\"steady_ns\"}\n";
			m_output.write(header.data(), header.size());
			m_device = device;
			m_thread = std::thread([this]
				{
					run();
				});
			return true;
		}

		// The next present id for a swapchain; ids only grow, as VK_KHR_present_id requires per swapchain.
		u64 next_id()
		{
			std::lock_guard lock(m_mutex);
			return ++m_next_id;
		}

		void presented(VkSwapchainKHR swapchain, u64 id, u64 presented_ns)
		{
			{
				std::lock_guard lock(m_mutex);
				if (!m_thread.joinable() || m_frames.size() >= 64)
					return;
				m_frames.push_back({swapchain, id, presented_ns});
			}
			m_wake.notify_all();
		}

		// Before a swapchain is destroyed: its queued frames are dropped and a wait on it ends first.
		void retire(VkSwapchainKHR swapchain)
		{
			std::unique_lock lock(m_mutex);
			std::erase_if(m_frames, [swapchain](const frame& f)
				{
					return f.swapchain == swapchain;
				});
			m_retiring = swapchain;
			m_wake.wait(lock, [this, swapchain]
				{
					return m_in_driver != swapchain;
				});
			m_retiring = VK_NULL_HANDLE;
		}
	};
} // namespace pbd::present_wait
