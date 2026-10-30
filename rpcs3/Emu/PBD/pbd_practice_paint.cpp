#include "stdafx.h"
#include "pbd_practice_paint.h"
#include "pbd_t5dr_memory.h"
#include "../Cell/pbd_practice_native.h"
#include "../Cell/PPUThread.h"
#include "Emu/System.h"
#include "Emu/Memory/vm.h"

#include <atomic>

namespace pbd::practice::paint
{
	namespace
	{
		std::atomic<u64> paint_generation{0};
		struct paint_scope
		{
			u64 generation = 0, sp = 0, lr = 0;
			u32 object = 0;
		};
		thread_local paint_scope paint;
		thread_local u64 pause_object_generation = 0;
		thread_local u32 pause_object = 0;
		u32 word(u32 address) noexcept
		{
			be_t<u32> value{};
			return vm::try_access(address, &value, sizeof(value), false) ? u32{value} : ~0u;
		}
		bool supported() noexcept
		{
			return Emu.GetTitleID() == t5dr::title_id && Emu.GetExecutableHash() == t5dr::executable_hash;
		}
	} // namespace

	void claim_paint(bool owned, u64 generation) noexcept
	{
		paint_generation.store(owned ? generation : 0, std::memory_order_release);
	}
	namespace
	{
		bool suppress_menu_resource(ppu_thread& ppu, u64 generation) noexcept
		{
			if (!pause_object || pause_object_generation != generation || word(pause_object) != 0x10082138 ||
				word(pause_object + 0x5c) != 6 || word(0x10092eb8) != 0 ||
				!pbd::practice::native::inspect(true).pause_acknowledged)
				return false;
			const u32 resource = static_cast<u32>(ppu.gpr[3]);
			// The menu's resource handle identifies the persistent banner and row artwork, which has its own draw callback.
			// Its update and allocation callbacks must still run while the final resource draw belongs to the PBD pause.
			return resource && word(pause_object + 0x54) == resource && word(resource) == 0x10082158 &&
			       word(resource + 0x0c) == word(pause_object + 0x58) && word(resource + 0x58) != 0 &&
			       word(0x10082168) == 0x44dbd0 && word(0x44dbd0) == 0x108248 && word(0x44dbd4) == t5dr::toc &&
			       word(0x108248) == 0x38000070 && word(0x10824c) == 0xf821ff41 && word(0x1082c4) == 0x481aff69;
		}
	} // namespace
	bool suppress_paint(ppu_thread& ppu, u32 address) noexcept
	{
		if (address == 0x1eed14 && ppu.id == t5dr::main_ppu_id && supported() && word(static_cast<u32>(ppu.gpr[3]) + 0x5c) == 6 &&
			word(address) == 0x38000070 && word(address + 4) == 0xf821ff21)
		{
			pause_object = static_cast<u32>(ppu.gpr[3]);
			pause_object_generation = static_cast<u64>(Emu.GetEmulationIdentifier());
		}
		const u64 generation = paint_generation.load(std::memory_order_acquire);
		if (!generation || generation != static_cast<u64>(Emu.GetEmulationIdentifier()) || ppu.id != t5dr::main_ppu_id || !supported())
		{
			paint = {};
			return false;
		}
		if (address == 0x108248)
			return suppress_menu_resource(ppu, generation);
		if (address == 0x1eedd0)
		{
			if (paint.generation == generation && ppu.gpr[1] == paint.sp && ppu.lr == paint.lr)
				paint = {};
			return false;
		}
		if (address == 0x1eed14)
		{
			const u32 object = static_cast<u32>(ppu.gpr[3]);
			const auto owner = pbd::practice::native::inspect(true);
			paint = {};
			if (owner.pause_acknowledged && word(object + 0x5c) == 6 &&
				word(0x1eed14) == 0x38000070 && word(0x1eed18) == 0xf821ff21 && word(0x1eed58) == 0x8003005c &&
				word(0x1eede4) == 0x4bfff3d9 && word(0x1eedd0) == 0x4e800020)
				paint = {generation, ppu.gpr[1], ppu.lr, object};
			return false;
		}
		// Suppress formatted menu text within the dispatcher. Persistent artwork has a separate resource callback.
		return address == 0x139d44 && paint.generation == generation && ppu.gpr[1] < paint.sp &&
		       word(paint.object + 0x5c) == 6 && pbd::practice::native::inspect(true).pause_acknowledged &&
		       word(0x139d44) == 0x38000130 && word(0x139d48) == 0xf821fe81 && word(0x139d98) == 0x4bfff33d;
	}
	bool screen_returned() noexcept
	{
		return pause_object && pause_object_generation == static_cast<u64>(Emu.GetEmulationIdentifier()) &&
		       word(pause_object + 0x5c) == 6 && word(0x10092eb8) == 0 && pbd::practice::native::inspect(true).pause_acknowledged;
	}
} // namespace pbd::practice::paint
