#pragma once

// Sealed replay page images are compressed by one host worker. At most two images are in flight; the current checkpoint's
// first-write journal stays with the simulation owner. Jobs own their bytes, so deleting a checkpoint cannot leave a worker
// referencing the session or guest memory. Compression uses the codec already linked by the game.
#include "../../../3rdparty/zstd/zstd/lib/zstd.h"

#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

namespace pbd::replay_checkpoint
{
	struct block
	{
		std::vector<std::uint8_t> bytes;
		std::size_t raw_size = 0;
		bool packed = false;

		void compress() noexcept
		{
			raw_size = bytes.size();
			if (bytes.empty())
				return;
			try
			{
				std::vector<std::uint8_t> compressed(ZSTD_compressBound(bytes.size()));
				const auto size = ZSTD_compress(compressed.data(), compressed.size(), bytes.data(), bytes.size(), 1);
				if (ZSTD_isError(size) || size >= bytes.size())
					return;
				compressed.resize(size);
				compressed.shrink_to_fit();
				bytes = std::move(compressed);
				packed = true;
			}
			catch (...)
			{
				// Compression is optional; retain the original bytes when allocating the packed form fails.
			}
		}
		bool expand(std::vector<std::uint8_t>& out) const noexcept
		{
			try
			{
				if (!packed)
				{
					out = bytes;
					return true;
				}
				out.resize(raw_size);
				const auto size = ZSTD_decompress(out.data(), out.size(), bytes.data(), bytes.size());
				return !ZSTD_isError(size) && size == raw_size;
			}
			catch (...)
			{
				return false;
			}
		}
	};
	struct image
	{
		block pages, journal;
		std::atomic<bool> ready{false};
		void wait() const noexcept
		{
			while (!ready.load(std::memory_order_acquire))
				ready.wait(false, std::memory_order_acquire);
		}
		std::size_t bytes() const noexcept
		{
			return pages.bytes.capacity() + journal.bytes.capacity();
		}
	};
	class compressor
	{
		std::mutex m_mutex;
		std::condition_variable m_wake;
		std::deque<std::shared_ptr<image>> m_queue;
		std::size_t m_pending = 0;
		bool m_stopping = false;
		std::thread m_worker;

		void run() noexcept
		{
			for (;;)
			{
				std::shared_ptr<image> next;
				{
					std::unique_lock lock(m_mutex);
					m_wake.wait(lock, [&]
						{
							return m_stopping || !m_queue.empty();
						});
					if (m_queue.empty())
						return;
					next = std::move(m_queue.front());
					m_queue.pop_front();
				}
				next->pages.compress();
				next->journal.compress();
				next->ready.store(true, std::memory_order_release);
				next->ready.notify_all();
				std::lock_guard lock(m_mutex);
				--m_pending;
			}
		}

	public:
		static constexpr std::size_t capacity = 2;
		compressor() : m_worker([this]
						   {
							   run();
						   })
		{
		}
		~compressor()
		{
			{
				std::lock_guard lock(m_mutex);
				m_stopping = true;
			}
			m_wake.notify_one();
			m_worker.join();
		}
		compressor(const compressor&) = delete;
		compressor& operator=(const compressor&) = delete;

		// A full queue leaves both source buffers intact. The owner may retry at the next checkpoint without waiting.
		std::shared_ptr<image> submit(std::vector<std::uint8_t>& pages, std::vector<std::uint8_t>& journal) noexcept
		{
			try
			{
				std::lock_guard lock(m_mutex);
				if (m_stopping || m_pending == capacity)
					return {};
				auto next = std::make_shared<image>();
				m_queue.push_back(next);
				next->pages.bytes.swap(pages);
				next->journal.bytes.swap(journal);
				++m_pending;
				m_wake.notify_one();
				return next;
			}
			catch (...)
			{
				return {};
			}
		}
	};
} // namespace pbd::replay_checkpoint
