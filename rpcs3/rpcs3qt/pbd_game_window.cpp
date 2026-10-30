#include "stdafx.h"
#include "pbd_game_window.h"
#include "pbd_game_window_settings.h"
#include "pbd_game_host.h"
#include "pbd_clips_controller.h"
#include "Emu/Cell/pbd_rollback_session.h"
#include "Emu/PBD/pbd_output_gain.h"
#include "gs_frame.h"
#include "util/video_provider.h"
#include <QShowEvent>

#include "Emu/System.h"
#include "Emu/system_config.h"
#include "Emu/IdManager.h"
#include "Emu/Cell/pbd_game_overlay.h"
#include "Emu/PBD/pbd_replay_viewer_menu.h"
#include "Emu/PBD/pbd_practice_presentation.h"
#include "Emu/Cell/pbd_practice_mouse.h"
#include "Emu/PBD/pbd_env.h"
#include "Emu/PBD/pbd_held_presentation.h"
#include "Emu/RSX/RSXThread.h"
#include "Emu/pbd_session_mode.h"

#include <QEnterEvent>
#include <QGuiApplication>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QScreen>
#include <QWheelEvent>

#include <algorithm>
#include <array>
#include <string_view>

LOG_CHANNEL(gui_log, "GUI");

extern atomic_t<bool> g_game_window_focused;

namespace pbd::game_window
{
	bool hooks::event(gs_frame& frame, QEvent* ev)
	{
		return hosted_event(frame, ev) || menu_pointer(frame, ev) || replay_pointer(frame, ev) || practice_pointer(frame, ev);
	}

	hooks::game_area hooks::area(gs_frame& frame)
	{
		const size2u dimensions{static_cast<u32>(std::max(frame.client_width(), 1)), static_cast<u32>(std::max(frame.client_height(), 1))};
		const auto& avconf = g_fxo->get<rsx::avconf>();
		const auto viewport = g_cfg.video.stretch_to_display_area ? areau{0, 0, dimensions.width, dimensions.height} : avconf.aspect_convert_region(avconf.video_frame_size(), dimensions);
		return {dimensions, viewport};
	}

	std::pair<float, float> hooks::pixels(gs_frame& frame, const game_area& area, const QPointF& position)
	{
		// Qt positions are DIPs; RSX draws in physical pixels.
		return {static_cast<float>(position.x() * area.dimensions.width / std::max(frame.width(), 1)),
			static_cast<float>(position.y() * area.dimensions.height / std::max(frame.height(), 1))};
	}

	// The launcher-hosted window's own input, and its Esc, which opens Settings: hosted input targets a hidden Qt window, so
	// its window-scoped shortcuts cannot.
	bool hooks::hosted_event(gs_frame& frame, QEvent* ev)
	{
		if (frame.m_game_host && frame.m_game_host->event(ev))
			return true;
		if (frame.m_game_host && frame.m_game_host->configured() && frame.m_settings && !Emu.IsStopped() &&
			(ev->type() == QEvent::KeyPress || ev->type() == QEvent::KeyRelease))
		{
			const auto* key = static_cast<QKeyEvent*>(ev);
			if (key->key() == Qt::Key_Escape)
			{
				if (ev->type() == QEvent::KeyPress && !key->isAutoRepeat())
					frame.m_settings->escape();
				ev->accept();
				return true;
			}
		}
		if (frame.m_game_host && frame.m_game_host->configured() &&
			(ev->type() == QEvent::FocusIn || ev->type() == QEvent::FocusOut))
			g_game_window_focused = ev->type() == QEvent::FocusIn && frame.m_game_host->focused();
		return false;
	}

	// The in-game Settings menu draws over every other PBD panel, so its pointer comes first; then a lobby or Practice menu's
	// right click, which goes back.
	bool hooks::menu_pointer(gs_frame& frame, QEvent* ev)
	{
		if (frame.m_settings && frame.m_settings->pointer_event(ev))
		{
			ev->accept();
			return true;
		}
		if (ev->type() == QEvent::FocusOut)
			pbd::game_overlay::pointer_captured.store(false, std::memory_order_release);
		if (ev->type() == QEvent::MouseButtonRelease && pbd::game_overlay::pointer_captured.exchange(false, std::memory_order_acq_rel))
		{
			ev->accept();
			return true;
		}
		if (!Emu.IsStopped() && ev->type() == QEvent::MouseButtonPress &&
			static_cast<QMouseEvent*>(ev)->button() == Qt::RightButton && pbd::game_overlay::request_back())
		{
			pbd::game_overlay::pointer_captured.store(true, std::memory_order_release);
			ev->accept();
			return true;
		}
		return false;
	}

