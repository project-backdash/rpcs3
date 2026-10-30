#pragma once

#include "../Cell/pbd_practice_native.h"
#include "pbd_practice_stage.h"

namespace pbd::practice::commands
{
	enum class stock_operation : u8
	{
		sound,
		select_stage,
		request_load
	};
	struct bindings
	{
		bool (*matches)(u32 address, u32 bytes, const char* expected);
		bool (*invoke)(ppu_thread& ppu, stock_operation operation, u32 value);
	};
	pbd::practice::native::result execute(ppu_thread& ppu, const pbd::practice::native::request& command,
		const pbd::practice::native::owner_view& owner, bindings calls);
	stage::progress observe_load(u32 boundary) noexcept;
} // namespace pbd::practice::commands
