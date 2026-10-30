#pragma once

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>

namespace pbd::session_trace
{
	// The owner publishes copies of confirmed rows. Slow or failed diagnostic storage never owns game history.
	template <typename Row, std::size_t Capacity = 4096>
	class buffer
	{
		std::array<Row, Capacity> rows{};
		std::atomic<std::uint64_t> read{0}, written{0}, lost{0};
		std::atomic<bool> failed{false};

	public:
		bool push(const Row& row) noexcept
		{
			const auto next = written.load(std::memory_order_relaxed);
			if (failed.load(std::memory_order_acquire) || next - read.load(std::memory_order_acquire) == Capacity)
			{
				lost.fetch_add(1, std::memory_order_relaxed);
				return false;
			}
			rows[next % Capacity] = row;
			written.store(next + 1, std::memory_order_release);
			return true;
		}
		template <typename Write>
		void consume(Write write)
		{
			for (std::size_t count = 0; count < Capacity && !disabled(); ++count)
			{
				const auto next = read.load(std::memory_order_relaxed);
				if (next == written.load(std::memory_order_acquire))
					return;
				const Row row = rows[next % Capacity];
				read.store(next + 1, std::memory_order_release);
				if (!write(row))
				{
					failed.store(true, std::memory_order_release);
					return;
				}
			}
		}
		bool disabled() const noexcept
		{
			return failed.load(std::memory_order_acquire);
		}
		std::uint64_t dropped() const noexcept
		{
			return lost.load(std::memory_order_relaxed);
		}
	};
} // namespace pbd::session_trace