	// A replay viewer's controls take the mouse; its events never reach the game's mouse/gyro path or double-click
	// fullscreen behaviour.
	bool hooks::replay_pointer(gs_frame& frame, QEvent* ev)
	{
		if (ev->type() == QEvent::Wheel && !Emu.IsStopped() && pbd::replay_viewer::presentation.enabled.load(std::memory_order_relaxed))
		{
			ev->accept();
			return true;
		}
		const auto pointer_event = ev->type() == QEvent::MouseMove || ev->type() == QEvent::MouseButtonPress ||
		                           ev->type() == QEvent::MouseButtonRelease || ev->type() == QEvent::MouseButtonDblClick;
		if (!pointer_event || Emu.IsStopped() || !pbd::replay_viewer::presentation.enabled.load(std::memory_order_relaxed))
			return false;
		const auto* mouse = static_cast<QMouseEvent*>(ev);
		const auto game = area(frame);
		const auto [x, y] = pixels(frame, game, mouse->position());
		// RSX draws the controls in physical pixels inside the aspect-correct game area.
		const auto position = pbd::replay_viewer::pointer_coordinates(x, y,
			{static_cast<s32>(game.viewport.x1), static_cast<s32>(game.viewport.y1), static_cast<s32>(game.viewport.width()), static_cast<s32>(game.viewport.height())});
		const auto hit = pbd::replay_viewer::pointer_ui.hit(position.x, position.y);
		frame.handle_cursor(frame.visibility(), false, false, true);
		frame.setCursor(hit == pbd::replay_viewer::pointer_none ? Qt::ArrowCursor : Qt::PointingHandCursor);
		if (ev->type() == QEvent::MouseButtonPress && mouse->button() == Qt::LeftButton)
			pbd::replay_viewer::pointer_ui.press(position.x, position.y);
		if (ev->type() == QEvent::MouseButtonPress && mouse->button() == Qt::RightButton)
			pbd::replay_viewer::pointer_ui.back();
		ev->accept();
		return true;
	}

	void hooks::practice_point(gs_frame& frame, const QPointF& position, bool choose)
	{
		if (Emu.IsStopped())
			return;
		const auto game = area(frame);
		const auto [x, y] = pixels(frame, game, position);
		// The menu is drawn in the game viewport; Qt coordinates also need the physical-pixel scale.
		pbd::practice::mouse::pointer(x, y, static_cast<float>(game.viewport.width()), static_cast<float>(game.viewport.height()), choose,
			static_cast<float>(game.viewport.x1), static_cast<float>(game.viewport.y1));
	}

	// Practice's menus take the mouse while Practice holds the input or a press made in them is still down.
	bool hooks::practice_pointer(gs_frame& frame, QEvent* ev)
	{
		if (ev->type() == QEvent::FocusOut)
			pbd::practice::mouse::captured_buttons.store(false, std::memory_order_release);
		const bool owned = pbd::practice::requested() &&
		                   (pbd::practice::input_claim.load(std::memory_order_acquire) || pbd::practice::mouse::captured_buttons.load(std::memory_order_acquire));
		if (!owned)
			return false;
		if (ev->type() == QEvent::Enter)
		{
			// Qt turns the first unbuttoned motion into Enter; it already carries the position needed to hover.
			frame.m_mousehide_timer.stop();
			frame.m_show_mouse.store(true);
			frame.setCursor(Qt::ArrowCursor);
			practice_point(frame, static_cast<QEnterEvent*>(ev)->position(), false);
			return true;
		}
		if (ev->type() == QEvent::Leave)
		{
			// Leaving clears tab hover without releasing a closing click that still owns its button-up event.
			pbd::practice::mouse::pointer(-1.f, -1.f, 1.f, 1.f, false);
			return true;
		}
		if (ev->type() == QEvent::MouseMove || ev->type() == QEvent::MouseButtonPress ||
			ev->type() == QEvent::MouseButtonRelease || ev->type() == QEvent::MouseButtonDblClick)
		{
			auto* mouse = static_cast<QMouseEvent*>(ev);
			frame.m_mousehide_timer.stop();
			frame.m_show_mouse.store(true);
			frame.setCursor(Qt::ArrowCursor);
			if (ev->type() == QEvent::MouseButtonPress || ev->type() == QEvent::MouseButtonDblClick)
				pbd::practice::mouse::captured_buttons.store(true, std::memory_order_release);
			// QWindow receives the second press before its double-click notification. Apply the press once and
			// consume that extra notification so two fast presses cannot change a setting three times.
			const bool choose = ev->type() == QEvent::MouseButtonPress && mouse->button() == Qt::LeftButton;
			if (ev->type() == QEvent::MouseButtonPress && mouse->button() == Qt::RightButton)
				pbd::practice::mouse::back();
			if (ev->type() == QEvent::MouseMove || choose)
			{
				practice_point(frame, mouse->position(), choose);
			}
			if (ev->type() == QEvent::MouseButtonRelease && mouse->buttons() == Qt::NoButton)
				pbd::practice::mouse::captured_buttons.store(false, std::memory_order_release);
			return true;
		}
		if (ev->type() == QEvent::Wheel)
		{
			const auto delta = static_cast<QWheelEvent*>(ev)->angleDelta().y();
			if (delta)
				pbd::practice::mouse::wheel(delta > 0);
			return true;
		}
		return false;
	}

