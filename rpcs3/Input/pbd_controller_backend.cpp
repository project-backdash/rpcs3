#include "stdafx.h"
#include "pbd_controller_backend.h"
#include "Emu/PBD/pbd_controller_display.h"
#include "Emu/PBD/pbd_controller_settings.h"
#include "Emu/PBD/pbd_env.h"
#include "Emu/PBD/pbd_player_seats.h"
#include "Emu/PBD/pbd_versus_input.h"
#include "Emu/pbd_session_mode.h"
#include "Emu/Cell/pbd_controller_takeover.h"
#include "Emu/PBD/pbd_clip_buffer.h"
#include "util/pbd_clips.h"
#include "Emu/Cell/pbd_practice_controls.h"
#include "Emu/PBD/pbd_settings_runtime.h"
#include "sdl_instance.h"
#include "rpcs3qt/pbd_raw_keyboard.h"
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QDateTime>
#include <QCryptographicHash>
#include <chrono>
#include <cstdio>
#include <thread>
#ifdef _WIN32
#include <Windows.h>
#endif
#ifdef HAVE_SDL3
#include <SDL3/SDL.h>
#include "pbd_gamecontrollerdb.h"
#endif

namespace pbd::controllers
{
	namespace
	{
		QJsonArray json_raw(const raw_state& raw)
		{
			QJsonArray buttons, axes, hats;
			for (const bool b : raw.buttons)
				buttons.append(b);
			for (const float a : raw.axes)
				axes.append(a);
			for (const unsigned h : raw.hats)
				hats.append(static_cast<int>(h));
			return {buttons, axes, hats};
		}
		std::string text(const char* value)
		{
			return value ? value : "";
		}
		constexpr std::array<std::string_view, count> default_keys{
			"J", "K", "U", "I", "O", "P", "L", ";", "Backspace", "Return", "F", "R", "", "", "W", "S", "A", "D"};
		std::optional<binding> read_binding(QJsonObject b)
		{
			const auto index = [&](const char* key)
			{
				return b.value(key).toInt(-1);
			};
			if (b.contains("button") && index("button") >= 0 && index("button") < 128)
				return binding{binding::button, static_cast<unsigned>(index("button"))};
			if (b.contains("hat") && index("hat") >= 0 && index("hat") < 16 && index("mask") > 0 && index("mask") <= 15)
				return binding{binding::hat, static_cast<unsigned>(index("hat")), index("mask")};
			if (b.contains("axis") && index("axis") >= 0 && index("axis") < 64 && (index("sign") == 1 || index("sign") == -1))
				return binding{binding::axis, static_cast<unsigned>(index("axis")), index("sign"), static_cast<float>(b.value("rest").toDouble())};
			return {};
		}
#ifdef _WIN32
		int virtual_key(std::string_view key)
		{
			if (key.size() == 1)
			{
				const SHORT code = VkKeyScanExW(static_cast<wchar_t>(key.front()), GetKeyboardLayout(0));
				return code == -1 ? 0 : code & 255;
			}
			static const std::map<std::string_view, int> codes{
				{"Up", VK_UP}, {"Down", VK_DOWN}, {"Left", VK_LEFT}, {"Right", VK_RIGHT}, {"Return", VK_RETURN}, {"Space", VK_SPACE},
				{"Backspace", VK_BACK}, {"Tab", VK_TAB}, {"Ins", VK_INSERT}, {"Del", VK_DELETE}, {"Home", VK_HOME}, {"End", VK_END},
				{"PgUp", VK_PRIOR}, {"PgDown", VK_NEXT}, {"Shift Left", VK_LSHIFT}, {"Shift Right", VK_RSHIFT}, {"Ctrl Left", VK_LCONTROL},
				{"Ctrl Right", VK_RCONTROL}, {"Alt Left", VK_LMENU}, {"Alt Right", VK_RMENU},
				{"Num++", VK_ADD}, {"Num+-", VK_SUBTRACT}, {"Num+*", VK_MULTIPLY}, {"Num+/", VK_DIVIDE}};
			if (key.size() == 5 && key.starts_with("Num+") && key.back() >= '0' && key.back() <= '9')
				return VK_NUMPAD0 + key.back() - '0';
			const auto found = codes.find(key);
			return found == codes.end() ? 0 : found->second;
		}
		bool foreground_process(unsigned pid)
		{
			DWORD foreground = 0;
			GetWindowThreadProcessId(GetForegroundWindow(), &foreground);
			return foreground == pid;
		}
#endif
		// The left stick also as the directions, so an analog stick moves the fighter and the menus.
		void fold_left_stick(state& value)
		{
			if (value.sticks[0] < -.5f)
				value.buttons |= bit(left);
			if (value.sticks[0] > .5f)
				value.buttons |= bit(right);
			if (value.sticks[1] < -.5f)
				value.buttons |= bit(up);
			if (value.sticks[1] > .5f)
				value.buttons |= bit(down);
		}
		// The launcher's layout (gameSettings.ts buttonMapping, or the Controls page's last change): each physical button's
		// action, so a face button or shoulder can be any attack or combination and a stick click with an action is that action.
		state map_actions(state value, const settings::layout& mapping)
		{
			const auto original = value.buttons;
			value.buttons &= ~(bit(square) | bit(triangle) | bit(cross) | bit(circle) | bit(l1) | bit(r1) | bit(l2) | bit(r2));
			constexpr std::array<unsigned, 10> physical{square, triangle, cross, circle, l1, r1, l2, r2, l3, r3};
			constexpr std::array<unsigned, 4> attacks{square, triangle, cross, circle};
			for (unsigned i = 0; i < physical.size(); ++i)
			{
				const bool click = physical[i] == l3 || physical[i] == r3;
				if (!click && !(original & bit(physical[i])))
					continue;
				const auto& assigned = mapping[i];
				std::uint32_t jobs = 0;
				for (const auto c : assigned)
					if (c >= '1' && c <= '4')
						jobs |= bit(attacks[c - '1']);
				if (click && jobs)
					value.buttons &= ~bit(physical[i]);
				if (original & bit(physical[i]))
					value.buttons |= jobs;
			}
			return value;
		}

