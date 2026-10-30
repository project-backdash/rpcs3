#pragma once

// Private SDL device state and sampling for pbd_controller_backend.cpp.
struct pbd::controllers::backend::impl
{
	std::string config;
	bool standalone = false, ready = false, lock = false;
	std::int64_t stamp = -1;
	std::uint64_t last_reload = 0;
	std::uint64_t controls_revision = 0, controls_published = 0;
	std::optional<pbd::controllers::settings::values> controls_override;
	QJsonObject settings;
	QJsonArray pads;
	std::array<std::vector<std::string>, count> keys;
#ifdef _WIN32
	std::shared_ptr<pbd::raw_keyboard::sink> keyboard;
#endif
	selector selection;
	// Offline Player 2 locks to its Start-joined seat; other modes select among devices Player 1 is not using.
	selector second{std::string()};
	// Offline play: each local player keeps its controller (pbd_player_seats.h), and each controller its own layout once
	// Button setup changed it there (`layouts`; until then player 1 has the launcher's and player 2 the default).
	bool seated = false;
	seats::table players;
	pbd::controllers::settings::layouts layouts;
	std::uint64_t seats_shown = 0;
	std::uint32_t seats_opened = 0;
#ifdef PBD_DIAGNOSTIC_BUILD
	std::uint64_t test_pads_read_us = 0;
	std::vector<std::pair<std::string, std::uint16_t>> test_pads;
#endif
	std::string requested;
	state last_report;
	std::uint64_t last_poll_us = 0;
	bool focused = false;
	std::vector<source> sources;
	impl()
	{
		for (unsigned i = 0; i < count; ++i)
			if (!default_keys[i].empty())
				keys[i].push_back(std::string(default_keys[i]));
		constexpr std::array<std::string_view, 4> arrows{"Up", "Down", "Left", "Right"};
		for (unsigned i = 0; i < arrows.size(); ++i)
			keys[up + i].push_back(std::string(arrows[i]));
	}
#ifdef HAVE_SDL3
	struct device
	{
		SDL_Joystick* joystick = nullptr;
		SDL_Gamepad* gamepad = nullptr;
		std::string id, key, name, family, button_type;
		layout defaults;
		std::array<std::optional<binding>, 4> stick_bindings;
		pbd::practice::controls::touchpad_click click;
		pbd::clips::gesture clip;
		bool home_down = false;
	};
	std::map<SDL_JoystickID, device> devices;
	static constexpr std::array<SDL_GamepadButton, 16> sdl_buttons{
		SDL_GAMEPAD_BUTTON_SOUTH, SDL_GAMEPAD_BUTTON_EAST, SDL_GAMEPAD_BUTTON_WEST, SDL_GAMEPAD_BUTTON_NORTH,
		SDL_GAMEPAD_BUTTON_LEFT_SHOULDER, SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER, SDL_GAMEPAD_BUTTON_INVALID, SDL_GAMEPAD_BUTTON_INVALID,
		SDL_GAMEPAD_BUTTON_BACK, SDL_GAMEPAD_BUTTON_START, SDL_GAMEPAD_BUTTON_LEFT_STICK, SDL_GAMEPAD_BUTTON_RIGHT_STICK,
		SDL_GAMEPAD_BUTTON_GUIDE, SDL_GAMEPAD_BUTTON_TOUCHPAD, SDL_GAMEPAD_BUTTON_DPAD_UP, SDL_GAMEPAD_BUTTON_DPAD_DOWN};

