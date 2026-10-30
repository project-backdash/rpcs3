#pragma once

#include "Crypto/sha256.h"
#include "Crypto/utils.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <optional>
#include <span>

namespace pbd::peer_auth
{
	using key = std::array<std::uint8_t, 32>;
	using tag = std::array<std::uint8_t, 16>;
	inline constexpr std::array<std::uint8_t, 8> magic{'P', 'B', 'D', '-', 'A', 'U', 'T', 'H'};
	inline constexpr std::uint32_t version = 1;
	inline constexpr std::size_t header_bytes = 24, maximum_payload_bytes = 512;
	inline constexpr std::size_t maximum_bytes = header_bytes + maximum_payload_bytes + tag{}.size();

	inline std::optional<key> authenticate_full(std::span<const std::uint8_t> secret, std::span<const std::uint8_t> message)
	{
		if (secret.size() > 64)
			return {};
		std::array<std::uint8_t, 64> inner_pad{}, outer_pad{};
		for (std::size_t i = 0; i < inner_pad.size(); ++i)
		{
			const auto byte = i < secret.size() ? secret[i] : 0;
			inner_pad[i] = byte ^ 0x36;
			outer_pad[i] = byte ^ 0x5c;
		}
		mbedtls_sha256_context context;
		mbedtls_sha256_init(&context);
		std::array<std::uint8_t, 32> digest{};
		const bool ok = mbedtls_sha256_starts_ret(&context, 0) == 0 &&
		                mbedtls_sha256_update_ret(&context, inner_pad.data(), inner_pad.size()) == 0 &&
		                mbedtls_sha256_update_ret(&context, message.data(), message.size()) == 0 &&
		                mbedtls_sha256_finish_ret(&context, digest.data()) == 0 &&
		                mbedtls_sha256_starts_ret(&context, 0) == 0 &&
		                mbedtls_sha256_update_ret(&context, outer_pad.data(), outer_pad.size()) == 0 &&
		                mbedtls_sha256_update_ret(&context, digest.data(), digest.size()) == 0 &&
		                mbedtls_sha256_finish_ret(&context, digest.data()) == 0;
		mbedtls_sha256_free(&context);
		mbedtls_zeroize(inner_pad.data(), inner_pad.size());
		mbedtls_zeroize(outer_pad.data(), outer_pad.size());
		if (!ok)
		{
			mbedtls_zeroize(digest.data(), digest.size());
			return {};
		}
		std::optional<key> result = digest;
		mbedtls_zeroize(digest.data(), digest.size());
		return result;
	}

	inline std::optional<tag> authenticate(std::span<const std::uint8_t> secret, std::span<const std::uint8_t> message)
	{
		auto digest = authenticate_full(secret, message);
		if (!digest)
			return {};
		tag result;
		std::copy_n(digest->begin(), result.size(), result.begin());
		mbedtls_zeroize(digest->data(), digest->size());
		return result;
	}

	// Opposite directions use different keys: reflecting an authentic outbound packet cannot consume an inbound sequence.
	inline std::optional<key> direction_key(const key& secret, std::uint32_t sender_role)
	{
		if (sender_role > 1)
			return {};
		const std::array<std::uint8_t, 12> domain{'P', 'B', 'D', '-', 'T', 'X', '-', '1', static_cast<std::uint8_t>(sender_role), 0, 0, 0};
		return authenticate_full(secret, domain);
	}

	namespace detail
	{
		template <typename Value>
		void store(std::span<std::uint8_t> bytes, std::size_t offset, Value value)
		{
			for (std::size_t i = 0; i < sizeof(Value); ++i)
				bytes[offset + i] = static_cast<std::uint8_t>(value >> (8 * i));
		}
		template <typename Value>
		Value load(std::span<const std::uint8_t> bytes, std::size_t offset)
		{
			Value result = 0;
			for (std::size_t i = 0; i < sizeof(Value); ++i)
				result |= static_cast<Value>(Value{bytes[offset + i]} << (8 * i));
			return result;
		}
	} // namespace detail

	// The sequence and the complete inner codec bytes are authenticated before a receiver interprets any game field.
	inline std::size_t seal(const key& secret, std::uint64_t sequence, std::span<const std::uint8_t> payload,
		std::span<std::uint8_t> output)
	{
		if (!sequence || payload.empty() || payload.size() > maximum_payload_bytes ||
			output.size() < header_bytes + payload.size() + tag{}.size())
			return 0;
		std::copy(magic.begin(), magic.end(), output.begin());
		detail::store(output, 8, version);
		detail::store(output, 12, sequence);
		detail::store(output, 20, static_cast<std::uint32_t>(payload.size()));
		std::copy(payload.begin(), payload.end(), output.begin() + header_bytes);
		const auto authenticated_bytes = header_bytes + payload.size();
		const auto signed_tag = authenticate(secret, output.first(authenticated_bytes));
		if (!signed_tag)
			return 0;
		std::copy(signed_tag->begin(), signed_tag->end(), output.begin() + authenticated_bytes);
		return authenticated_bytes + signed_tag->size();
	}

	struct opened
	{
		std::uint64_t sequence;
		std::span<const std::uint8_t> payload;
	};

	inline std::optional<opened> open(const key& secret, std::span<const std::uint8_t> bytes)
	{
		if (bytes.size() < header_bytes + tag{}.size() || bytes.size() > maximum_bytes ||
			!std::equal(magic.begin(), magic.end(), bytes.begin()) || detail::load<std::uint32_t>(bytes, 8) != version)
			return {};
		const auto sequence = detail::load<std::uint64_t>(bytes, 12);
		const auto size = detail::load<std::uint32_t>(bytes, 20);
		if (!sequence || !size || bytes.size() != header_bytes + size + tag{}.size())
			return {};
		const auto signed_tag = authenticate(secret, bytes.first(header_bytes + size));
		if (!signed_tag)
			return {};
		std::uint8_t difference = 0;
		for (std::size_t i = 0; i < signed_tag->size(); ++i)
			difference |= (*signed_tag)[i] ^ bytes[header_bytes + size + i];
		if (difference)
			return {};
		return opened{sequence, bytes.subspan(header_bytes, size)};
	}

	// Reordered UDP packets within one input window remain usable; a replay never changes endpoint or peer state.
	class sequence_window
	{
		std::uint64_t m_highest = 0, m_seen = 0;

	public:
		bool accept(std::uint64_t sequence)
		{
			if (!sequence)
				return false;
			if (sequence > m_highest)
			{
				const auto distance = sequence - m_highest;
				m_seen = (distance >= 64 ? 0 : m_seen << distance) | 1;
				m_highest = sequence;
				return true;
			}
			const auto distance = m_highest - sequence;
			if (distance >= 64 || (m_seen & (std::uint64_t{1} << distance)))
				return false;
			m_seen |= std::uint64_t{1} << distance;
			return true;
		}
	};
} // namespace pbd::peer_auth
