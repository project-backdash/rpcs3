#pragma once

#include "pbd_game_host_policy.h"
#include "window_geometry.h"

#ifdef _WIN32
#include <windows.h>
#include <dwmapi.h>

namespace pbd::game_host
{
	struct visibility_observation
	{
		window_visibility state;
		HWND root = nullptr;
		RECT bounds{}, clipped{};
		HMONITOR monitor = nullptr;
		MONITORINFO screen{sizeof(screen)};
	};

	inline bool client_screen_rect(HWND window, RECT& bounds)
	{
		POINT origin{};
		if (!GetClientRect(window, &bounds) || !ClientToScreen(window, &origin))
			return false;
		OffsetRect(&bounds, origin.x, origin.y);
		return true;
	}

	// `launcher_owner`: the launcher, for a game in its own window owned by it; the launcher in front counts as the game in front.
	inline visibility_observation observe_window_visibility(HWND window, HWND expected_root, HWND launcher_owner = nullptr)
	{
		visibility_observation result;
		DWORD owner = 0;
		GetWindowThreadProcessId(window, &owner);
		result.root = GetAncestor(window, GA_ROOT);
		result.state.exists = window && IsWindow(window) && owner == GetCurrentProcessId() && expected_root && result.root == expected_root;
		if (!result.state.exists)
			return result;

		result.state.visible = IsWindowVisible(window) != FALSE;
		GetWindowRect(window, &result.bounds);
		bool clipped = client_screen_rect(window, result.clipped);
		for (HWND ancestor = window; ancestor; ancestor = GetParent(ancestor))
		{
			result.state.minimized = result.state.minimized || IsIconic(ancestor);
			if (ancestor != window)
			{
				RECT client{};
				clipped = clipped && client_screen_rect(ancestor, client) && IntersectRect(&result.clipped, &result.clipped, &client);
			}
			if (ancestor == result.root)
				break;
		}
		if (clipped && !IsRectEmpty(&result.clipped))
			result.monitor = MonitorFromRect(&result.clipped, MONITOR_DEFAULTTONULL);
		result.state.on_screen = result.monitor && GetMonitorInfoW(result.monitor, &result.screen);
		const auto foreground = GetForegroundWindow();
		const auto foreground_root = foreground ? GetAncestor(foreground, GA_ROOT) : nullptr;
		result.state.foreground = foreground_root && (foreground_root == result.root || foreground_root == launcher_owner);

		// An unknown cloak state never verifies visibility.
		using get_attribute = HRESULT(WINAPI*)(HWND, DWORD, PVOID, DWORD);
		static const auto query = []() -> get_attribute
		{
			const auto module = LoadLibraryExW(L"dwmapi.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
			return module ? reinterpret_cast<get_attribute>(GetProcAddress(module, "DwmGetWindowAttribute")) : nullptr;
		}();
		DWORD cloaked = 0;
		result.state.cloak_known = query && SUCCEEDED(query(result.root, DWMWA_CLOAKED, &cloaked, sizeof(cloaked)));
		result.state.cloaked = cloaked != 0;
		return result;
	}

	// Each display with the graphics adapter that drives it, as [DISPLAY1|primary|left,top,right,bottom|adapter] entries in
	// physical pixels. A game drawn by the adapter that drives its screen is presented differently from one drawn by another
	// adapter and copied across, so reports need both.
	inline std::string display_adapters()
	{
		std::string text;
		const auto each = [](HMONITOR monitor, HDC, LPRECT, LPARAM data) -> BOOL
		{
			MONITORINFOEXW info{};
			info.cbSize = sizeof(info);
			if (!GetMonitorInfoW(monitor, &info))
				return TRUE;
			std::wstring adapter;
			DISPLAY_DEVICEW device{};
			device.cb = sizeof(device);
			for (DWORD index = 0; EnumDisplayDevicesW(nullptr, index, &device, 0); ++index, device.cb = sizeof(device))
			{
				if (std::wstring_view(device.DeviceName) == info.szDevice)
				{
					adapter = device.DeviceString;
					break;
				}
			}
			std::wstring_view name(info.szDevice);
			if (name.starts_with(L"\\\\.\\"))
				name.remove_prefix(4);
			const auto& r = info.rcMonitor;
			auto& out = *reinterpret_cast<std::string*>(data);
			out += (out.empty() ? "[" : " [") + printable_ascii(name) + '|' + ((info.dwFlags & MONITORINFOF_PRIMARY) ? "primary" : "") + '|' +
			       std::to_string(r.left) + ',' + std::to_string(r.top) + ',' + std::to_string(r.right) + ',' + std::to_string(r.bottom) +
			       '|' + printable_ascii(adapter) + ']';
			return TRUE;
		};
		EnumDisplayMonitors(nullptr, nullptr, each, reinterpret_cast<LPARAM>(&text));
		return text;
	}

	struct window_recovery
	{
		bool restored = false;
		bool positioned = false;
		bool foreground = false;
		gui::utils::window_geometry target{};
	};

	// Puts a window back where the player can see it (plan_window_repair): a minimized or hidden one is restored, to its
	// maximized or full screen state when it had one, and only one that no monitor shows is centred on the primary monitor.
	inline window_recovery recover_window_position(HWND root)
	{
		window_recovery result;
		MONITORINFO monitor{sizeof(monitor)};
		RECT bounds{};
		if (!IsWindow(root) || !GetWindowRect(root, &bounds) ||
			!GetMonitorInfoW(MonitorFromPoint(POINT{}, MONITOR_DEFAULTTOPRIMARY), &monitor))
			return result;
		const bool minimized = IsIconic(root) != FALSE;
		bool maximized = IsZoomed(root) != FALSE;
		if (minimized)
		{
			WINDOWPLACEMENT placement{sizeof(placement)};
			if (GetWindowPlacement(root, &placement))
			{
				bounds = placement.rcNormalPosition;
				maximized = (placement.flags & WPF_RESTORETOMAXIMIZED) != 0;
			}
		}
		MONITORINFO own{sizeof(own)};
		const auto shown_on = MonitorFromRect(&bounds, MONITOR_DEFAULTTONULL);
		const bool full_screen = shown_on && GetMonitorInfoW(shown_on, &own) && EqualRect(&bounds, &own.rcMonitor);
		const auto plan = plan_window_repair(minimized, !IsWindowVisible(root), shown_on != nullptr, maximized, full_screen);
		const gui::utils::window_geometry work{monitor.rcWork.left, monitor.rcWork.top,
			monitor.rcWork.right - monitor.rcWork.left, monitor.rcWork.bottom - monitor.rcWork.top};
		result.target = plan.reposition ?
		                    gui::utils::centered_window_geometry(work, work, bounds.right - bounds.left, bounds.bottom - bounds.top) :
		                    gui::utils::window_geometry{bounds.left, bounds.top, bounds.right - bounds.left, bounds.bottom - bounds.top};
		// The launcher may be on a different thread. Never wait for its message loop during recovery.
		if (plan.restore)
		{
			if (GetWindowThreadProcessId(root, nullptr) == GetCurrentThreadId())
			{
				ShowWindow(root, SW_RESTORE);
				result.restored = true;
			}
			else
				result.restored = ShowWindowAsync(root, SW_RESTORE) != FALSE;
		}
		if (plan.reposition)
			result.positioned = SetWindowPos(root, HWND_NOTOPMOST, result.target.x, result.target.y,
									result.target.width, result.target.height, SWP_ASYNCWINDOWPOS | SWP_SHOWWINDOW | SWP_NOACTIVATE) != FALSE;
		result.foreground = SetForegroundWindow(root) != FALSE;
		return result;
	}
} // namespace pbd::game_host
#endif