	void open(SDL_JoystickID instance)
	{
		device d;
		if (SDL_IsGamepad(instance))
			d.gamepad = SDL_OpenGamepad(instance);
		d.joystick = d.gamepad ? SDL_GetGamepadJoystick(d.gamepad) : SDL_OpenJoystick(instance);
		if (!d.joystick)
			return;
		char guid[33]{};
		SDL_GUIDToString(SDL_GetJoystickGUID(d.joystick), guid, sizeof(guid));
		const std::string mode = SDL_GetJoystickConnectionState(d.joystick) == SDL_JOYSTICK_CONNECTION_WIRELESS ? "wireless" : "wired";
		d.key = "sdl3:" + std::string(guid) + ":" + std::to_string(SDL_GetJoystickVendor(d.joystick)) + ":" + std::to_string(SDL_GetJoystickProduct(d.joystick)) + ":" + mode;
		// The type key saves mappings; the unit and device path keep identical pads separate.
		const auto unit = text(SDL_GetJoystickSerial(d.joystick));
		auto identity = unit + "|" + text(SDL_GetJoystickPath(d.joystick));
		if (identity == "|")
			identity = std::to_string(instance);
		d.id = d.key + "@" + QCryptographicHash::hash(QByteArray::fromStdString(identity), QCryptographicHash::Sha256).toHex().left(16).toStdString();
		d.name = text(SDL_GetJoystickName(d.joystick));
		d.family = "xbox";
		if (d.gamepad)
		{
			switch (SDL_GetGamepadType(d.gamepad))
			{
			case SDL_GAMEPAD_TYPE_PS3: d.button_type = "ps3"; break;
			case SDL_GAMEPAD_TYPE_PS4: d.button_type = "ps4"; break;
			case SDL_GAMEPAD_TYPE_PS5: d.button_type = "ps5"; break;
			default: break;
			}
			const auto label = SDL_GetGamepadButtonLabel(d.gamepad, SDL_GAMEPAD_BUTTON_SOUTH);
			if (label == SDL_GAMEPAD_BUTTON_LABEL_CROSS)
				d.family = "playstation";
			else if (label == SDL_GAMEPAD_BUTTON_LABEL_B)
				d.family = "switch";
			if (SDL_GetJoystickVendor(d.joystick) == 0x28de)
				d.family = "steamdeck";
			read_gamepad_bindings(d);
		}
		else if (SDL_GetJoystickVendor(d.joystick) == 0x054c || SDL_GetJoystickVendor(d.joystick) == 0x1532 || SDL_GetJoystickVendor(d.joystick) == 0x0f0d)
		{
			d.family = "playstation";
			constexpr std::array<unsigned, 14> sony{square, cross, circle, triangle, l1, r1, l2, r2, select, start, l3, r3, home, touchpad};
			for (unsigned i = 0; i < sony.size(); ++i)
				d.defaults[sony[i]] = binding{binding::button, i};
			d.defaults[up] = binding{binding::hat, 0, SDL_HAT_UP};
			d.defaults[down] = binding{binding::hat, 0, SDL_HAT_DOWN};
			d.defaults[left] = binding{binding::hat, 0, SDL_HAT_LEFT};
			d.defaults[right] = binding{binding::hat, 0, SDL_HAT_RIGHT};
		}
		if (SDL_GetJoystickType(d.joystick) == SDL_JOYSTICK_TYPE_ARCADE_STICK)
			d.button_type = "arcade";
		devices.emplace(instance, std::move(d));
	}

