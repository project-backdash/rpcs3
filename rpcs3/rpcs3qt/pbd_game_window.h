#pragma once

#include "shortcut_settings.h"
#include "util/types.hpp"
#include "Utilities/geometry.h"

#include <utility>

class gs_frame;
class QEvent;
class QShowEvent;
class QKeySequence;
class QPointF;
class QWindow;

namespace pbd::game_window
{
	// The game window's PBD hooks that reach its private state (gs_frame names this struct a friend).
	struct hooks
	{
		// The launcher-hosted window, the in-game menus and the replay and Practice pointers see each event first. True
		// when one of them took it.
		static bool event(gs_frame& frame, QEvent* ev);
		static void create_controls(gs_frame& frame);
		static bool show_event(gs_frame& frame, QShowEvent* event);
		static bool shortcut(gs_frame& frame, gui::shortcuts::shortcut key, const QKeySequence& sequence);
		static bool show(gs_frame& frame);
		static void before_flip(gs_frame& frame, bool skip_frame);
		static void first_frame();
		static void window_closed();
		static void shutdown(gs_frame& frame);

		// The game's area of the window in physical pixels: the client size, and the aspect-correct viewport inside it.
		struct game_area
		{
			size2u dimensions;
			areau viewport;
		};
		static game_area area(gs_frame& frame);
		// A Qt position, in device-independent pixels, as physical pixels of the window.
		static std::pair<float, float> pixels(gs_frame& frame, const game_area& area, const QPointF& position);

	private:
		static bool hosted_event(gs_frame& frame, QEvent* ev);
		static bool menu_pointer(gs_frame& frame, QEvent* ev);
		static bool replay_pointer(gs_frame& frame, QEvent* ev);
		static bool practice_pointer(gs_frame& frame, QEvent* ev);
		static void practice_point(gs_frame& frame, const QPointF& position, bool choose);
	};

	// In a session the game runs on two PCs in lockstep, and some shortcuts would act on this PC alone; they are ignored.
	bool session_ignores_shortcut(gui::shortcuts::shortcut shortcut_key, const QKeySequence& key_sequence);

	// PBD_QUIET_WINDOW=1: the process plays the game unattended for the first-time setup (tools/make_cs_savestate.py)
	// while the player keeps using the PC, so its window stays beyond the right edge of the virtual desktop.
	bool quiet_window();
	// Keeps a quiet window off screen when it is shown; false (and nothing done) for any other run.
	bool keep_quiet_window_off_screen(QWindow& window);
	// Shows a quiet window off screen without taking the focus; false (and nothing done) for any other run.
	bool show_quiet_window(QWindow& window);
} // namespace pbd::game_window
