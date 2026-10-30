#pragma once

#include "Emu/PBD/pbd_settings_runtime.h"

#include <QWindow>

#include <string>
#include <vector>

// The devices the in-game Settings menu offers, named as the launcher stores them.
namespace pbd::settings_devices
{
	// The monitors with the identity and label the launcher uses (tools/game_displays.py): "monitor:" + the monitor's device
	// interface name (it survives unplugging), labelled "<Windows display number>: <width> x <height>", primary first as Qt
	// orders its screens. `current` is the monitor the game window is on.
	struct displays
	{
		std::vector<pbd::settings_menu::option> list;
		std::string current;
	};

	// Windows' default output first, then the devices the audio backend lists, with the IDs the launcher saves (WASAPI
	// endpoint IDs) and their names.
	std::vector<pbd::settings_menu::option> audio_outputs();
	displays game_displays(WId window);
} // namespace pbd::settings_devices
