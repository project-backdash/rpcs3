#pragma once

#include "pbd_set_channel_model.h"

#include <atomic>
#include <functional>
#include <mutex>
#include <thread>

namespace pbd::set_channel
{
	// Read only the requested immutable serial. The game validates the cached bytes and advances after applying it.
	class command_reader
	{
		std::function<result<std::string>(std::uint64_t)> read;
		std::atomic<std::uint64_t> requested{1};
		std::atomic<bool> stopping{false};
		std::mutex mutex;
		std::uint64_t serial = 0;
		result<std::string> text{{}, "unavailable"};
		std::thread worker;

		void run() noexcept
		{
			std::uint64_t completed = 0;
			while (!stopping.load(std::memory_order_acquire))
			{
				const auto next = requested.load(std::memory_order_acquire);
				if (next != completed)
				{
					result<std::string> value;
					try
					{
						value = read(next);
					}
					catch (...)
					{
						value = {{}, "read_failed"};
					}
					if (value.value || (value.error != "unavailable" && value.error != "read_failed"))
						completed = next;
					std::lock_guard lock(mutex);
					serial = next;
					text = std::move(value);
				}
				std::this_thread::sleep_for(std::chrono::milliseconds(8));
			}
		}

	public:
		explicit command_reader(std::function<result<std::string>(std::uint64_t)> source)
			: read(std::move(source)), worker([this]
										   {
											   run();
										   })
		{
		}
		~command_reader()
		{
			stopping.store(true, std::memory_order_release);
			worker.join();
		}
		result<std::string> poll(std::uint64_t next)
		{
			requested.store(next, std::memory_order_release);
			std::unique_lock lock(mutex, std::try_to_lock);
			return lock && serial == next ? text : result<std::string>{{}, "unavailable"};
		}
	};

	// One owner queues immutable envelopes; one writer publishes them in order. No filesystem call or writer lock
	// reaches the game callback. A failed serial poisons the stream, so no later result can overtake missing input.
	template <std::size_t Capacity = 256, typename Value = std::string>
	class publication_writer
	{
		struct item
		{
			std::uint64_t serial = 0;
			Value value;
		};
		std::array<item, Capacity> rows;
		std::atomic<std::uint64_t> head{0}, tail{0}, completed{0};
		std::atomic<const char*> failure{nullptr};
		std::atomic<bool> stopping{false};
		std::function<publication_result(std::uint64_t, const Value&)> publish;
		std::thread worker;

		void run() noexcept
		{
			try
			{
				for (;;)
				{
					const auto next = tail.load(std::memory_order_relaxed);
					if (next == head.load(std::memory_order_acquire))
					{
						if (stopping.load(std::memory_order_acquire) && next == head.load(std::memory_order_acquire))
							return;
						std::this_thread::sleep_for(std::chrono::milliseconds(1));
						continue;
					}
					auto& row = rows[next % Capacity];
					const auto result = publish(row.serial, row.value);
					if (!result.published)
					{
						failure.store(result.error ? result.error : "event_publish_failed", std::memory_order_release);
						return;
					}
					completed.store(row.serial, std::memory_order_release);
					row.value = {};
					tail.store(next + 1, std::memory_order_release);
				}
			}
			catch (...)
			{
				failure.store("event_publish_failed", std::memory_order_release);
			}
		}

	public:
		explicit publication_writer(std::function<publication_result(std::uint64_t, const Value&)> write)
			: publish(std::move(write)), worker([this]
											 {
												 run();
											 })
		{
		}
		~publication_writer()
		{
			stopping.store(true, std::memory_order_release);
			worker.join();
		}
		const char* error() const noexcept
		{
			return failure.load(std::memory_order_acquire);
		}
		bool submit(std::uint64_t serial, Value value)
		{
			const auto next = head.load(std::memory_order_relaxed);
			if (error() || next - tail.load(std::memory_order_acquire) == Capacity)
				return false;
			rows[next % Capacity] = {serial, std::move(value)};
			head.store(next + 1, std::memory_order_release);
			return true;
		}
		// Only shutdown/output publication may wait. Queued acceptance is never a claim that the bytes are on disk.
		bool flush(std::uint64_t serial) const noexcept
		{
			while (completed.load(std::memory_order_acquire) < serial)
			{
				if (error())
					return false;
				std::this_thread::sleep_for(std::chrono::milliseconds(1));
			}
			return !error();
		}
	};
} // namespace pbd::set_channel
