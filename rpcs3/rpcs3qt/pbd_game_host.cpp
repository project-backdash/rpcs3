#include "pbd_game_host.h"
#include "pbd_game_host_policy.h"
#include "pbd_raw_keyboard.h"
#include "Emu/PBD/pbd_env.h"
#include "Emu/PBD/pbd_presentation_journal.h"
#include "Emu/PBD/pbd_held_presentation.h"
#include "Emu/PBD/pbd_output_gain.h"
#include "util/atomic.hpp"
#include "util/logs.hpp"
#include "Utilities/StrFmt.h"

#include <QCoreApplication>
#include <QFocusEvent>
#include <QJsonDocument>
#include <QJsonObject>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QWheelEvent>
#include <QWindow>

#include <algorithm>
#include <atomic>
#include <future>
#include <stdexcept>
#include <thread>
#include <utility>

#ifdef _WIN32
#include <windows.h>
#include <windowsx.h>
#endif

extern atomic_t<bool> g_game_window_focused;
LOG_CHANNEL(game_host_log, "PBD Game Host");

#include "pbd_game_host_platform.h"
#include "pbd_game_host_surface.h"
#include "pbd_game_host_window.h"

namespace pbd::game_host
{
#ifdef _WIN32

	bool window::state::io_complete(OVERLAPPED& operation, DWORD timeout)
	{
		const HANDLE waits[]{stop_event.value, launcher.value, operation.hEvent, refresh_event.value};
		const auto deadline = GetTickCount64() + timeout;
		DWORD result;
		do
		{
			const auto now = GetTickCount64();
			const auto remaining =
				timeout == INFINITE ? INFINITE : static_cast<DWORD>(deadline > now ? deadline - now : 0);
			result = WaitForMultipleObjects(4, waits, FALSE, remaining);
			if (result == WAIT_OBJECT_0 + 3)
				update_refresh();
		} while (result == WAIT_OBJECT_0 + 3);
		// Parent process exit is different from a delayed GUI; no heartbeat or healthy-load timeout kills the game.
		if (result == WAIT_OBJECT_0 + 1)
			TerminateProcess(GetCurrentProcess(), 0x50424402);
		if (result == WAIT_OBJECT_0 + 2)
			return true;
		CancelIoEx(pipe.value, &operation);
		WaitForSingleObject(operation.hEvent, INFINITE);
		return false;
	}

	bool window::state::read_message(OVERLAPPED& operation, char* bytes, DWORD capacity, DWORD& length)
	{
		ResetEvent(operation.hEvent);
		if (ReadFile(pipe.value, bytes, capacity, &length, &operation))
			return true;
		return GetLastError() == ERROR_IO_PENDING && io_complete(operation, 1000) &&
		       GetOverlappedResult(pipe.value, &operation, &length, FALSE);
	}

	void window::state::run_control(std::stop_token stop)
	{
		update_refresh();
		handle completed{CreateEventW(nullptr, TRUE, FALSE, nullptr)};
		if (!completed.value)
			return;
		while (!stop.stop_requested())
		{
			OVERLAPPED operation{};
			operation.hEvent = completed.value;
			ResetEvent(completed.value);
			const bool connected = ConnectNamedPipe(pipe.value, &operation) != FALSE;
			const auto error = connected ? ERROR_SUCCESS : GetLastError();
			if (!connected && error != ERROR_PIPE_CONNECTED &&
				(error != ERROR_IO_PENDING || !io_complete(operation, INFINITE)))
				break;
			char bytes[128]{};
			DWORD length = 0;
			if (!read_message(operation, bytes, sizeof(bytes) - 1, length))
			{
				DisconnectNamedPipe(pipe.value);
				continue;
			}
			const int action = parse_control(std::string_view(bytes, length), static_cast<std::uint32_t>(token));
			bool valid = action != 0;
			if (valid)
			{
				if (action == 2)
					update_refresh();
				if (hosted)
				{
					const auto child = render_window.load();
					valid = child && PostMessageW(child, control_message, token, action);
				}
				else if (action == 1)
					valid = PostMessageW(qt_window, WM_CLOSE, 0, 0) != FALSE;
			}
			const char* reply = valid ? "OK\n" : "ERR\n";
			DWORD written = 0;
			ResetEvent(completed.value);
			if (!WriteFile(pipe.value, reply, valid ? 3 : 4, &written, &operation) &&
				GetLastError() == ERROR_IO_PENDING)
				io_complete(operation, 1000);
			// The client confirms receipt before disconnect; unread pipe output is otherwise discarded by Windows.
			read_message(operation, bytes, sizeof(bytes) - 1, length);
			DisconnectNamedPipe(pipe.value);
		}
	}

#else
	struct window::state
	{
	};
#endif

