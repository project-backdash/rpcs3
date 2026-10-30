#include "pbd_test.h"

#include "Emu/PBD/pbd_env.h"

#include <cstdlib>
#include <string>
#include <string_view>

namespace
{
	using pbd::test::check;

	// Sets a variable for this process, or removes it when value is null.
	void put(const char* name, const char* value)
	{
#ifdef _WIN32
		_putenv_s(name, value ? value : "");
#else
		if (value)
			setenv(name, value, 1);
		else
			unsetenv(name);
#endif
	}

	bool equals(const char* actual, std::string_view expected)
	{
		return actual && std::string_view(actual) == expected;
	}

#ifdef _WIN32
	bool equals(const wchar_t* actual, std::wstring_view expected)
	{
		return actual && std::wstring_view(actual) == expected;
	}
#endif
} // namespace

PBD_TEST(PbdEnv, SetAndUnset)
{
	put("PBD_ENV_TEST_A", "on");
	check(equals(pbd::env::find("PBD_ENV_TEST_A"), "on"), "A set switch reads back its value");
	put("PBD_ENV_TEST_A", nullptr);
	check(!pbd::env::find("PBD_ENV_TEST_A"), "An unset switch reads as unset");
}

PBD_TEST(PbdEnv, NamesThatDoNotFit)
{
	const std::string name = "PBD_" + std::string(100, 'X');
	check(!pbd::env::find(name), "A name longer than the lookup buffer reads as unset");
	check(!pbd::env::find({}), "An empty name reads as unset");
}

#ifdef _WIN32
PBD_TEST(PbdEnv, WideLookup)
{
	_wputenv_s(L"PBD_ENV_TEST_D", L"C:\\Spiele\\\u00dcbung");
	check(equals(pbd::env::find_wide("PBD_ENV_TEST_D"), L"C:\\Spiele\\\u00dcbung"),
		"The wide lookup returns text outside ASCII unchanged");
	_wputenv_s(L"PBD_ENV_TEST_D", L"");
	check(!pbd::env::find_wide("PBD_ENV_TEST_D"), "The wide lookup reads a removed switch as unset");
}

PBD_TEST(PbdEnv, SettingASwitch)
{
	check(pbd::env::set_wide("PBD_ENV_TEST_E", L"new"), "Setting a PBD_ name succeeds");
	check(equals(pbd::env::find_wide("PBD_ENV_TEST_E"), L"new"), "A set switch reads back its value");
	check(equals(pbd::env::find("PBD_ENV_TEST_E"), "new"), "A set switch reads back through the narrow lookup");
	check(pbd::env::set_wide("PBD_ENV_TEST_E", L""), "Clearing a switch succeeds");
	check(!pbd::env::find_wide("PBD_ENV_TEST_E"), "A cleared switch reads as unset");
	check(!pbd::env::set_wide("ENV_TEST_E", L"x"), "Only PBD_ names can be set");
}
#endif
