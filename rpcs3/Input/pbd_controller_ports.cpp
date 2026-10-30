#include "stdafx.h"
#include "pbd_controller_ports.h"
#include "pbd_controller_backend.h"
#include "pad_thread.h"
#include "pbd_pad_spectator.h"
#include "Emu/Io/pad_config.h"
#include "Emu/PBD/pbd_controller_display.h"
#include "Emu/PBD/pbd_env.h"
#include "Emu/System.h"
#include "Emu/pbd_session_mode.h"

#include <algorithm>
#include <chrono>
#include <cmath>

extern atomic_t<bool> g_game_window_focused;

namespace
{
	u64 steady_us()
	{
		return static_cast<u64>(std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now().time_since_epoch()).count());
	}

	void show_buttons(Pad& pad, u16 word)
	{
		for (auto& button : pad.m_buttons)
		{
			const u16 mask = button.m_offset == CELL_PAD_BTN_OFFSET_DIGITAL1 ? static_cast<u16>(button.m_outKeyCode << 8) : static_cast<u16>(button.m_outKeyCode);
			button.m_pressed = (word & mask) != 0;
			button.m_value = button.m_pressed ? 255 : 0;
		}
		pad.m_buttons_external.resize(pad.m_buttons.size());
		for (usz i = 0; i < pad.m_buttons.size(); ++i)
		{
			const auto& b = pad.m_buttons[i];
			pad.m_buttons_external[i] = {b.m_offset, b.m_outKeyCode, b.m_value, b.m_pressed};
		}
	}

	// One complete report, including the button and stick copies read under the pad lock.
	void feed_pad(const std::shared_ptr<Pad>& pad, const pbd::controllers::state& value, u16 word)
	{
		show_buttons(*pad, word);
		for (usz i = 0; i < pad->m_sticks.size() && i < value.sticks.size(); ++i)
		{
			const float v = std::isfinite(value.sticks[i]) ? value.sticks[i] : 0.f;
			pad->m_sticks[i].m_value = static_cast<u16>(std::clamp(128.f + v * 127.f, 0.f, 255.f));
			pad->m_sticks_external[i] = {pad->m_sticks[i].m_offset, pad->m_sticks[i].m_value};
		}
	}
} // namespace

namespace pbd::pads
{
	std::unique_ptr<controller_ports> controller_ports::create()
	{
		if (Emu.IsVsh())
			return nullptr;
		const char* config = pbd::env::find("PBD_CONTROLLER_CONFIG");
		if (!config || !*config)
			return nullptr;
		auto backend = std::make_unique<pbd::controllers::backend>(config);
		if (!backend->available())
			return nullptr;
		return std::make_unique<controller_ports>(std::move(backend));
	}

	controller_ports::controller_ports(std::unique_ptr<pbd::controllers::backend> backend)
		: m_backend(std::move(backend))
	{
	}

	controller_ports::~controller_ports() = default;

	controller_ports::ports controller_ports::choose(const std::function<bool(u32 port)>& is_virtual)
	{
		for (auto& port : m_ports)
			port.pad.reset();
		ports chosen;
		for (u32 i = 0; i < CELL_PAD_MAX_PORT_NUM; ++i)
			if (!is_virtual(i) && g_cfg_input.player[i]->handler.get() != pad_handler::null)
			{
				chosen.controller = i;
				break;
			}
		// Offline play: Player 2's port, left without a controller in the input config, is a second PBD device for a second
		// player on this PC (poll).
		if (chosen.controller == 0 && pbd::session_mode::offline_play() && !is_virtual(1) &&
			g_cfg_input.player[1]->handler.get() == pad_handler::null)
			chosen.second = 1;
		return chosen;
	}

