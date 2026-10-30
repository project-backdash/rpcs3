#pragma once

#include "Emu/Cell/pbd_controller_takeover.h"
#include "Emu/Io/pad_types.h"
#include "Emu/Io/pad_config_types.h"
#include "util/types.hpp"

#include <array>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

class PadHandlerBase;

namespace pbd::pads
{
	// Controller takeover (Emu/Cell/pbd_controller_takeover.h), in Practice, replays and spectating only: a button press
	// on another pad hands it Player 1. The other pads are polled through handler instances of its own, since the pad
	// thread's handlers belong to their own threads.
	class takeover_watch
	{
	public:
		using port_pads = std::array<std::shared_ptr<Pad>, CELL_PAD_MAX_PORT_NUM>;

		// The watch when this session offers takeover; null when PBD's own controllers are in use, in the VSH, or with
		// another PC.
		static std::unique_ptr<takeover_watch> create(bool pbd_controllers);

		// Before the pads are rebuilt: a pad that took Player 1 over stands in for the configured one, with its handler's
		// default mapping.
		void apply_config();
		// From a takeover until the reset that applies it has run: the game's ports stay connected through that reset.
		bool applying() const;
		// Once the pads are rebuilt: the new Player 1's hold starts counting from here.
		void ports_rebuilt();
		// Keeps the new Player 1's buttons out of the game, sticks centred, until it has released all of them, so the press
		// that took over never also acts in the game. Under the pad lock, after the copilots.
		void hold_new_player(const std::shared_ptr<Pad>& pad);
		// Looks for a press on another pad; one asks for the pad reset that applies the takeover.
		void poll(const port_pads& pads);

	private:
		struct watch_state
		{
			pbd::controller_takeover::detector detector;
			std::vector<std::pair<pad_handler, std::shared_ptr<PadHandlerBase>>> handlers;
			std::map<pad_handler, std::vector<std::string>> devices;
			bool handlers_ready = false;
			u64 next_poll_ms = 0, next_list_ms = 0;
			// The pad that took Player 1 over; empty while the configured one plays. Every Init points Player 1 at it.
			std::optional<pbd::controller_takeover::device_id> taken;
			// Player 1 as the input config file has it: when that pad takes back over, Player 1 returns to its own mapping.
			std::optional<pbd::controller_takeover::device_id> configured;
			// From a takeover until the reset that applies it has run.
			bool applying = false;
			// From a takeover until the new Player 1 has released every button (hold_new_player).
			pbd::controller_takeover::release_hold hold;
			// The session has no Player 1 pad to take over (logged once).
			bool inert_logged = false;
		};
		void prepare_handlers();
		void update_device_list(u64 now);
		std::optional<pbd::controller_takeover::device_id> find_controller(const port_pads& pads, const pbd::controller_takeover::device_id& player1, u64 now);
		void apply_takeover(const pbd::controller_takeover::device_id& chosen, const pbd::controller_takeover::device_id& player1, u64 now);
		watch_state m_watch;
	};
} // namespace pbd::pads
