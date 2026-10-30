#pragma once

#include <algorithm>
#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>
#include "../PBD/pbd_character_rule.h"

// Selection is a local digital input producer. It never writes a cursor, character row, stage or unlock bit;
// the stock handlers resolve those values and the battle readback must match before physical input is released.
namespace pbd::hidden_selection
{
	using u32 = std::uint32_t;
	using u16 = std::uint16_t;
	inline constexpr std::array<std::uint8_t, 20> pinned_loader_sha1{
		0x3b, 0x68, 0xa1, 0xec, 0xc7, 0x53, 0x1d, 0x1a, 0xe1, 0x70, 0xfb, 0x00, 0xe2, 0xdb, 0x03, 0xd1, 0x7b, 0xff, 0xcd, 0x44};
	inline constexpr std::array<u16, 4> directions{0x1000, 0x4000, 0x8000, 0x2000};
	inline constexpr std::array<u16, 5> confirms{0x80, 0x10, 0x40, 0x20, 0x800};
	struct cell
	{
		u16 character = 0;
		std::array<std::uint8_t, 4> neighbor{};
	};
	struct row
	{
		u16 character = 0, variant = 0;
	};
	struct variants
	{
		std::vector<u32> rows;
	};
	struct tables
	{
		std::array<cell, 36> grid{};
		std::array<row, 85> characters{};
		std::array<variants, 35> costumes{};
		std::array<std::uint8_t, 25> buttons{};
		std::uint64_t availability = 0;
		bool verified = false;
		bool ban_jinpachi = false;
	};
	// The samples a selection may take from its origin before it is refused: the previous game's exit, both fighters, the
	// handicap, the stage lists and the VS load.
	inline constexpr u32 selection_samples = 3600;
	// `repick` (change character): that side (0 left, 1 right) picks again in the open select and its row is not driven, so it
	// binds nothing at the battle readback. The other side is still steered to its frozen row and costume, and the stage owner's PC
	// still steers the stage. `limit` then also covers the chooser's character countdown.
	struct request
	{
		u32 left_row = 0, right_row = 0, stage = 0, rounds = 3, local_role = 0;
		std::optional<u32> repick;
		u32 limit = selection_samples;
	};
	struct observation
	{
		u32 scene = 0, subscene = 0, mode = 0, grid_mode = 0, state = 0, pad_role = 0;
		u32 cursor = 0, base_character = 0, committed_row = 0xffffffffu, handicap = 100;
		u32 stage_mode = 0, stage_cursor = 0, stage_owner = 0;
		std::vector<u32> new_stages, old_stages;
		u32 applied_left = 0, applied_right = 0, applied_stage = 0, applied_rounds = 0;
		bool verified = false, applied_verified = false;
	};
	struct plan
	{
		u32 cursor = 0, character = 0, row = 0;
		u16 confirm = 0;
	};
	inline bool supported_availability(std::uint64_t value)
	{
		// Verified VS saves: the normal roster is unlocked; Jinpachi may remain locked.
		return value == 0x3ffffffffull || value == 0x7ffffffffull;
	}
	inline bool cursor_available(const tables& value, u32 cursor)
	{
		return pbd::character_rule::cursor_available(value.ban_jinpachi ? pbd::character_rule::rule::ban_jinpachi :
																		  pbd::character_rule::rule::stock,
			value, cursor);
	}
	inline std::optional<u32> directional_neighbor(const tables& value, u32 source, std::size_t direction)
	{
		if (!value.verified || !supported_availability(value.availability) || source >= value.grid.size() || direction >= directions.size())
			return {};
		auto cursor = source;
		// Stock 0xaf668..0xaf68c follows the same directional neighbor until its availability predicate succeeds.
		for (std::size_t skipped = 0; skipped < value.grid.size(); ++skipped)
		{
			cursor = value.grid[cursor].neighbor[direction];
			if (cursor >= value.grid.size())
				return {};
			if (cursor_available(value, cursor))
				return cursor;
		}
		return {};
	}
	inline std::optional<plan> resolve(const tables& value, u32 wanted)
	{
		if (!value.verified || wanted >= value.characters.size() || !supported_availability(value.availability))
			return {};
		const auto base = value.characters[wanted].character;
		if (value.ban_jinpachi && base == pbd::character_rule::jinpachi)
			return {};
		if (base >= value.costumes.size() || !(value.availability & (std::uint64_t{1} << base)))
			return {};
		const auto& rows = value.costumes[base].rows;
		if (rows.empty() || rows.size() > 5)
			return {};
		const auto count = rows.size();
		const auto offset = count <= 2 ? 0u : count == 3 && base == 7 ? 5u :
		                                  count == 3                  ? 10u :
		                                  count == 4                  ? 15u :
		                                                                20u;
		u16 button = 0;
		for (std::size_t i = 0; i < confirms.size(); ++i)
			if (value.buttons[offset + i] < count && rows[value.buttons[offset + i]] == wanted)
			{
				button = confirms[i];
				break;
			}
		if (!button)
			return {};
		std::optional<u32> cursor;
		for (u32 i = 0; i < value.grid.size(); ++i)
		{
			for (const auto next : value.grid[i].neighbor)
				if (next >= value.grid.size())
					return {};
			if (value.grid[i].character == base)
			{
				if (cursor)
					return {};
				cursor = i;
			}
		}
		if (!cursor || *cursor == 24)
			return {};
		return plan{*cursor, base, wanted, button};
	}
	inline std::optional<u16> move_toward(const tables& value, u32 source, u32 wanted)
	{
		if (!value.verified || !supported_availability(value.availability) || source >= value.grid.size() || !cursor_available(value, wanted))
			return {};
		if (source == wanted)
			return u16{0};
		std::array<int, 36> first{};
		first.fill(-1);
		first[source] = 4;
		std::array<u32, 36> queue{};
		std::size_t head = 0, tail = 1;
		queue[0] = source;
		while (head < tail)
		{
			const auto cursor = queue[head++];
			for (int direction = 0; direction < 4; ++direction)
			{
				const auto next = directional_neighbor(value, cursor, static_cast<std::size_t>(direction));
				if (!next)
					return {};
				if (first[*next] >= 0)
					continue;
				first[*next] = cursor == source ? direction : first[cursor];
				queue[tail++] = *next;
				if (*next == wanted)
					return directions[static_cast<std::size_t>(first[*next])];
			}
		}
		return {};
	}
	class driver
	{
		request wanted{};
		tables mapping{};
		plan local{};
		u32 origin = 0, last_source = 0;
		u16 last_pad = 0;
		bool begun = false, sampled = false, complete_ = false;
		const char* error_ = nullptr;
		u16 reject(const char* reason)
		{
			error_ = reason;
			return 0;
		}

