#include "stdafx.h"
#include "pbd_game_window_settings.h"
#include "pbd_game_window.h"
#include "pbd_clips_controller.h"
#include "pbd_game_host.h"
#include "pbd_settings_devices.h"
#include "gs_frame.h"

#include "Utilities/File.h"
#include "Emu/System.h"
#include "Emu/system_config.h"
#include "Emu/localized_string.h"
#include "Emu/PBD/pbd_practice_presentation.h"
#include "Emu/Cell/pbd_practice_mouse.h"
#include "Emu/Cell/pbd_player_volume.h"
#include "Emu/PBD/pbd_controller_settings.h"
#include "Emu/RSX/Overlays/overlay_message.h"

#include <QEnterEvent>
#include <QMouseEvent>
#include <QWheelEvent>

#include <algorithm>
#include <chrono>
#include <cmath>

#ifdef _WIN32
#include <windows.h>
#endif

LOG_CHANNEL(gui_log, "GUI");

namespace rsx
{
	void set_native_ui_flip();
}

std::optional<u16> pbd_local_pad_word_try() noexcept; // Emu/Cell/Modules/pbd_pad_words.cpp
u16 pbd_spectator_pad_word() noexcept;                // Input/pbd_pad_spectator.cpp

namespace pbd::game_window
{
	namespace
	{
		// Spectator game ports stay neutral; its menu reads the watcher's own controller instead.
		std::optional<u16> pbd_menu_pad_word_try() noexcept
		{
			if (pbd::controllers::settings::active.load(std::memory_order_acquire))
				return pbd::controllers::settings::menu_word.load(std::memory_order_acquire);
			std::optional<u16> word = pbd_local_pad_word_try();
			if (word)
				*word = static_cast<u16>(*word | pbd_spectator_pad_word());
			return word;
		}

		// The menu reads the controller every 8 ms while it is open and while its closing press is released, and otherwise only
		// looks for Practice's open request every 50 ms.
		constexpr int settings_open_interval_ms = 8, settings_idle_interval_ms = 50;
		// Longer than the launcher's own 10 s close deadline (it polls for a navigate request every 250 ms): the launcher acts first.
		constexpr u64 settings_leave_grace_ms = 15000;

		u64 steady_ms()
		{
			return static_cast<u64>(std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count());
		}

		// UTF-8, as fs:: and the log take paths (the narrow std::filesystem form is the system code page on Windows).
		std::string settings_path_text(const std::filesystem::path& path)
		{
			const auto utf8 = path.u8string();
			return std::string(utf8.begin(), utf8.end());
		}
	} // namespace

	std::unique_ptr<settings_menu> settings_menu::create(gs_frame& frame)
	{
		if (!pbd::settings_menu::available())
			return nullptr;
		return std::make_unique<settings_menu>(frame);
	}

	settings_menu::settings_menu(gs_frame& frame)
		: m_frame(frame)
	{
		QObject::connect(&m_timer, &QTimer::timeout, &m_frame, [this]()
			{
				tick();
			});
		m_timer.start(settings_idle_interval_ms);
		gui_log.notice("PBD settings: the in-game Settings menu is available (%s); changes go to %s; Return to Launcher and Exit Game: %s", std::string(pbd::settings_menu::context()),
			m_state.changes_file ? settings_path_text(*m_state.changes_file) : std::string("nowhere (no run folder)"),
			m_state.navigate_file ? settings_path_text(*m_state.navigate_file) : std::string("not offered (no request file)"));
#ifdef _WIN32
		if (const auto launcher = pbd::settings_menu::launcher_process_id())
			gui_log.notice("PBD settings: while in front, the game window lets the launcher (process %u) take the foreground", *launcher);
		else
			gui_log.notice("PBD settings: no launcher process named (PBD_LAUNCHER_PID); a lobby turn cannot bring the launcher over this window");
#endif
	}