	void read_devices()
	{
		if (standalone)
			SDL_UpdateJoysticks();
		else
			sdl_instance::get_instance().pump_events();
		int size = 0;
		if (auto ids = SDL_GetJoysticks(&size))
		{
			for (int i = 0; i < size; ++i)
				if (!devices.contains(ids[i]))
					open(ids[i]);
			SDL_free(ids);
		}
		for (auto it = devices.begin(); it != devices.end();)
		{
			auto& d = it->second;
			if (!SDL_JoystickConnected(d.joystick))
			{
				if (d.gamepad)
					SDL_CloseGamepad(d.gamepad);
				else
					SDL_CloseJoystick(d.joystick);
				it = devices.erase(it);
				continue;
			}
			read_device(d, it->first);
			++it;
		}
	}
	void read_device(device& d, SDL_JoystickID instance)
	{
		raw_state raw;
		for (int i = 0; i < std::min(128, SDL_GetNumJoystickButtons(d.joystick)); ++i)
			raw.buttons.push_back(SDL_GetJoystickButton(d.joystick, i));
		for (int i = 0; i < std::min(64, SDL_GetNumJoystickAxes(d.joystick)); ++i)
			raw.axes.push_back(SDL_GetJoystickAxis(d.joystick, i) / 32767.f);
		for (int i = 0; i < std::min(16, SDL_GetNumJoystickHats(d.joystick)); ++i)
			raw.hats.push_back(SDL_GetJoystickHat(d.joystick, i));
		state value;
		if (d.gamepad)
		{
			for (unsigned i = 0; i < sdl_buttons.size(); ++i)
				if (sdl_buttons[i] != SDL_GAMEPAD_BUTTON_INVALID && SDL_GetGamepadButton(d.gamepad, sdl_buttons[i]))
					value.buttons |= bit(i);
			if (SDL_GetGamepadButton(d.gamepad, SDL_GAMEPAD_BUTTON_DPAD_LEFT))
				value.buttons |= bit(left);
			if (SDL_GetGamepadButton(d.gamepad, SDL_GAMEPAD_BUTTON_DPAD_RIGHT))
				value.buttons |= bit(right);
			if (SDL_GetGamepadAxis(d.gamepad, SDL_GAMEPAD_AXIS_LEFT_TRIGGER) > 8192)
				value.buttons |= bit(l2);
			if (SDL_GetGamepadAxis(d.gamepad, SDL_GAMEPAD_AXIS_RIGHT_TRIGGER) > 8192)
				value.buttons |= bit(r2);
			for (unsigned i = 0; i < 4; ++i)
				value.sticks[i] = SDL_GetGamepadAxis(d.gamepad, static_cast<SDL_GamepadAxis>(i)) / 32767.f;
		}
		else
			value = map(value, raw, d.defaults);
		apply_device_layout(d, raw, value);
		const bool click = (value.buttons & bit(touchpad)) != 0;
		const auto clip_button = settings.value("clips").toObject().value("padButton").toString("create");
		const bool versus = seated && pbd::versus::input::active.load(std::memory_order_acquire);
		bool clip_pressed = false;
		// Create/Share (the game's Select) held alone; any other button with it is a game combo, such as Select + O.
		const bool create = clip_button == "create";
		if (create)
			clip_pressed = value.buttons == bit(select) && !seated;
		if (clip_button == "touchpad")
			clip_pressed = click;
		if ((create || clip_button == "share") && d.gamepad)
			clip_pressed |= SDL_GetGamepadButton(d.gamepad, SDL_GAMEPAD_BUTTON_MISC1);
		if (clip_button == "home" && !versus)
			clip_pressed = (value.buttons & bit(home)) != 0;
		if (clip_button.startsWith("button-"))
		{
			bool ok = false;
			const auto index = clip_button.mid(7).toUInt(&ok);
			clip_pressed = ok && index < raw.buttons.size() && raw.buttons[index];
		}
		// The pad's PS / Home / Guide button opens the game's menu on its press, unless Clips was given that button.
		const bool home_pressed = (value.buttons & bit(home)) != 0;
		if (!standalone && !versus && clip_button != "home" && home_pressed && !d.home_down)
			pbd::settings_menu::request_top_open();
		d.home_down = home_pressed;
		if (versus)
			value = pbd::versus::input::home_as_start(value);
		if (!standalone)
		{
			const auto action = d.clip.update(clip_pressed, SDL_GetTicks(), create);
			if (action == pbd::clips::action::save)
				pbd::clips::request_save();
			if (action == pbd::clips::action::record)
				pbd::clips::request_record();
		}
		const auto mode = settings.value("gameSettings").toObject().value("controls").toObject().value("touchpadClick").toString("split").toStdString();
		d.click.mode = pbd::practice::controls::parse_touchpad_mode(mode);
		bool finger = false;
		float x = 0.f, y = 0.f, pressure = 0.f;
		if (d.gamepad)
			SDL_GetGamepadTouchpadFinger(d.gamepad, 0, 0, &finger, &x, &y, &pressure);
		const bool clip_touchpad = !standalone && clip_button == "touchpad";
		const auto touch = d.click.update(click && !clip_touchpad, finger ? std::optional<float>(x) : std::nullopt, true);
		if (touch & 0x0100)
			value.buttons |= bit(select);
		if (touch & 0x0800)
			value.buttons |= bit(start);
		publish_device_sample(d, instance, raw, value);
	}
#endif

