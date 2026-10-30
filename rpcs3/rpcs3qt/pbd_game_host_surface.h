#pragma once

// Placement for a render window that owns its foreground root while retaining the launcher handoff.
namespace pbd::game_host
{
#ifdef _WIN32
	inline RECT place_overlay_surface(HWND surface, HWND launcher)
	{
		RECT bounds{};
		GetClientRect(launcher, &bounds);
		POINT origin{};
		ClientToScreen(launcher, &origin);
		const auto width = std::max(bounds.right, 1L), height = std::max(bounds.bottom, 1L);
		SetWindowPos(surface, nullptr, origin.x, origin.y, width, height, SWP_NOACTIVATE | SWP_NOZORDER);
		return {origin.x, origin.y, origin.x + std::min(width, 1280L), origin.y + std::min(height, 720L)};
	}

	inline void log_overlay_surface(HWND surface)
	{
		DWORD render_pid = 0, root_pid = 0;
		GetWindowThreadProcessId(surface, &render_pid);
		GetWindowThreadProcessId(GetAncestor(surface, GA_ROOT), &root_pid);
		game_host_log.notice("Steam launch: process %u, render owner %u, root owner %u, overlay renderer DLL %s",
			GetCurrentProcessId(), render_pid, root_pid, GetModuleHandleW(L"gameoverlayrenderer64.dll") ? "loaded" : "not loaded");
	}
#endif
} // namespace pbd::game_host