	// In a session (pbd_session_mode.h) the game runs on two PCs in lockstep, and these shortcuts act on this PC alone:
	// pause, stop, restart and savestates end the session or freeze one side; the frame limit toggle changes this PC's pacing;
	// the Home Menu sends system-menu events into this PC's game only; recording and RSX capture read frames back from the GPU
	// (every frame, or one long stall) and slow this PC's presentation; the keyboard and mouse-gyro toggles change what this
	// PC's game reads as input. Fullscreen, exit fullscreen, screenshot, mouse lock, volume and the log mark only change this
	// PC's window, sound or log, so they keep working. In Practice, replays and spectating, exit fullscreen (Esc) opens the
	// in-game menu instead (gs_frame::handle_shortcut).
	bool session_ignores_shortcut(gui::shortcuts::shortcut shortcut_key, const QKeySequence& key_sequence)
	{
		switch (shortcut_key)
		{
		case gui::shortcuts::shortcut::gw_pause_play:
		case gui::shortcuts::shortcut::gw_stop:
		case gui::shortcuts::shortcut::gw_savestate:
		case gui::shortcuts::shortcut::gw_savestate_1:
		case gui::shortcuts::shortcut::gw_savestate_2:
		case gui::shortcuts::shortcut::gw_savestate_3:
		case gui::shortcuts::shortcut::gw_savestate_4:
		case gui::shortcuts::shortcut::gw_restart:
		case gui::shortcuts::shortcut::gw_frame_limit:
		case gui::shortcuts::shortcut::gw_home_menu:
		case gui::shortcuts::shortcut::gw_toggle_recording:
		case gui::shortcuts::shortcut::gw_rsx_capture:
		case gui::shortcuts::shortcut::gw_toggle_mouse_and_keyboard:
		case gui::shortcuts::shortcut::gw_toggle_mouse_gyro:
			break;
		default:
			return false;
		}

		if (!pbd::session_mode::active())
		{
			return false;
		}

		// One line per shortcut, the first time it is pressed in this process. Shortcuts arrive on the GUI thread only.
		static std::array<bool, static_cast<usz>(gui::shortcuts::shortcut::count)> logged{};

		if (const usz index = static_cast<usz>(shortcut_key); index < logged.size() && !logged[index])
		{
			logged[index] = true;
			gui_log.warning("PBD session: ignoring the %s shortcut (%s); it acts on this PC only and would break the session.",
				shortcut_key, key_sequence.toString());
		}

		return true;
	}

	bool quiet_window()
	{
		static const bool quiet = []
		{
			const char* value = pbd::env::find("PBD_QUIET_WINDOW");
			return value && std::string_view(value) == "1";
		}();
		return quiet;
	}

