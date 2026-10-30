#include "stdafx.h"
#include "pbd_pick_transition.h"
#include "pbd_t5dr_memory.h"
#include "pbd_env.h"
#include "../Cell/PPUThread.h"
#include "Emu/System.h"
#include "Emu/Memory/vm.h"

LOG_CHANNEL(pbd_pick_log, "PBD Pick transition");

namespace pbd::pick_transition
{
	namespace
	{
		template <typename T>
		bool read(u32 address, T& out)
		{
			be_t<T> value{};
			if (!vm::try_access(address, &value, sizeof(value), false))
				return false;
			out = value;
			return true;
		}
		bool pins()
		{
			constexpr std::array<std::pair<u32, u32>, 12> expected{{
				{0x264f28, 0x807f0080}, {hook_pc, 0x4802357d}, {0x2656ac, 0x90be0054}, {0x2656b0, 0x90de0058},
				{0x2a3a9c, 0x38170878}, {0x2a3ab8, 0x2fb90000}, {0x2a3abc, 0x419e004c}, {0x2a3b74, 0x833f0000},
				{0x13cac, 0x90090004}, {0x13cb0, 0x91690000}, {0xd53a4, 0x934a0004}, {0xd53a8, 0x900a0000}}};
			for (const auto& [address, opcode] : expected)
			{
				u32 held = 0;
				if (!read(address, held) || held != opcode)
					return false;
			}
			return true;
		}
	}

	void event(ppu_thread& owner) noexcept
	{
		if (owner.id != t5dr::main_ppu_id || get_current_cpu_thread() != &owner || owner.gpr[2] != t5dr::toc ||
			Emu.GetTitleID() != t5dr::title_id || Emu.GetExecutableHash() != t5dr::executable_hash ||
			owner.gpr[31] > 0xffffff7bu || owner.gpr[30] > 0xfffff72fu)
			return;
		observation state;
		state.actor = static_cast<u32>(owner.gpr[30]);
		const u32 model = static_cast<u32>(owner.gpr[31]);
		u32 table = 0, container = 0, bank = 0, first = 0, magic = 0;
		u16 side = 0, fighter = 0, row = 0, row_fighter = 0, costume = 0;
		if (!read(t5dr::game_mode_address, state.mode) || !read(0x1008f060, state.manager) ||
			!read(t5dr::scene_address, state.scene) || state.mode != 4 || state.manager != 4 || state.scene != 2 ||
			!read(model, table) || table != 0x100841b4 || !read(model + 0x48, state.model_state) ||
			!read(model + 0x54, state.built_row) || !read(model + 0x58, state.model_actor) || !read(model + 0x80, container) ||
			!container || container > 0xffffff3fu || !read(container + 0xbc, state.bone_count) ||
			!read(state.actor + 0x12, side) || side > 1 || !read(state.actor + 0x40, fighter) ||
			!read(state.actor + 0x42, row) || row > 84 || !read(0x10093f50 + 4 * side, state.selected_row) ||
			!read(0x4403c0 + 0x24 * row, row_fighter) || !read(0x4403c2 + 0x24 * row, costume) ||
			!read(state.actor + slot_offset, state.clips[0]) || !read(state.actor + slot_offset + 4, state.clips[1]) ||
			!read(state.actor + slot_offset + 0xc, state.slot_flags) || !read(state.actor + 0x50, bank))
			return;
		state.side = side;
		state.fighter = fighter;
		state.row = row;
		state.row_fighter = row_fighter;
		state.costume = costume;
		// A pair already installed from the loaded COM image belongs to that load and must survive an overlapping model.
		std::array<u32, 2> loaded{};
		if (bank && bank <= 0xfffffd97u && read(bank, first) && read(bank + 4, magic) &&
			first == 0x100 && magic == 0x434f4d00 && read(bank + 0x260, loaded[0]) && read(bank + 0x264, loaded[1]))
			state.loaded_clip = state.clips == loaded;
		state.pinned = pins();
		const auto write = plan(state);
		if (!write.address)
			return;
		// Off unless PBD_PICK_TRANSITION=1: the repair writes guest memory the stock game never writes there.
		const auto switch_value = pbd::env::find("PBD_PICK_TRANSITION");
		const bool enabled = switch_value && std::string_view(switch_value) == "1";
		pbd_pick_log.warning("Pick transition: side %u model 0x%x row 0x%x -> 0x%x, clips 0x%x/0x%x, bank 0x%x header 0x%x/0x%x, repair %u",
			side, model, state.built_row, row, state.clips[0], state.clips[1], bank, first, magic, enabled);
		if (!enabled)
			return;
		std::array<be_t<u32>, 2> pair{write.clips[0], write.clips[1]};
		if (!vm::try_access(write.address, pair.data(), sizeof(pair), true))
			pbd_pick_log.error("Pick transition: pair at 0x%x was not writable", write.address);
	}
} // namespace pbd::pick_transition
