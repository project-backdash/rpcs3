#include "stdafx.h"
#include "pbd_pad_takeover.h"
#include "pad_thread.h"
#include "Emu/Io/PadHandler.h"
#include "Emu/Io/pad_config.h"
#include "Emu/System.h"

#include <algorithm>

namespace pbd::pads
{
	std::unique_ptr<takeover_watch> takeover_watch::create(bool pbd_controllers)
	{
		// Practice, replays and spectating only, never with a second PC.
		if (pbd_controllers || Emu.IsVsh() || !pbd::controller_takeover::enabled())
			return nullptr;
		input_log.notice("PBD takeover: a button press on another pad hands it Player 1 in this session");
		return std::make_unique<takeover_watch>();
	}

	bool takeover_watch::applying() const
	{
		return m_watch.applying;
	}

	void takeover_watch::ports_rebuilt()
	{
		// Start the first-poll grace period after rebinding; device enumeration can outlast the entire period.
		if (m_watch.applying && m_watch.hold.active())
		{
			m_watch.hold.start(pbd::controller_takeover::steady_ms());
		}

		m_watch.applying = false;
	}

	void takeover_watch::hold_new_player(const std::shared_ptr<Pad>& pad)
	{
		// Every reader of the pads (cellPad, the latch's own pad word, the in-game menus) reads these copies under the pad lock.
		if (!m_watch.hold.active())
			return;

		bool pressed = false;

		if (pad && pad->is_connected())
		{
			for (const Button& button : pad->m_buttons)
			{
				pressed |= button.m_pressed;
			}
		}

		if (m_watch.hold.observe(pbd::controller_takeover::steady_ms(), pressed) && pad)
		{
			for (ButtonExternal& button : pad->m_buttons_external)
			{
				button.m_pressed = false;
				button.m_value = 0;
			}

			for (AnalogStickExternal& stick : pad->m_sticks_external)
			{
				stick.m_value = 128;
			}
		}
	}

	void takeover_watch::apply_config()
	{
		auto& watch = m_watch;
		cfg_player* player = g_cfg_input.player[0];
		watch.configured = pbd::controller_takeover::device_id{static_cast<u32>(player->handler.get()), player->device.to_string()};

		if (!watch.taken)
		{
			return;
		}

		// The handler's default mapping: the configured pad's own mapping was made for that pad.
		const pad_handler type = static_cast<pad_handler>(watch.taken->handler);
		player->handler.set(type);
		player->device.set(watch.taken->device);
		std::shared_ptr<PadHandlerBase> defaults;
		pad_thread::InitPadConfig(player->config, type, defaults);
		input_log.notice("PBD takeover: Player 1 is %s '%s' (configured: %s '%s')", type, watch.taken->device,
			static_cast<pad_handler>(watch.configured->handler), watch.configured->device);
	}

	static bool pressed_on(PadHandlerBase& handler, const std::string& name)
	{
		bool pressed = false;
		handler.get_next_button_press(name,
			[&pressed](std::map<std::string, u16>&& buttons, std::array<std::pair<std::string, u16>, 2>&&, std::string, u32, pad_preview_values&&, pad_capabilities&&)
			{
				pressed = !buttons.empty();
			},
			nullptr, PadHandlerBase::gui_call_type::normal, {});
		return pressed;
	}

	static bool mapped_active(const std::shared_ptr<Pad>& pad)
	{
		bool active = false;

		if (pad && !pad->ldd && pad->m_pad_handler != pad_handler::null && pad->is_connected())
		{
			for (const Button& button : pad->m_buttons)
			{
				active |= button.m_pressed;
			}

			for (const AnalogStick& stick : pad->m_sticks)
			{
				active |= stick.m_value < 64 || stick.m_value > 192;
			}
		}

		return active;
	}

	void takeover_watch::prepare_handlers()
	{
		auto& watch = m_watch;
		if (!watch.handlers_ready)
		{
			watch.handlers_ready = true;

			// Exclude the multimedia joystick handler: it duplicates XInput pads and reports off-centre resting axes.
			static constexpr std::array watched{
#ifdef _WIN32
				pad_handler::xinput,
#endif
				pad_handler::ds4,
				pad_handler::dualsense,
#ifdef HAVE_SDL3
				pad_handler::sdl,
#endif
			};

			for (const pad_handler type : watched)
			{
				std::shared_ptr<PadHandlerBase> handler = pad_thread::GetHandler(type);

				if (handler && handler->Init())
				{
					watch.handlers.emplace_back(type, std::move(handler));
				}
			}

			input_log.notice("PBD takeover: watching %d pad handler(s)", watch.handlers.size());
		}
	}

