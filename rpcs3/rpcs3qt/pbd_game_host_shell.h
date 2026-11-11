#pragma once

#ifdef _WIN32
#include <windows.h>
#include <string_view>

namespace pbd::game_host
{
	constexpr bool qt_shell_required(const char* contract, const char* quiet)
	{
		return (contract && *contract) || (quiet && std::string_view(quiet) == "1");
	}

	constexpr DWORD qt_shell_ex_style(DWORD style)
	{
		return (style & ~WS_EX_APPWINDOW) | WS_EX_TOOLWINDOW;
	}

	// Owned render surfaces and the hidden input window never represent the app in the shell. Ownership alone can let
	// the shell choose the last active popup as the launcher's representative; the tool style excludes it explicitly.
	constexpr DWORD surface_ex_style(bool external_overlay)
	{
		return external_overlay ? WS_EX_APPWINDOW : WS_EX_TOOLWINDOW;
	}

	inline bool exclude_from_shell(HWND window)
	{
		SetLastError(0);
		const auto previous = SetWindowLongPtrW(window, GWL_EXSTYLE,
			qt_shell_ex_style(static_cast<DWORD>(GetWindowLongPtrW(window, GWL_EXSTYLE))));
		return previous || !GetLastError();
	}

	inline bool launcher_window_exists(HWND window, DWORD pid)
	{
		DWORD actual = 0;
		return GetWindowThreadProcessId(window, &actual) && actual == pid;
	}
} // namespace pbd::game_host
#endif
