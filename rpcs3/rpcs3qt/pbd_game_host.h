#pragma once

#include <memory>
#include <cstdint>
#include "Emu/RSX/pbd_display_rate.h"

class QWindow;
class QEvent;

namespace pbd::game_host
{
	class window
	{
		struct state;
		std::unique_ptr<state> m_state;

	public:
		explicit window(QWindow& game);
		~window();
		bool managed() const;
		bool configured() const;
		bool attached() const;
		bool focused() const;
		bool fullscreen() const;
		std::uintptr_t native_handle() const;
		int width_px() const;
		int height_px() const;
		display_rate::sample refresh_rate() const;
		bool visible() const;
		void show_loading();
		void first_frame();
		void toggle_fullscreen();
		bool event(QEvent* event);
		void shutdown();
	};
} // namespace pbd::game_host
