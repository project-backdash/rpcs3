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
		// The surface refresh (see refresh_surface): when it is due, when it last finished, and whether the window is shrunk
		// by a pixel now.
		ULONGLONG refresh_due = 0, refresh_done = 0;
		bool refresh_shrunk = false, refreshed_once = false;
		// detached: the render window is a top-level window, not the launcher's child. A Steam launch's window has no owner
		// (external_overlay); any other is owned by the launcher and follows it (owned_popup). Windowed mode starts that way,
		// and a hosted game whose picture the on-screen check finds missing moves there.
		std::atomic<bool> detached{false}, owned_popup{false};
		RECT normal_bounds{};
		HWINEVENTHOOK follow_hook = nullptr;
		DWORD launcher_thread = 0;
		bool hosted = false, painted_return = false, external_overlay = false;
		// An owned window waits to take the keyboard back from the launcher until doing so cancels nothing (try_hand_back).
		bool hand_back_pending = false;
		// The on-screen check of the latest show (screen_check_step). The visible answer says OK only once it has passed.
		std::atomic<bool> screen_passed{false};
		bool force_hidden = false;
		struct
		{
			ULONGLONG due = 0, requested_at = 0, first_at = 0; // due 0: no check is running
			ULONGLONG started = 0;                             // the show this check is for
			std::uint64_t request = 0;                         // the frame colours asked for the next sample
			int taken = 0;
			screen_sample first{}, previous{}, launcher{};
			bool has_launcher = false;
			long long slowest_read_us = 0;
			const char* held = nullptr; // what a sample last waited for, for the log
		} check;
		inline static thread_local state* event_owner = nullptr;
		// The keyboard, read on a thread of its own (pbd_raw_keyboard.h).
		std::shared_ptr<raw_keyboard::sink> raw_keys;

		explicit state(QWindow& target) : game(target)
		{
			bool popup_surface = false;
			parent = read_contract(launcher_pid, token, hosted, painted_return, external_overlay, popup_surface, force_hidden);
			if (!parent)
				return;
			owned_popup.store(popup_surface);
			launcher_thread = GetWindowThreadProcessId(parent, nullptr);
			if (force_hidden)
				game_host_log.warning("Hardware test: every on-screen check finds the game picture missing.");
			qt_window = reinterpret_cast<HWND>(game.winId());
			if (hosted && !exclude_from_shell(qt_window))
				throw std::runtime_error("The hidden game input window could not be configured.");
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
			pbd::presentation::journal::get().hosted(hosted && !detached.load());
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
		// The player is in front of the game: its own window, or the launcher it is drawn in or owned by.
		bool game_root_foreground() const
		{
			const auto child = render_window.load();
			if (external_overlay)
				return foreground_within(child);
			if (owned_popup.load())
				return foreground_within(child) || foreground_within(parent);
			return foreground_within(parent);
		}
		static void CALLBACK foreground_event(HWINEVENTHOOK, DWORD, HWND, LONG, LONG, DWORD, DWORD)
		{
			if (event_owner)
			{
				const auto child = event_owner->render_window.load();
				const bool front = event_owner->game_root_foreground();
				// Restore the active game's keyboard target only on foreground transitions, never by polling. A child window
				// takes the focus inside the launcher's active window; an owned window must take the activation itself, which
				// waits until it cancels nothing (try_hand_back).
				if (child && event_owner->visible.load() && !event_owner->closing.load() && front && GetFocus() != child)
				{
					if (event_owner->owned_popup.load() && event_owner->detached.load())
					{
						event_owner->hand_back_pending = true;
						event_owner->try_hand_back(child);
					}
					else
						SetFocus(child);
				}
				event_owner->focus(child && front && GetFocus() == child);
			}
		}
		// An owned window follows each move and resize of the launcher as it happens, not only when the launcher asks.
		static void CALLBACK launcher_moved(HWINEVENTHOOK, DWORD, HWND moved, LONG object, LONG item, DWORD, DWORD)
		{
			if (event_owner && moved == event_owner->parent && object == OBJID_WINDOW && item == CHILDID_SELF && event_owner->owned_popup.load())
				if (const auto child = event_owner->render_window.load())
					event_owner->place_popup(child);
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
		// An owned window covers the launcher's client area exactly, wherever the launcher is. A minimized launcher hides
		// its owned windows itself, so its parked position is never followed. The window's own rectangle is compared, not
		// the last one set: Windows moves windows itself on display and scale changes.
		void place_popup(HWND child)
		{
			RECT client{}, actual{};
			POINT origin{};
			if (IsIconic(parent) || !GetClientRect(parent, &client) || !ClientToScreen(parent, &origin))
				return;
			const RECT target{origin.x, origin.y, origin.x + std::max(client.right, 1L), origin.y + std::max(client.bottom, 1L)};
			if (GetWindowRect(child, &actual) && EqualRect(&target, &actual))
				return;
			normal_bounds = place_overlay_surface(child, parent);
		}
		// The launcher's thread is moving or sizing its window, or shows a menu: activating another window would end that.
		bool launcher_busy() const
		{
			GUITHREADINFO info{};
			info.cbSize = sizeof(info);
			return GetGUIThreadInfo(launcher_thread, &info) &&
			       (info.flags & (GUI_INMOVESIZE | GUI_INMENUMODE | GUI_POPUPMENUMODE | GUI_SYSTEMMENUMODE));
		}
		// Physical buttons, whichever is primary: a press on the launcher's frame can be the start of a drag.
		static bool mouse_button_down()
		{
			return ((GetAsyncKeyState(VK_LBUTTON) | GetAsyncKeyState(VK_RBUTTON) | GetAsyncKeyState(VK_MBUTTON)) & 0x8000) != 0;
		}
		// An owned window takes the keyboard back from its launcher once that cancels nothing; called on each pass of the
		// window thread's loop while it waits.
		void try_hand_back(HWND child)
		{
			if (!hand_back_pending)
				return;
			if (!visible.load() || closing.load() || !detached.load() || !owned_popup.load())
			{
				hand_back_pending = false;
				return;
			}
			const bool game_in_front = foreground_within(child);
			if (game_in_front && GetFocus() != child)
				SetFocus(child);
			switch (plan_hand_back(game_in_front, foreground_within(parent), launcher_busy(), mouse_button_down()))
			{
			case hand_back::none: hand_back_pending = false; break;
			case hand_back::later: break;
			case hand_back::now:
				hand_back_pending = false;
				SetForegroundWindow(child);
				SetFocus(child);
				break;
			}
		}
		void follow_launcher(HWND child)
		{
			place_popup(child);
			if (!follow_hook)
				follow_hook = SetWinEventHook(EVENT_OBJECT_LOCATIONCHANGE, EVENT_OBJECT_LOCATIONCHANGE, nullptr, launcher_moved,
					launcher_pid, GetWindowThreadProcessId(parent, nullptr), WINEVENT_OUTOFCONTEXT);
		}
		void resize(HWND child)
		{
			if (detached)
			{
				if (owned_popup.load())
					place_popup(child);
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
				layers += fmt::format("%s%s[%s pid %u %s ex 0x%x %dx%d]", count ? " " : "", w == child ? "*" : "", printable_ascii(name), pid,
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
			refresh_done = now;
			refreshed_once = true;
			log_layers(child, "after the surface refresh");
		}
		// Moves the render window out of the launcher's child list into a top-level window the launcher owns. Owned, it stays
		// above the launcher, follows it and has no taskbar button, so it still looks like one window; and no layer of the
		// launcher's page can cover a top-level window. The render HWND and its Vulkan surface stay the same.
		bool make_owned_popup(HWND child, std::string_view reason)
		{
			const bool shown = visible.load();
			const bool owned_focus = has_focus.load() && foreground_within(parent);
			// Hidden while it changes hands, so no window without an owner is ever shown, not even for a moment: it would
			// flash a taskbar button.
			if (shown)
				ShowWindow(child, SW_HIDE);
			SetLastError(0);
			if (!SetParent(child, nullptr) && GetLastError())
			{
				game_host_log.error("The game window could not leave the launcher window: %u", GetLastError());
				if (shown)
					ShowWindow(child, SW_SHOWNOACTIVATE);
				return false;
			}
			SetWindowLongPtrW(child, GWL_STYLE, WS_POPUP | WS_CLIPSIBLINGS);
			SetWindowLongPtrW(child, GWL_EXSTYLE, surface_ex_style(false));
			// On a top-level window this sets the owner.
			SetWindowLongPtrW(child, GWLP_HWNDPARENT, reinterpret_cast<LONG_PTR>(parent));
			detached.store(true);
			owned_popup.store(true);
			// The surface refresh runs only in the launcher window: one under way now would hold every sample of the check
			// from here on. Following the launcher gives the window its full size back.
			refresh_due = 0;
			refresh_shrunk = false;
			pbd::presentation::journal::get().hosted(false);
			follow_launcher(child);
			SetWindowPos(child, HWND_TOP, 0, 0, 0, 0,
				SWP_NOMOVE | SWP_NOSIZE | SWP_FRAMECHANGED | SWP_NOACTIVATE | (shown ? SWP_SHOWWINDOW : 0));
			publish_geometry(child);
			if (owned_focus)
			{
				hand_back_pending = true;
				try_hand_back(child);
			}
			PostMessageW(parent, host_message, token, (static_cast<LPARAM>(GetCurrentProcessId()) << 8) | 9);
			game_host_log.warning("The game draws in its own window over the launcher from now on: %s.", reason);
			return true;
		}
		// Shift+F11 (escape_key): the player sees no picture. A game in the launcher window moves into its own window, the
		// fix the on-screen check would make; one there already asks the launcher for its way out.
		void player_escape(HWND child)
		{
			game_host_log.warning("The player pressed Shift+F11: no game picture for them (%s).", surface_name());
			if (!detached.load() && !external_overlay && make_owned_popup(child, "the player pressed Shift+F11"))
			{
				screen_check_start(nullptr);
				return;
			}
			PostMessageW(parent, host_message, token, (static_cast<LPARAM>(GetCurrentProcessId()) << 8) | 10);
		}
		// The composed desktop at the check's points of the game's client area, as the player sees it. A point counts only
		// where the window under it is the game's or the launcher's: another program's window there proves nothing. Each
		// desktop read waits on the compositor, and this thread shares the launcher's input, so the four rows of points are
		// read as four one-pixel strips at their exact positions. CAPTUREBLT is left out: the compositor's desktop has every
		// window in it already, and that flag hides software cursors while it reads (remote and virtual displays).
		// `colours`: the presented frame the screen is compared with, or none. Its points are read where the window shows
		// them; a scaled frame's in both readings (screen_sample::scaled), four strips each, and a point is usable only where
		// both of its spots are. The time measured covers the window lookups too.
		bool read_screen(HWND child, screen_sample& sample, const pbd::screen_check::frame_colours* colours = nullptr)
		{
			RECT client{};
			POINT origin{};
			if (!GetClientRect(child, &client) || !ClientToScreen(child, &origin) || client.right < 8 || client.bottom < 8)
				return false;
			std::uint32_t drawn_width = 0, drawn_height = 0, image_width = 0, image_height = 0;
			if (colours && colours->width && colours->height)
			{
				sample.frame = colours->rgb;
				sample.framed = true;
				sample.scaled = !frame_fits_window(colours->image_width, colours->image_height, client.right, client.bottom);
				drawn_width = colours->width;
				drawn_height = colours->height;
				image_width = colours->image_width;
				image_height = colours->image_height;
			}
			const std::uint32_t readings = sample.scaled ? 2 : 1;
			const auto x_at = [&](std::uint32_t i, std::uint32_t reading)
			{
				return pbd::screen_check::client_x(i, drawn_width, image_width, static_cast<std::uint32_t>(client.right), reading != 0);
			};
			const auto y_at = [&](std::uint32_t i, std::uint32_t reading)
			{
				return pbd::screen_check::client_y(i, drawn_height, image_height, static_cast<std::uint32_t>(client.bottom), reading != 0);
			};
			const auto started = std::chrono::steady_clock::now();
			const auto own_root = GetAncestor(child, GA_ROOT);
			sample.usable = (1u << screen_points) - 1;
			for (std::uint32_t reading = 0; reading < readings; ++reading)
			{
				for (std::uint32_t i = 0; i < screen_points; ++i)
				{
					const POINT at{origin.x + static_cast<LONG>(x_at(i, reading)), origin.y + static_cast<LONG>(y_at(i, reading))};
					const auto over = WindowFromPoint(at);
					const auto root = over ? GetAncestor(over, GA_ROOT) : nullptr;
					if (!root || (root != parent && root != own_root))
						sample.usable &= ~(1u << i);
				}
			}
			const auto screen_dc = GetDC(nullptr);
			if (!screen_dc)
				return false;
			const auto memory = CreateCompatibleDC(screen_dc);
			BITMAPINFO info{};
			info.bmiHeader.biSize = sizeof(info.bmiHeader);
			info.bmiHeader.biWidth = client.right;
			info.bmiHeader.biHeight = -static_cast<LONG>(readings * pbd::screen_check::grid);
			info.bmiHeader.biPlanes = 1;
			info.bmiHeader.biBitCount = 32;
			info.bmiHeader.biCompression = BI_RGB;
			void* bits = nullptr;
			const auto bitmap = memory ? CreateDIBSection(memory, &info, DIB_RGB_COLORS, &bits, nullptr, 0) : nullptr;
			bool read = bitmap && bits;
			if (read)
			{
				const auto previous = SelectObject(memory, bitmap);
				for (std::uint32_t row = 0; row < readings * pbd::screen_check::grid; ++row)
				{
					const auto reading = row / pbd::screen_check::grid, line = row % pbd::screen_check::grid;
					const int y = origin.y + static_cast<int>(y_at(line * pbd::screen_check::grid, reading));
					read = BitBlt(memory, 0, static_cast<int>(row), client.right, 1, screen_dc, origin.x, y, SRCCOPY) && read;
				}
				GdiFlush();
				const auto* pixels = static_cast<const std::uint32_t*>(bits);
				for (std::uint32_t reading = 0; reading < readings; ++reading)
				{
					auto& target = reading ? sample.stretched : sample.screen;
					for (std::uint32_t i = 0; i < screen_points; ++i)
					{
						const auto row = reading * pbd::screen_check::grid + i / pbd::screen_check::grid;
						target[i] = pixels[row * client.right + x_at(i, reading)] & 0xffffff;
					}
				}
				SelectObject(memory, previous);
			}
			if (bitmap)
				DeleteObject(bitmap);
			if (memory)
				DeleteDC(memory);
			ReleaseDC(nullptr, screen_dc);
			check.slowest_read_us = std::max<long long>(check.slowest_read_us,
				std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - started).count());
			return read;
		}
		static std::string describe_points(const std::array<std::uint32_t, screen_points>& colours)
		{
			std::string text;
			for (const auto colour : colours)
				text += fmt::format("%s%06x", text.empty() ? "" : ",", colour);
			return text;
		}
		// Starts the check of a show. `launcher_view`: the screen just before it, while the launcher was what it showed.
		void screen_check_start(const screen_sample* launcher_view)
		{
			screen_passed.store(false);
			const auto slowest_read_us = check.slowest_read_us;
			check = {};
			check.slowest_read_us = slowest_read_us;
			check.started = GetTickCount64();
			check.due = check.started + screen_first_sample_ms;
			if (launcher_view)
			{
				check.launcher = *launcher_view;
				check.has_launcher = true;
			}
		}
		const char* surface_name() const
		{
			return !detached.load() ? "in the launcher window" : external_overlay ? "Steam window" :
			                                                                        "own window";
		}
		void log_screen_check(const char* outcome, ULONGLONG now, const screen_sample& last)
		{
			game_host_log.notice("Screen check (%s): %s after %d sample(s) in %llu ms; slowest screen read %lld us; usable=0x%04x screen=%s frame=%s%s",
				surface_name(), outcome, check.taken, check.first_at ? now - check.first_at : 0ull, check.slowest_read_us, last.usable,
				describe_points(last.screen), last.framed ? describe_points(last.frame) : std::string("none"),
				last.scaled ? " stretched=" + describe_points(last.stretched) : std::string());
		}
		// One step of the running check, from the window thread's loop. A sample waits for the player to be in front of the
		// game (other windows could cover it otherwise) and for a surface refresh to settle; it pairs the screen with the
		// colours of an image presented after it was asked for, or with none when the renderer does not answer in time.
		void screen_check_step(HWND child, ULONGLONG now)
		{
			if (!check.due || now < check.due || !visible.load() || closing.load())
				return;
			if (screen_check_overdue(detached.load(), now - check.started, screen_passed.load()))
			{
				// The waits below last as long as their causes; a check still undecided at its bound passes as one whose
				// samples ran out does, and the slow re-check goes on from when the first sample was due.
				if (!check.first_at)
					check.first_at = check.started + screen_first_sample_ms;
				const auto outcome = fmt::format("inconclusive at the time bound, passed; re-checking (samples last waited for %s)",
					check.held ? check.held : "nothing");
				log_screen_check(outcome.c_str(), now, check.previous);
				screen_passed.store(true);
			}
			const char* held = refresh_due || refresh_shrunk || now < refresh_done + 150 ? "a surface refresh" :
			                   IsIconic(parent)                                          ? "the launcher to be restored" :
			                   !game_root_foreground()                                   ? "the player to be in front" :
			                                                                               nullptr;
			if (held)
			{
				// An answer asked before this wait would be paired with a screen read long after it: asked again after.
				check.held = held;
				check.request = 0;
				return;
			}
			auto& frames = pbd::screen_check::frames();
			if (!check.request)
			{
				check.request = frames.request();
				check.requested_at = now;
				return;
			}
			const auto colours = frames.answered(check.request);
			if (!colours && now - check.requested_at < screen_answer_ms)
				return;
			check.request = 0;
			screen_sample sample{};
			if (!read_screen(child, sample, colours ? &*colours : nullptr))
				sample.usable = 0;
			const auto* launcher_view = check.has_launcher ? &check.launcher : nullptr;
			const bool first = ++check.taken == 1;
			if (first)
			{
				check.first = sample;
				check.first_at = now;
			}
			const auto verdict = force_hidden ? screen_verdict::forced_hidden :
			                     first        ? (screen_confirmed(sample, launcher_view) ? screen_verdict::confirmed : screen_verdict::undecided) :
			                                    judge_screen(launcher_view, check.first, check.previous, sample, detached.load());
			check.previous = sample;
			if (verdict != screen_verdict::undecided)
			{
				screen_check_finish(child, verdict, now, sample);
				return;
			}
			const auto next = schedule_after_undecided(now - check.first_at, check.taken, screen_passed.load());
			if (next.pass)
			{
				// Screen reads can miss a picture the display shows directly, and windows of other programs can cover points,
				// so no evidence either way passes; the slow re-check goes on.
				log_screen_check("inconclusive, passed; re-checking", now, sample);
				screen_passed.store(true);
			}
			if (!next.next_ms)
			{
				log_screen_check("re-check over, inconclusive", now, sample);
				check.due = 0;
				return;
			}
			check.due = now + next.next_ms;
		}
		void screen_check_finish(HWND child, screen_verdict verdict, ULONGLONG now, const screen_sample& last)
		{
			check.due = 0;
			log_screen_check(std::string(screen_verdict_name(verdict)).c_str(), now, last);
			// The hidden child's last sample is what the launcher shows: the view an own window must change.
			if (screen_hidden(verdict) && !detached.load() && !external_overlay && make_owned_popup(child, screen_verdict_name(verdict)))
			{
				screen_check_start(&last);
				return;
			}
			// Nothing of the launcher can cover a top-level window. A picture still missing there is beyond what moving
			// windows can mend, and a desktop read can miss a picture the display shows directly, so the game keeps its
			// window as it is and the player has Shift+F11 for the launcher's way out; the log keeps the evidence.
			if (screen_hidden(verdict))
				game_host_log.warning("The screen still showed no game picture (%s); the game keeps its window.", surface_name());
			screen_passed.store(true);
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
			const bool grant = already_granted || (hosted ? has_focus.load() && game_root_foreground() : foreground_within(qt_window));
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
			const QPointF screen_point(global.x / scale, global.y / scale);
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
				post(new QWheelEvent(local, screen_point, QPoint(), QPoint(0, GET_WHEEL_DELTA_WPARAM(key)), buttons,
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
				post(new QMouseEvent(type, local, screen_point, button, buttons, modifiers()));
			}
		}
		void keyboard(HWND child, UINT message, WPARAM key, LPARAM value)
		{
			const bool pressed = message == WM_KEYDOWN || message == WM_SYSKEYDOWN;
			const auto held = modifiers();
			if (escape_key(key == VK_F11, held.testFlag(Qt::ShiftModifier),
					held.testFlag(Qt::ControlModifier) || held.testFlag(Qt::AltModifier) || held.testFlag(Qt::MetaModifier)))
			{
				if (pressed && !(value & (1 << 30)))
					player_escape(child);
				return;
			}
			if (fullscreen_key(key == VK_F11, held != Qt::NoModifier))
			{
				// A Steam window has its own full screen; an owned window follows the launcher's.
				if (pressed && !(value & (1 << 30)))
					if (external_overlay)
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
					return visible.load() && has_focus.load() && game_root_foreground();
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
		bool presentation_ready() const
		{
			return !closing.load() && can_show_frame(first_frame.load() != 0, pbd::held_presentation::output().can_show());
		}
		struct visibility_answer
		{
			bool verified = false;        // the visible answer: shown, in front, and confirmed on screen
			bool window_verified = false; // the window state alone
			bool front = false;           // the player is in front of the game or the launcher it belongs to
			bool pending = false;         // the game's own window is shown and in front while its screen check runs
		};
		visibility_answer observe_visibility(const char* action)
		{
			const auto child = hosted ? render_window.load() : qt_window;
			const bool popup = hosted && owned_popup.load() && detached.load();
			const auto root = hosted && !detached.load() ? parent : child;
			const auto observed = observe_window_visibility(child, root, popup ? parent : nullptr);
			const auto& value = observed.state;
			const bool ready = presentation_ready();
			const bool on_screen = !hosted || screen_passed.load();
			game_host_log.notice("Window visibility %s: ready=%d exists=%d visible=%d minimized=%d cloakKnown=%d cloaked=%d rect=%d,%d,%d,%d clippedRect=%d,%d,%d,%d monitor=%d foreground=%d screen=%s",
				action, ready, value.exists, value.visible, value.minimized, value.cloak_known, value.cloaked,
				observed.bounds.left, observed.bounds.top, observed.bounds.right, observed.bounds.bottom,
				observed.clipped.left, observed.clipped.top, observed.clipped.right, observed.clipped.bottom,
				value.on_screen, value.foreground, on_screen ? "passed" : "pending");
			if (value.on_screen)
				game_host_log.notice("Window monitor: rect=%d,%d,%d,%d workArea=%d,%d,%d,%d dpi=%u",
					observed.screen.rcMonitor.left, observed.screen.rcMonitor.top, observed.screen.rcMonitor.right, observed.screen.rcMonitor.bottom,
					observed.screen.rcWork.left, observed.screen.rcWork.top, observed.screen.rcWork.right, observed.screen.rcWork.bottom, GetDpiForWindow(child));
			return {ready && value.verified() && on_screen, value.verified(), hosted ? game_root_foreground() : foreground_within(qt_window),
				screen_check_pending(hosted && detached.load(), ready && value.verified(), on_screen)};
		}
		void recover(HWND child)
		{
			const auto observed = observe_visibility("before recovery");
			if (!presentation_ready() || !child || !IsWindow(child))
				return;
			// The launcher asks while the visible answer is NO, which it is while the check runs.
			if (hosted && recovery_skipped(check.due && !screen_passed.load(), observed.window_verified))
			{
				game_host_log.notice("Window recovery skipped: the window is shown and in front, and the screen check is running.");
				return;
			}
			if (hosted)
				command(child, 4);
			else
				game.showNormal();
			// An owned window goes where the launcher goes, so the launcher is what is put back on screen.
			const auto root = hosted && (!detached.load() || owned_popup.load()) ? parent : child;
			const auto repair = recover_window_position(root);
			if (hosted)
			{
				ShowWindow(child, SW_SHOWNOACTIVATE);
				resize(child);
				if (!detached.load())
				{
					raise(child);
					refresh_shrunk = false;
					refresh_due = GetTickCount64() + 300;
				}
				if (foreground_within(root))
					SetFocus(child);
			}
			pbd::output_gain::output().visible(true);
			game_host_log.notice("Window recovery: restore=%d fit=%d foreground=%d target=%d,%d,%d,%d",
				repair.restored, repair.positioned, repair.foreground, repair.target.x, repair.target.y, repair.target.width, repair.target.height);
			observe_visibility("after recovery");
		}
		void command(HWND child, LPARAM value)
		{
			if (value == 6)
			{
				recover(child);
				return;
			}
			if (value == 2)
				resize(child);
			if (value == 3)
			{
				// A top-level game window in front hands the foreground back to the launcher before it hides.
				if (detached && foreground_within(child))
				{
					AllowSetForegroundWindow(launcher_pid);
					SetForegroundWindow(parent);
				}
				show_requested = false;
				visible.store(false);
				check.due = 0;
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
				if (detached && !visible.load())
				{
					if (external_overlay)
						normal_bounds = place_overlay_surface(child, parent);
					else
						follow_launcher(child);
				}
				const bool shown = !visible.exchange(true);
				if (shown)
				{
					// What the screen shows before the game: the view the game must replace for the check to pass.
					screen_sample launcher_view{};
					check.slowest_read_us = 0;
					const bool seen = read_screen(child, launcher_view);
					ResetEvent(page_ready.value);
					ShowWindow(child, SW_SHOWNOACTIVATE);
					pbd::output_gain::output().visible(true);
					screen_check_start(seen ? &launcher_view : nullptr);
				}
				if (!detached)
					raise(child);
				if (shown && !detached && first_frame.load())
					refresh_due = GetTickCount64() + 300;
				if (shown && !detached)
					log_layers(child, "when shown");
				// An owned window takes the keyboard on a real show, once that cancels nothing. The repeats come with every
				// focus, move and resize of the launcher; one handed back on each would take the activation in the middle of
				// a drag by the launcher's frame. The launcher's activation is followed by foreground_event instead.
				if (detached && owned_popup.load())
				{
					if (shown)
					{
						hand_back_pending = true;
						try_hand_back(child);
					}
				}
				// A game the player started shows while they are in another app (a match found, a lobby turn): it comes to
				// the front, so its first input reaches it and Windows runs it as the foreground program.
				else if (shown && !foreground_within(parent) && !foreground_within(child))
				{
					const bool front = bring_to_front(detached ? child : parent);
					game_host_log.notice("Hosted game shown behind another window; brought to the front: %s", front ? "yes" : "no");
					if (front)
						SetFocus(child);
				}
				else if (foreground_within(parent))
				{
					if (detached)
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
				check.due = 0;
				pbd::output_gain::output().visible(false);
				focus(false);
				ShowWindow(child, SW_HIDE);
				PostMessageW(qt_window, WM_CLOSE, 0, 0);
			}
			if (value == 7 && external_overlay)
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
			// Steam needs the render surface and foreground root to belong to this process, so its window has no owner and
			// its own taskbar button. Any other top-level game window is owned by the launcher: no taskbar button, always
			// above it. The same first-frame, paint-before-return and private control protocol govern when either appears.
			detached.store(external_overlay || owned_popup.load());
			const auto child = CreateWindowExW(surface_ex_style(external_overlay), type.lpszClassName, L"PROJECT BACKDASH",
				(detached ? WS_POPUP : WS_CHILD) | WS_CLIPSIBLINGS,
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
			else if (owned_popup.load())
			{
				follow_launcher(child);
				game_host_log.notice("Windowed: the game draws in its own window over the launcher, owned by it.");
			}
			game_host_log.notice("Displays (adapter driving each): %s", display_adapters());
			resize(child);
			ready.set_value(child);
			bool announced = false;
			MSG message{};
			bool quit = false;
			while (!quit && !stop.stop_requested())
			{
				// The window can close while its process is still shutting down. Do not keep a surface or accept another
				// show against that dead owner, even if its numeric handle is reused by a different process.
				if (!launcher_window_exists(parent, launcher_pid))
					break;
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
					check.due = 0;
					pbd::output_gain::output().visible(false);
					focus(false);
					ShowWindow(child, SW_HIDE);
				}
				const auto now = GetTickCount64();
				if (!detached && visible.load() && !IsIconic(parent))
				{
					if (GetWindow(parent, GW_CHILD) != child)
						raise(child);
					const auto first = first_frame.load();
					if (first && !refreshed_once && !refresh_due)
						refresh_due = first + 500;
					refresh_surface(child, now);
				}
				// The screen, not the window order, decides whether the game is seen (screen_check_step).
				screen_check_step(child, now);
				try_hand_back(child);
			}
			// Owner destruction can destroy the surface while dispatching a message, which exits through WM_QUIT rather
			// than the next loop check. Both routes must also close the hidden Qt window.
			if (!launcher_window_exists(parent, launcher_pid))
				PostMessageW(qt_window, WM_CLOSE, 0, 0);
			if (follow_hook)
				UnhookWinEvent(follow_hook);
			follow_hook = nullptr;
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
