#pragma once

#include "Emu/Io/pad_types.h"
#include "Emu/PBD/pbd_controller_mapper.h"
#include "Emu/PBD/pbd_input_taps.h"
#include "util/types.hpp"

#include <array>
#include <functional>
#include <memory>
#include <string>

namespace pbd::controllers
{
	class backend;
}

namespace pbd::test
{
	struct controller_ports_fixture;
}

namespace pbd::pads
{
	class spectator_watch;

	// This PC's controller through PBD's controller backend (pbd_controller_backend.h) on Player 1's port, and offline
	// play's second player on Player 2's when the input config leaves that port empty. To the game each is one stable PS3
	// pad; only the report underneath changes. Except poll(), callers hold the pad lock.
	class controller_ports
	{
	public:
		using port_pads = std::array<std::shared_ptr<Pad>, CELL_PAD_MAX_PORT_NUM>;

		// The ports when PBD_CONTROLLER_CONFIG names a usable configuration, never in the VSH; null otherwise.
		static std::unique_ptr<controller_ports> create();
		explicit controller_ports(std::unique_ptr<pbd::controllers::backend> backend);
		~controller_ports();

		// The ports a PBD device takes in one Init.
		struct ports
		{
			u32 controller = CELL_PAD_MAX_PORT_NUM;
			u32 second = CELL_PAD_MAX_PORT_NUM;

			bool contains(u32 port) const
			{
				return port == controller || port == second;
			}
		};
		// Init, before a takeover changes Player 1's config: Player 1 is the first port the input config gives a pad, unless
		// it is virtual. Forgets the previous pads.
		ports choose(const std::function<bool(u32 port)>& is_virtual);
		// Init: the chosen port's new pad becomes the PBD device's.
		void bind(const ports& chosen, u32 port, const std::shared_ptr<Pad>& pad);
		// How many ports a PBD device holds.
		u32 connected() const;

		// After the copilots, without the pad lock: sample devices/settings, then lock only to apply the completed report.
		void poll(const port_pads& pads, spectator_watch* watcher);
		// This PC's controller as one reader that reads it once per frame sees it: the buttons held now and those pressed
		// since that reader's previous read. 0 for any other pad.
		u16 read_taps(input_taps::reader reader, const Pad& pad);
		// After the game's read of a pad: the taps this PC's controller held for that read have been seen, so it shows the
		// buttons held now until the next tap.
		void game_read(const Pad& pad);

	private:
		friend struct pbd::test::controller_ports_fixture;
		std::unique_ptr<pbd::controllers::backend> m_backend;
		struct local_port
		{
			std::shared_ptr<Pad> pad;
			input_taps::tap_buffer taps;
		};
		std::array<local_port, 2> m_ports;
		std::string m_locked_controller;

		void apply_reports(const std::array<pbd::controllers::state, 2>& reports);
		local_port* find(const Pad& pad);
	};
} // namespace pbd::pads
