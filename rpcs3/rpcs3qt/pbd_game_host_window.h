#pragma once

// The owned Windows game surface and its message handling. Included by pbd_game_host.cpp.
namespace pbd::game_host
{
#ifdef _WIN32
	struct window::state
	{
		QWindow& game;
		HWND parent = nullptr, qt_window = nullptr;
		std::atomic<HWND> render_window{nullptr};
		DWORD launcher_pid = 0;
		WPARAM token = 0;
		handle launcher, stop_event, refresh_event, page_ready, pipe;
		std::jthread window_thread, control_thread;
		std::atomic<bool> has_focus{false}, visible{false}, closing{false};
		std::atomic<int> width{160}, height{90};
		std::atomic<double> refresh{60};
		std::atomic<ULONGLONG> first_frame{0};
		bool show_requested = false;
		// The surface refresh (see refresh_surface): when it is due, and whether the window is shrunk by a pixel now.
		ULONGLONG refresh_due = 0;
		bool refresh_shrunk = false, refreshed_once = false;
		std::atomic<bool> detached{false};
		RECT normal_bounds{};
		bool hosted = false, painted_return = false, external_overlay = false;
		inline static thread_local state* event_owner = nullptr;
		// The keyboard, read on a thread of its own (pbd_raw_keyboard.h).
		std::shared_ptr<raw_keyboard::sink> raw_keys;

