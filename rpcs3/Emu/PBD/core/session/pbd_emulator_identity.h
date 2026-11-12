#pragma once

#include "Crypto/sha256.h"

#include <array>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#endif

namespace pbd::emulator_identity
{
	// The running emulator's SHA-256 in lower-case hex, or empty when it cannot be read. Hashed once: Windows does not
	// let the file of a running program change, and a standby watcher hashes it before its bind instead of during it.
	inline const std::string& executable_sha256()
	{
		static const std::string value = []() -> std::string
		{
#ifdef _WIN32
			std::array<wchar_t, 32768> path{};
			const DWORD length = GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size()));
			std::ifstream input(std::filesystem::path(path.data()), std::ios::binary);
			if (!length || length >= path.size() || !input)
				return {};
			mbedtls_sha256_context context{};
			mbedtls_sha256_init(&context);
			bool ok = mbedtls_sha256_starts_ret(&context, 0) == 0;
			std::array<unsigned char, 65536> bytes{};
			std::uint64_t total = 0;
			while (ok && input)
			{
				input.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
				const auto count = input.gcount();
				ok = count >= 0 && total + static_cast<std::uint64_t>(count) <= 1024ull * 1024 * 1024;
				total += ok ? static_cast<std::uint64_t>(count) : 0;
				ok = ok && mbedtls_sha256_update_ret(&context, bytes.data(), static_cast<std::size_t>(count)) == 0;
			}
			std::array<unsigned char, 32> hash{};
			ok = ok && input.eof() && total > 0 && mbedtls_sha256_finish_ret(&context, hash.data()) == 0;
			mbedtls_sha256_free(&context);
			constexpr char hex[] = "0123456789abcdef";
			std::string actual;
			for (const auto octet : hash)
			{
				actual += hex[octet >> 4];
				actual += hex[octet & 15];
			}
			return ok ? actual : std::string{};
#else
			return {};
#endif
		}();
		return value;
	}
} // namespace pbd::emulator_identity
