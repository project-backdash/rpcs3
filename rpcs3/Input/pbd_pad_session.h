#pragma once

#include "Emu/Io/pad_types.h"
#include "Emu/PBD/pbd_match_pad.h"
#include "util/types.hpp"

#include <array>
#include <functional>
#include <memory>

namespace pbd::pads
{
	using port_pads = std::array<std::shared_ptr<Pad>, CELL_PAD_MAX_PORT_NUM>;
	using port_flags = std::array<bool, CELL_PAD_MAX_PORT_NUM>;

	struct device_identity
	{
		u32 class_type;
		u16 vendor_id, product_id;
	};

	// Which of the two game ports this PC's player holds in a lobby match (Emu/PBD/pbd_match_pad.h). The ports swap while
	// this PC plays the second side. Called under the pad lock.
	class match_sides
	{
	public:
		// Only while the verified local match checkpoint is held.
		bool available(const port_pads& pads) const noexcept;
		bool bind(port_pads& pads, port_flags& connected, u32 role) noexcept;
		// Puts the ports back in their saved slots before a rebuild, and the bound side back once it is done.
		void swap_if_bound(port_pads& pads, port_flags& connected) noexcept;

	private:
		match_pad::assignment m_assignment;
	};

	// PBD_LDD_PADS="1" (comma list of ports 0..6) makes each listed port whose configured handler is Null a connected
	// virtual pad: the emulator's own LDD pad, reported to the game as a standard PS3 controller, reading neutral. The
	// netcode injects the network-controlled player's input at the game's update site; the pad exists so the game sees a
	// second controller (joining, character select, VS mode). A port with a real handler is never replaced (logged as an
	// error and left alone). make_virtual turns a port's pad into an LDD pad with that status when it is not one yet.
	void make_virtual_ports(port_pads& pads, const std::function<void(u32 port, const u32* status)>& make_virtual);

	// In a session the Home Menu would send DRAWING_BEGIN, SYSTEM_MENU_OPEN and BGMPLAYBACK_PLAY into this PC's game only
	// (and pause it when pause_during_home_menu is set), so the two PCs' games would diverge. Session input carries no PS
	// button, so ignoring the press takes nothing from the player. In offline play the PS button opens the in-game menu
	// (pbd_controller_backend.cpp) and the Home Menu would open over it. True there, logged once.
	bool home_menu_blocked();
} // namespace pbd::pads