	void reload()
	{
		const auto now = static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count());
		if (now - last_reload < 500)
			return;
		last_reload = now;
		const QFileInfo info(QString::fromStdString(config));
		if (stamp == info.lastModified().toMSecsSinceEpoch())
			return;
		QFile file(info.filePath());
		if (!file.open(QIODevice::ReadOnly) || file.size() > 1024 * 1024)
			return;
		const auto doc = QJsonDocument::fromJson(file.readAll());
		if (!doc.isObject())
			return;
		stamp = info.lastModified().toMSecsSinceEpoch();
		const auto fresh = doc.object();
		const auto mapping = fresh.value("gameSettings").toObject().value("controls").toObject().value("buttonMapping").toObject();
		if (!controls_override || mapping != settings.value("gameSettings").toObject().value("controls").toObject().value("buttonMapping").toObject() ||
			fresh.value("controllerLastUsed") != settings.value("controllerLastUsed"))
		{
			controls_override.reset();
			layouts.first = read_mapping(mapping);
		}
		settings = fresh;
		const auto requested_source = settings.value("controllerLastUsed").toString().toStdString();
		if (requested_source != requested && (requested_source == "keyboard" || requested_source.starts_with("sdl3:")))
		{
			requested = requested_source;
			selection = selector(requested);
		}
		lock = settings.value("controllerLock").toBool(false);
		const auto saved = settings.value("gameSettings").toObject().value("controls").toObject().value("keyboardKeys").toObject();
		for (unsigned i = 0; i < count; ++i)
		{
			keys[i].clear();
			const auto name = QString::fromUtf8(names[i].data(), static_cast<int>(names[i].size()));
			if (saved.contains(name))
				for (const auto& key : saved.value(name).toArray())
					keys[i].push_back(key.toString().toStdString());
			else if (!default_keys[i].empty())
				keys[i].push_back(std::string(default_keys[i]));
			if (!saved.contains(name) && i >= up)
			{
				constexpr std::array<std::string_view, 4> arrows{"Up", "Down", "Left", "Right"};
				keys[i].push_back(std::string(arrows[i - up]));
			}
		}
	}
#ifdef HAVE_SDL3
	void read_gamepad_bindings(device& d)
	{
		int size = 0;
		if (auto bindings = SDL_GetGamepadBindings(d.gamepad, &size))
		{
			for (int i = 0; i < size; ++i)
			{
				const auto& b = *bindings[i];
				if (b.output_type == SDL_GAMEPAD_BINDTYPE_AXIS && b.input_type == SDL_GAMEPAD_BINDTYPE_AXIS)
				{
					const auto index = static_cast<unsigned>(b.output.axis.axis);
					const binding axis{binding::axis, static_cast<unsigned>(b.input.axis.axis), b.input.axis.axis_max > b.input.axis.axis_min ? 1 : -1};
					if (index < d.stick_bindings.size())
						d.stick_bindings[index] = axis;
					if (b.output.axis.axis == SDL_GAMEPAD_AXIS_LEFT_TRIGGER)
						d.defaults[l2] = axis;
					if (b.output.axis.axis == SDL_GAMEPAD_AXIS_RIGHT_TRIGGER)
						d.defaults[r2] = axis;
				}
				if (b.output_type != SDL_GAMEPAD_BINDTYPE_BUTTON)
					continue;
				unsigned a = count;
				for (unsigned n = 0; n < sdl_buttons.size(); ++n)
					if (sdl_buttons[n] == b.output.button)
						a = n;
				if (b.output.button == SDL_GAMEPAD_BUTTON_DPAD_LEFT)
					a = left;
				if (b.output.button == SDL_GAMEPAD_BUTTON_DPAD_RIGHT)
					a = right;
				if (a == count)
					continue;
				if (b.input_type == SDL_GAMEPAD_BINDTYPE_BUTTON)
					d.defaults[a] = binding{binding::button, static_cast<unsigned>(b.input.button)};
				if (b.input_type == SDL_GAMEPAD_BINDTYPE_HAT)
					d.defaults[a] = binding{binding::hat, static_cast<unsigned>(b.input.hat.hat), b.input.hat.hat_mask};
				if (b.input_type == SDL_GAMEPAD_BINDTYPE_AXIS)
					d.defaults[a] = binding{binding::axis, static_cast<unsigned>(b.input.axis.axis), b.input.axis.axis_max > b.input.axis.axis_min ? 1 : -1};
			}
			SDL_free(bindings);
		}
	}

	void apply_device_layout(const device& d, const raw_state& raw, state& value)
	{
		const auto taught = settings.value("padLayouts").toObject().value(QString::fromStdString(d.key)).toObject();
		layout overrides;
		for (unsigned i = 0; i < count; ++i)
			overrides[i] = read_binding(taught.value(QString::fromUtf8(names[i].data(), static_cast<int>(names[i].size()))).toObject());
		for (unsigned i = 0; i < count; ++i)
			if (d.defaults[i])
				for (const auto& assigned : overrides)
					if (assigned && same_input(*assigned, *d.defaults[i]))
						value.buttons &= ~bit(i);
		for (unsigned i = 0; i < d.stick_bindings.size(); ++i)
			if (d.stick_bindings[i])
				for (const auto& assigned : overrides)
					if (assigned && assigned->type == binding::axis && assigned->index == d.stick_bindings[i]->index)
						value.sticks[i] = 0.f;
		value = map(value, raw, overrides);
		for (unsigned a = up; a <= right; ++a)
			if (overrides[a] && overrides[a]->type == binding::axis && overrides[a]->index < raw.axes.size())
			{
				const auto& binding = *overrides[a];
				const float sign = a == up || a == left ? -1.f : 1.f;
				value.sticks[a == up || a == down ? 1 : 0] = std::clamp((raw.axes[binding.index] - binding.rest) * binding.direction * sign, -1.f, 1.f);
			}
	}
	void publish_device_sample(const device& d, SDL_JoystickID instance, const raw_state& raw, const state& value)
	{
		auto family = settings.value("controllerGlyphs").toObject().value(QString::fromStdString(d.key)).toString(QString::fromStdString(d.family)).toStdString();
		QJsonArray logical;
		for (unsigned i = 0; i < count; ++i)
			logical.append((value.buttons & bit(i)) != 0);
		QJsonArray sticks;
		for (const float v : value.sticks)
			sticks.append(v);
		QJsonObject pad{{"id", QString::fromStdString(d.id)}, {"key", QString::fromStdString(d.key)}, {"name", QString::fromStdString(d.name)},
			{"index", static_cast<int>(instance)}, {"family", QString::fromStdString(family)}, {"logical", logical}, {"axes", sticks}, {"raw", json_raw(raw)}};
		if (!d.button_type.empty())
			pad.insert("buttonType", QString::fromStdString(d.button_type));
		pads.append(pad);
		sources.push_back({d.id, value});
	}
