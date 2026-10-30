#pragma once

#include "pbd_env.h"
#include "pbd_peer_auth_state.h"

#include <chrono>
#include <string>
#include <string_view>

#ifdef _WIN32
#include <winsock2.h>
#include <windows.h>
#endif

namespace pbd::peer_key_pipe
{
	inline constexpr std::array<std::uint8_t, 8> magic{'P', 'B', 'D', 'K', 'E', 'Y', '0', '1'};
	using nonce = std::array<std::uint8_t, 32>;
	using request_bytes = std::array<std::uint8_t, 120>;
	using response_bytes = std::array<std::uint8_t, 48>;
	using ack_bytes = std::array<std::uint8_t, 16>;

	inline std::optional<request_bytes> request(std::uint32_t role, std::uint32_t transport_tag,
		std::string_view set_id, const nonce& child_nonce)
	{
		if (role > 1 || set_id.empty() || set_id.size() > 64 ||
			std::any_of(set_id.begin(), set_id.end(), [](unsigned char c)
				{
					return c < 0x21 || c > 0x7e;
				}))
			return {};
		request_bytes result{};
		std::copy(magic.begin(), magic.end(), result.begin());
		peer_auth::detail::store(std::span(result), 8, std::uint32_t{1});
		peer_auth::detail::store(std::span(result), 12, role);
		peer_auth::detail::store(std::span(result), 16, transport_tag);
		peer_auth::detail::store(std::span(result), 20, static_cast<std::uint32_t>(set_id.size()));
		std::copy(child_nonce.begin(), child_nonce.end(), result.begin() + 24);
		std::copy(set_id.begin(), set_id.end(), result.begin() + 56);
		return result;
	}

	inline std::optional<peer_auth::key> read_response(std::span<const std::uint8_t> bytes)
	{
		if (bytes.size() != response_bytes{}.size() || !std::equal(magic.begin(), magic.end(), bytes.begin()) ||
			peer_auth::detail::load<std::uint32_t>(bytes, 8) != 1 || peer_auth::detail::load<std::uint32_t>(bytes, 12) != 1)
			return {};
		peer_auth::key result;
		std::copy_n(bytes.begin() + 16, result.size(), result.begin());
		return result;
	}

	inline bool complete_install(peer_auth::state& state, bool acknowledged)
	{
		if (!acknowledged)
			state.retire();
		return acknowledged;
	}

#ifdef _WIN32
	struct descriptor
	{
		std::wstring path;
		nonce child_nonce;
		std::string initial_set_id;
	};
	inline std::optional<descriptor> inherited_descriptor()
	{
		const auto path = env::find_wide("PBD_PEER_AUTH_PIPE");
		const auto secret = env::find_wide("PBD_PEER_AUTH_NONCE");
		const auto set_id = env::find_wide("PBD_PEER_AUTH_SET_ID");
		if (!path || !secret || !set_id)
			return {};
		const std::wstring_view pipe(path), hex(secret), id(set_id);
		constexpr std::wstring_view prefix = L"\\\\.\\pipe\\pbd-peer-key-";
		if (!pipe.starts_with(prefix) || pipe.size() != prefix.size() + 36 || hex.size() != 64 || id.empty() || id.size() > 64)
			return {};
		descriptor result{std::wstring(pipe), {}, {}};
		for (std::size_t i = 0; i < result.child_nonce.size(); ++i)
		{
			const auto digit = [](wchar_t c) -> int
			{
				return c >= L'0' && c <= L'9' ? c - L'0' : c >= L'a' && c <= L'f' ? c - L'a' + 10 :
				                                                                    -1;
			};
			const int high = digit(hex[2 * i]), low = digit(hex[2 * i + 1]);
			if (high < 0 || low < 0)
				return {};
			result.child_nonce[i] = static_cast<std::uint8_t>(high * 16 + low);
		}
		for (const auto c : id)
		{
			if (c < 0x21 || c > 0x7e)
				return {};
			result.initial_set_id += static_cast<char>(c);
		}
		return result;
	}

	namespace detail
	{
		struct handle
		{
			HANDLE value;
			~handle()
			{
				if (value != INVALID_HANDLE_VALUE && value)
					::CloseHandle(value);
			}
		};
		using clock = std::chrono::steady_clock;
		inline bool transfer(HANDLE pipe, std::span<std::uint8_t> bytes, bool writing, clock::time_point deadline)
		{
			while (!bytes.empty())
			{
				const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - clock::now()).count();
				if (remaining <= 0)
					return false;
				handle event{::CreateEventW(nullptr, TRUE, FALSE, nullptr)};
				if (!event.value)
					return false;
				OVERLAPPED operation{};
				operation.hEvent = event.value;
				DWORD done = 0;
				const BOOL started = writing ? ::WriteFile(pipe, bytes.data(), static_cast<DWORD>(bytes.size()), &done, &operation) :
				                               ::ReadFile(pipe, bytes.data(), static_cast<DWORD>(bytes.size()), &done, &operation);
				if (!started)
				{
					if (::GetLastError() != ERROR_IO_PENDING)
						return false;
					if (::WaitForSingleObject(event.value, static_cast<DWORD>(remaining)) != WAIT_OBJECT_0)
					{
						::CancelIoEx(pipe, &operation);
						// OVERLAPPED and its event must outlive the cancellation completion.
						::GetOverlappedResult(pipe, &operation, &done, TRUE);
						return false;
					}
					if (!::GetOverlappedResult(pipe, &operation, &done, FALSE))
						return false;
				}
				if (!done || done > bytes.size())
					return false;
				bytes = bytes.subspan(done);
			}
			return true;
		}
	} // namespace detail

	// The key is consumed directly into the epoch state; no caller or file receives its printable representation.
	inline bool install(peer_auth::state& state, const descriptor& pipe, std::uint32_t role,
		std::uint32_t transport_tag, std::string_view set_id)
	{
		auto bytes = request(role, transport_tag, set_id, pipe.child_nonce);
		if (!bytes)
			return false;
		const auto deadline = detail::clock::now() + std::chrono::seconds(2);
		if (!::WaitNamedPipeW(pipe.path.c_str(), 1000))
			return false;
		detail::handle connection{::CreateFileW(pipe.path.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr,
			OPEN_EXISTING, FILE_FLAG_OVERLAPPED, nullptr)};
		if (connection.value == INVALID_HANDLE_VALUE || !detail::transfer(connection.value, *bytes, true, deadline))
			return false;
		response_bytes response{};
		const bool complete = detail::transfer(connection.value, response, false, deadline);
		auto secret = complete ? read_response(response) : std::nullopt;
		mbedtls_zeroize(response.data(), response.size());
		if (!secret)
			return false;
		const bool installed = state.install(*secret, role);
		mbedtls_zeroize(secret->data(), secret->size());
		if (!installed)
			return false;
		ack_bytes ack{};
		std::copy(magic.begin(), magic.end(), ack.begin());
		peer_auth::detail::store(std::span(ack), 8, std::uint32_t{1});
		peer_auth::detail::store(std::span(ack), 12, std::uint32_t{2});
		return complete_install(state, detail::transfer(connection.value, ack, true, deadline));
	}
#endif
} // namespace pbd::peer_key_pipe