	settings_menu::~settings_menu()
	{
		m_timer.stop();
		write_changes();
		pbd::settings_menu::showing.store(false, std::memory_order_release);
		pbd::settings_menu::claim.store(false, std::memory_order_release);
		pbd::settings_menu::pointer_captured.store(false, std::memory_order_release);
		pbd::settings_menu::publish({});
	}

	void settings_menu::grant_launcher_foreground()
	{
#ifdef _WIN32
		// Keep the launcher's foreground grant valid after keyboard and mouse input.
		if (const auto launcher = pbd::settings_menu::launcher_process_id(); launcher && m_frame.isActive() && pbd::settings_menu::context() != "Online set")
			AllowSetForegroundWindow(*launcher);
#endif
	}

	void settings_menu::stop_polling()
	{
		namespace menu = pbd::settings_menu;
		auto& s = m_state;
		if (s.menu.open)
		{
			s.menu.hide();
			menu::showing.store(false, std::memory_order_release);
			write_changes();
			publish();
		}
		s.releasing = false;
		menu::claim.store(false, std::memory_order_release);
	}

	void settings_menu::poll_closed(const std::optional<u16>& held)
	{
		namespace menu = pbd::settings_menu;
		auto& s = m_state;
		if (const auto leave = menu::take_leave_request())
		{
			choose_leave(static_cast<u32>(*leave));
			return;
		}
		if (menu::take_open_request())
		{
			open(false);
			return;
		}
		if (menu::take_top_open_request())
		{
			open(true);
			return;
		}
		if (const auto device = menu::take_controls_open_request())
		{
			open(false);
			if (!s.menu.open)
				return;
			// Button setup from offline play's player question: the Controls page alone, on the first action of that
			// controller's own layout.
			s.menu.has_top = false;
			s.menu.controls_only = true;
			s.menu.level = menu::page::controls;
			s.menu.controls_focus = 1;
			menu::show_controller(s.model, *device);
			++s.menu.revision;
			publish();
			return;
		}
		// The game gets the controller back once every button that was down when the menu closed is up.
		if (s.releasing && held && !*held)
		{
			s.releasing = false;
			menu::claim.store(false, std::memory_order_release);
			m_timer.setInterval(settings_idle_interval_ms);
		}
	}

	void settings_menu::refresh_inputs()
	{
		namespace menu = pbd::settings_menu;
		auto& s = m_state;
		const auto input = pbd::controllers::settings::read();
		const auto input_display = pbd::controllers::read_display();
		if (s.input_display.device != input_display.device || s.input_display.family != input_display.family || s.input_display.keys != input_display.keys)
		{
			s.input_display = input_display;
			++s.menu.revision;
		}
		if (input.available && s.model.controllers != input.devices)
		{
			s.model.controllers = input.devices;
			++s.menu.revision;
		}
		// A recording that ends at its 60-minute limit, or an encoder that stops, changes the recording row while the menu is open.
		if (m_frame.m_clips && menu::set_clips(s.model, m_frame.m_clips->available(), m_frame.m_clips->recording()))
			++s.menu.revision;
	}

	void settings_menu::list_outputs()
	{
		namespace menu = pbd::settings_menu;
		auto& s = m_state;
		if (!s.menu.open || s.menu.level != menu::page::settings || s.outputs_listed)
			return;
		s.outputs_listed = true;
		s.model.audio_outputs = pbd::settings_devices::audio_outputs();
		++s.menu.revision;
	}