	window::window(QWindow& game)
	{
#ifdef _WIN32
		m_state = std::make_unique<state>(game);
		if (!configured())
			pbd::output_gain::output().visible(pbd::output_gain::initially_visible(nullptr, pbd::env::find("PBD_QUIET_WINDOW")));
#else
		(void)game;
#endif
	}
	window::~window()
	{
		shutdown();
	}
	bool window::managed() const
	{
#ifdef _WIN32
		return m_state && m_state->parent;
#else
		return false;
#endif
	}
	bool window::configured() const
	{
#ifdef _WIN32
		return managed() && m_state->hosted;
#else
		return false;
#endif
	}
	bool window::attached() const
	{
		return configured() && native_handle();
	}
	bool window::focused() const
	{
#ifdef _WIN32
		return attached() && m_state->has_focus.load();
#else
		return false;
#endif
	}
	std::uintptr_t window::native_handle() const
	{
#ifdef _WIN32
		return configured() ? reinterpret_cast<std::uintptr_t>(m_state->render_window.load()) : 0;
#else
		return 0;
#endif
	}
	int window::width_px() const
	{
#ifdef _WIN32
		return configured() ? m_state->width.load() : 0;
#else
		return 0;
#endif
	}
	int window::height_px() const
	{
#ifdef _WIN32
		return configured() ? m_state->height.load() : 0;
#else
		return 0;
#endif
	}
	double window::refresh_rate() const
	{
#ifdef _WIN32
		return managed() ? m_state->refresh.load() : 0;
#else
		return 0;
#endif
	}
	bool window::visible() const
	{
#ifdef _WIN32
		if (!configured() || !m_state->visible.load())
			return false;
		if (pbd::held_presentation::output().active())
		{
			const auto child = m_state->render_window.load();
			const auto top = m_state->detached ? child : m_state->parent;
			return child && IsWindowVisible(child) && !IsIconic(top);
		}
		return true;
#else
		return false;
#endif
	}
	bool window::fullscreen() const
	{
#ifdef _WIN32
		if (!configured())
			return false;
		MONITORINFO monitor{sizeof(monitor)};
		RECT bounds{};
		const auto target = m_state->detached ? m_state->render_window.load() : m_state->parent;
		return GetWindowRect(target, &bounds) &&
		       GetMonitorInfoW(MonitorFromWindow(target, MONITOR_DEFAULTTONEAREST), &monitor) &&
		       EqualRect(&bounds, &monitor.rcMonitor);
#else
		return false;
#endif
	}
	void window::show_loading()
	{
#ifdef _WIN32
		if (configured())
			m_state->game.hide();
#endif
	}
	void window::toggle_fullscreen()
	{
#ifdef _WIN32
		if (configured())
			if (m_state->detached)
				PostMessageW(m_state->render_window.load(), control_message, m_state->token, 7);
			else
				PostMessageW(m_state->parent, host_message, m_state->token, 2);
#endif
	}
	void window::first_frame()
	{
#ifdef _WIN32
		if (configured())
		{
			ULONGLONG expected = 0;
			if (m_state->first_frame.compare_exchange_strong(expected, GetTickCount64()))
			{
				if (m_state->external_overlay)
					log_overlay_surface(m_state->render_window.load());
				if (const auto child = m_state->render_window.load())
					PostMessageW(child, WM_NULL, 0, 0);
			}
		}
#endif
	}
	void window::shutdown()
	{
		// Separate the input queues before a healthy shutdown can block while joining emulation threads.
#ifdef _WIN32
		if (!managed() || m_state->closing.exchange(true))
			return;
		const bool return_grant = m_state->hosted && m_state->has_focus.load() && foreground_within(m_state->detached ? m_state->render_window.load() : m_state->parent);
		if (return_grant)
			AllowSetForegroundWindow(m_state->launcher_pid);
		// A game ending itself still covers the page until the launcher has painted its return frame. Older launchers
		// do not opt in; a gone or unresponsive launcher never holds the close beyond this short paint budget.
		if (m_state->painted_return && m_state->hosted && (!m_state->detached || m_state->external_overlay) && m_state->visible.load())
		{
			PostMessageW(m_state->parent, host_message, m_state->token, (static_cast<LPARAM>(GetCurrentProcessId()) << 8) | 8);
			const HANDLE waits[]{m_state->page_ready.value, m_state->launcher.value};
			WaitForMultipleObjects(2, waits, FALSE, 350);
		}
		m_state->return_foreground(return_grant);
		if (const auto child = m_state->render_window.load())
			PostMessageW(child, control_message, m_state->token, 3);
#endif
	}
	bool window::event(QEvent* event)
	{
#ifdef _WIN32
		if (managed() && (event->type() == QEvent::ScreenChangeInternal || event->type() == QEvent::Move))
			SetEvent(m_state->refresh_event.value);
		if (configured() && event->type() == geometry_event)
		{
			m_state->game.resize(static_cast<geometry_change*>(event)->size);
			return true;
		}
#endif
		if (!configured() || (event->type() != QEvent::KeyPress && event->type() != QEvent::KeyRelease))
			return false;
		const auto key = static_cast<QKeyEvent*>(event);
		if (!fullscreen_key(key->key() == Qt::Key_F11, key->modifiers() != Qt::NoModifier))
			return false;
		if (event->type() == QEvent::KeyPress && !key->isAutoRepeat())
			toggle_fullscreen();
		event->accept();
		return true;
	}
} // namespace pbd::game_host
