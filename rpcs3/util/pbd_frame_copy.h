#pragma once

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <cstring>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <thread>
#include <vector>

namespace pbd::clips
{
	// The renderer lends a completed, persistently mapped readback until the worker releases its lease.
	// One fence check can find several presented frames finished at once, so their copies queue in presentation order.
	class frame_copy
	{
	public:
		using delivery = std::function<void(std::vector<std::uint8_t>&&, unsigned, unsigned, unsigned, bool, std::uint64_t)>;
		// More than the renderer's frames in flight. A waiting job holds only a pointer into its mapped readback.
		static constexpr std::size_t capacity = 8;

	private:
		struct job
		{
			const void* pixels;
			unsigned pitch, width, height;
			bool bgra;
			std::uint64_t time_us;
			std::shared_ptr<std::atomic<bool>> lease;
			delivery deliver;
		};
		std::mutex m_mutex;
		std::condition_variable m_wake;
		std::deque<job> m_jobs;
		std::thread m_worker;
		bool m_stop = false;
		void run()
		{
			while (true)
			{
				std::optional<job> next;
				{
					std::unique_lock lock(m_mutex);
					m_wake.wait(lock, [&]
						{
							return m_stop || !m_jobs.empty();
						});
					if (m_jobs.empty())
						return;
					next = std::move(m_jobs.front());
					m_jobs.pop_front();
				}
				try
				{
					std::vector<std::uint8_t> bytes(static_cast<std::size_t>(next->pitch) * next->height);
					std::memcpy(bytes.data(), next->pixels, bytes.size());
					next->lease->store(false, std::memory_order_release);
					next->deliver(std::move(bytes), next->pitch, next->width, next->height, next->bgra, next->time_us);
				}
				catch (...)
				{
					next->lease->store(false, std::memory_order_release);
				}
			}
		}

	public:
		~frame_copy()
		{
			stop();
		}
		void stop()
		{
			{
				std::lock_guard lock(m_mutex);
				m_stop = true;
			}
			m_wake.notify_one();
			if (m_worker.joinable())
				m_worker.join();
		}
		// False means the frame is lost; the caller counts it as dropped. The worker holds the lock only to take a job,
		// never while copying, so the renderer thread waits at most for that.
		bool offer(const void* pixels, unsigned pitch, unsigned width, unsigned height, bool bgra, std::uint64_t time_us,
			std::shared_ptr<std::atomic<bool>> lease, delivery deliver)
		{
			std::lock_guard lock(m_mutex);
			if (m_stop || m_jobs.size() >= capacity || !pixels || !width || !height || lease->load(std::memory_order_acquire))
				return false;
			if (!m_worker.joinable())
				m_worker = std::thread([this]
					{
						run();
					});
			lease->store(true, std::memory_order_release);
			m_jobs.push_back(job{pixels, pitch, width, height, bgra, time_us, std::move(lease), std::move(deliver)});
			m_wake.notify_one();
			return true;
		}
	};
} // namespace pbd::clips