		settings::layout read_mapping(const QJsonObject& mapping)
		{
			auto result = settings::values{}.mapping;
			for (unsigned i = 0; i < settings::button_slots.size(); ++i)
				result[i] = mapping.value(QString::fromUtf8(settings::button_slots[i].data(), static_cast<int>(settings::button_slots[i].size())))
				                .toString(QString::fromStdString(result[i]))
				                .toStdString();
			return result;
		}
	} // namespace

} // namespace pbd::controllers

#include "pbd_controller_backend_state.h"

namespace pbd::controllers
{

	backend::backend(std::string config, bool standalone) : m(std::make_unique<impl>())
	{
		m->config = std::move(config);
		m->standalone = standalone;
#ifdef _WIN32
		m->keyboard = pbd::raw_keyboard::shared_sink();
#endif
		m->seated = !standalone && pbd::session_mode::offline_play();
		if (!standalone)
			m->controls_revision = settings::current_revision();
		if (!standalone)
			if (const char* initial = pbd::env::find("PBD_CONTROLLER_INITIAL"); initial && *initial)
				m->selection = selector(initial);
		m->reload();
		if (!standalone && !pbd::env::find("PBD_CONTROLLER_INITIAL"))
		{
			const auto saved = m->settings.value("savedPad").toObject().value("deviceName").toString().toStdString();
			if (saved.starts_with("sdl3:"))
				m->selection = selector(saved);
		}
#ifdef HAVE_SDL3
		if (standalone)
		{
			SDL_SetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS, "1");
			SDL_SetHint(SDL_HINT_JOYSTICK_THREAD, "1");
			// Match the game's SDL device backends, so GUIDs and raw bindings agree in both windows.
#ifdef _WIN32
			SDL_SetHint(SDL_HINT_JOYSTICK_HIDAPI_PS3_SIXAXIS_DRIVER, "1");
			SDL_SetHint(SDL_HINT_JOYSTICK_HIDAPI_LG4FF, "0");
#else
			SDL_SetHint(SDL_HINT_JOYSTICK_HIDAPI_PS3, "1");
#endif
			m->ready = SDL_Init(SDL_INIT_GAMEPAD);
		}
		else
			m->ready = sdl_instance::get_instance().initialize();
		if (m->ready)
		{
			for (const auto block : gamecontrollerdb)
				if (auto io = SDL_IOFromConstMem(block.data(), block.size()))
					SDL_AddGamepadMappingsFromIO(io, true);
		}
#endif
	}
	backend::~backend()
	{
#ifdef HAVE_SDL3
		for (auto& [id, d] : m->devices)
			if (d.gamepad)
				SDL_CloseGamepad(d.gamepad);
			else
				SDL_CloseJoystick(d.joystick);
		if (m->standalone && m->ready)
			SDL_Quit();
#endif
	}
	bool backend::available() const
	{
		return m->ready;
	}
	bool backend::local_lock() const
	{
		return m->lock;
	}
	state backend::poll(bool focused, bool typing, std::string_view locked)
	{
		const auto now_us = static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now().time_since_epoch()).count());
		// The game samples at the same rate with or without the focus, since its controllers keep playing in the background
		// and a press and release between two samples is never seen. Only the launcher's stream slows down behind the game.
		if (focused == m->focused && now_us - m->last_poll_us < (focused || !m->standalone ? 2000 : 16000))
			return m->last_report;
		if (focused != m->focused)
			m->last_reload = 0;
		m->focused = focused;
		m->last_poll_us = now_us;
		m->reload();
		if (!m->standalone)
		{
			for (auto& change : settings::take(m->controls_revision))
			{
				// Offline play: the layout belongs to the controller the Controls page showed, and the players keep theirs.
				if (m->seated)
					m->layouts.saved[change.device] = change.mapping;
				else
				{
					m->controls_override = std::move(change);
					m->selection = selector(m->controls_override->device);
					m->layouts.first = m->controls_override->mapping;
				}
			}
		}
		m->sources.clear();
		m->pads = {};
