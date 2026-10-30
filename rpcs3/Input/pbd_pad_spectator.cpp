#include "stdafx.h"
#include "pbd_pad_spectator.h"
#include "pad_thread.h"
#include "Emu/Io/PadHandler.h"
#include "Emu/Io/pad_config.h"
#include "Emu/PBD/pbd_env.h"
#include "Emu/System.h"
#include "Emu/Cell/pbd_controller_takeover.h"

#include <atomic>

extern atomic_t<bool> g_game_window_focused;

namespace
{
	// The watcher's controllers as one canonical word (digital1 << 8 | digital2); 0 outside spectating.
	std::atomic<u16> g_pbd_spectator_word{0};

	bool pbd_spectator_requested()
	{
		const char* text = pbd::env::find("PBD_SPECTATOR_CHANNEL");
		return text && *text;
	}
} // namespace

u16 pbd_spectator_pad_word() noexcept
{
	return g_pbd_spectator_word.load(std::memory_order_acquire);
}

namespace pbd::pads
{
	std::unique_ptr<spectator_watch> spectator_watch::create()
	{
		// The watcher's own controllers drive its screen, never the watched game.
		if (Emu.IsVsh() || !pbd_spectator_requested())
			return nullptr;
		input_log.notice("PBD spectating: the watcher's controller drives its own screen (button bar and chat)");
		return std::make_unique<spectator_watch>();
	}

	void spectator_watch::publish(u16 word)
	{
		g_pbd_spectator_word.store(word, std::memory_order_release);
	}

	void spectator_watch::prepare_handlers()
	{
		auto& watch = m_watch;
		if (!watch.ready)
		{
			watch.ready = true;

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

				if (!handler || !handler->Init())
				{
					continue;
				}

				// The handler's own default mapping names its buttons (A, Cross or South for Cross, and so on).
				cfg_pad defaults;
				handler->init_config(&defaults);

				watch_state::family family;
				family.handler = std::move(handler);

				const std::array<std::pair<const cfg::string*, u16>, 18> mapped{{
					{&defaults.cross, 0x0040},
					{&defaults.circle, 0x0020},
					{&defaults.square, 0x0080},
					{&defaults.triangle, 0x0010},
					{&defaults.l1, 0x0004},
					{&defaults.r1, 0x0008},
					{&defaults.l2, 0x0001},
					{&defaults.r2, 0x0002},
					{&defaults.start, 0x0800},
					{&defaults.select, 0x0100},
					{&defaults.up, 0x1000},
					{&defaults.right, 0x2000},
					{&defaults.down, 0x4000},
					{&defaults.left, 0x8000},
					{&defaults.ls_up, 0x1000},
					{&defaults.ls_right, 0x2000},
					{&defaults.ls_down, 0x4000},
					{&defaults.ls_left, 0x8000},
				}};

				for (const auto& [setting, bit] : mapped)
				{
					const std::string name = setting->def_to_string();

					if (!name.empty())
					{
						family.buttons.emplace_back(name, bit);
					}
				}

				watch.families.push_back(std::move(family));
			}

			input_log.notice("PBD spectating: reading the watcher's controllers through %d pad handler(s)", watch.families.size());
		}
	}

	void spectator_watch::update_device_list(u64 now)
	{
		auto& watch = m_watch;
		if (now >= watch.next_list_ms)
		{
			watch.next_list_ms = now + 2000;

			for (auto& family : watch.families)
			{
				family.devices.clear();

				for (const pad_list_entry& entry : family.handler->list_devices())
				{
					if (!entry.is_buddy_only)
					{
						family.devices.push_back(entry.name);
					}
				}
			}
		}
	}

	u16 spectator_watch::read_word()
	{
		auto& watch = m_watch;
		// Every listed pad of every family adds what it shows: a button beyond its handler's press threshold, or the left stick
		// pushed past its threshold (one direction per stick, as the handler reports it). A pad that is not connected answers
		// "disconnected" without calling back and shows nothing; one with no new data answers "no_data" without calling back and
		// keeps what it showed last. The same pad listed by two families shows the same buttons twice, which changes nothing.
		u16 word = 0;

		for (auto& family : watch.families)
		{
			for (const std::string& name : family.devices)
			{
				u16 shown = 0;
				const PadHandlerBase::connection status = family.handler->get_next_button_press(name,
					[&shown, &family](std::map<std::string, u16>&& buttons, std::array<std::pair<std::string, u16>, 2>&& sticks, std::string, u32, pad_preview_values&&, pad_capabilities&&)
					{
						for (const auto& [button, bit] : family.buttons)
						{
							if (buttons.contains(button) || (!sticks[0].first.empty() && sticks[0].first == button))
							{
								shown |= bit;
							}
						}
					},
					nullptr, PadHandlerBase::gui_call_type::normal, {});

				u16& last = family.held[name];

				if (status == PadHandlerBase::connection::no_data)
				{
					shown = last;
				}

				last = shown;
				word |= shown;
			}
		}
		return word;
	}

	void spectator_watch::poll()
	{
		auto& watch = m_watch;
		const u64 now = pbd::controller_takeover::steady_ms();

		if (now < watch.next_poll_ms)
		{
			return;
		}

		watch.next_poll_ms = now + 8;

		// Only while the game window itself has the focus. The watcher's profile allows background input, so input being allowed
		// says nothing here, and the launcher's lobby reads the same pad while it has the focus: a Circle pressed there (to go back,
		// to close a panel) must never also stop the watch.
		if (!g_game_window_focused)
		{
			watch.focused = false;
			g_pbd_spectator_word.store(0, std::memory_order_release);
			return;
		}

		prepare_handlers();
		// Each handler processes new devices and events before its listed pads are read.
		for (auto& family : watch.families)
			family.handler->process();
		update_device_list(now);
		const u16 word = read_word();

		// A button already held when the game window takes the focus (the press in the launcher that brought it forward) is no
		// press on the watcher's screen: it stays out until it is released.
		if (!watch.focused)
		{
			watch.focused = true;
			watch.withheld = word;
		}

		watch.withheld &= word;
		g_pbd_spectator_word.store(static_cast<u16>(word & ~watch.withheld), std::memory_order_release);
	}
} // namespace pbd::pads