	void takeover_watch::update_device_list(u64 now)
	{
		auto& watch = m_watch;
		if (now >= watch.next_list_ms)
		{
			watch.next_list_ms = now + 2000;

			for (auto& [type, handler] : watch.handlers)
			{
				auto& names = watch.devices[type];
				names.clear();

				for (const pad_list_entry& entry : handler->list_devices())
				{
					if (!entry.is_buddy_only)
					{
						names.push_back(entry.name);
					}
				}
			}
		}
	}

	std::optional<pbd::controller_takeover::device_id> takeover_watch::find_controller(const port_pads& pads, const pbd::controller_takeover::device_id& player1, u64 now)
	{
		namespace takeover = pbd::controller_takeover;
		auto& watch = m_watch;
		bool player1_active = mapped_active(pads[0]);

		// Read Player 1 raw too: an unmapped button can appear under another handler's listing of the same pad.
		for (auto& [type, handler] : watch.handlers)
		{
			if (static_cast<u32>(type) == player1.handler)
			{
				player1_active |= pressed_on(*handler, player1.device);
			}
		}

		// Exclude pads assigned to other ports and count their activity to reject duplicate handler listings.
		std::vector<takeover::device_id> bound;

		for (usz i = 1; i < g_cfg_input.player.size() && i < pads.size(); i++)
		{
			const cfg_player* other = g_cfg_input.player[i];

			if (other->handler.get() != pad_handler::null)
			{
				bound.push_back({static_cast<u32>(other->handler.get()), other->device.to_string()});
				player1_active |= mapped_active(pads[i]);
			}
		}

		std::vector<takeover::reading> others;

		for (auto& [type, handler] : watch.handlers)
		{
			for (const std::string& name : watch.devices[type])
			{
				takeover::device_id id{static_cast<u32>(type), name};

				if (id == player1 || std::find(bound.cbegin(), bound.cend(), id) != bound.cend())
				{
					continue;
				}

				const bool pressed = pressed_on(*handler, name);
				others.push_back({std::move(id), pressed});
			}
		}
		return watch.detector.observe(now, player1_active, others);
	}

	void takeover_watch::apply_takeover(const pbd::controller_takeover::device_id& chosen, const pbd::controller_takeover::device_id& player1, u64 now)
	{
		namespace takeover = pbd::controller_takeover;
		auto& watch = m_watch;
		// The configured pad regains its saved mapping; other pads use their handler defaults on the next locked reset.
		if (watch.configured && chosen == *watch.configured)
		{
			watch.taken.reset();
		}
		else
		{
			watch.taken = chosen;
		}

		watch.applying = true;
		watch.hold.start(now);
		pad::g_reset = true;
		input_log.success("PBD takeover: %s '%s' takes Player 1 over from %s '%s'", static_cast<pad_handler>(chosen.handler), chosen.device,
			static_cast<pad_handler>(player1.handler), player1.device);
		takeover::announce(chosen.device);
	}

	void takeover_watch::poll(const port_pads& pads)
	{
		namespace takeover = pbd::controller_takeover;
		auto& watch = m_watch;
		const u64 now = takeover::steady_ms();

		if (now < watch.next_poll_ms)
		{
			return;
		}

		watch.next_poll_ms = now + 16;

		// A virtual or unconfigured Player 1 cannot bind a replacement pad, so leave its handlers unopened.
		if (!watch.configured || static_cast<pad_handler>(watch.configured->handler) == pad_handler::null || !pads[0] || pads[0]->ldd)
		{
			if (!watch.inert_logged)
			{
				watch.inert_logged = true;
				input_log.notice("PBD takeover: off in this session (Player 1 has no configured pad, or is a virtual pad)");
			}

			return;
		}
		prepare_handlers();
		for (auto& [type, handler] : watch.handlers)
			handler->process();
		update_device_list(now);
		const cfg_player* player = g_cfg_input.player[0];
		const takeover::device_id player1{static_cast<u32>(player->handler.get()), player->device.to_string()};
		const auto chosen = find_controller(pads, player1, now);
		if (chosen)
			apply_takeover(*chosen, player1, now);
	}
} // namespace pbd::pads
