#pragma once

#include <cstdint>
#include <limits>
#include <string>

namespace pbd::peer_diagnostics
{
	enum class rejection
	{
		none,
		tag,
		role,
		session,
		epoch,
		format,
		replay,
		source
	};
	// Fixed storage and saturating counts keep hostile traffic from growing records or wrapping evidence.
	struct counters
	{
		std::uint32_t send_ok = 0, send_failed = 0, last_send_error = 0;
		std::uint32_t datagrams = 0, source_match = 0, source_mismatch = 0;
		std::uint32_t tag = 0, role = 0, session = 0, epoch = 0, format = 0, replay = 0, source = 0;
		std::uint64_t first_datagram_us = 0;
		static void increment(std::uint32_t& count)
		{
			if (count != std::numeric_limits<std::uint32_t>::max())
				++count;
		}
		void sent(bool ok, std::uint32_t error)
		{
			increment(ok ? send_ok : send_failed);
			if (!ok)
				last_send_error = error;
		}
		void received(bool matches, std::uint64_t now)
		{
			increment(datagrams);
			increment(matches ? source_match : source_mismatch);
			if (!first_datagram_us)
				first_datagram_us = now;
		}
		void rejected(rejection reason)
		{
			switch (reason)
			{
			case rejection::tag: increment(tag); break;
			case rejection::role: increment(role); break;
			case rejection::session: increment(session); break;
			case rejection::epoch: increment(epoch); break;
			case rejection::format: increment(format); break;
			case rejection::replay: increment(replay); break;
			case rejection::source: increment(source); break;
			case rejection::none: break;
			}
		}
	};
	inline std::string json(const counters& c)
	{
		return "{\"sendOk\":" + std::to_string(c.send_ok) + ",\"sendFailed\":" + std::to_string(c.send_failed) +
		       ",\"lastSendError\":" + std::to_string(c.last_send_error) + ",\"datagrams\":" + std::to_string(c.datagrams) +
		       ",\"sourceMatch\":" + std::to_string(c.source_match) + ",\"sourceMismatch\":" + std::to_string(c.source_mismatch) +
		       ",\"firstDatagramUs\":" + std::to_string(c.first_datagram_us) + ",\"rejectTag\":" + std::to_string(c.tag) +
		       ",\"rejectRole\":" + std::to_string(c.role) + ",\"rejectSession\":" + std::to_string(c.session) +
		       ",\"rejectEpoch\":" + std::to_string(c.epoch) + ",\"rejectFormat\":" + std::to_string(c.format) +
		       ",\"rejectReplay\":" + std::to_string(c.replay) + ",\"rejectSource\":" + std::to_string(c.source) + "}";
	}
} // namespace pbd::peer_diagnostics
