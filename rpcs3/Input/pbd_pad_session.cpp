#include "stdafx.h"
#include "pbd_pad_session.h"
#include "pad_thread.h"
#include "pbd_controller_ports.h"
#include "pbd_pad_takeover.h"
#include "pbd_pad_spectator.h"
#include "Emu/Cell/pbd_rollback_session.h"
#include "Emu/System.h"
#include "Emu/Cell/timers.hpp"
#include "Emu/Io/PadHandler.h"
#include "Emu/Io/pad_config.h"
#include "Emu/PBD/pbd_env.h"
#include "Emu/pbd_session_mode.h"

#include <algorithm>
#include <chrono>
#include <string_view>
#include <utility>

namespace pbd::pads
{
	namespace
	{
		std::array<match_pad::port, 2> match_ports(const port_pads& pads) noexcept
		{
			std::array<match_pad::port, 2> ports{};
			// Only the two admitted game ports may supply a connected device while their assignment changes.
			for (usz index = ports.size(); index < pads.size(); ++index)
				if (pads[index] && pads[index]->is_connected())
					return ports;
			for (u32 index = 0; index < ports.size(); ++index)
			{
				const auto& device = pads[index];
				if (!device || !device->is_connected() || device->is_fake_pad || device->is_copilot() || !device->copilots.empty())
					continue;
				ports[index].real = !device->ldd && device->m_pad_handler != pad_handler::null;
				ports[index].neutral = device->ldd && device->m_pad_handler == pad_handler::null &&
				                       device->ldd_data.len == 8 && !device->ldd_data.button[CELL_PAD_BTN_OFFSET_DIGITAL1] &&
				                       !device->ldd_data.button[CELL_PAD_BTN_OFFSET_DIGITAL2];
				for (u32 axis = 0; axis < 4 && ports[index].neutral; ++axis)
					ports[index].neutral = device->ldd_data.button[CELL_PAD_BTN_OFFSET_ANALOG_RIGHT_X + axis] == 128;
			}
			return ports;
		}
	} // namespace

	bool match_sides::available(const port_pads& pads) const noexcept
	{
		const auto* requested = pbd::env::find("PBD_MATCH_STANDBY_CHANNEL");
		return requested && *requested && m_assignment.available(match_ports(pads));
	}

	bool match_sides::bind(port_pads& pads, port_flags& connected, u32 role) noexcept
	{
		if (!available(pads) || !m_assignment.bind(match_ports(pads), role))
			return false;
		swap_if_bound(pads, connected);
		return true;
	}

	void match_sides::swap_if_bound(port_pads& pads, port_flags& connected) noexcept
	{
		if (!m_assignment.swapped())
			return;
		std::swap(pads[0], pads[1]);
		std::swap(connected[0], connected[1]);
	}

	void make_virtual_ports([[maybe_unused]] port_pads& pads, [[maybe_unused]] const std::function<void(u32 port, const u32* status)>& make_virtual)
	{
#ifdef _WIN32
		if (const wchar_t* requested = pbd::env::find_wide("PBD_LDD_PADS"); requested && *requested)
		{
			std::wstring_view text(requested);
			while (!text.empty())
			{
				const auto comma = text.find(L',');
				const std::wstring_view item = text.substr(0, comma);
				text = comma == std::wstring_view::npos ? std::wstring_view{} : text.substr(comma + 1);
				if (item.size() != 1 || item[0] < L'0' || item[0] > L'6')
				{
					input_log.error("PBD_LDD_PADS: invalid port entry (expected single digits 0..6 separated by commas)");
					continue;
				}
				const u32 port = static_cast<u32>(item[0] - L'0');
				if (g_cfg_input.player[port]->handler != pad_handler::null)
				{
					input_log.error("PBD_LDD_PADS: port %d has handler %s configured and is NOT made virtual", port, g_cfg_input.player[port]->handler.get());
					continue;
				}
				auto& pad = pads[port];
				if (!pad->ldd)
				{
					const u32 status = CELL_PAD_STATUS_CONNECTED | CELL_PAD_STATUS_ASSIGN_CHANGES;
					make_virtual(port, &status);
				}
				// Neutral report: no digital buttons, sticks centred, the length/report word the guest expects.
				std::fill(std::begin(pad->ldd_data.button), std::end(pad->ldd_data.button), 0);
				pad->ldd_data.len = 8;                    // CELL_PAD_LEN_CHANGE_DEFAULT (cellPad.h): digital words + four stick axes
				pad->ldd_data.button[1] = (0x7 << 4) | 4; // the report word cellPadGetData writes for that length
				for (u32 i = 0; i < 4; ++i)
					pad->ldd_data.button[CELL_PAD_BTN_OFFSET_ANALOG_RIGHT_X + i] = 128;
				input_log.success("PBD_LDD_PADS: port %d is a connected virtual pad (LDD, neutral) for the network-controlled player", port);
			}
		}
#endif
	}

