#pragma once

#include <array>
#include <cstdint>

class ppu_thread;

namespace pbd::pick_transition
{
	// Last model-task instruction before its bone update enters the clip sampler.
	inline constexpr std::uint32_t hook_pc = 0x264f30;
	inline constexpr std::uint32_t slot_offset = 0x878;
	// F (0x2a39f4) returns at 0x2a3abc when the selected clip pointer is null.
	inline constexpr std::uint32_t no_clip = 0;

	struct observation
	{
		bool pinned = false;
		std::uint32_t mode = 0;
		std::uint32_t manager = 0;
		std::uint32_t scene = 0;
		std::uint32_t model_state = 0;
		std::uint32_t actor = 0;
		std::uint32_t model_actor = 0;
		std::uint32_t side = 2;
		std::uint32_t built_row = 0;
		std::uint32_t row = 0;
		std::uint32_t selected_row = 0;
		std::uint32_t fighter = 0;
		std::uint32_t row_fighter = 0;
		std::uint32_t costume = 0;
		std::uint32_t bone_count = 0;
		std::uint16_t slot_flags = 0;
		std::array<std::uint32_t, 2> clips{};
		bool loaded_clip = false;
	};

	struct write_plan
	{
		std::uint32_t address = 0;
		std::array<std::uint32_t, 2> clips{no_clip, no_clip};
	};

	// Only the old model in the next VS load may lose its obsolete clip. The loader/rebinder installs the new pair.
	constexpr write_plan plan(const observation& state) noexcept
	{
		const std::uint32_t expected_actor = 0x10090040 + state.side * 0x8d0;
		const bool reads_slot = state.fighter == 27 || (state.fighter == 9 && state.costume == 1);
		const auto selected = (state.slot_flags & 0x4000) ? 1u : 0u;
		if (!state.pinned || state.mode != 4 || state.manager != 4 || state.scene != 2 || state.model_state != 3 || state.side > 1 ||
			state.actor != expected_actor || state.model_actor != state.actor || state.row > 84 || state.built_row > 84 ||
			state.row != state.selected_row || state.row == state.built_row || state.fighter != state.row_fighter ||
			!reads_slot || !state.bone_count || !state.clips[selected] || state.loaded_clip)
			return {};
		return {state.actor + slot_offset, {no_clip, no_clip}};
	}

	void event(ppu_thread& owner) noexcept;
} // namespace pbd::pick_transition
