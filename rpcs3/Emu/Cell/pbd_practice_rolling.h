#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>

namespace pbd::practice::rolling
{
	template <std::size_t Capacity>
	class window
	{
		// Producer publication and writer retirement synchronize these tags. A slot cannot be reused while the trace writer
		// still owns its row; a new epoch invalidates discarded future rows after a completed rewind handshake.
		static_assert(Capacity > 1 && (Capacity & (Capacity - 1)) == 0);
		struct tag
		{
			std::uint32_t index = 0;
			std::uint64_t epoch = 0;
		};
		std::array<tag, Capacity> m_tags{};
		std::uint64_t m_epoch = 1;

	public:
		static constexpr std::size_t capacity = Capacity;
		static constexpr std::size_t slot(std::uint32_t index) noexcept
		{
			return index & (Capacity - 1);
		}
		bool owns(std::uint32_t index) const noexcept
		{
			const auto& tag = m_tags[slot(index)];
			return tag.epoch == m_epoch && tag.index == index;
		}
		bool reclaimable(std::uint32_t index, std::uint32_t retired_through) const noexcept
		{
			const auto& tag = m_tags[slot(index)];
			return tag.epoch != m_epoch || tag.index == index || tag.index < retired_through;
		}
		bool claim(std::uint32_t index, std::uint32_t retired_through) noexcept
		{
			if (!reclaimable(index, retired_through))
				return false;
			m_tags[slot(index)] = {index, m_epoch};
			return true;
		}
		bool can_branch() const noexcept
		{
			return m_epoch != std::numeric_limits<std::uint64_t>::max();
		}
		bool begin_branch() noexcept
		{
			if (!can_branch())
				return false;
			++m_epoch;
			return true;
		}
	};
} // namespace pbd::practice::rolling
