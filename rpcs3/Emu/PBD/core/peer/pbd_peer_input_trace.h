#pragma once

#include <cstddef>

#include <array>
#include <atomic>
#include <cstdint>
#include <optional>

namespace pbd::peer_input_trace
{
	enum class kind
	{
		receive,
		fill,
		assign
	};
	struct record
	{
		kind type = kind::receive;
		std::uint32_t boundary = 0, tick = 0, value = 0;
		std::uint32_t physical = 0, suppress = 0, deferred = 0, carried = 0, delay = 0;
		bool lockstep = false, predicted = false;
		const char* how = "assigned";
	};
	// One input owner produces, the existing record writer consumes. A full trace never waits on a game boundary.
	template <std::size_t Capacity = 4096>
	class queue
	{
		std::array<record, Capacity> records{};
		std::atomic<std::uint64_t> read{0}, written{0}, lost{0};

	public:
		void push(record value) noexcept
		{
			const auto next = written.load(std::memory_order_relaxed);
			if (next - read.load(std::memory_order_acquire) == Capacity)
			{
				lost.fetch_add(1, std::memory_order_relaxed);
				return;
			}
			records[next % Capacity] = value;
			written.store(next + 1, std::memory_order_release);
		}
		std::optional<record> pop() noexcept
		{
			const auto next = read.load(std::memory_order_relaxed);
			if (next == written.load(std::memory_order_acquire))
				return {};
			const auto value = records[next % Capacity];
			read.store(next + 1, std::memory_order_release);
			return value;
		}
		std::uint64_t dropped() const noexcept
		{
			return lost.load(std::memory_order_relaxed);
		}
	};
} // namespace pbd::peer_input_trace