	void settings_menu::tick()
	{
		namespace menu = pbd::settings_menu;
		auto& s = m_state;
		grant_launcher_foreground();
		if (Emu.IsStopped())
		{
			stop_polling();
			return;
		}
		const auto held = pbd_menu_pad_word_try();
		if (!s.menu.open)
		{
			poll_closed(held);
			return;
		}
		if (s.menu.leaving && s.leave_by_ms && !s.shutdown_asked && steady_ms() >= s.leave_by_ms)
		{
			// Nothing ended this run after the request: the game closes as a click on its close button would.
			s.shutdown_asked = true;
			gui_log.warning("PBD menu: this run was not ended within %u s of the request; the game closes itself", settings_leave_grace_ms / 1000);
			m_frame.m_game_host->shutdown();
			Emu.GracefulShutdown(true, true);
			m_frame.hide_on_close();
			return;
		}
#ifdef _WIN32
		// Renew the foreground grant through the release of the input that chose Return to Launcher.
		if (s.menu.leaving && s.leave_by_ms && s.menu.chosen() == menu::entry::launcher && m_frame.isActive())
			AllowSetForegroundWindow(ASFW_ANY);
#endif
		// The pad lock is busy (the pad thread is waiting on this thread); the next tick reads it.
		if (!held)
			return;
		const u64 now = steady_ms();
		refresh_inputs();
		list_outputs();
		result(s.menu.poll(s.model, *held, now));
		list_outputs();
		publish();
		// While the game presents nothing (a paused replay, a held boundary), UI-only flips draw the menu about 30 times a second.
		if (s.menu.open && now - menu::composed_ms.load(std::memory_order_relaxed) > 50 && now - s.last_flip_ms >= 33)
		{
			s.last_flip_ms = now;
			rsx::set_native_ui_flip();
		}
	}

	void settings_menu::escape()
	{
		auto& s = m_state;
		// The Esc shortcut does not auto-repeat (shortcut_settings.cpp), so a held Esc acts once. It goes back as Circle does: out
		// of an open list, otherwise out of the menu.
		if (s.menu.open)
			result(s.menu.back(s.model));
		else
			open(true);
		publish();
	}

	// The values in effect now. The first call also keeps them as the session's start, which the changes file is measured
	// against; a later one takes what moved since the menu last closed (Alt+Enter, the volume keys).
	void settings_menu::take_values()
	{
		namespace menu = pbd::settings_menu;
		auto& s = m_state;
		auto& m = s.model;
		const auto displays = pbd::settings_devices::game_displays(m_frame.winId());
		menu::values now;
		now.volume = std::clamp(pbd::player_volume::get(), 0, 100);
		now.resolution = static_cast<u32>(g_cfg.video.resolution_scale_percent.get());
		now.fullscreen = m_frame.m_game_host->configured() ? m_frame.m_game_host->fullscreen() : m_frame.visibility() == QWindow::FullScreen;
		now.vsync = g_cfg.video.vsync.get() != vsync_mode::off;
		now.audio_output = g_cfg.audio.audio_device.to_string();
		now.display = displays.current;
		const auto input = pbd::controllers::settings::read();
		if (input.available)
			now.input = input.current;
		m.controllers = input.devices;
		m.online = menu::context() == "Online set";
		m.borderless = g_cfg.video.vk.exclusive_fullscreen_mode.get() == vk_exclusive_fs_mode::disable;
		if (!s.started)
		{
			s.started = true;
			m.start = now;
			m.current = now;
			s.written = menu::changes_json(m);
		}
		else
		{
			// The device and the monitor stay as the player chose them for the next session.
			m.current.volume = now.volume;
			m.current.resolution = now.resolution;
			m.current.fullscreen = now.fullscreen;
			m.current.vsync = now.vsync;
			m.current.input = now.input;
		}
		m.applied = now;
		m.displays = displays.list;
	}

	// The volume keys step the player's volume by 5 within the launcher's range. Like the menu's Volume row they set the
	// player's volume, so a host mute stays on and brings the new level back when it ends, and the change is saved.
	void settings_menu::volume_key(settings_menu* menu, int step)
	{
		if (menu && !menu->m_state.started)
			menu->take_values();
		const int volume = std::clamp(pbd::player_volume::get() + step, 0, 100);
		pbd::player_volume::set(volume);
		rsx::overlays::queue_message(localized_string(localized_string_id::AUDIO_CHANGED, "%d%%", volume), 3'000'000, {},
			rsx::overlays::message_pin_location::top_left, {}, true, true);
		if (menu)
			menu->m_state.model.current.volume = menu->m_state.model.applied.volume = volume;
	}

