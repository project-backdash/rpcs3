#pragma once

#include "pbd_env.h"
#include <cstdint>
#include <map>
#include <mutex>
#include <optional>
#include <string_view>

namespace pbd::fight_timing
{
	struct result
	{
		std::uint32_t fight_frames = 0, on_time_frames = 0;
	};

	inline bool requested() noexcept
	{
		static const bool enabled = []
		{
			const auto one = [](const char* name)
			{
				const auto* value = pbd::env::find(name);
				return value && std::string_view(value) == "1";
			};
			const auto channel = [](const char* name)
			{
				const auto* value = pbd::env::find(name);
				return value && *value;
			};
			return one("PBD_SESSION") && one("PBD_PEER_LIVE") &&
			       (channel("PBD_SET_CHANNEL") || channel("PBD_MATCH_STANDBY_CHANNEL"));
		}();
		return enabled;
	}

	// A forward render command in a verified fight defines a frame; only its proved visible delivery can meet a deadline.
	class counter
	{
		struct frame
		{
			std::uint64_t span = 0, ordinal = 0;
			bool seen = false, measured = true, on_time = false, unavailable = false;
		};
		struct clock
		{
			std::uint64_t at = 0, deadline = 0, ordinal = 0;
		};
		std::mutex m_mutex;
		std::map<std::uint32_t, frame> m_frames;
		std::map<std::uint64_t, clock> m_clocks;
		std::uint64_t m_epoch = 0, m_span = 0, m_ordinal = 0;
		std::uint32_t m_current_boundary = ~std::uint32_t{0};
		bool m_fighting = false, m_overflow = false;

	public:
		static constexpr std::uint64_t period_ns = 1'000'000'000ull / 60;
		// The allowance is shared across successive deadlines, rather than added to every frame's budget.
		static constexpr std::uint64_t timestamp_allowance_ns = 1'000'000;
		static constexpr std::size_t maximum_frames = 216000;

		void forward(std::uint64_t epoch, std::uint32_t boundary, bool fighting)
		{
			std::lock_guard lock(m_mutex);
			if (epoch != m_epoch)
			{
				m_frames.clear();
				m_clocks.clear();
				m_epoch = epoch;
				m_span = m_ordinal = 0;
				m_fighting = m_overflow = false;
			}
			m_current_boundary = boundary;
			if (!fighting)
			{
				m_fighting = false;
				return;
			}
			if (!m_fighting)
			{
				++m_span;
				m_ordinal = 0;
				m_fighting = true;
			}
		}

		void produced(std::uint64_t epoch, std::uint32_t boundary)
		{
			std::lock_guard lock(m_mutex);
			if (epoch != m_epoch || boundary != m_current_boundary || !m_fighting || m_frames.contains(boundary))
				return;
			if (m_frames.size() >= maximum_frames)
			{
				m_overflow = true;
				return;
			}
			m_frames.emplace(boundary, frame{m_span, m_ordinal++});
		}

		void delivered(std::uint64_t epoch, std::uint32_t boundary, std::uint64_t now_ns, bool live, bool proved, bool visible)
		{
			if (!live)
				return;
			std::lock_guard lock(m_mutex);
			const auto found = m_frames.find(boundary);
			if (epoch != m_epoch || found == m_frames.end())
				return;
			auto& value = found->second;
			if (!proved || !visible || !now_ns)
			{
				value.unavailable = true;
				return;
			}
			if (value.seen)
				return;
			value.seen = true;
			const auto prior = m_clocks.find(value.span);
			if (prior == m_clocks.end())
			{
				// One delivered frame anchors each fight span; it supplies no interval measurement itself.
				value.measured = false;
				m_clocks.emplace(value.span, clock{now_ns, now_ns, value.ordinal});
				return;
			}
			auto& previous = prior->second;
			if (now_ns < previous.at || value.ordinal <= previous.ordinal)
			{
				value.unavailable = true;
				return;
			}
			const auto due = previous.deadline + (value.ordinal - previous.ordinal) * period_ns;
			value.on_time = now_ns <= due + timestamp_allowance_ns;
			previous = {now_ns, value.on_time ? due : now_ns, value.ordinal};
		}

		std::optional<result> take(std::uint64_t epoch, std::uint32_t first, std::uint32_t end)
		{
			std::lock_guard lock(m_mutex);
			if (epoch != m_epoch || first >= end)
				return {};
			result counts;
			bool available = !m_overflow, delivered = false;
			for (auto it = m_frames.lower_bound(first); it != m_frames.end() && it->first < end;)
			{
				const auto& value = it->second;
				available &= !value.unavailable;
				delivered |= value.seen;
				counts.fight_frames += value.measured ? 1u : 0u;
				counts.on_time_frames += value.measured && value.on_time ? 1u : 0u;
				m_clocks.erase(value.span);
				it = m_frames.erase(it);
			}
			m_fighting = false;
			m_overflow = false;
			return available && delivered && counts.fight_frames ? std::optional{counts} : std::nullopt;
		}
	};
	inline counter samples;
}