#ifdef HAVE_SDL3
		if (m->ready)
			m->read_devices();
#endif
#ifdef PBD_DIAGNOSTIC_BUILD
		if (!m->standalone)
			m->read_test_pads(now_us);
#endif
		state keyboard;
#ifdef _WIN32
		if (focused)
			keyboard = sample_keyboard(m->keys, [&](std::string_view key)
				{
					return m->keyboard->returns().pressed(key, [](std::string_view name)
						{
							const int vk = virtual_key(name);
							return vk && (GetAsyncKeyState(vk) & 0x8000);
						});
				});
		// Escape opens the game's menu through its window, without adding a guest button.
		if (focused && (GetAsyncKeyState(VK_ESCAPE) & 0x8000))
			keyboard.buttons |= bit(home);
#endif
		m->sources.push_back({"keyboard", keyboard, true});
		if (m->seated)
		{
			m->seat_players();
			m->publish_players();
		}
		const std::string_view selected_lock = !locked.empty() && m->controls_override ? std::string_view(m->controls_override->device) : locked;
		auto value = m->seated ? m->sample_first(focused, typing) : m->selection.sample(m->sources, focused, typing, selected_lock);
		display d;
		d.active = true;
		d.device = m->selection.owner();
		d.family = "keyboard";
		for (const auto& pad : m->pads)
			if (pad.toObject().value("id").toString().toStdString() == d.device)
			{
				d.family = pad.toObject().value("family").toString().toStdString();
				d.button_type = pad.toObject().value("buttonType").toString().toStdString();
			}
		for (unsigned i = 0; i < count; ++i)
			if (!m->keys[i].empty())
				d.keys[i] = m->keys[i].front();
		publish(std::move(d));
		fold_left_stick(value);
		if (!m->standalone)
		{
			settings::menu_word.store(canonical(value), std::memory_order_release);
			settings::active.store(true, std::memory_order_release);
			if (now_us - m->controls_published >= 250000)
			{
				m->controls_published = now_us;
				settings::values controls;
				controls.device = m->selection.owner();
				controls.mapping = m->layouts.first;
				std::vector<settings::device> devices{{"keyboard", "Keyboard"}};
				for (const auto& pad : m->pads)
				{
					settings::device entry{pad.toObject().value("id").toString().toStdString(), pad.toObject().value("name").toString().toStdString()};
					devices.push_back(std::move(entry));
				}
				if (m->seated)
				{
					auto view = m->layouts.presentation(controls.device, std::move(devices), m->players.player(0));
					controls = std::move(view.current);
					devices = std::move(view.devices);
				}
				settings::publish(std::move(controls), std::move(devices), m->controls_revision);
			}
		}
		// Practice consumes its own raw action map after its menu and shortcuts have had their buttons.
		const bool raw_controls = pbd::env::find("PBD_PRACTICE_BUTTON_MAP") || pbd::env::find("PBD_REPLAY_VIEWER") ||
		                          pbd::env::find("PBD_SESSION_REPLAY") || pbd::env::find("PBD_REPLAY_V2_GAME") || pbd::env::find("PBD_SPECTATOR_CHANNEL");
		// In a fight a stick click with an action is that action; with none it stays the click it was. Offline play's Versus reads
		// the buttons themselves at its select (pbd_versus_input.h).
		if (m->seated)
			pbd::versus::input::words[0].store(canonical(value), std::memory_order_release);
		if (!m->standalone && !raw_controls && (!m->seated || !pbd::versus::input::select.load(std::memory_order_acquire)))
			value = map_actions(value, m->seated ? m->layouts.for_player(m->players.player(0), 0) : m->layouts.first);
		m->last_report = value;
		return value;
	}
	state backend::poll_second(bool focused)
	{
		state value;
		// Offline play's player 2 is its own controller, or nobody until a controller joins with Start (seat_players).
		if (m->seated)
		{
			if (!m->players.player(1).empty())
				value = m->second.sample(m->sources, focused, false, m->players.player(1));
		}
		else
			value = m->second.sample(without(m->sources, m->selection.owner()), focused, false);
		fold_left_stick(value);
		// Either player works the in-game menu, which holds both ports while it is open (Emu/Cell/Modules/pbd_pad_read.cpp).
		if (!m->standalone)
			settings::menu_word.fetch_or(canonical(value), std::memory_order_acq_rel);
		// Offline play's player 2 plays with its controller's own layout (Button setup) or the default one.
		if (m->seated)
		{
			pbd::versus::input::words[1].store(canonical(value), std::memory_order_release);
			if (!pbd::versus::input::select.load(std::memory_order_acquire))
				value = map_actions(value, m->layouts.for_player(m->players.player(1), 1));
		}
		return value;
	}
	std::string backend::snapshot() const
	{
		return QJsonDocument(QJsonObject{{"version", 1}, {"pads", m->pads}, {"owner", QString::fromStdString(m->selection.owner())},
								 {"sequence", static_cast<qint64>(m->selection.serial())}})
		    .toJson(QJsonDocument::Compact)
		    .toStdString();
	}
	int run_controller_stream(int argc, char** argv)
	{
#if defined(HAVE_SDL3) && defined(_WIN32)
		if (argc != 4)
			return 2;
		char* end = nullptr;
		const unsigned pid = static_cast<unsigned>(std::strtoul(argv[3], &end, 10));
		if (!pid || !end || *end)
			return 2;
		HANDLE parent = OpenProcess(SYNCHRONIZE, FALSE, pid);
		if (!parent)
			return 2;
		backend input(argv[2], true);
		if (!input.available())
		{
			CloseHandle(parent);
			return 3;
		}
		std::string previous;
		while (WaitForSingleObject(parent, 0) == WAIT_TIMEOUT)
		{
			SDL_PumpEvents();
			input.poll(foreground_process(pid), true);
			const auto sample = input.snapshot();
			if (sample != previous)
			{
				if (std::printf("%s\n", sample.c_str()) < 0 || std::fflush(stdout) != 0)
					break;
				previous = sample;
			}
			std::this_thread::sleep_for(std::chrono::milliseconds(4));
		}
		CloseHandle(parent);
		return 0;
#else
		return 3;
#endif
	}
} // namespace pbd::controllers
