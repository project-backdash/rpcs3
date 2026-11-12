#pragma once

#include <cstdint>
#include <optional>

// The original Tekken 5 stages. The stage select lists them in its Original row once bit 0x10 of the save's unlock byte
// is set, which the game itself does only after 50 completed matches (0x2a76f8) and a title screen visit (0x2a7358).
// Every session sets the bit at its first owner boundary, so every player has the same stage lists from the first game.
namespace pbd::stage_rule
{
	// Read by 0x2a732c(0x10): the stage predicate 0x613f8 (ids 0x00-0x0d, 0x0f, 0x25) and the row stepper 0xaf750.
	// Offset 0x4a of the save image (pbd::setup_digest::save_image), so the session start digest covers it.
	inline constexpr std::uint32_t unlock_byte = 0x106849aa;
	// 0x2a732c reaches the byte through the save object in the TOC slot and its pointer at +0x230.
	inline constexpr std::uint32_t save_object = 0x10684690;
	inline constexpr std::uint32_t save_object_slot = 0x470540 - 0x173c;
	inline constexpr std::uint32_t unlock_pointer = save_object + 0x230;
	// The save setting that owns the byte (0x1e9e8 writes it through +8). Its change callback (0x26b7f8) only returns,
	// so writing the byte directly leaves nothing else to update.
	inline constexpr std::uint32_t unlock_setting = 0x106848b8 + 8;
	// Original stages and Inferno only: bit 0x20 unlocks Jinpachi and changes the roster mask at 0x100934a0.
	inline constexpr std::uint8_t original_stages = 0x10;

	// A Tekken 5 original: ids 0x00-0x0d, each the original of stage id + 0x11, and 0x0f (Final Stage, before Inferno).
	constexpr bool original(std::uint32_t stage) noexcept
	{
		return stage <= 0x0d || stage == 0x0f;
	}

	// The ids a direct load (Practice, rematch, a replay's start) may use, since it skips the game's predicates: the
	// originals, the Dark Resurrection stages 0x11-0x22 and Inferno (0x25). 0x0e, 0x10, 0x23 and 0x24 have no stage.
	constexpr bool loadable(std::uint32_t stage) noexcept
	{
		return original(stage) || (stage >= 0x11 && stage <= 0x22) || stage == 0x25;
	}

	// What the rule reads before it writes.
	struct observation
	{
		// The supported executable, unpatched (pbd::hidden_selection::pinned_binary).
		bool pinned = false;
		std::uint32_t save_slot = 0, pointer = 0, setting = 0;
		std::uint8_t value = 0;
	};

	// The byte with the original stages' bit set, keeping every other bit (a Jinpachi save, 0x22, becomes 0x32), or
	// nothing when the save is not where this rule expects it.
	constexpr std::optional<std::uint8_t> unlocked(const observation& seen) noexcept
	{
		if (!seen.pinned || seen.save_slot != save_object || seen.pointer != unlock_byte || seen.setting != unlock_byte)
			return {};
		return static_cast<std::uint8_t>(seen.value | original_stages);
	}
} // namespace pbd::stage_rule
