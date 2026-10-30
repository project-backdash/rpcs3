#pragma once

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <optional>
#include <thread>

namespace pbd::stall_metrics
{
	enum class sampling
	{
		cpu,
		elapsed
	};
	// A resident return replaces the boot environment after joining the old guest's threads.
	class generation_flag
	{
		std::atomic<std::uint64_t> epoch{~std::uint64_t{0}};
		std::atomic<bool> value{false};
		std::mutex mutex;

	public:
		template <typename Read>
		bool get(std::uint64_t generation, Read read)
		{
			if (epoch.load(std::memory_order_acquire) != generation)
			{
				std::lock_guard lock(mutex);
				if (epoch.load(std::memory_order_relaxed) != generation)
				{
					value.store(read(), std::memory_order_relaxed);
					epoch.store(generation, std::memory_order_release);
				}
			}
			return value.load(std::memory_order_relaxed);
		}
	};
	struct cpu_sample
	{
		std::uint64_t kernel = 0, user = 0, cycles = 0;
		bool valid = false;
	};
	struct record
	{
		const char* site = nullptr;
		std::uint64_t begin_ns = 0, end_ns = 0, bytes = 0, detail = 0;
		cpu_sample before{}, after{};
		std::uint32_t thread = 0, boundary = 0;
	};
	// Each producer reserves its own slot. The single writer retires it only after copying the published row.
	template <std::size_t Capacity = 65536>
	class queue
	{
		static_assert(Capacity > 1 && (Capacity & (Capacity - 1)) == 0);
		struct slot
		{
			std::atomic<std::uint64_t> sequence{0};
			record row{};
		};
		std::array<slot, Capacity> rows{};
		static constexpr std::uint64_t closed = std::uint64_t{1} << 63;
		alignas(64) std::atomic<std::uint64_t> written{0};
		alignas(64) std::uint64_t read = 0;
		alignas(64) std::atomic<std::uint64_t> lost{0};

	public:
		static constexpr std::size_t capacity = Capacity;
		queue() noexcept
		{
			restart();
		}
		// A new guest starts only after the previous guest and its writer have joined.
		void restart() noexcept
		{
			for (std::size_t index = 0; index < Capacity; ++index)
				rows[index].sequence.store(index, std::memory_order_relaxed);
			read = 0;
			lost.store(0, std::memory_order_relaxed);
			written.store(0, std::memory_order_release);
		}
		void close() noexcept
		{
			written.fetch_or(closed, std::memory_order_acq_rel);
		}
		bool push(record value) noexcept
		{
			auto next = written.load(std::memory_order_relaxed);
			for (;;)
			{
				if (next & closed)
					return false;
				auto& slot = rows[next & (Capacity - 1)];
				const auto sequence = slot.sequence.load(std::memory_order_acquire);
				const auto distance = static_cast<std::int64_t>(sequence - next);
				if (distance == 0)
				{
					if (!written.compare_exchange_weak(next, next + 1, std::memory_order_relaxed))
						continue;
					slot.row = value;
					slot.sequence.store(next + 1, std::memory_order_release);
					return true;
				}
				if (distance < 0)
				{
					lost.fetch_add(1, std::memory_order_relaxed);
					return false;
				}
				next = written.load(std::memory_order_relaxed);
			}
		}
		std::optional<record> pop() noexcept
		{
			auto& slot = rows[read & (Capacity - 1)];
			if (slot.sequence.load(std::memory_order_acquire) != read + 1)
				return {};
			const auto value = slot.row;
			slot.sequence.store(read + Capacity, std::memory_order_release);
			++read;
			return value;
		}
		std::uint64_t dropped() const noexcept
		{
			return lost.load(std::memory_order_relaxed);
		}
		template <typename Consume>
		std::size_t consume(Consume consume, std::size_t limit = Capacity)
		{
			std::size_t count = 0;
			while (count < limit)
			{
				const auto row = pop();
				if (!row)
				{
					const auto end = written.load(std::memory_order_acquire);
					if (!(end & closed) || read == (end & ~closed))
						break;
					// Closing freezes reservations; finish a producer already publishing before the footer.
					std::this_thread::yield();
					continue;
				}
				consume(*row);
				++count;
			}
			return count;
		}
	};
	bool requested() noexcept;
	void boundary(std::uint32_t index) noexcept;
	void point(const char* site, std::uint32_t represented_boundary, std::uint64_t bytes = 0, std::uint64_t detail = 0) noexcept;
	void drain(bool ending) noexcept;
	class scope
	{
		record value{};
		std::uint64_t minimum_ns = 0;
		std::uint64_t construction_ns = 0;
		bool sample_cpu = true;

	public:
		explicit scope(const char* site, std::uint64_t bytes = 0, std::uint64_t detail = 0, bool enabled = true, std::uint64_t minimum_ns = 0, sampling samples = sampling::cpu) noexcept;
		~scope();
		void finish() noexcept;
		scope(const scope&) = delete;
		scope& operator=(const scope&) = delete;
	};
} // namespace pbd::stall_metrics