		u16 choose_character(const observation& now, bool press, const tables* live)
		{
			if (now.cursor >= mapping.grid.size())
				return reject("selection_cursor_unverified");
			if (now.cursor != local.cursor)
			{
				const auto direction = move_toward(mapping, now.cursor, local.cursor);
				if (!direction)
					return reject("selection_cursor_unreachable");
				return press ? *direction : 0;
			}
			// Resolve the live metadata again at confirmation; cached row arithmetic cannot bless a costume.
			const auto current = live ? resolve(*live, local.row) : std::optional<plan>{};
			if (!current || current->cursor != local.cursor || current->confirm != local.confirm || now.base_character != local.character)
				return reject("selection_costume_unverified");
			return press ? local.confirm : 0;
		}

		u16 choose_stage(const observation& now, bool press)
		{
			if (now.stage_owner != wanted.local_role || now.stage_mode > 2 || now.new_stages.size() > 21 || now.old_stages.size() > 21)
				return reject("selection_stage_unverified");
			const auto find = [&](const std::vector<u32>& list)
			{
				return std::find(list.begin(), list.end(), wanted.stage);
			};
			const auto fresh = find(now.new_stages), old = find(now.old_stages);
			if (fresh == now.new_stages.end() && old == now.old_stages.end())
				return reject("selection_stage_unavailable");
			const auto mode = fresh != now.new_stages.end() ? 0u : 2u;
			if (now.stage_mode != mode)
				return press ? u16{0x1000} : 0;
			const auto& list = mode ? now.old_stages : now.new_stages;
			if (now.stage_cursor >= list.size())
				return reject("selection_stage_cursor_unverified");
			if (list[now.stage_cursor] == wanted.stage)
				return press ? u16{0x40} : 0;
			return press ? u16{0x2000} : 0;
		}

