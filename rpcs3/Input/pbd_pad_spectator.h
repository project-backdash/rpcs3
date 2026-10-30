#pragma once

#include "util/types.hpp"

#include <map>
#include <memory>
#include <string>
#include <utility>
#include <vector>

class PadHandlerBase;

// The watcher's controllers as the game's 16-bit word (digital1 << 8 | digital2) for its own screen; 0 outside spectating.
u16 pbd_spectator_pad_word() noexcept;

namespace pbd::pads
{
	// Spectating (Emu/Cell/pbd_spectator_ui.h): the watcher's controllers, polled through handler instances of its own
	// (the families a player is likely to hold, as the takeover watches them), each read with its handler's default
	// mapping. The update latch reads the word for the watcher's screen only; the spectator's game ports stay virtual and
	// neutral, so nothing here reaches the watched game.
	class spectator_watch
	{
	public:
		// The watch when this run spectates (PBD_SPECTATOR_CHANNEL), never in the VSH; null otherwise.
		static std::unique_ptr<spectator_watch> create();

		// Reads the watcher's controllers while the game window has the focus.
		void poll();
		// Publishes the word the watcher's screen reads: PBD's own controllers' word, or 0 while input is not allowed.
		void publish(u16 word);

	private:
		struct watch_state
		{
			struct family
			{
				std::shared_ptr<PadHandlerBase> handler;
				// The default mapping's name of each button the watcher's screen reads, with its bit in the game's canonical word.
				std::vector<std::pair<std::string, u16>> buttons;
				std::vector<std::string> devices;
				// What each listed pad showed at its last read with data: a pad with nothing new since (SDL between two of its events)
				// still holds it, so a held button never reads as a release and then as a second press.
				std::map<std::string, u16> held;
			};
			std::vector<family> families;
			bool ready = false;
			// Whether the game window had the focus at the last poll, and the buttons held when it took it, kept out until released.
			bool focused = false;
			u16 withheld = 0;
			u64 next_poll_ms = 0, next_list_ms = 0;
		};
		void prepare_handlers();
		void update_device_list(u64 now);
		u16 read_word();
		watch_state m_watch;
	};
} // namespace pbd::pads