	bool home_menu_blocked()
	{
		if (!pbd::session_mode::active() && !pbd::session_mode::offline_play())
			return false;

		static bool logged = false;

		if (!std::exchange(logged, true))
		{
			if (pbd::session_mode::active())
				input_log.warning("PBD session: ignoring the PS button / Home Menu request; the menu acts on this PC only and would break the session.");
			else
				input_log.notice("PBD offline play: the PS button opens the in-game menu instead.");
		}

		return true;
	}
} // namespace pbd::pads

bool pad_thread::pbd_match_side_available() const noexcept
{
	return m_pbd_match.available(m_pads);
}

bool pad_thread::pbd_bind_match_side(u32 role) noexcept
{
	return m_pbd_match.bind(m_pads, m_pads_connected, role);
}

u16 pad_thread::pbd_read_taps(pbd::input_taps::reader reader, const Pad& pad)
{
	return m_pbd_ports ? m_pbd_ports->read_taps(reader, pad) : 0;
}

void pad_thread::pbd_game_read(const Pad& pad)
{
	if (m_pbd_ports)
		m_pbd_ports->game_read(pad);
}

void pad_thread::pbd_update_controllers()
{
	// PROJECT BACKDASH: a new Player 1's held press stays out of the game, and PBD's controllers update their ports
	// (Input/pbd_pad_takeover.h, Input/pbd_controller_ports.h).
	if (m_pbd_takeover)
	{
		std::lock_guard lock(pad::g_pad_mutex);
		m_pbd_takeover->hold_new_player(m_pads[0]);
	}
	if (m_pbd_ports)
		m_pbd_ports->poll(m_pads, m_pbd_spectator.get());
}

void pad_thread::pbd_create_controllers()
{
	// PROJECT BACKDASH: its controllers, controller takeover and a watcher's controllers (Input/pbd_*.h).
	m_pbd_ports = pbd::pads::controller_ports::create();
	m_pbd_takeover = pbd::pads::takeover_watch::create(m_pbd_ports != nullptr);
	m_pbd_spectator = pbd::pads::spectator_watch::create();
}

void pad_thread::pbd_poll_controllers()
{
	// PBD: another pad's press may take Player 1 over; it asks for the reset below, which applies it.
	if (m_pbd_takeover && Emu.IsRunning())
	{
		m_pbd_takeover->poll(m_pads);
	}

	// PBD spectating: the watcher's controllers, for its own screen.
	if (m_pbd_spectator && !m_pbd_ports && Emu.IsRunning())
	{
		m_pbd_spectator->poll();
	}
}

void pad_thread::pbd_note_controller_poll()
{
	g_pbd_pad_poll_ns.store(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count(), std::memory_order_relaxed);
}

pbd::pads::controller_ports::ports pad_thread::pbd_configure_ports(const std::function<bool(u32)>& is_virtual)
{
	// PROJECT BACKDASH: this PC's PBD controllers take their ports, and a pad that took Player 1 over stands in for the
	// configured one (Input/pbd_controller_ports.h, Input/pbd_pad_takeover.h).
	pbd::pads::controller_ports::ports pbd_ports;
	if (m_pbd_ports)
		pbd_ports = m_pbd_ports->choose(is_virtual);
	if (m_pbd_takeover)
		m_pbd_takeover->apply_config();

	return pbd_ports;
}

void pad_thread::pbd_refresh_connection(u32 port, Pad& pad, pbd::pads::device_identity previous)
{
	// If the user changes the emulated controller, then simulate unplugging and plugging in a new controller. Not for a
	// PBD controller takeover: every port the game had stays connected through it, so the game never sees Player 1
	// unplugged (the new handler marks the port connected again on its first poll).
	const bool takeover_applying = m_pbd_takeover && m_pbd_takeover->applying();
	if (m_pads_connected[port] && !takeover_applying && (previous.class_type != pad.m_class_type || previous.vendor_id != pad.m_vendor_id || previous.product_id != pad.m_product_id))
	{
		pad.m_disconnection_timer = get_system_time() + 30'000ull;
	}
	if (takeover_applying && m_pads_connected[port] && pad.m_pad_handler != pad_handler::null)
	{
		pad.m_port_status |= CELL_PAD_STATUS_CONNECTED;
	}
}

void pad_thread::pbd_release_controllers()
{
	// The watcher's screen sees no button while input is not allowed (another window has the focus).
	if (m_pbd_spectator)
	{
		m_pbd_spectator->publish(0);
	}
}

void pad_thread::pbd_open_home_menu()
{
	if (!pbd::pads::home_menu_blocked())
		open_home_menu();
}
