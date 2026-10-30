#pragma once

#include "pbd_controller_mapper.h"
#include <atomic>
#include <map>
#include <mutex>
#include <vector>

namespace pbd::controllers::settings
{
	// The buttons an action can be given to, as the launcher saves them (gameSettings.ts GAME_BUTTONS): the face buttons, the
	// shoulders, then the two stick clicks, which are off unless the player gives them an action.
	inline constexpr std::array<std::string_view, 10> button_slots{"square", "triangle", "cross", "circle", "l1", "r1", "l2", "r2", "l3", "r3"};
	// Each slot's bit in the game's 16-bit pad word: digital2's square, triangle, cross and circle, then L1, R1, L2 and R2, and
	// digital1's L3 and R3.
	inline constexpr std::array<std::uint16_t, 10> slot_bits{0x80, 0x10, 0x40, 0x20, 0x04, 0x08, 0x01, 0x02, 0x0200, 0x0400};
	inline constexpr std::uint16_t stick_clicks = 0x0600;
	inline constexpr std::array<std::string_view, 16> assignments{"off", "1", "2", "3", "4", "1+2", "3+4", "1+3", "2+4", "1+4", "2+3",
		"1+2+3", "1+2+4", "1+3+4", "2+3+4", "1+2+3+4"};
	using layout = std::array<std::string, button_slots.size()>;
	struct values
	{
		std::string device;
		layout mapping{"1", "2", "3", "4", "off", "off", "off", "off", "off", "off"};
		bool operator==(const values&) const = default;
	};
	struct device
	{
		std::string id, label;
		// Offline play keeps a layout per controller (pbd_player_seats.h): the Controls page shows this one when it switches to
		// the controller. Without it (`mapped` false) the page keeps the layout it shows.
		bool mapped = false;
		std::array<std::string, button_slots.size()> mapping{};
		bool operator==(const device&) const = default;
	};
	struct snapshot
	{
		bool available = false;
		values current;
		std::vector<device> devices;
	};

	// Offline edits belong to a device. Before its first edit, Player 1 uses the launcher's layout and Player 2 the default.
	struct layouts
	{
		layout first = values{}.mapping;
		std::map<std::string, layout, std::less<>> saved;

		const layout& for_player(std::string_view id, unsigned player) const
		{
			if (const auto own = saved.find(id); own != saved.end())
				return own->second;
			static const layout defaults = values{}.mapping;
			return player == 0 ? first : defaults;
		}

		snapshot presentation(std::string_view selected, std::vector<device> devices, std::string_view player1) const
		{
			values current;
			current.device = selected;
			current.mapping = for_player(selected, selected == player1 ? 0 : 1);
			for (auto& entry : devices)
			{
				entry.mapped = true;
				entry.mapping = for_player(entry.id, entry.id == player1 ? 0 : 1);
			}
			return {true, std::move(current), std::move(devices)};
		}
	};
	inline std::mutex mutex;
	inline snapshot current;
	struct change
	{
		values value;
		std::uint64_t revision;
	};
	inline std::vector<change> requested;
	inline std::uint64_t revision = 0;
	inline std::atomic<std::uint16_t> menu_word{0};
	inline std::atomic<bool> active{false}, mapping_changed{false};
	// Each slot's attacks in four bits (attack_mask >> 4), slot i at bit 4 * i, so all ten fit one atomic word.
	inline std::atomic<std::uint64_t> attack_masks{0};

	inline std::uint16_t attack_mask(std::string_view assignment)
	{
		constexpr std::array<std::uint16_t, 4> bits{0x80, 0x10, 0x40, 0x20};
		std::uint16_t result = 0;
		for (const char c : assignment)
			if (c >= '1' && c <= '4')
				result |= bits[c - '1'];
		return result;
	}
	// Slot `slot`'s attack bits in a packed attack_masks value.
	inline std::uint16_t slot_attacks(std::uint64_t packed, unsigned slot) noexcept
	{
		return static_cast<std::uint16_t>(((packed >> (4 * slot)) & 0xf) << 4);
	}
	inline snapshot read()
	{
		std::lock_guard lock(mutex);
		return current;
	}
	inline void apply_mapping(const values& value)
	{
		std::uint64_t packed = 0;
		for (unsigned i = 0; i < value.mapping.size(); ++i)
			packed |= static_cast<std::uint64_t>(attack_mask(value.mapping[i]) >> 4) << (4 * i);
		attack_masks.store(packed, std::memory_order_release);
		mapping_changed.store(true, std::memory_order_release);
	}
	// The stick clicks in `packed` that have an action: in a fight such a click is its action's attacks, not a click.
	inline std::uint16_t clicks_with_actions(std::uint64_t packed) noexcept
	{
		std::uint16_t result = 0;
		for (unsigned i = 0; i < slot_bits.size(); ++i)
			if ((slot_bits[i] & stick_clicks) && slot_attacks(packed, i))
				result |= slot_bits[i];
		return result;
	}
	// The same for the live mapping; none before the controller's first poll.
	inline std::uint16_t clicks_with_actions() noexcept
	{
		return mapping_changed.load(std::memory_order_acquire) ? clicks_with_actions(attack_masks.load(std::memory_order_acquire)) : 0;
	}
	// A taken fighter's word (a replay's Take Control): each slot's attacks in place of the slot, and a stick click with an action
	// is that action alone. Directions, Start, Select and a stick click with none stay as they are.
	inline std::uint16_t remap_attacks(std::uint16_t raw)
	{
		if (!mapping_changed.load(std::memory_order_acquire))
			return raw;
		const auto packed = attack_masks.load(std::memory_order_acquire);
		std::uint16_t result = static_cast<std::uint16_t>(raw & 0xff00 & ~clicks_with_actions(packed));
		for (unsigned i = 0; i < slot_bits.size(); ++i)
			if (raw & slot_bits[i])
				result |= slot_attacks(packed, i);
		return result;
	}
	inline std::uint64_t current_revision()
	{
		std::lock_guard lock(mutex);
		return revision;
	}
	inline void publish(values value, std::vector<device> devices, std::uint64_t seen = 0)
	{
		std::lock_guard lock(mutex);
		// A poll that predates the menu's latest change cannot replace it.
		if (revision == seen)
		{
			apply_mapping(value);
			current.current = std::move(value);
		}
		current.available = true;
		current.devices = std::move(devices);
	}
	inline bool submit(const values& value)
	{
		for (const auto& assignment : value.mapping)
			if (std::find(assignments.begin(), assignments.end(), assignment) == assignments.end())
				return false;
		std::lock_guard lock(mutex);
		if (!current.available || std::none_of(current.devices.begin(), current.devices.end(), [&](const auto& d)
									  {
										  return d.id == value.device;
									  }))
			return false;
		// Keep only each device's latest layout, in submission order for the single-controller menu.
		std::erase_if(requested, [&](const auto& pending)
			{
				return pending.value.device == value.device;
			});
		requested.push_back({value, ++revision});
		current.current = value;
		apply_mapping(value);
		return true;
	}
	inline std::vector<values> take(std::uint64_t& seen)
	{
		std::lock_guard lock(mutex);
		std::vector<values> result;
		for (auto& pending : requested)
			if (pending.revision > seen)
				result.push_back(std::move(pending.value));
		requested.clear();
		seen = revision;
		return result;
	}
} // namespace pbd::controllers::settings
