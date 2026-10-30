#pragma once

#include "pbd_t5dr_memory.h"
#include "util/types.hpp"
#include <array>
#include <cstdlib>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace pbd
{
	struct input_command_model
	{
		struct field_def
		{
			const char* name;
			u32 addr;
			u8 width;
		};
		// Menu/scene fields.
		static constexpr std::array<field_def, 17> known_fields{{{"frame", 0x1008eb9c, 4}, {"scene_timer", 0x1008eba8, 4}, {"mode", pbd::t5dr::game_mode_address, 4}, {"rounds_source", 0x1008ebd4, 1},
			{"handicap_p1", 0x1008ebf6, 2}, {"handicap_p2", 0x1008ebfa, 2}, {"mode_req", 0x1008f064, 4},
			{"scene", pbd::t5dr::scene_address, 4},
			{"subscene", pbd::t5dr::subscene_address, 4}, {"title_last", 0x10093460, 1}, {"title_cursor", 0x10093464, 4},
			{"pref_difficulty", 0x10684964, 1}, {"pref_rounds", 0x10684965, 1}, {"pref_round_time", 0x10684966, 1},
			{"pref_guard", 0x10684967, 1}, {"pref_csc", 0x10684968, 1}, {"pref_5", 0x10684969, 1}}};

		struct field_ref
		{
			std::string name;
			u32 addr = 0;
			u8 width = 0;
		};
		enum class cmp
		{
			eq,
			ne,
			changed,
			lt,
			gt
		};
		struct predicate
		{
			field_ref field;
			cmp op = cmp::eq;
			u32 value = 0;
		};
		struct dump_req
		{
			u32 addr = 0, size = 0;
		};
		struct command
		{
			u32 seq = 0;
			std::string op; // observe | press | wait | checkpoint | stop
			u16 digital1 = 0, digital2 = 0;
			u32 hold_polls = 3, release_polls = 6, stable_polls = 2, timeout_polls = 600;
			bool ack_any = false;
			std::vector<predicate> ack;
			std::vector<dump_req> dumps;
		};
		// Pad facts the checkpoint predicate needs, gathered by the caller (pbd_pad_read.cpp) at the same poll.
		struct pad_facts
		{
			bool virtual_pads = false, port1_neutral = false;
		};

		static std::optional<field_ref> resolve_field(std::string_view name)
		{
			for (const auto& f : known_fields)
				if (name == f.name)
					return field_ref{f.name, f.addr, f.width};
			// Generic reads: u8:0xADDR, u16:0xADDR, u32:0xADDR.
			const auto colon = name.find(':');
			if (colon == std::string_view::npos)
				return std::nullopt;
			const auto kind = name.substr(0, colon);
			const u8 width = kind == "u8" ? 1 : kind == "u16" ? 2 :
			                                kind == "u32"     ? 4 :
			                                                    0;
			if (!width)
				return std::nullopt;
			const std::string addr_text(name.substr(colon + 1));
			char* end = nullptr;
			const u64 addr = std::strtoull(addr_text.c_str(), &end, 0);
			if (!end || *end || addr > 0xfffffffcull)
				return std::nullopt;
			return field_ref{std::string(name), static_cast<u32>(addr), width};
		}
	};
} // namespace pbd