	void controller_ports::bind(const ports& chosen, u32 port, const std::shared_ptr<Pad>& pad)
	{
		// One stable PS3 device; only the complete physical report underneath it changes.
		pad->Init(CELL_PAD_STATUS_CONNECTED | CELL_PAD_STATUS_ASSIGN_CHANGES, CELL_PAD_CAPABILITY_PS3_CONFORMITY | CELL_PAD_CAPABILITY_PRESS_MODE,
			CELL_PAD_DEV_TYPE_STANDARD, CELL_PAD_PCLASS_TYPE_STANDARD, 0, 0x054c, 0x0268, 50);
		for (unsigned a = 0; a < pbd::controllers::count; ++a)
		{
			const u16 mask = pbd::controllers::ps3_bits[a];
			if (mask)
				pad->m_buttons.emplace_back(mask > 255 ? CELL_PAD_BTN_OFFSET_DIGITAL1 : CELL_PAD_BTN_OFFSET_DIGITAL2,
					std::vector<std::set<u32>>{}, mask > 255 ? mask >> 8 : mask);
		}
		constexpr std::array<u32, 4> offsets{CELL_PAD_BTN_OFFSET_ANALOG_LEFT_X, CELL_PAD_BTN_OFFSET_ANALOG_LEFT_Y, CELL_PAD_BTN_OFFSET_ANALOG_RIGHT_X, CELL_PAD_BTN_OFFSET_ANALOG_RIGHT_Y};
		for (usz axis = 0; axis < offsets.size(); ++axis)
			pad->m_sticks[axis] = AnalogStick(offsets[axis], {}, {});
		m_ports[port == chosen.controller ? 0 : 1].pad = pad;
	}

	u32 controller_ports::connected() const
	{
		return (m_ports[0].pad ? 1 : 0) + (m_ports[1].pad ? 1 : 0);
	}

	void controller_ports::poll(const port_pads& pads, spectator_watch* watcher)
	{
		std::size_t local_players = 0;
		{
			std::lock_guard lock(pad::g_pad_mutex);
			local_players = std::count_if(pads.begin(), pads.end(), [](const auto& pad)
				{
					return pad && !pad->ldd && !pad->is_fake_pad && pad->m_pad_handler != pad_handler::null;
				});
		}
		if (local_players > 1 && m_backend->local_lock())
		{
			if (m_locked_controller.empty())
				m_locked_controller = pbd::controllers::read_display().device;
		}
		else
			m_locked_controller.clear();
		const auto value = m_backend->poll(g_game_window_focused, false, m_locked_controller);
		const auto second = m_ports[1].pad ? m_backend->poll_second(g_game_window_focused) : pbd::controllers::state{};
		const u16 word = pbd::controllers::canonical(value);
		if (watcher)
			watcher->publish(word);
		apply_reports({value, second});
	}

	void controller_ports::apply_reports(const std::array<pbd::controllers::state, 2>& reports)
	{
		const u64 now_us = steady_us();
		// Only completed reports cross the pad lock; SDL, files and menu settings stay on this thread outside it.
		std::lock_guard lock(pad::g_pad_mutex);
		for (usz i = 0; i < m_ports.size(); ++i)
			if (auto& port = m_ports[i]; port.pad)
			{
				port.taps.sample(pbd::controllers::canonical(reports[i]), now_us);
				// Each port keeps short taps until its own guest read consumes them.
				feed_pad(port.pad, reports[i], port.taps.peek(input_taps::reader::game, now_us));
			}
	}

	controller_ports::local_port* controller_ports::find(const Pad& pad)
	{
		for (auto& port : m_ports)
			if (port.pad.get() == &pad)
				return &port;
		return nullptr;
	}

	u16 controller_ports::read_taps(input_taps::reader reader, const Pad& pad)
	{
		if (auto* port = find(pad))
			return port->taps.read(reader, steady_us());
		return 0;
	}

	void controller_ports::game_read(const Pad& pad)
	{
		if (auto* port = find(pad))
		{
			port->taps.read(input_taps::reader::game, steady_us());
			show_buttons(*port->pad, port->taps.level());
		}
	}
} // namespace pbd::pads
