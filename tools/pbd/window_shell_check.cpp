#include "../../rpcs3/rpcs3qt/pbd_game_host_shell.h"

#include <dwmapi.h>
#include <iostream>
#include <stdexcept>
#include <string>

namespace
{
	void require(bool value, const char* message)
	{
		if (!value)
			throw std::runtime_error(message);
	}

	std::wstring desktop_name(HDESK desktop)
	{
		wchar_t name[256]{};
		require(GetUserObjectInformationW(desktop, UOI_NAME, name, sizeof(name), nullptr) != FALSE, "desktop name");
		return name;
	}

	bool eligible(HWND window)
	{
		if (!IsWindowVisible(window) || (GetWindowLongPtrW(window, GWL_STYLE) & WS_CHILD))
			return false;
		const auto style = GetWindowLongPtrW(window, GWL_EXSTYLE);
		if (style & WS_EX_TOOLWINDOW)
			return false;
		if ((style & WS_EX_NOACTIVATE) && !(style & WS_EX_APPWINDOW))
			return false;
		DWORD cloaked = 0;
		if (SUCCEEDED(DwmGetWindowAttribute(window, DWMWA_CLOAKED, &cloaked, sizeof(cloaked))) && (cloaked & ~DWM_CLOAKED_SHELL))
			return false;
		if (style & WS_EX_APPWINDOW)
			return true;
		auto walk = GetAncestor(window, GA_ROOTOWNER);
		for (int i = 0; i < 64; ++i)
		{
			const auto popup = GetLastActivePopup(walk);
			if (popup == walk || !popup)
				break;
			if (IsWindowVisible(popup) && !(GetWindowLongPtrW(popup, GWL_EXSTYLE) & WS_EX_TOOLWINDOW))
			{
				walk = popup;
				break;
			}
			if (GetWindowLongPtrW(popup, GWL_EXSTYLE) & WS_EX_TOOLWINDOW)
				break;
			walk = popup;
		}
		return walk == window;
	}

	int count()
	{
		MSG message{};
		for (int i = 0; i < 64 && PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE); ++i)
		{
			TranslateMessage(&message);
			DispatchMessageW(&message);
		}
		int result = 0;
		require(EnumWindows([](HWND window, LPARAM context) -> BOOL
					{
						DWORD pid = 0;
						GetWindowThreadProcessId(window, &pid);
						if (pid == GetCurrentProcessId() && eligible(window))
						{
							++*reinterpret_cast<int*>(context);
						}
						return TRUE;
					},
					reinterpret_cast<LPARAM>(&result)) != FALSE,
			"EnumWindows");
		return result;
	}

	struct owned_window
	{
		HWND value;
		~owned_window()
		{
			if (IsWindow(value))
				DestroyWindow(value);
		}
	};

	HWND make(DWORD ex_style, DWORD style, HWND owner = nullptr)
	{
		const auto window = CreateWindowExW(ex_style | WS_EX_NOACTIVATE, L"PbdShellCheck", L"PROJECT BACKDASH", style,
			10, 10, 320, 180, owner, nullptr, GetModuleHandleW(nullptr), nullptr);
		require(window != nullptr, "CreateWindowExW");
		return window;
	}
} // namespace

int main()
{
	try
	{
		SetPriorityClass(GetCurrentProcess(), BELOW_NORMAL_PRIORITY_CLASS);
		const auto input = OpenInputDesktop(0, FALSE, DESKTOP_READOBJECTS);
		require(input != nullptr, "input desktop");
		const auto input_name = desktop_name(input);
		CloseDesktop(input);
		require(desktop_name(GetThreadDesktop(GetCurrentThreadId())) != input_name, "requires an inactive desktop");
		using namespace pbd::game_host;
		WNDCLASSW type{};
		type.lpfnWndProc = DefWindowProcW;
		type.hInstance = GetModuleHandleW(nullptr);
		type.lpszClassName = L"PbdShellCheck";
		require(RegisterClassW(&type) != 0, "register window class");
		for (int cycle = 0; cycle < 30; ++cycle)
		{
			owned_window launcher{make(WS_EX_APPWINDOW, WS_OVERLAPPEDWINDOW | WS_VISIBLE)};
			ShowWindow(launcher.value, SW_SHOWNOACTIVATE);
			require(launcher_window_exists(launcher.value, GetCurrentProcessId()), "live owner");
			require(!launcher_window_exists(launcher.value, GetCurrentProcessId() + 1), "wrong owner process");
			require(count() == 1, "launcher alone");
			owned_window qt{make(WS_EX_APPWINDOW, WS_POPUP)};
			require(exclude_from_shell(qt.value), "exclude hidden input window");
			owned_window child{make(surface_ex_style(false), WS_CHILD | WS_VISIBLE, launcher.value)};
			owned_window popup{make(surface_ex_style(false), WS_POPUP, launcher.value)};
			for (int handoff = 0; handoff < 3; ++handoff)
			{
				ShowWindow(popup.value, SW_SHOWNOACTIVATE);
				ShowWindow(qt.value, SW_SHOWNOACTIVATE);
				require(count() == 1, "adopted game and input helper stay out of Alt-Tab");
				ShowWindow(popup.value, SW_HIDE);
				ShowWindow(qt.value, SW_HIDE);
				require(count() == 1, "return to search");
			}
			// The same render HWND survives promotion from a child to an owned popup.
			ShowWindow(child.value, SW_HIDE);
			SetParent(child.value, nullptr);
			SetWindowLongPtrW(child.value, GWL_STYLE, WS_POPUP);
			SetWindowLongPtrW(child.value, GWL_EXSTYLE, surface_ex_style(false) | WS_EX_NOACTIVATE);
			SetWindowLongPtrW(child.value, GWLP_HWNDPARENT, reinterpret_cast<LONG_PTR>(launcher.value));
			ShowWindow(child.value, SW_SHOWNOACTIVATE);
			require(count() == 1, "promoted surface stays out of Alt-Tab");
			owned_window external{make(surface_ex_style(true), WS_POPUP | WS_VISIBLE)};
			require(count() == 2, "intentional external game has its own entry");
			ShowWindow(external.value, SW_HIDE);
			require(count() == 1, "parked external game stays out of Alt-Tab");
			require(DestroyWindow(launcher.value) != FALSE, "destroy owner");
			require(!IsWindow(child.value) && !IsWindow(popup.value), "owned surfaces die with owner");
			require(!launcher_window_exists(launcher.value, GetCurrentProcessId()), "dead owner");
			require(count() == 0, "no eligible windows after owner closes");
		}
		std::cout << "30 cycles, 90 handoffs: one launcher entry; external game only while shown; owner teardown passed\n";
		return 0;
	}
	catch (const std::exception& error)
	{
		std::cerr << error.what() << '\n';
		return 1;
	}
}