#endif
#ifdef PBD_DIAGNOSTIC_BUILD
	// Scripted controllers for diagnostic builds: PBD_TEST_PADS names a text file of "<name> <hex game word>" lines that a test
	// rewrites while the game runs. Each line is a controller holding that word's buttons and reaches everything a real one
	// does (the players, the menus, Controller Setup). The file is read at most every 8 ms.
	void read_test_pads(std::uint64_t now_us)
	{
		static const std::string file = []
		{
			const char* value = pbd::env::find("PBD_TEST_PADS");
			return std::string(value ? value : "");
		}();
		if (file.empty())
			return;
		if (now_us - test_pads_read_us >= 8000)
		{
			test_pads_read_us = now_us;
			test_pads.clear();
			QFile in(QString::fromStdString(file));
			if (in.open(QIODevice::ReadOnly | QIODevice::Text))
				for (const auto& line : QString::fromUtf8(in.readAll()).split('\n', Qt::SkipEmptyParts))
				{
					const auto parts = line.simplified().split(' ');
					bool ok = false;
					const auto word = parts.size() == 2 ? parts[1].toUInt(&ok, 16) : 0u;
					if (ok)
						test_pads.emplace_back(parts[0].toStdString(), static_cast<std::uint16_t>(word));
				}
		}
		for (const auto& [name, word] : test_pads)
		{
			state value;
			for (unsigned i = 0; i < count; ++i)
				if (ps3_bits[i] && (word & ps3_bits[i]))
					value.buttons |= bit(i);
			const auto id = "test:" + name;
			QJsonArray logical;
			for (unsigned i = 0; i < count; ++i)
				logical.append((value.buttons & bit(i)) != 0);
			pads.append(QJsonObject{{"id", QString::fromStdString(id)}, {"key", QString::fromStdString(id)},
				{"name", QString::fromStdString("Test pad " + name)}, {"index", -1}, {"family", "playstation"}, {"logical", logical},
				{"axes", QJsonArray{0, 0, 0, 0}}});
			sources.push_back({id, value});
		}
	}
