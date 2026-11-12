#pragma once

#include <algorithm>
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <cstdio>
#include <chrono>
#include <condition_variable>
#include <functional>
#include <filesystem>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <utility>

namespace pbd::peer_log
{
	class diagnostic_file
	{
		std::FILE* file = nullptr;

	public:
		~diagnostic_file()
		{
			if (file)
				std::fclose(file);
		}
		bool open(const std::string& path)
		{
			if (file)
				std::fclose(file);
#ifdef _WIN32
			file = _wfopen(std::filesystem::u8path(path).c_str(), L"wbx");
#else
			file = std::fopen(path.c_str(), "wbx");
#endif
			if (!file)
				return false;
			// A cancelled write must report a short count, never invoke fs::file's fatal WriteFile check.
			if (std::setvbuf(file, nullptr, _IONBF, 0) != 0)
			{
				std::fclose(file);
				file = nullptr;
				return false;
			}
			return true;
		}
		std::size_t write(const void* data, std::size_t size) noexcept
		{
			return std::fwrite(data, 1, size, file);
		}
	};

	// The boundary owns write(); a disposable log worker owns drain(). Neither health nor game progress waits on disk.
	template <typename File, std::size_t Capacity = 4 * 1024 * 1024>
	class buffered_file
	{
		static_assert(Capacity > 0);
		File output;
		std::unique_ptr<std::array<char, Capacity>> bytes;
		alignas(64) std::atomic<std::uint64_t> written{0};
		alignas(64) std::atomic<std::uint64_t> read{0};
		std::atomic<bool> good{false};
		bool output_failed = false, failure_written = false;
		std::thread worker;
		std::function<void(std::thread&)> cancel;
		std::atomic<bool> stopping{false}, aborting{false};
		std::mutex done_mutex;
		std::condition_variable done_changed;
		bool done = false;

	public:
		~buffered_file()
		{
			stop();
		}
		void start(std::function<void(std::thread&)> cancel_write = {})
		{
			if (worker.joinable())
				return;
			cancel = std::move(cancel_write);
			worker = std::thread([this]
				{
					for (;;)
					{
						drain();
						if (stopping.load(std::memory_order_acquire))
						{
							drain();
							break;
						}
						std::this_thread::sleep_for(std::chrono::milliseconds(5));
					}
					{
						std::lock_guard lock(done_mutex);
						done = true;
					}
					done_changed.notify_one();
				});
		}
		// Called only after the producer has stopped. Give final rows a short drain, then cancel stalled diagnostic I/O.
		void stop() noexcept
		{
			if (!worker.joinable())
				return;
			stopping.store(true, std::memory_order_release);
			std::unique_lock lock(done_mutex);
			while (!done_changed.wait_for(lock, std::chrono::milliseconds(100), [&]
				{
					return done;
				}))
			{
				aborting.store(true, std::memory_order_release);
				if (cancel)
					cancel(worker);
			}
			lock.unlock();
			worker.join();
		}
		template <typename... Args>
		bool open(Args&&... args)
		{
			stop();
			good.store(false, std::memory_order_release);
			if (!output.open(std::forward<Args>(args)...))
				return false;
			bytes = std::make_unique<std::array<char, Capacity>>();
			written.store(0, std::memory_order_relaxed);
			read.store(0, std::memory_order_relaxed);
			output_failed = failure_written = done = false;
			stopping.store(false, std::memory_order_relaxed);
			aborting.store(false, std::memory_order_relaxed);
			good.store(true, std::memory_order_release);
			return true;
		}
		explicit operator bool() const noexcept
		{
			return good.load(std::memory_order_acquire);
		}
		std::size_t write(const void* source, std::size_t size) noexcept
		{
			if (!good.load(std::memory_order_acquire))
				return 0;
			const auto next = written.load(std::memory_order_relaxed);
			if (size > Capacity - (next - read.load(std::memory_order_acquire)))
			{
				good.store(false, std::memory_order_release);
				return 0;
			}
			const auto offset = static_cast<std::size_t>(next % Capacity);
			const auto first = std::min(size, Capacity - offset);
			if (first)
				std::memcpy(bytes->data() + offset, source, first);
			if (size > first)
				std::memcpy(bytes->data(), static_cast<const char*>(source) + first, size - first);
			written.store(next + size, std::memory_order_release);
			return size;
		}
		bool drain() noexcept
		{
			if (!bytes)
				return true;
			if (output_failed)
				return false;
			try
			{
				const auto stop = written.load(std::memory_order_acquire);
				auto next = read.load(std::memory_order_relaxed);
				while (next < stop)
				{
					if (aborting.load(std::memory_order_acquire))
					{
						good.store(false, std::memory_order_release);
						return false;
					}
					const auto offset = static_cast<std::size_t>(next % Capacity);
					const auto size = std::min<std::uint64_t>(stop - next, Capacity - offset);
					if (output.write(bytes->data() + offset, size) != size)
					{
						good.store(false, std::memory_order_release);
						output_failed = true;
						return false;
					}
					next += size;
					read.store(next, std::memory_order_release);
				}
				if (!good.load(std::memory_order_acquire) && !failure_written && !aborting.load(std::memory_order_acquire))
				{
					failure_written = true;
					constexpr char failure[] = "\n{\"type\":\"peer_log_incomplete\"}\n";
					output.write(failure, sizeof(failure) - 1);
				}
			}
			catch (...)
			{
				good.store(false, std::memory_order_release);
				output_failed = true;
			}
			return good.load(std::memory_order_acquire);
		}
	};
} // namespace pbd::peer_log
