#pragma once

#ifdef _WIN32
#include <windows.h>

namespace pbd::game_host
{
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
			(GetWindowLongPtrW(window, GWL_EXSTYLE) & ~WS_EX_APPWINDOW) | WS_EX_TOOLWINDOW);
		return previous || !GetLastError();
	}

	inline bool launcher_window_exists(HWND window, DWORD pid)
	{
		DWORD actual = 0;
		return GetWindowThreadProcessId(window, &actual) && actual == pid;
	}
} // namespace pbd::game_host
#endif
