#pragma once

#include <cstddef>
#include <cstdlib>
#include <filesystem>
#include <string_view>

// The emulator's switches: environment variables named PBD_*.
namespace pbd::env
{
	namespace detail
	{
		inline constexpr std::string_view prefix = "PBD_";
		// Longer than any switch name, terminator included.
		inline constexpr std::size_t key_capacity = 96;

#ifdef _MSC_VER
#pragma warning(push)
#pragma warning(disable : 4996) // getenv: callers use the value before the environment can change
#endif
		inline const char* read(const char* key) noexcept
		{
			return std::getenv(key);
		}
#ifdef _WIN32
		inline const wchar_t* read(const wchar_t* key) noexcept
		{
			return _wgetenv(key);
		}
#endif
#ifdef _MSC_VER
#pragma warning(pop)
#endif

		// Writes `name` into `key` as a terminated name; false when it does not fit. A string_view need not be terminated,
		// so the lookups copy it before they call getenv.
		template <typename Char>
		constexpr bool spell(Char (&key)[key_capacity], std::string_view name) noexcept
		{
			if (name.size() >= key_capacity)
				return false;
			std::size_t at = 0;
			for (const char c : name)
				key[at++] = static_cast<Char>(c);
			key[at] = 0;
			return true;
		}

		template <typename Char>
		const Char* find(std::string_view name) noexcept
		{
			Char key[key_capacity];
			return spell(key, name) ? read(key) : nullptr;
		}
	} // namespace detail

	// The value of the switch `name`, or nullptr when it is not set. Like getenv's, the pointer is valid until the process
	// environment next changes.
	inline const char* find(std::string_view name) noexcept
	{
		return detail::find<char>(name);
	}

#ifdef _WIN32
	// The same lookup in the wide environment, for values that may hold text outside the ANSI code page, such as paths.
	inline const wchar_t* find_wide(std::string_view name) noexcept
	{
		return detail::find<wchar_t>(name);
	}

	// Sets the switch `name` for this process and the processes it starts, so the lookups above return `value`; an empty
	// value removes the switch. False when `name` is not a PBD_ name or could not be written.
	inline bool set_wide(std::string_view name, const wchar_t* value) noexcept
	{
		wchar_t key[detail::key_capacity];
		return name.starts_with(detail::prefix) && detail::spell(key, name) && !_wputenv_s(key, value);
	}
#endif

	// Paths retain the platform's complete native representation.
	inline std::filesystem::path path(std::string_view name)
	{
#ifdef _WIN32
		const auto value = find_wide(name);
#else
		const auto value = find(name);
#endif
		return value ? std::filesystem::path(value) : std::filesystem::path{};
	}
} // namespace pbd::env
