#pragma once

#include "pbd_peer_auth.h"

#include <limits>
#include <mutex>
#include <new>
#include <vector>

namespace pbd::peer_auth
{
	struct received
	{
		std::span<const std::uint8_t> payload;
		std::uint64_t epoch;
	};

	// The socket receiver and the owning boundary share one key epoch. Queued bytes from a retired epoch stay unusable.
	class state
	{
		mutable std::mutex m_mutex;
		std::vector<key> m_fingerprints;
		std::optional<key> m_send_key, m_receive_key;
		bool m_required = false;
		sequence_window m_received;
		std::uint64_t m_sent = 0, m_epoch = 0;

	public:
		~state()
		{
			if (m_send_key)
				mbedtls_zeroize(m_send_key->data(), m_send_key->size());
			if (m_receive_key)
				mbedtls_zeroize(m_receive_key->data(), m_receive_key->size());
		}

		bool install(const key& secret, std::uint32_t local_role)
		{
			std::lock_guard lock(m_mutex);
			key fingerprint{};
			if (mbedtls_sha256_ret(secret.data(), secret.size(), fingerprint.data(), 0) != 0 ||
				std::find(m_fingerprints.begin(), m_fingerprints.end(), fingerprint) != m_fingerprints.end() ||
				m_epoch == std::numeric_limits<std::uint64_t>::max())
				return false;
			auto send_key = direction_key(secret, local_role), receive_key = direction_key(secret, 1 - local_role);
			if (!send_key || !receive_key)
			{
				if (send_key)
					mbedtls_zeroize(send_key->data(), send_key->size());
				if (receive_key)
					mbedtls_zeroize(receive_key->data(), receive_key->size());
				return false;
			}
			try
			{
				m_fingerprints.push_back(fingerprint);
			}
			catch (const std::bad_alloc&)
			{
				mbedtls_zeroize(send_key->data(), send_key->size());
				mbedtls_zeroize(receive_key->data(), receive_key->size());
				return false;
			}
			if (m_send_key)
				mbedtls_zeroize(m_send_key->data(), m_send_key->size());
			if (m_receive_key)
				mbedtls_zeroize(m_receive_key->data(), m_receive_key->size());
			m_required = true;
			m_send_key = *send_key;
			m_receive_key = *receive_key;
			mbedtls_zeroize(send_key->data(), send_key->size());
			mbedtls_zeroize(receive_key->data(), receive_key->size());
			m_sent = 0;
			m_received = {};
			++m_epoch;
			return true;
		}

		bool can_learn() const
		{
			std::lock_guard lock(m_mutex);
			return m_receive_key.has_value();
		}

		bool retired() const
		{
			std::lock_guard lock(m_mutex);
			return m_required && !m_receive_key;
		}

		void retire()
		{
			std::lock_guard lock(m_mutex);
			if (!m_required || !m_receive_key)
				return;
			mbedtls_zeroize(m_send_key->data(), m_send_key->size());
			mbedtls_zeroize(m_receive_key->data(), m_receive_key->size());
			m_send_key.reset();
			m_receive_key.reset();
			if (m_epoch != std::numeric_limits<std::uint64_t>::max())
				++m_epoch;
		}

		bool current(std::uint64_t epoch) const
		{
			std::lock_guard lock(m_mutex);
			return m_epoch == epoch && (!m_required || m_receive_key.has_value());
		}
		std::uint64_t epoch() const
		{
			std::lock_guard lock(m_mutex);
			return m_epoch;
		}

		std::optional<received> receive(std::span<const std::uint8_t> bytes)
		{
			std::lock_guard lock(m_mutex);
			if (!m_receive_key)
			{
				if (m_required)
					return {};
				return received{bytes, m_epoch};
			}
			const auto message = open(*m_receive_key, bytes);
			if (!message || !m_received.accept(message->sequence))
				return {};
			return received{message->payload, m_epoch};
		}

		std::size_t send(std::span<const std::uint8_t> payload, std::span<std::uint8_t> output)
		{
			std::lock_guard lock(m_mutex);
			if (!m_send_key)
			{
				if (m_required)
					return 0;
				if (payload.size() > output.size())
					return 0;
				std::copy(payload.begin(), payload.end(), output.begin());
				return payload.size();
			}
			if (m_sent == std::numeric_limits<std::uint64_t>::max())
				return 0;
			const auto size = seal(*m_send_key, m_sent + 1, payload, output);
			if (size)
				++m_sent;
			return size;
		}
	};
} // namespace pbd::peer_auth