	void settings_menu::open(bool from_escape)
	{
		namespace menu = pbd::settings_menu;
		auto& s = m_state;
		if (s.menu.open || Emu.IsStopped())
			return;
		// Practice's Settings row may have asked for this open moments before Esc did; a request left pending would reopen the
		// menu as soon as it closes.
		menu::take_open_request();
		menu::take_top_open_request();
		auto& m = s.model;
		take_values();
		if (m_frame.m_clips)
			menu::set_clips(m, m_frame.m_clips->available(), m_frame.m_clips->recording());
		// Audio enumeration can stall the window thread. List outputs only when the settings page first needs them.
		s.outputs_listed = false;
		if (m.audio_outputs.empty())
			m.audio_outputs = {{std::string(menu::default_audio_output), "Windows default"}};
		// The press that opened the menu (Cross on Practice's row) waits for its release; a busy pad lock counts as held. Esc opens
		// on the top level where the launcher can be asked to end this run; Practice's Settings row opens the settings page.
		s.menu.show(pbd_menu_pad_word_try().value_or(0xffff), from_escape && s.navigate_file.has_value());
		s.menu.controls_only = m.online;
		if (m.online)
		{
			s.menu.has_top = false;
			s.menu.level = menu::page::controls;
			s.menu.controls_focus = 0;
		}
		menu::claim.store(true, std::memory_order_release);
		menu::showing.store(true, std::memory_order_release);
		// Over Practice's fight, Practice pauses as START does and opens its menu under this one. A Return to Game left from an
		// earlier open must not resume this pause.
		menu::take_practice_resume();
		if (from_escape && pbd::practice::requested())
			menu::request_practice_pause();
		s.releasing = false;
		m_timer.setInterval(settings_open_interval_ms);
		// The pointer shows while the menu is open, also in fullscreen.
		m_frame.m_mousehide_timer.stop();
		m_frame.m_show_mouse.store(true);
		m_frame.setCursor(Qt::ArrowCursor);
		publish();
		if (steady_ms() - menu::composed_ms.load(std::memory_order_relaxed) > 50)
			rsx::set_native_ui_flip();
		gui_log.notice("PBD settings: opened (%s)", from_escape ? "Esc" : "Practice menu");
	}

	// Return to Launcher or Exit Game from the replay's Options menu (pbd::settings_menu::request_leave): this menu opens on its
	// top level with that row chosen, so the player sees the same leaving line and the launcher gets the same request.
	void settings_menu::choose_leave(u32 row)
	{
		namespace menu = pbd::settings_menu;
		auto& s = m_state;
		if (!s.navigate_file || row >= menu::entry_count)
			return;
		open(true);
		if (!s.menu.open || !s.menu.has_top)
			return;
		s.menu.top_focus = row;
		result(s.menu.pick(static_cast<menu::entry>(row)));
		publish();
	}

	void settings_menu::result(pbd::settings_menu::effect effect)
	{
		auto& s = m_state;
		switch (effect)
		{
		case pbd::settings_menu::effect::controls:
			if (pbd::controllers::settings::submit(s.model.current.input))
				write_changes();
			else
			{
				s.menu.notice = "That controller is unavailable. Choose a connected controller.";
				++s.menu.revision;
			}
			break;
		case pbd::settings_menu::effect::volume:
			pbd::player_volume::set(s.model.current.volume);
			s.model.applied.volume = s.model.current.volume;
			break;
		case pbd::settings_menu::effect::apply:
			// Back from the settings page to the top level: the video rows apply as they would on a close.
			apply_video();
			write_changes();
			break;
		case pbd::settings_menu::effect::close:
			close();
			break;
		case pbd::settings_menu::effect::navigate:
			navigate();
			break;
		case pbd::settings_menu::effect::record:
			// The menu closes first, so a new recording starts on the game rather than on the menu.
			close();
			if (m_frame.m_clips)
				m_frame.m_clips->record();
			break;
		case pbd::settings_menu::effect::none:
			break;
		}
	}