		explicit state(QWindow& target) : game(target)
		{
			parent = read_contract(launcher_pid, token, hosted, painted_return, external_overlay);
			if (!parent)
				return;
			qt_window = reinterpret_cast<HWND>(game.winId());
			launcher.value = OpenProcess(SYNCHRONIZE, FALSE, launcher_pid);
			stop_event.value = CreateEventW(nullptr, TRUE, FALSE, nullptr);
			refresh_event.value = CreateEventW(nullptr, FALSE, FALSE, nullptr);
			page_ready.value = CreateEventW(nullptr, TRUE, FALSE, nullptr);
			const auto name = L"\\\\.\\pipe\\PBD-game-control-" + std::to_wstring(GetCurrentProcessId());
			pipe.value = CreateNamedPipeW(name.c_str(),
				PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED | FILE_FLAG_FIRST_PIPE_INSTANCE,
				PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS, 1, 128, 128, 0, nullptr);
			if (!launcher.value || !stop_event.value || !refresh_event.value || !page_ready.value || pipe.value == INVALID_HANDLE_VALUE)
				throw std::runtime_error("The game could not start safely. Restart the launcher.");
			if (hosted)
			{
				// Only this small message thread joins the launcher queue; Qt, rendering and disk work stay on other
				// threads.
				std::promise<HWND> ready;
				auto result = ready.get_future();
				window_thread = std::jthread(
					[this, ready = std::move(ready)](std::stop_token stop) mutable
					{
						run_window(ready, stop);
					});
				if (!result.get())
					throw std::runtime_error("The game could not join the launcher window.");
				// An external overlay must receive the window's keyboard messages before the game does.
				if (!external_overlay)
					start_raw_keys();
			}
			// For the run summary: where the game is drawn and how its keys arrive.
			pbd::presentation::journal::get().hosted(hosted && !external_overlay);
			pbd::presentation::journal::get().keyboard(raw_keys_active());
			control_thread = std::jthread(
				[this](std::stop_token stop)
				{
					run_control(stop);
				});
			if (!hosted)
				PostMessageW(parent, host_message, token, (static_cast<LPARAM>(GetCurrentProcessId()) << 8) | 5);
		}
		~state()
		{
			if (!parent)
				return;
			closing.store(true);
			SetEvent(stop_event.value);
			control_thread.request_stop();
			if (control_thread.joinable())
				control_thread.join();
			window_thread.request_stop();
			if (window_thread.joinable())
				window_thread.join();
			// After the window thread, whose focus changes release keys through it.
			if (raw_keys)
				raw_keys->route({}, {}, {});
			raw_keys.reset();
		}
		void post(QEvent* event)
		{
			if (!closing.load())
				QCoreApplication::postEvent(&game, event);
			else
				delete event;
		}
		void focus(bool value)
		{
			if (has_focus.exchange(value) == value)
				return;
			if (!value && raw_keys)
				raw_keys->release();
			g_game_window_focused = value;
			post(new QFocusEvent(value ? QEvent::FocusIn : QEvent::FocusOut));
		}
		static void CALLBACK foreground_event(HWINEVENTHOOK, DWORD, HWND, LONG, LONG, DWORD, DWORD)
		{
			if (event_owner)
			{
				const auto child = event_owner->render_window.load();
				const auto root = event_owner->detached ? child : event_owner->parent;
				// Restore the active game's keyboard target only on foreground transitions, never by polling.
				if (child && event_owner->visible.load() && !event_owner->closing.load() && foreground_within(root) && GetFocus() != child)
					SetFocus(child);
				event_owner->focus(child && foreground_within(root) && GetFocus() == child);
			}
		}
		void publish_geometry(HWND child)
		{
			RECT bounds{};
			if (!GetClientRect(child, &bounds))
				return;
			const auto dpi = std::max(GetDpiForWindow(child), 96u);
			width.store(std::max(bounds.right, 1L));
			height.store(std::max(bounds.bottom, 1L));
			post(new geometry_change(QSize(MulDiv(width.load(), 96, dpi), MulDiv(height.load(), 96, dpi))));
		}
		void resize(HWND child)
		{
			if (detached)
			{
				publish_geometry(child);
				return;
			}
			RECT bounds{};
			if (!GetClientRect(parent, &bounds))
				return;
			SetWindowPos(child, HWND_TOP, 0, 0, std::max(bounds.right, 1L), std::max(bounds.bottom, 1L), SWP_NOACTIVATE);
			publish_geometry(child);
		}
		void raise(HWND child)
		{
			SetWindowPos(child, HWND_TOP, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
		}
		// The launcher's child windows in order, top first, for the log: what could be drawn over the game.
		void log_layers(HWND child, const char* when)
		{
			std::string layers;
			int count = 0;
			for (HWND w = GetWindow(parent, GW_CHILD); w && count < 12; w = GetWindow(w, GW_HWNDNEXT), ++count)
			{
				wchar_t name[64]{};
				GetClassNameW(w, name, 63);
				DWORD pid = 0;
				GetWindowThreadProcessId(w, &pid);
				RECT r{};
				GetWindowRect(w, &r);
				std::string cls;
				for (const wchar_t* c = name; *c; ++c)
					cls += *c < 128 ? static_cast<char>(*c) : '?';
				layers += fmt::format("%s%s[%s pid %u %s ex 0x%x %dx%d]", count ? " " : "", w == child ? "*" : "", cls, pid,
					IsWindowVisible(w) ? "shown" : "hidden", static_cast<u32>(GetWindowLongPtrW(w, GWL_EXSTYLE)), static_cast<int>(r.right - r.left), static_cast<int>(r.bottom - r.top));
			}
			game_host_log.notice("Hosted layers %s (top first, * = game): %s", when, layers);
		}
		// A surface made before the hosted window had its final size and visibility can present nowhere on some PCs: the
		// game ran and played sound while the launcher showed, until a display mode change resized the window. Shrinking the
		// window by a pixel for a moment rebuilds the surface the same way, once after the first frame and after every show.
		void refresh_surface(HWND child, ULONGLONG now)
		{
			if (!refresh_due || now < refresh_due)
				return;
			RECT bounds{};
			if (!refresh_shrunk && GetClientRect(parent, &bounds) && bounds.bottom > 2)
			{
				SetWindowPos(child, HWND_TOP, 0, 0, std::max(bounds.right, 1L), bounds.bottom - 1, SWP_NOACTIVATE);
				publish_geometry(child);
				refresh_shrunk = true;
				refresh_due = now + 120;
				return;
			}
			resize(child);
			refresh_shrunk = false;
			refresh_due = 0;
			refreshed_once = true;
			log_layers(child, "after the surface refresh");
		}
		void fallback(HWND child)
		{
			// Keep the render HWND and its Vulkan surface alive while removing Chromium's covering layers.
			const bool owned_focus = has_focus.load() && foreground_within(parent);
			RECT bounds{};
			GetWindowRect(child, &bounds);
			SetLastError(0);
			if (!SetParent(child, nullptr) && GetLastError())
			{
				game_host_log.error("Standalone fallback could not detach the render window: %u", GetLastError());
				return;
			}
			detached.store(true);
			pbd::presentation::journal::get().hosted(false);
			normal_bounds = bounds;
			MONITORINFO monitor{sizeof(monitor)};
			if (GetMonitorInfoW(MonitorFromWindow(child, MONITOR_DEFAULTTONEAREST), &monitor) && EqualRect(&bounds, &monitor.rcMonitor))
			{
				const LONG width = std::min(1280L, monitor.rcWork.right - monitor.rcWork.left);
				const LONG height = std::min(720L, monitor.rcWork.bottom - monitor.rcWork.top);
				normal_bounds = {monitor.rcWork.left, monitor.rcWork.top, monitor.rcWork.left + width, monitor.rcWork.top + height};
			}
			SetWindowLongPtrW(child, GWL_STYLE, WS_POPUP | WS_VISIBLE | WS_CLIPSIBLINGS);
			SetWindowLongPtrW(child, GWL_EXSTYLE, WS_EX_APPWINDOW);
			SetWindowPos(child, HWND_TOP, bounds.left, bounds.top, bounds.right - bounds.left, bounds.bottom - bounds.top,
				SWP_FRAMECHANGED | SWP_SHOWWINDOW | SWP_NOACTIVATE);
			if (owned_focus)
			{
				SetForegroundWindow(child);
				SetFocus(child);
			}
			PostMessageW(parent, host_message, token, (static_cast<LPARAM>(GetCurrentProcessId()) << 8) | 5);
			game_host_log.error("Hosted frame remained below launcher layers for 2000 ms; using the standalone render window.");
		}
		void update_refresh()
		{
			MONITORINFOEXW monitor{};
			monitor.cbSize = sizeof(monitor);
			DEVMODEW mode{};
			mode.dmSize = sizeof(mode);
			if (GetMonitorInfoW(MonitorFromWindow(hosted ? render_window.load() : qt_window, MONITOR_DEFAULTTONEAREST),
					reinterpret_cast<MONITORINFO*>(&monitor)) &&
				EnumDisplaySettingsW(monitor.szDevice, ENUM_CURRENT_SETTINGS, &mode) && mode.dmDisplayFrequency > 1)
				refresh.store(mode.dmDisplayFrequency);
		}
		void return_foreground(bool already_granted = false)
		{
			// The standalone window owns focus directly; its hosted-child focus flag is never set.
			const bool grant = already_granted || (hosted ? has_focus.load() && foreground_within(detached ? render_window.load() : parent) : foreground_within(qt_window));
			if (grant)
				AllowSetForegroundWindow(launcher_pid);
			PostMessageW(
				parent, host_message, token, (static_cast<LPARAM>(GetCurrentProcessId()) << 8) | (grant ? 3 : 6));
		}
		void pointer(HWND child, UINT message, WPARAM key, LPARAM value)
		{
			POINT point{GET_X_LPARAM(value), GET_Y_LPARAM(value)};
			if (message == WM_MOUSEWHEEL)
				ScreenToClient(child, &point);
			const double scale = std::max(GetDpiForWindow(child), 96u) / 96.;
			const QPointF local(point.x / scale, point.y / scale);
			POINT global = point;
			ClientToScreen(child, &global);
			const QPointF screen(global.x / scale, global.y / scale);
			Qt::MouseButtons buttons{};
			if (key & MK_LBUTTON)
				buttons |= Qt::LeftButton;
			if (key & MK_RBUTTON)
				buttons |= Qt::RightButton;
			if (key & MK_MBUTTON)
				buttons |= Qt::MiddleButton;
			const auto button = (message == WM_LBUTTONDOWN || message == WM_LBUTTONUP || message == WM_LBUTTONDBLCLK) ?
			                        Qt::LeftButton :
			                    (message == WM_RBUTTONDOWN || message == WM_RBUTTONUP || message == WM_RBUTTONDBLCLK) ?
			                        Qt::RightButton :
			                    (message == WM_MBUTTONDOWN || message == WM_MBUTTONUP || message == WM_MBUTTONDBLCLK) ?
			                        Qt::MiddleButton :
			                        Qt::NoButton;
			if (message == WM_MOUSEWHEEL)
				post(new QWheelEvent(local, screen, QPoint(), QPoint(0, GET_WHEEL_DELTA_WPARAM(key)), buttons,
					modifiers(), Qt::NoScrollPhase, false));
			else
			{
				const auto type =
					message == WM_MOUSEMOVE ? QEvent::MouseMove :
					(message == WM_LBUTTONUP || message == WM_RBUTTONUP || message == WM_MBUTTONUP) ?
											  QEvent::MouseButtonRelease :
					(message == WM_LBUTTONDBLCLK || message == WM_RBUTTONDBLCLK || message == WM_MBUTTONDBLCLK) ?
											  QEvent::MouseButtonDblClick :
											  QEvent::MouseButtonPress;
				post(new QMouseEvent(type, local, screen, button, buttons, modifiers()));
			}
		}
		void keyboard(HWND child, UINT message, WPARAM key, LPARAM value)
		{
			const bool pressed = message == WM_KEYDOWN || message == WM_SYSKEYDOWN;
			if (fullscreen_key(key == VK_F11, modifiers() != Qt::NoModifier))
			{
				if (pressed && !(value & (1 << 30)))
					if (detached)
						command(child, 7);
					else
						PostMessageW(parent, host_message, token, 2);
				return;
			}
			if (key == VK_F4 && (value & (1 << 29)))
			{
				if (pressed)
					PostMessageW(child, control_message, token, 1);
				return;
			}
			post_key(message, key, value, modifiers());
		}
		// One key for the game as a Qt key event, from the window's messages or from the raw keyboard thread.
		void post_key(UINT message, WPARAM key, LPARAM value, Qt::KeyboardModifiers mods)
		{
			const bool pressed = message == WM_KEYDOWN || message == WM_SYSKEYDOWN;
			if ((key >= VK_NUMPAD0 && key <= VK_DIVIDE) || (key == VK_RETURN && (value & (1 << 24))))
				mods |= Qt::KeypadModifier;
			const auto scan = static_cast<quint32>((value >> 16) & 0xff) | ((value & (1 << 24)) ? 0x100 : 0);
			post(new QKeyEvent(pressed ? QEvent::KeyPress : QEvent::KeyRelease, qt_key(key, value), mods, scan,
				static_cast<quint32>(key), static_cast<quint32>(mods), QString(), pressed && (value & (1 << 30)), 1));
		}
		// The game takes keys only while its window is shown, has the keyboard focus and is in the foreground.
		void start_raw_keys()
		{
			raw_keys = raw_keyboard::shared_sink();
			raw_keys->route(
				[this](const raw_keyboard::key_message& key, const raw_keyboard::modifiers& held)
				{
					post_key(key.message, key.vkey, static_cast<LPARAM>(key.lparam), modifiers(held));
				},
				[this]()
				{
					return visible.load() && has_focus.load() && foreground_within(detached ? render_window.load() : parent);
				},
				[]()
				{
					game_host_log.warning("Another handler took the raw keyboard; the game reads keys from its window again.");
					pbd::presentation::journal::get().keyboard(false);
				});
			if (raw_keys->active())
				game_host_log.notice("The game reads the keyboard as raw input on its own thread.");
			else
				game_host_log.warning("The raw keyboard could not be registered; the game reads keys from its window.");
		}
		bool raw_keys_active() const
		{
			return raw_keys && raw_keys->active();
		}
		void command(HWND child, LPARAM value)
		{
			if (value == 2)
				resize(child);
			if (value == 3 && (!detached || external_overlay || closing.load()))
			{
				if (external_overlay && foreground_within(child))
				{
					AllowSetForegroundWindow(launcher_pid);
					SetForegroundWindow(parent);
				}
				show_requested = false;
				visible.store(false);
				pbd::output_gain::output().visible(false);
				focus(false);
				ShowWindow(child, SW_HIDE);
				SetEvent(page_ready.value);
			}
			if (value == 4 && !closing.load())
			{
				show_requested = true;
				if (!can_show_frame(first_frame.load() != 0, pbd::held_presentation::output().can_show()))
					return;
				// The launcher repeats this command on every focus, move and resize of its window. Only a real show (hidden
				// to visible) shows the window and rebuilds the surface: a rebuild on each repeat drops frames and opens the
				// one-pixel strip where the launcher shows through, which a launcher filling the same screen does constantly.
				if (external_overlay && !visible.load())
					normal_bounds = place_overlay_surface(child, parent);
				const bool shown = !visible.exchange(true);
				if (shown)
				{
					ResetEvent(page_ready.value);
					ShowWindow(child, SW_SHOWNOACTIVATE);
					pbd::output_gain::output().visible(true);
				}
				if (!detached)
					raise(child);
				if (shown && !detached && first_frame.load())
					refresh_due = GetTickCount64() + 300;
				if (shown)
					log_layers(child, "when shown");
				if (foreground_within(parent))
				{
					if (external_overlay)
						SetForegroundWindow(child);
					SetFocus(child);
				}
				if (shown)
				{
					game_host_log.notice("Hosted first frame shown; keyboard focus %s", has_focus.load() ? "game" : "background");
				}
				PostMessageW(parent, host_message, token, (static_cast<LPARAM>(GetCurrentProcessId()) << 8) | 4);
			}
			if (value == 1)
			{
				show_requested = false;
				return_foreground();
				visible.store(false);
				pbd::output_gain::output().visible(false);
				focus(false);
				ShowWindow(child, SW_HIDE);
				PostMessageW(qt_window, WM_CLOSE, 0, 0);
			}
			if (value == 7 && detached)
			{
				RECT bounds{};
				MONITORINFO monitor{sizeof(monitor)};
				if (!GetWindowRect(child, &bounds) || !GetMonitorInfoW(MonitorFromWindow(child, MONITOR_DEFAULTTONEAREST), &monitor))
					return;
				if (EqualRect(&bounds, &monitor.rcMonitor))
					bounds = normal_bounds;
				else
				{
					normal_bounds = bounds;
					bounds = monitor.rcMonitor;
				}
				SetWindowPos(child, HWND_TOP, bounds.left, bounds.top, bounds.right - bounds.left, bounds.bottom - bounds.top, SWP_NOACTIVATE);
			}
		}
		static LRESULT CALLBACK dispatch(HWND child, UINT message, WPARAM key, LPARAM value)
		{
			auto* self = reinterpret_cast<state*>(GetWindowLongPtrW(child, GWLP_USERDATA));
			if (message == WM_NCCREATE)
			{
				self = static_cast<state*>(reinterpret_cast<CREATESTRUCTW*>(value)->lpCreateParams);
				SetWindowLongPtrW(child, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
			}
			if (!self)
				return DefWindowProcW(child, message, key, value);
			switch (message)
			{
			case WM_SETFOCUS: self->focus(true); return 0;
			case WM_KILLFOCUS: self->focus(false); return 0;
			case WM_SIZE: self->publish_geometry(child); return 0;
			case WM_KEYDOWN:
			case WM_KEYUP:
			case WM_SYSKEYDOWN:
			case WM_SYSKEYUP:
			{
				// With the raw keyboard the window keeps only F11 and Alt+F4; every key message still stops here.
				if (!self->raw_keys_active() || key == VK_F11 || (key == VK_F4 && (value & (1 << 29))))
					self->keyboard(child, message, key, value);
				return 0;
			}
			case control_message:
				if (key != self->token)
					return 0;
				self->command(child, value);
				return 0;
			case WM_DPICHANGED:
			case WM_DPICHANGED_AFTERPARENT:
			case WM_DISPLAYCHANGE: self->resize(child); return 0;
			case WM_CLOSE: self->command(child, 1); return 0;
			case WM_DESTROY:
				self->render_window.store(nullptr);
				PostQuitMessage(0);
				return 0;
			default: break;
			}
			if (message == WM_LBUTTONDOWN || message == WM_RBUTTONDOWN || message == WM_MBUTTONDOWN)
			{
				SetFocus(child);
				SetCapture(child);
			}
			if ((message == WM_LBUTTONUP || message == WM_RBUTTONUP || message == WM_MBUTTONUP) &&
				!(key & (MK_LBUTTON | MK_RBUTTON | MK_MBUTTON)) && GetCapture() == child)
				ReleaseCapture();
			if (message == WM_MOUSEMOVE || message == WM_LBUTTONDOWN || message == WM_LBUTTONUP ||
				message == WM_RBUTTONDOWN || message == WM_RBUTTONUP || message == WM_MBUTTONDOWN ||
				message == WM_MBUTTONUP || message == WM_LBUTTONDBLCLK || message == WM_RBUTTONDBLCLK ||
				message == WM_MBUTTONDBLCLK || message == WM_MOUSEWHEEL)
			{
				self->pointer(child, message, key, value);
				return 0;
			}
			return DefWindowProcW(child, message, key, value);
		}
		void run_window(std::promise<HWND>& ready, std::stop_token stop)
		{
			SetThreadDpiAwarenessContext(GetWindowDpiAwarenessContext(parent));
			WNDCLASSW type{};
			type.lpfnWndProc = dispatch;
			type.hInstance = GetModuleHandleW(nullptr);
			type.lpszClassName = L"PbdHostedGame";
			type.hCursor = LoadCursorW(nullptr, IDC_ARROW);
			type.style = CS_DBLCLKS;
			RegisterClassW(&type);
			// Steam needs the render surface and foreground root to belong to this process. The same first-frame,
			// paint-before-return and private control protocol still govern when it may appear.
			detached.store(external_overlay);
			const auto child = CreateWindowExW(external_overlay ? WS_EX_APPWINDOW : 0, type.lpszClassName, L"PROJECT BACKDASH",
				(external_overlay ? WS_POPUP : WS_CHILD) | WS_CLIPSIBLINGS,
				0, 0, 160, 90, external_overlay ? nullptr : parent, nullptr, type.hInstance, this);
			render_window.store(child);
			if (!child)
			{
				ready.set_value(nullptr);
				return;
			}
			event_owner = this;
			const auto foreground = SetWinEventHook(EVENT_SYSTEM_FOREGROUND, EVENT_SYSTEM_FOREGROUND, nullptr,
				foreground_event, 0, 0, WINEVENT_OUTOFCONTEXT);
			if (!foreground)
			{
				DestroyWindow(child);
				event_owner = nullptr;
				ready.set_value(nullptr);
				return;
			}
			if (external_overlay)
			{
				normal_bounds = place_overlay_surface(child, parent);
				game_host_log.notice("Steam launch: game-owned top-level render window; keyboard uses window messages.");
			}
			resize(child);
			ready.set_value(child);
			bool announced = false;
			MSG message{};
			bool quit = false;
			while (!quit && !stop.stop_requested())
			{
				// A parked game must not claim the launcher's input or replace a visible Practice window.
				if (!announced && pbd::held_presentation::output().can_show())
				{
					PostMessageW(parent, host_message, token, (static_cast<LPARAM>(GetCurrentProcessId()) << 8) | 7);
					PostMessageW(parent, host_message, token, (static_cast<LPARAM>(GetCurrentProcessId()) << 8) | 1);
					announced = true;
				}
				if (MsgWaitForMultipleObjects(1, &stop_event.value, FALSE, 50, QS_ALLINPUT) == WAIT_OBJECT_0)
					break;
				while (!stop.stop_requested() && PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE))
				{
					if (message.message == WM_QUIT)
					{
						quit = true;
						break;
					}
					TranslateMessage(&message);
					DispatchMessageW(&message);
				}
				if (show_requested && !visible.load() && can_show_frame(first_frame.load() != 0, pbd::held_presentation::output().can_show()))
					command(child, 4);
				if (visible.load() && !pbd::held_presentation::output().can_show())
				{
					visible.store(false);
					pbd::output_gain::output().visible(false);
					focus(false);
					ShowWindow(child, SW_HIDE);
				}
				if (!detached && visible.load() && !IsIconic(parent))
				{
					if (GetWindow(parent, GW_CHILD) != child)
						raise(child);
					const auto first = first_frame.load();
					if (first && !refreshed_once && !refresh_due)
						refresh_due = first + 500;
					refresh_surface(child, GetTickCount64());
					if (first && GetTickCount64() - first >= 2000 && GetWindow(parent, GW_CHILD) != child)
						fallback(child);
				}
			}
			UnhookWinEvent(foreground);
			event_owner = nullptr;
			if (IsWindow(child))
				DestroyWindow(child);
		}
		bool io_complete(OVERLAPPED& operation, DWORD timeout);
		bool read_message(OVERLAPPED& operation, char* bytes, DWORD capacity, DWORD& length);
		void run_control(std::stop_token stop);
	};
#endif
} // namespace pbd::game_host
