#include "pbd_test.h"

#include "Emu/PBD/pbd_env.h"
#include "Emu/PBD/pbd_boot_environment.h"

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
PBD_TEST(PbdEnv, BootWitnessPreservesUnicode)
{
	const auto name = "PBD_ENV_TEST_BOOT";
	check(pbd::env::set_wide(name, L"D:\\Player's \u062a\u062c\u0631\u0628\u0629\u4e2d\u6587\\boot-witness.jsonl"));
	const std::u8string expected = u8"D:\\Player's \u062a\u062c\u0631\u0628\u0629\u4e2d\u6587\\boot-witness.jsonl";
	const auto actual = pbd::boot_request::detail::environment(name);
	check(pbd::env::set_wide(name, L""));
	check(actual == std::string(expected.begin(), expected.end()), "The boot witness uses complete UTF-8 path bytes");
	check(pbd::boot_request::detail::environment(name).empty(), "An unset boot switch stays empty");
}

PBD_TEST(PbdEnv, PathSwitchesKeepUnicodeFolders)
{
	// A folder named in the system code page (e-acute in Windows-1252) and beyond it (Arabic, Chinese), as a user folder can be.
	const auto name = "PBD_ENV_TEST_PATH";
	const std::wstring folder = L"D:\\Games\\Jos\u00e9 \u062a\u062c\u0631\u0628\u0629\u4e2d\u6587\\overlay";
	const std::u8string utf8 = u8"D:\\Games\\Jos\u00e9 \u062a\u062c\u0631\u0628\u0629\u4e2d\u6587\\overlay";
	check(pbd::env::set_wide(name, folder.c_str()));
	check(pbd::env::path(name).native() == folder, "A path switch keeps every letter of the folder");
	check(pbd::env::local_path(name).native() == folder, "A folder on this PC is used whole");
	check(pbd::env::utf8_path(name) == std::string(utf8.begin(), utf8.end()), "UTF-8 text keeps every letter");
	check(pbd::env::set_wide(name, L""));
	check(pbd::env::local_path(name).empty(), "An unset switch names no folder");
	check(pbd::env::utf8_path(name).empty(), "An unset switch has no text");
}

PBD_TEST(PbdEnv, LocalPathsRefuseSharesAndRelativePaths)
{
	const auto name = "PBD_ENV_TEST_LOCAL";
	for (const wchar_t* value : {L"\\\\server\\share\\overlay", L"//server/share/overlay", L"overlay", L"\\overlay", L"C:overlay"})
	{
		check(pbd::env::set_wide(name, value));
		check(pbd::env::local_path(name).empty(), "Only an absolute folder on this PC is used");
	}
	check(pbd::env::set_wide(name, L"D:/Games/overlay"));
	check(!pbd::env::local_path(name).empty(), "Forward slashes after a drive still name a folder on this PC");
	check(pbd::env::set_wide(name, L""));
}

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