#endif

	bool present(std::string_view id) const
	{
		return std::any_of(sources.begin(), sources.end(), [&](const source& input)
			{
				return input.id == id;
			});
	}

	std::string name_of(std::string_view id) const
	{
		if (id == "keyboard")
			return "Keyboard";
		for (const auto& pad : pads)
			if (pad.toObject().value("id").toString().toStdString() == id)
				return pad.toObject().value("name").toString().toStdString();
		return std::string(id);
	}

	// Offline play (pbd_player_seats.h): Start on a controller without a player takes the first free player, and the
	// controller being asked, or still letting go of the press that joined or answered, gives the game nothing. The keyboard
	// is one more controller, with Enter as its Start; it is never unplugged, so it leaves a player when the VS mode opens
	// again or by changing sides.
	void seat_players()
	{
		std::vector<seats::pad> list;
		for (const auto& input : sources)
		{
			// Select asks which player a controller is only at the Versus character select, and not from a player's open
			// Controller Setup, where it removes a button. A lever that reports as a left stick moves through the question.
			auto folded = input.value;
			fold_left_stick(folded);
			auto buttons = folded.buttons;
			if (!pbd::versus::input::select.load(std::memory_order_acquire))
				buttons &= ~bit(select);
			for (unsigned i = 0; i < seats::players; ++i)
				if (players.player(i) == input.id && pbd::versus::input::panel[i].load(std::memory_order_acquire))
					buttons &= ~bit(select);
			list.push_back({input.id, name_of(input.id), buttons});
		}
		const auto result = players.update(list);
		if (const auto opened = pbd::versus::input::opened.load(std::memory_order_acquire); opened != seats_opened)
		{
			seats_opened = opened;
			players.reset();
		}
		for (const auto& input : sources)
			if ((input.value.buttons & bit(start)) && !players.seated(input.id))
				for (unsigned i = 0; i < seats::players; ++i)
					if (players.claim(i, input.id))
					{
						pbd::controller_takeover::announce_text("Player " + std::to_string(i + 1) + ": " + name_of(input.id));
						break;
					}
		if (result == seats::outcome::setup)
			pbd::settings_menu::request_controls_open(players.answered());
		if (result == seats::outcome::seated)
			pbd::controller_takeover::announce_text(
				"Player " + std::to_string(players.answered_player() + 1) + ": " + name_of(players.answered()));
		for (auto& input : sources)
			if (players.withheld(input.id))
				input.value = state{};
		if (const auto view = players.snapshot(); view.revision != seats_shown)
		{
			seats_shown = view.revision;
			seats::publish(view);
		}
	}

	void publish_players() const
	{
		std::array<display, seats::players> shown;
		for (unsigned i = 0; i < seats::players; ++i)
		{
			auto id = players.player(i);
			if (id.empty())
			{
				for (const auto& pad : pads)
				{
					const auto candidate = pad.toObject().value("id").toString().toStdString();
					if (!players.seated(candidate))
					{
						id = candidate;
						break;
					}
				}
				if (id.empty() && !players.seated("keyboard"))
					id = "keyboard";
			}
			if (id.empty())
				continue;
			shown[i].active = true;
			shown[i].device = id;
			shown[i].family = "keyboard";
			for (const auto& pad : pads)
				if (pad.toObject().value("id").toString().toStdString() == id)
				{
					shown[i].family = pad.toObject().value("family").toString().toStdString();
					shown[i].button_type = pad.toObject().value("buttonType").toString().toStdString();
				}
			for (unsigned k = 0; k < count; ++k)
				if (!keys[k].empty())
					shown[i].keys[k] = keys[k].front();
		}
		pbd::controllers::publish_players(std::move(shown));
	}

	// Player 1: its controller, whoever else presses. While it has none the keyboard works the game's menus, unless it is
	// player 2; at the Versus select a free player waits for a controller's Start.
	state sample_first(bool focused, bool typing)
	{
		const auto& first = players.player(0);
		if (!first.empty())
			return selection.sample(sources, focused, typing, first);
		std::vector<source> keyboard;
		if (!pbd::versus::input::select.load(std::memory_order_acquire))
			for (const auto& input : sources)
				if (input.keyboard && !players.seated(input.id))
					keyboard.push_back(input);
		return selection.sample(keyboard, focused, typing);
	}
};
