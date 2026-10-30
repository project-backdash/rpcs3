#pragma once

#include "Emu/Cell/pbd_replay_game_file.h"
#include "pbd_character_rule.h"
#include "pbd_replay_restore.h"
#include "pbd_replay_notices.h"
#include "pbd_stage_rule.h"

#include <memory>

namespace pbd::replay_start
{
	struct pbd_capsule_start
	{
		std::uint32_t left = 0, right = 0, stage = 0, prior = 0, mob_batches = 0, counter = 0;
		std::uint32_t ordinal = 0, pre_ordinal = 0, input_counter = 0;
		bool allow_jinpachi = true, input_context = false;
		std::array<std::uint32_t, 2> handicaps{};
		std::array<std::uint32_t, 2> ranks{};
		std::array<std::uint8_t, 13> options{};
		std::array<std::uint8_t, 1> rounds_option{};
		std::array<std::uint8_t, 44> rules{};
		replay_carry::game_values carry;
		std::optional<replay_notices::placements> placements;
		std::unique_ptr<replay_restore::generators> before_styles, before_match;
	};

	// Read a game_start.capsule or the JSON payload of capsule_start. The caller authenticates its transport identity;
	// this reader checks exactly the same start values the fresh-game player uses, without needing a completed game.
	inline pbd::set_channel::result<pbd_capsule_start> pbd_read_capsule_start(
		const pbd::set_channel::json_value& cap, std::uint32_t seed) noexcept
	{
		using namespace pbd::replay_game_file;
		try
		{
			if (!seed || integer(cap, "sessionSeed") != seed || integer(cap, "initializer") != 2 ||
				!is(cap, "schema", "pbd-game-start-capsule-draft-2") || !is(cap, "anchor", "before-native-match-initializer"))
				return {{}, "capsule_start_identity"};
			pbd_capsule_start out;
			if (const auto* players = cap.find("players"))
			{
				if (players->kind != kind::array || players->items.size() != 2)
					return {{}, "capsule_start_players"};
				for (std::size_t side = 0; side < out.ranks.size(); ++side)
					if (const auto* rank = players->items[side].find("rank"); rank &&
																			  !pbd::json::detail::number32(rank, 0, 36, out.ranks[side]))
						return {{}, "capsule_start_ranks"};
			}
			out.placements = replay_notices::read(cap.find("placements"));
			if (cap.find("placements") && !out.placements)
				return {{}, "capsule_start_placements"};
			const auto& choice = field(cap, "selection");
			if (const auto* allow = choice.find("allowJinpachi"); allow &&
																  !pbd::set_channel::detail::boolean(allow, out.allow_jinpachi))
				return {{}, "capsule_start_selection"};
			out.left = integer(choice, "leftRow", 84);
			out.right = integer(choice, "rightRow", 84);
			out.stage = integer(choice, "stage", 0x25);
			const auto rule = out.allow_jinpachi ? character_rule::rule::stock : character_rule::rule::ban_jinpachi;
			if (!character_rule::row_available(rule, out.left) || !character_rule::row_available(rule, out.right) ||
				!stage_rule::loadable(out.stage))
				return {{}, "capsule_start_selection"};
			const auto time = integer(choice, "roundTimeIndex", 4), rounds = integer(choice, "roundsToWin", 5);
			if (!rounds)
				return {{}, "capsule_start_selection"};
			const auto& host = field(cap, "hostContext");
			out.prior = integer(host, "matchReseedsBefore", 125);
			out.mob_batches = integer(host, "mobBatchesBefore", 4096);
			out.counter = integer(host, "segmentCounter");
			const auto& carry = field(cap, "carryOver");
			auto decoded = replay_carry::decode_game_values(carry);
			if (!decoded.value)
				return {{}, "capsule_start_carry"};
			out.carry = std::move(*decoded.value);
			const auto& handicaps = field(carry, "appliedHandicapPercent");
			if (handicaps.kind != kind::array || handicaps.items.size() != 2)
				return {{}, "capsule_start_handicaps"};
			for (std::size_t side = 0; side < out.handicaps.size(); ++side)
			{
				out.handicaps[side] = integer(handicaps.items[side], 200);
				if (!out.handicaps[side])
					return {{}, "capsule_start_handicaps"};
			}
			if (const auto* input = carry.find("nativeInputContext"))
			{
				if (!is(*input, "schema", "dr-native-keyread-clock-draft-1"))
					return {{}, "capsule_start_input_clock"};
				out.input_counter = integer(*input, "counter");
				out.input_context = true;
			}
			const auto& producer = field(carry, "styleProducer");
			if (integer(carry, "nativeReplayFlag") || integer(producer, "nativeReplayFlag") ||
				integer(producer, "leftRow") != out.left || integer(producer, "rightRow") != out.right)
				return {{}, "capsule_start_style_producer"};
			out.ordinal = integer(carry, "nativeOrdinal", 32);
			out.pre_ordinal = integer(producer, "nativeOrdinal", 32);
			out.options = hex<13>(field(carry, "roundOptionBytesHex"));
			out.rounds_option = hex<1>(field(carry, "roundsOptionHex"));
			out.rules = hex<44>(field(carry, "gameRuleWordsHex"));
			const auto rule_word = [&](std::size_t at)
			{
				return std::uint32_t{out.rules[at]} << 24 | std::uint32_t{out.rules[at + 1]} << 16 |
				       std::uint32_t{out.rules[at + 2]} << 8 | out.rules[at + 3];
			};
			if (rule_word(4) != time || rule_word(8) != rounds || out.rounds_option[0] != rounds)
				return {{}, "capsule_start_rules"};
			out.before_styles = std::make_unique<replay_restore::generators>(field(producer, "generatorContext"));
			out.before_match = std::make_unique<replay_restore::generators>(field(cap, "generatorContext"));
			return {std::move(out), {}};
		}
		catch (...)
		{
			return {{}, "capsule_start_invalid"};
		}
	}

	inline pbd::set_channel::result<pbd_capsule_start> pbd_read_capsule_start(
		std::string_view text, std::uint32_t seed) noexcept
	{
		try
		{
			if (text.empty() || text.size() > 256 * 1024)
				return {{}, "capsule_start_extent"};
			auto parsed = pbd::set_channel::detail::json_parser(text, 16384, 256 * 1024, 65536).parse();
			if (!parsed.value)
				return {{}, "capsule_start_json"};
			return pbd_read_capsule_start(*parsed.value, seed);
		}
		catch (...)
		{
			return {{}, "capsule_start_invalid"};
		}
	}
} // namespace pbd::replay_start