	// The show event's clamp pulls a window back inside the screen it was saved on, which put the hidden setup and
	// preparation runs on screen at the player's saved game-window position, so a quiet window skips it.
	bool keep_quiet_window_off_screen(QWindow& window)
	{
		if (!quiet_window())
			return false;
		if (const QScreen* primary = QGuiApplication::primaryScreen())
		{
			const QRect desktop = primary->virtualGeometry();
			window.setFramePosition(QPoint(desktop.right() + 64, desktop.top()));
		}
		return true;
	}

	bool show_quiet_window(QWindow& window)
	{
		if (!quiet_window() || pbd::held_presentation::output().active())
			return false;
		// Shown without taking the focus, as a tool window (no taskbar button), just beyond the right edge of the virtual
		// desktop. It still renders and presents every frame, so the run's timing is that of a visible window; a hidden
		// or minimised window could have no surface to present to.
		window.setFlags(window.flags() | Qt::Tool | Qt::WindowDoesNotAcceptFocus);
		window.setProperty("_q_showWithoutActivating", true);
		if (const QScreen* primary = QGuiApplication::primaryScreen())
		{
			const QRect desktop = primary->virtualGeometry();
			window.setPosition(desktop.right() + 64, desktop.top());
		}
		window.QWindow::show();
		return true;
	}
} // namespace pbd::game_window

bool gs_frame::nativeEvent(const QByteArray& event_type, void* message, qintptr* result)
{
	if (m_clips && m_clips->native_event(message))
	{
		*result = 0;
		return true;
	}
	return QWindow::nativeEvent(event_type, message, result);
}

bool gs_frame::shown()
{
	if (m_game_host->configured())
		return m_game_host->visible();
	return QWindow::isVisible();
}

bool gs_frame::hosted_by_launcher() const
{
	return m_game_host && m_game_host->configured();
}

namespace pbd::game_window
{
	bool hooks::show(gs_frame& frame)
	{
		if (!pbd::held_presentation::output().can_show())
			return true;
		if (frame.m_game_host->configured())
		{
			frame.m_game_host->show_loading();
			return true;
		}
		if (pbd::game_window::show_quiet_window(frame))
			return true;

		return false;
	}

	void hooks::before_flip(gs_frame& frame, bool skip_frame)
	{
		if (!skip_frame)
			frame.m_game_host->first_frame();
		if (pbd::held_presentation::output().active() && pbd::held_presentation::output().can_show() && !frame.shown())
			frame.show();
	}

	void hooks::shutdown(gs_frame& frame)
	{
		frame.m_clips.reset();
		frame.m_game_host->shutdown();
		frame.m_settings.reset();
	}

	void hooks::first_frame()
	{
		pbd::output_gain::output().first_frame(pbd::rollback_session::requested());
	}

	void hooks::window_closed()
	{
		pbd::session_mode::note_window_closed();
	}
} // namespace pbd::game_window

bool gs_frame::asynchronous_capture() const
{
	return g_fxo->get<utils::video_provider>().asynchronous_capture();
}

void gs_frame::present_frame_timed(std::vector<u8>&& data, u32 pitch, u32 width, u32 height, bool bgra, u64 time_us) const
{
	g_fxo->get<utils::video_provider>().present_frame(std::move(data), pitch, width, height, bgra, time_us);
}

namespace pbd::game_window
{
	void hooks::create_controls(gs_frame& frame)
	{
		frame.m_settings = pbd::game_window::settings_menu::create(frame);
#ifdef _WIN32
		frame.m_clips = std::make_unique<pbd::clips::controller>(&frame);
#endif
	}

	bool hooks::show_event(gs_frame& frame, QShowEvent* event)
	{
		// PROJECT BACKDASH: an unattended run keeps its window off screen (pbd_game_window.h).
		if (pbd::game_window::keep_quiet_window_off_screen(frame))
		{
			frame.QWindow::showEvent(event);
			return true;
		}

		return false;
	}

	bool hooks::shortcut(gs_frame& frame, gui::shortcuts::shortcut key, const QKeySequence& sequence)
	{
		if (session_ignores_shortcut(key, sequence))
			return true;
		// Esc goes back through the in-game menu before it can leave fullscreen.
		if (key == gui::shortcuts::shortcut::gw_exit_fullscreen && frame.m_settings)
		{
			frame.m_settings->escape();
			return true;
		}
		// Clips uses the global binding; the old window shortcut stays inactive after a rebind.
		return key == gui::shortcuts::shortcut::gw_toggle_recording && frame.m_clips;
	}
} // namespace pbd::game_window