	void settings_menu::apply_video()
	{
		auto& m = m_state.model;
		// The video rows together, once: the render scale applies at the next flip (render targets are rescaled), VSync and the
		// window mode each rebuild the swapchain.
		if (m.current.resolution != m.applied.resolution)
		{
			g_cfg.video.resolution_scale_percent.set(m.current.resolution);
			gui_log.notice("PBD settings: render scale %u percent", m.current.resolution);
		}
		if (m.current.vsync != m.applied.vsync)
		{
			g_cfg.video.vsync.set(m.current.vsync ? vsync_mode::full : vsync_mode::off);
			gui_log.notice("PBD settings: VSync %s", m.current.vsync ? "on" : "off");
		}
		if (m.current.fullscreen != (m_frame.m_game_host->configured() ? m_frame.m_game_host->fullscreen() : m_frame.visibility() == QWindow::FullScreen))
		{
			m_frame.toggle_fullscreen();
			gui_log.notice("PBD settings: %s", m.current.fullscreen ? "fullscreen" : "windowed");
		}
		m.applied.resolution = m.current.resolution;
		m.applied.vsync = m.current.vsync;
		m.applied.fullscreen = m.current.fullscreen;
	}

	void settings_menu::close()
	{
		namespace menu = pbd::settings_menu;
		auto& s = m_state;
		if (s.menu.open)
			s.menu.hide();
		apply_video();
		// Closing a menu that Esc opened on its top level returns to the game: Practice's pause menu under it resumes the fight.
		if (s.menu.has_top && pbd::practice::requested())
			menu::request_practice_resume();
		menu::showing.store(false, std::memory_order_release);
		// The controller stays claimed until the closing press is released (tick).
		s.releasing = true;
		write_changes();
		publish();
		m_frame.handle_cursor(m_frame.visibility(), false, false, true);
		if (steady_ms() - menu::composed_ms.load(std::memory_order_relaxed) > 50)
			rsx::set_native_ui_flip();
		gui_log.notice("PBD settings: closed");
	}

	void settings_menu::navigate()
	{
		namespace menu = pbd::settings_menu;
		auto& s = m_state;
		const auto chosen = s.menu.chosen();
		const std::string target(menu::entry_targets[static_cast<u32>(chosen)]);
		const std::string text = menu::navigate_json(chosen);
		// The player's settings changes first: the launcher saves them when this run ends.
		write_changes();
		if (!s.navigate_file || text.empty())
		{
			s.menu.cancel_leave("The launcher can't be asked from here.");
			publish();
			return;
		}
		// Written whole and renamed into place, so the launcher never reads half a request.
		const std::string path = settings_path_text(*s.navigate_file);
		fs::pending_file file(path);
		const bool written = file.file && file.file.write(text.data(), text.size()) == text.size();
#ifdef _WIN32
		// Grant the launcher foreground access before publishing its request; it may react immediately.
		if (written && chosen == menu::entry::launcher)
			AllowSetForegroundWindow(ASFW_ANY);
#endif
		if (!written || !file.commit())
		{
			gui_log.error("PBD menu: the request for %s could not be written to %s (%s)", target, path, fs::g_tls_error);
			s.menu.cancel_leave("The launcher could not be asked. Try again, or close the game.");
			publish();
			return;
		}
		// The menu stays up saying so and holds the controller until the launcher ends this run.
		s.leave_by_ms = steady_ms() + settings_leave_grace_ms;
		publish();
		gui_log.notice("PBD menu: %s chosen; the launcher was asked to end this %s (%s)", target, std::string(menu::context()), path);
	}

	void settings_menu::publish()
	{
		auto& s = m_state;
		if (s.published == s.menu.revision)
			return;
		s.published = s.menu.revision;
		pbd::settings_menu::publish(pbd::settings_menu::make_view(s.model, s.menu, pbd::settings_menu::context()));
	}

