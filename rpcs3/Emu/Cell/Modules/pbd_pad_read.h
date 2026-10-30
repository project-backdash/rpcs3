#pragma once

#include "Emu/Cell/ErrorCodes.h"
#include "Emu/Memory/vm_ptr.h"
#include "util/types.hpp"

struct CellPadData;
struct pad_info;
class pad_thread;

namespace pbd
{
	struct input_observation;
}

namespace pbd::pad_read
{
	// One read of a port by the game (cellPadGetData), after RPCS3 has written the device's record. Under the pad lock.
	struct port_read
	{
		pad_thread& handler;
		const pad_info& config;
		u32 port;
		CellPadData& data;
		pbd::input_observation* observation;
	};

	// When the game last read this port, for the input-age measurement.
	void note_read(u32 port);

	// What PBD changes in the record the game gets: the canonical record of a peer session, a scripted or live first-run
	// input, and neutral words while a replay viewer, Practice or the in-game menu holds the controller.
	void after_read(const port_read& r);

	// cellPadGetData through get_data, with the input diagnostics observing each read when they are on.
	using get_data_function = error_code (*)(u32 port, vm::ptr<CellPadData> data, pbd::input_observation* observation);
	error_code observed(u32 port, vm::ptr<CellPadData> data, get_data_function get_data);
} // namespace pbd::pad_read
