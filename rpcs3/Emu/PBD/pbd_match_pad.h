#pragma once

#include <array>
#include <chrono>
#include <optional>

namespace pbd::match_pad
{
	enum class connection_result
	{
		ready,
		cancelled,
		expired
	};
	template <typename Probe, typename Cancel, typename Wait>
	connection_result await_connection(Probe connected, Cancel cancelled, Wait wait,
		std::chrono::milliseconds limit = std::chrono::seconds(10))
	{
		const auto deadline = std::chrono::steady_clock::now() + limit;
		for (;;)
		{
			if (cancelled())
				return connection_result::cancelled;
			if (connected())
				return connection_result::ready;
			if (std::chrono::steady_clock::now() >= deadline)
				return connection_result::expired;
			wait();
		}
	}
	struct port
	{
		bool real = false, neutral = false;
	};

	// The configured device keeps its own handler and button mapping when assigned another game port.
	inline std::optional<unsigned> source(const std::array<port, 2>& ports) noexcept
	{
		for (unsigned index = 0; index < 2; ++index)
			if (ports[index].real && !ports[index].neutral && !ports[1 - index].real && ports[1 - index].neutral)
				return index;
		return {};
	}

	class assignment
	{
		std::optional<bool> m_swap;

	public:
		bool available(const std::array<port, 2>& ports) const noexcept
		{
			return !m_swap && source(ports).has_value();
		}
		bool bind(const std::array<port, 2>& ports, unsigned role) noexcept
		{
			const auto configured = source(ports);
			if (m_swap || !configured || role > 1)
				return false;
			m_swap = *configured != role;
			return true;
		}
		bool swapped() const noexcept
		{
			return m_swap.value_or(false);
		}
	};
} // namespace pbd::match_pad
