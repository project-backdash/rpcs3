#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <memory>

namespace pbd::session_history
{
	// Absolute identities distinguish a new tick from an older tick in the same physical slot.
	// The producer must retire trace and confirmed replay rows before reusing their storage.
	template <typename T, std::size_t Capacity>
	class buffer
	{
		static_assert(Capacity > 1 && (Capacity & (Capacity - 1)) == 0);
		struct entry
		{
			T value{};
			std::uint64_t identity = 0;
		};
		std::unique_ptr<entry[]> m_entries;
		std::size_t m_capacity = 0, m_extent = 0;
		T m_default{};
		std::size_t slot(std::uint32_t index) const noexcept
		{
			return m_capacity == Capacity ? index & (Capacity - 1) : index % m_capacity;
		}

	public:
		explicit buffer(std::size_t extent = Capacity)
		{
			resize(extent);
		}
		T& operator[](std::uint32_t index) noexcept
		{
			auto& item = m_entries[slot(index)];
			const std::uint64_t identity = std::uint64_t{index} + 1;
			if (item.identity != identity)
			{
				item.value = m_default;
				item.identity = identity;
			}
			return item.value;
		}
		// The publication/retirement handshake owns the lifetime of a reader's row.
		const T& operator[](std::uint32_t index) const noexcept
		{
			return m_entries[slot(index)].value;
		}
		bool contains(std::uint32_t index) const noexcept
		{
			return m_capacity && m_entries[slot(index)].identity == std::uint64_t{index} + 1;
		}
		void fill(const T& value)
		{
			m_default = value;
			for (std::size_t index = 0; index < m_capacity; ++index)
				m_entries[index] = {};
		}
		// Finite local playback keeps every row so an earlier checkpoint remains restorable.
		void resize(std::size_t extent, bool retain_all = false)
		{
			const auto capacity = retain_all ? extent : std::min(extent, Capacity);
			if (capacity != m_capacity)
			{
				m_entries = std::make_unique<entry[]>(capacity);
				m_capacity = capacity;
			}
			m_extent = extent;
		}
		std::size_t size() const noexcept
		{
			return m_extent;
		}
		static constexpr bool retained(std::uint32_t index, std::uint32_t newest) noexcept
		{
			return std::uint64_t{index} + Capacity > newest;
		}
		static constexpr bool reclaimable(std::uint32_t index, std::uint32_t retired) noexcept
		{
			return index < Capacity || index - Capacity < retired;
		}
	};
} // namespace pbd::session_history