		u16 choose(u32 source, const observation& now, u16 physical, const tables* live)
		{
			if (complete_)
				return physical;
			if (source < origin || source - origin > wanted.limit)
				return reject("selection_timeout");
			if (!now.verified || now.mode != 4 || now.grid_mode != 1 || now.subscene != 44)
				return reject("selection_state_unverified");
			if (now.scene == 17)
			{
				if (!now.applied_verified || (wanted.repick != 0u && now.applied_left != wanted.left_row) ||
					(wanted.repick != 1u && now.applied_right != wanted.right_row) ||
					now.applied_stage != wanted.stage || now.applied_rounds != wanted.rounds)
					return reject("selection_applied_mismatch");
				complete_ = true;
				return 0; // Release starts with a neutral sample; held confirmation cannot become a combat input.
			}
			// Natural VS continuation visits scene 1: its pinned 0x50b20 handler writes scene 2 at 0x50b2c.
			// It has no selection input; keep it neutral under the same VS/grid/subscene checks and timeout.
			// Pinned VS preparation also visits resource initialization (scene 2) before returning to selection.
			// Its 0x53c88 handler loads the selected resources and writes scene 3 at 0x53ee0 in VS mode 4.
			// These handlers need no selector input. Completion still requires the agreed first-battle readback.
			// Scene zero is the stock gap between handlers; the VS-mode and timeout guards still apply there.
			if (now.scene <= 2 || (now.scene >= 4 && now.scene <= 14) || now.scene == 16)
				return 0;
			if (now.scene != 3 || now.pad_role != wanted.local_role)
				return reject("selection_state_unsupported");
			const bool press = (source - origin) % 24 < 6;
			// A repick chooser's own word reaches its select in every state but the stage state, which is steered to the frozen
			// stage below like any other. The in-game countdown shapes that word after this (pbd::select_timer).
			if (wanted.repick == wanted.local_role && now.state != 13)
				return physical;
			if (now.state == 3)
				return choose_character(now, press, live);
			if (now.state == 9)
			{
				if (now.handicap != 100 || now.committed_row != local.row)
					return reject("selection_handicap_unsupported");
				return press ? u16{0x40} : 0;
			}
			if (now.state == 13)
				return choose_stage(now, press);
			switch (now.state)
			{
			case 0:
			case 1:
			case 4:
			case 5:
			case 6:
			case 8:
			case 10:
			case 11:
			case 12:
			case 14:
			case 15:
			case 21:
			case 22: return 0;
			default: return reject("selection_state_unsupported");
			}
		}

	public:
		bool begin(request value, tables native, u32 first_source)
		{
			if (begun || value.local_role > 1 || value.rounds != 3 || value.stage > 0x25 || (value.repick && *value.repick > 1) || !value.limit)
			{
				reject("selection_request_unsupported");
				return false;
			}
			const auto left = resolve(native, value.left_row), right = resolve(native, value.right_row);
			if ((value.repick != 0u && !left) || (value.repick != 1u && !right))
			{
				reject("selection_row_unavailable");
				return false;
			}
			wanted = value;
			mapping = std::move(native);
			origin = first_source;
			begun = true;
			if (value.repick != value.local_role)
				local = value.local_role ? *right : *left;
			return true;
		}
		bool active() const
		{
			return begun && !complete_ && !error_;
		}
		// A fixed selection runs behind the loading card, with the game muted. A repick runs in the open select.
		bool hidden() const
		{
			return active() && !wanted.repick;
		}
		// Whether this PC's word comes from the driver rather than its player. The chooser of a repick plays its own select; only
		// the stage, when this PC owns it, is still steered.
		bool owns_input() const
		{
			return active() && wanted.repick != wanted.local_role;
		}
		bool healthy() const
		{
			return !error_;
		}
		bool complete() const
		{
			return complete_;
		}
		const char* error() const
		{
			return error_;
		}
		u32 target_cursor() const
		{
			return local.cursor;
		}
		u16 confirm_button() const
		{
			return local.confirm;
		}
		// A repeated source returns its frozen word. Corrections consume already published input, not a second observation.
		u16 sample(u32 source, const observation& now, u16 physical, const tables* live = nullptr)
		{
			if (!begun || error_)
				return 0;
			if (sampled && source == last_source)
				return last_pad;
			if (sampled && source < last_source)
				return reject("selection_source_rewound");
			sampled = true;
			last_source = source;
			last_pad = choose(source, now, physical, live);
			return last_pad;
		}
	};

	// Runtime readers are separate from the deterministic model so model tests never attach to a running emulator.
	bool pinned_binary();
	std::optional<tables> read_native_tables(std::string* refusal = nullptr);
	std::optional<observation> read_native_observation(u32 role);
} // namespace pbd::hidden_selection
