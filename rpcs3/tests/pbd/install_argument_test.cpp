#include "pbd_test.h"
#include "util/pbd_install_argument.h"

#include <array>
#include <string_view>

PBD_TEST(PbdInstallArgument, PreservesUnicodeAndExistingOptionForms)
{
	using namespace std::literals;
	using pbd::test::check;
	const std::array args{L"rpcs3.exe"sv, L"--headless"sv, L"--installfw"sv, L"D:\\تجربة中文\\PS3UPDAT.PUP"sv};
	check(pbd::install_argument(args, L"--installfw") == args[3], "preserve the Windows firmware path");
	const std::array pkg{L"rpcs3.exe"sv, L"--installpkg=first.pkg"sv, L"--installpkg"sv, L"D:\\中文\\game.pkg"sv, L"--"sv, L"--installpkg=ignored"sv};
	check(pbd::install_argument(pkg, L"--installpkg") == pkg[3], "last parsed package value wins");
	check(!pbd::install_argument(args, L"--installpkg"), "unselected option remains absent");
}

PBD_TEST(PbdInstallArgument, RestoresSurvivingConfigAndBootArgumentPositions)
{
	using namespace std::literals;
	using pbd::test::check;
	// The strings here model Qt/CRT's lossy arguments. Restoration uses these indices in GetCommandLineW.
	const std::array original{L"rpcs3.exe"sv, L"-platform"sv, L"windows"sv, L"--no-gui"sv,
		L"--config"sv, L"D:\\???????\\config.yml"sv, L"--input-config"sv, L"capture-123"sv, L"D:\\???????\\EBOOT.BIN"sv};
	const std::array retained{original[0], original[3], original[4], original[5], original[6], original[7], original[8]};
	const auto indices = pbd::retained_argument_indices<std::wstring_view>(original, retained);
	check(indices.has_value(), "the surviving Qt arguments map to their original positions");
	check(*indices == std::vector<std::size_t>{0, 3, 4, 5, 6, 7, 8}, "config and positional boot paths use wide originals");
	const std::array unknown{L"rpcs3.exe"sv, L"new Qt argument"sv};
	check(!pbd::retained_argument_indices<std::wstring_view>(original, unknown), "unknown Qt rewriting preserves fallback");
	const std::array repeated{L"same"sv, L"same"sv};
	check(*pbd::retained_argument_indices<std::wstring_view>(repeated, repeated) == std::vector<std::size_t>{0, 1}, "duplicates stay ordered");
}