	void settings_menu::write_changes()
	{
		auto& s = m_state;
		if (!s.started || !s.changes_file)
			return;
		const std::string text = pbd::settings_menu::changes_json(s.model);
		if (text == s.written)
			return;
		// Written whole and renamed into place, so the launcher never reads half a file.
		const std::string path = settings_path_text(*s.changes_file);
		fs::pending_file file(path);
		if (!file.file || file.file.write(text.data(), text.size()) != text.size() || !file.commit())
		{
			gui_log.error("PBD settings: the changes could not be written to %s (%s)", path, fs::g_tls_error);
			return;
		}
		s.written = text;
		gui_log.notice("PBD settings: changes written: %s", text);
	}

	std::optional<std::pair<float, float>> settings_menu::canvas_point(const QPointF& position)
	{
		const auto game = hooks::area(m_frame);
		const auto& viewport = game.viewport;
		if (!viewport.width() || !viewport.height())
			return std::nullopt;
		// The menu is drawn in physical pixels inside the aspect-correct game area, on the 1280x720 canvas.
		const auto [px, py] = hooks::pixels(m_frame, game, position);
		const float x = px - static_cast<float>(viewport.x1);
		const float y = py - static_cast<float>(viewport.y1);
		if (!std::isfinite(x) || !std::isfinite(y))
			return std::nullopt;
		return std::pair{x * 1280.f / static_cast<float>(viewport.width()), y * 720.f / static_cast<float>(viewport.height())};
	}

	bool settings_menu::pointer_event(QEvent* ev)
	{
		namespace menu = pbd::settings_menu;
		auto& s = m_state;
		const auto type = ev->type();
		if (type == QEvent::FocusOut)
		{
			menu::pointer_captured.store(false, std::memory_order_release);
			return false;
		}
		if (Emu.IsStopped() || !menu::pointer_owned())
			return false;
		const bool press = type == QEvent::MouseButtonPress || type == QEvent::MouseButtonDblClick;
		if (type == QEvent::Enter || type == QEvent::MouseMove || press || type == QEvent::MouseButtonRelease)
		{
			m_frame.m_mousehide_timer.stop();
			m_frame.m_show_mouse.store(true);
			m_frame.setCursor(Qt::ArrowCursor);
			if (press)
				menu::pointer_captured.store(true, std::memory_order_release);
			const auto* mouse = type == QEvent::Enter ? nullptr : static_cast<QMouseEvent*>(ev);
			if (s.menu.open)
			{
				if (type == QEvent::MouseButtonPress && mouse->button() == Qt::RightButton)
				{
					// A right click goes back as Circle does.
					result(s.menu.back(s.model));
				}
				else if (type != QEvent::MouseButtonRelease)
				{
					// QWindow delivers the second press of a double click before its double-click notice: that notice only hovers,
					// so two fast clicks change a setting twice, not three times.
					const QPointF position = mouse ? mouse->position() : static_cast<QEnterEvent*>(ev)->position();
					const bool click = type == QEvent::MouseButtonPress && mouse->button() == Qt::LeftButton;
					if (const auto point = canvas_point(position))
						result(s.menu.pointer(s.model, point->first, point->second, click));
				}
				publish();
			}
			// A press made in the menu owns its release: neither the game nor a Practice menu under it sees the button go up.
			if (type == QEvent::MouseButtonRelease && mouse->buttons() == Qt::NoButton)
			{
				menu::pointer_captured.store(false, std::memory_order_release);
				pbd::practice::mouse::captured_buttons.store(false, std::memory_order_release);
			}
			return true;
		}
		if (type == QEvent::Wheel)
		{
			if (s.menu.open)
			{
				const auto delta = static_cast<QWheelEvent*>(ev)->angleDelta().y();
				if (delta)
					result(s.menu.wheel(s.model, delta > 0));
				publish();
			}
			return true;
		}
		return type == QEvent::Leave;
	}
} // namespace pbd::game_window
