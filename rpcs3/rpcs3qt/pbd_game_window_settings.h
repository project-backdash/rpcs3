#pragma once

#include "Emu/PBD/pbd_settings_runtime.h"
#include "Emu/PBD/pbd_controller_display.h"
#include "util/types.hpp"

#include <QTimer>

#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <utility>

class gs_frame;
class QEvent;
class QPointF;

namespace pbd::game_window
{
	// The in-game Settings menu (Emu/Cell/pbd_settings_menu.h) in Practice, replays and spectating, on the game window's
	// thread. Its timer reads the controller there, so the menu answers while the game waits or is paused.
	class settings_menu
	{
	public:
		// The menu when this run offers it, already looking for an open request; null otherwise.
		static std::unique_ptr<settings_menu> create(gs_frame& frame);
		explicit settings_menu(gs_frame& frame);
		// The choices stand even when the game closes with the menu open.
		~settings_menu();
		settings_menu(const settings_menu&) = delete;
		settings_menu& operator=(const settings_menu&) = delete;

		// Esc opens the menu, goes back a level in it and closes it.
		void escape();
		// True when the menu took this pointer event (it draws over every other PBD panel).
		bool pointer_event(QEvent* ev);
		// The volume keys. menu is null when this run offers no menu.
		static void volume_key(settings_menu* menu, int step);

	private:
		// The model the menu edits, its navigation, and what the changes file last held.
		struct state
		{
			pbd::settings_menu::model model;
			pbd::settings_menu::controller menu;
			pbd::controllers::display input_display;
			// The first open takes the session's starting values, which the changes file is measured against.
			bool started = false;
			// Closed, still holding the controller until every button is up.
			bool releasing = false;
			u64 published = 0;
			u64 last_flip_ms = 0;
			std::optional<std::filesystem::path> changes_file = pbd::settings_menu::changes_path();
			// What the changes file holds (nothing changed yet: no file is written).
			std::string written;
			// Where Return to Launcher and Exit Game write their request (PBD_NAVIGATE_REQUEST); without it Esc opens Settings directly.
			std::optional<std::filesystem::path> navigate_file = pbd::settings_menu::navigate_path();
			// After either is chosen the launcher ends this run as its own End or Stop would. If the game still runs at
			// leave_by_ms (no launcher took the request), it closes itself as the window's close button would.
			u64 leave_by_ms = 0;
			bool shutdown_asked = false;
			// The audio outputs are listed once per open, when the settings page is first shown (tick).
			bool outputs_listed = false;
		};

		gs_frame& m_frame;
		state m_state;
		QTimer m_timer;

		void tick();
		void grant_launcher_foreground();
		void stop_polling();
		void poll_closed(const std::optional<u16>& held);
		void refresh_inputs();
		void list_outputs();
		void take_values();
		void open(bool from_escape);
		void choose_leave(u32 row);
		void result(pbd::settings_menu::effect effect);
		void apply_video();
		void close();
		void navigate();
		void publish();
		void write_changes();
		std::optional<std::pair<float, float>> canvas_point(const QPointF& position);
	};
} // namespace pbd::game_window
