#pragma once

#include "Emu/PBD/pbd_controller_mapper.h"
#include <memory>

namespace pbd::controllers
{
	class backend
	{
		struct impl;
		std::unique_ptr<impl> m;
	public:
		explicit backend(std::string config, bool standalone = false);
		~backend();
		bool available() const;
		state poll(bool focused, bool typing = false, std::string_view locked = {});
		// Offline Player 2 uses its seated controller, joined with Start and withheld until release.
		// Uses the device reports collected by the preceding poll().
		state poll_second(bool focused);
		std::string snapshot() const;
		bool local_lock() const;
	};

	// A hidden, parent-bound stream of normalized and raw SDL samples for the launcher.
	int run_controller_stream(int argc, char** argv);
}
