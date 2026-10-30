#include "stdafx.h"
#include "pbd_practice_commands.h"
#include "pbd_t5dr_memory.h"
#include "Emu/System.h"
#include "Emu/Memory/vm.h"

namespace pbd::practice::commands
{
	namespace
	{
		thread_local stage::load_watch watch;
		u32 word(u32 address) noexcept
		{
			be_t<u32> value{};
			return vm::try_access(address, &value, sizeof(value), false) ? u32{value} : ~0u;
		}
		bool readable_model(u32 actor) noexcept
		{
			const u32 model = word(actor + 0x8a4);
			std::array<u8, 0x20> header{};
			return model && model != ~0u && vm::try_access(model, header.data(), header.size(), false);
		}
		pbd::practice::native::result sound(ppu_thread& ppu, u32 value, bindings calls)
		{
			pbd::practice::native::result out;
			out.entry = 0xac9b8;
			if (value != 2 && value != 3 && value != 6)
				return out;
			if (!calls.matches(out.entry, 0x70, "447e984d28c953a01f3a7ecf504a19520ba7e16e1dcd83c55d7f97af946c8bf7"))
				return out;
			out.preserved = calls.invoke(ppu, stock_operation::sound, value);
			out.readback = out.preserved;
			out.accepted = out.preserved;
			return out;
		}
	} // namespace
	pbd::practice::native::result execute(ppu_thread& ppu, const pbd::practice::native::request& command,
		const pbd::practice::native::owner_view& owner, bindings calls)
	{
		using namespace pbd::practice::native;
		if (command.kind == operation::sound)
			return sound(ppu, command.value, calls);
		result out;
		const u32 manager = word(t5dr::toc - 0x21a4);
		const std::array rows{word(0x10093f50), word(0x10093f54)};
		if (command.kind != operation::stage_load || watch.active || !stage::valid(command.value) ||
			manager != 0x1008f060 || word(manager) != 4 || word(manager + 4) != 14 ||
			word(t5dr::scene_address) != 17 || word(t5dr::subscene_address) != 44 || rows[0] >= 85 || rows[1] >= 85 ||
			word(t5dr::toc - 0x7f04) != 0x1008f060 || word(t5dr::toc - 0x7498) != 0x1008f060)
			return out;
		if (!calls.matches(0x1328c, 12, "b29055699a53ed38deb2196eb21f86f7ddd6994b90081ba156131b5ea7796349") ||
			!calls.matches(0x25da54, 200, "c43543e2fd2e1d6dacfdf5caa676ea2c4d4d3be30aa0422038079bfc0f480794") ||
			!calls.matches(0x53c88, 0x52c, "3cfe477ba6e26c1f9fd15e43109f20d5a4c7d5bf4973b241d0255528e5d65a96"))
			return out;
		// The scene manager owns teardown and model allocation. Retain both committed fighter rows and let its
		// Practice initializer rebuild the stage; a partial call must end Practice before another fight frame runs.
		out.entry = 0x25da54;
		out.load_started = true;
		try
		{
			out.preserved = calls.invoke(ppu, stock_operation::select_stage, command.value) &&
			                word(0x1008f088) == command.value && calls.invoke(ppu, stock_operation::request_load, 0);
			out.readback = out.preserved && word(manager + 4) == 14 && word(manager + 0x14) == 2 &&
			               word(0x10093f50) == rows[0] && word(0x10093f54) == rows[1];
		}
		catch (...)
		{
			return out;
		}
		out.accepted = out.preserved && out.readback;
		if (out.accepted)
			watch.begin(owner.generation, command.boundary, command.value, rows);
		return out;
	}
	stage::progress observe_load(u32 boundary) noexcept
	{
		if (!watch.active)
			return stage::progress::none;
		const stage::snapshot now{static_cast<u64>(Emu.GetEmulationIdentifier()), boundary,
			word(t5dr::game_mode_address), word(t5dr::scene_address), word(t5dr::subscene_address), word(0x1008f088), word(0x1008f05c),
			{word(0x10093f50), word(0x10093f54)}, word(0x10092e98) != 0 || word(0x10092ea4) != 0,
			readable_model(0x10090040) && readable_model(0x10090910)};
		return watch.step(now);
	}
} // namespace pbd::practice::commands
