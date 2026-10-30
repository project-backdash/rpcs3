#pragma once
#include "pbd_set_channel.h"
#include "pbd_lobby_set_driver.h"
#include "Emu/PBD/pbd_replay_carry_codec.h"
#include <algorithm>
#include <array>
#include <optional>
#include <span>
#include <stdexcept>

// Backend input for a finalized one-game recording. This does not admit a shared file or qualify it.
namespace pbd::replay_game_file
{
	using json = pbd::set_channel::json_value;
	using kind = pbd::set_channel::json_kind;
	inline constexpr std::size_t max_inputs = 100000;
	inline constexpr std::size_t max_file_bytes = 16 * 1024 * 1024; // Private replay facts; set-channel remains256KiB.
	// A one-frame input segment needs ten values; the remaining budget holds the game and up to 32 round capsules.
	inline constexpr std::size_t max_nodes = max_inputs * 10 + 262144;
	inline constexpr std::size_t max_sync_string_bytes = max_inputs * 64; // One encoded48-byte fact per confirmed input.
	struct input
	{
		std::uint32_t segment = 0, counter = 0;
		std::array<std::uint16_t, 2> words{};
	};
	struct round_point
	{
		std::uint32_t kind = 0, ordinal = 0, counter = 0, input_offset = 0, segment = 0, spawn_selector = 0;
		std::array<std::array<std::int16_t, 2>, 2> result{};
		json generators;
		std::optional<pbd::replay_carry::game_values> carry;
	};
	struct round
	{
		std::uint32_t number = 0;
		round_point ready;
		std::optional<round_point> cleanup;
	};
	struct game
	{
		std::uint32_t number = 0;
		std::array<std::uint32_t, 2> final_score{};
		std::string emulator; // The SHA-256 of the emulator that recorded the file, in lower-case hex.
		json capsule;
		std::vector<input> inputs;
		std::vector<round> rounds;
		std::vector<pbd::lobby_set::sync_facts> sync;
		// The players' names as the recording saved them (match.players), left and right, which the replayed game draws under
		// the life bars (pbd::game_profile). Display only: empty where the file names nobody.
		std::array<std::string, 2> names;
		// The values the game's own Ranked screens were written with (capsule.rankScreens, pbd::rank_stakes::values_json).
		// Display only: none where the file names none or names them in another form.
		std::optional<pbd::rank_stakes::game_values> rank_screens;
	};

	// match.players.<side>.name when it is a string; anything else names nobody. A file plays the same with or without names,
	// so a missing or odd one never refuses the file.
	inline std::array<std::string, 2> player_names(const json& match)
	{
		std::array<std::string, 2> out;
		const auto* players = match.find("players");
		if (!players || players->kind != kind::object)
			return out;
		for (const auto& [key, side] : {std::pair<std::string_view, std::size_t>{"left", 0}, {"right", 1}})
			if (const auto* player = players->find(key); player && player->kind == kind::object)
				if (const auto* name = player->find("name"); name && name->kind == kind::string)
					out[side] = name->text;
		return out;
	}

	inline const json& field(const json& value, std::string_view name)
	{
		const auto* found = value.find(name);
		if (!found)
			throw std::invalid_argument("file_missing_field");
		return *found;
	}
	inline std::uint32_t integer(const json& value, std::uint32_t maximum = ~0u)
	{
		std::uint32_t out = 0;
		if (!pbd::set_channel::detail::number32(&value, 0, maximum, out))
			throw std::invalid_argument("file_integer");
		return out;
	}
	inline std::uint32_t integer(const json& value, std::string_view name, std::uint32_t maximum = ~0u)
	{
		return integer(field(value, name), maximum);
	}
	// The N bytes a string of exactly 2N lowercase hex digits spells (pbd_json.h parse_lower_hex).
	template <std::size_t N>
	std::array<std::uint8_t, N> hex(const json& value)
	{
		std::array<std::uint8_t, N> out{};
		if (value.kind != kind::string || !pbd::json::parse_lower_hex(value.text, out))
			throw std::invalid_argument("file_hex");
		return out;
	}
	inline bool is(const json& value, std::string_view name, std::string_view expected)
	{
		return pbd::set_channel::detail::is(value.find(name), expected);
	}
	inline bool unqualified(const json& value, std::string_view name)
	{
		const auto* found = value.find(name);
		return found && found->kind == kind::boolean && !found->truth;
	}
	inline pbd::lobby_set::sync_facts sync_point(std::string_view text)
	{
		constexpr std::string_view alphabet = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
		if (text.size() != 64)
			throw std::invalid_argument("private_sync_extent");
		std::array<std::uint8_t, 48> bytes{};
		for (std::size_t i = 0; i < 16; ++i)
		{
			std::uint32_t bits = 0;
			for (std::size_t j = 0; j < 4; ++j)
			{
				const auto digit = alphabet.find(text[4 * i + j]);
				if (digit == alphabet.npos)
					throw std::invalid_argument("private_sync_encoding");
				bits = (bits << 6) | static_cast<std::uint32_t>(digit);
			}
			for (std::size_t j = 0; j < 3; ++j)
				bytes[3 * i + j] = static_cast<std::uint8_t>(bits >> (16 - 8 * j));
		}
		const auto word = [&](std::size_t offset, std::size_t size)
		{
			std::uint64_t out = 0;
			for (std::size_t i = 0; i < size; ++i)
				out |= std::uint64_t{bytes[offset + i]} << (8 * i);
			return out;
		};
		pbd::lobby_set::sync_facts out{static_cast<std::uint32_t>(word(0, 4)), static_cast<std::uint32_t>(word(4, 4)),
			static_cast<std::uint32_t>(word(8, 4)), static_cast<std::uint32_t>(word(12, 4)),
			static_cast<std::uint32_t>(word(16, 4)), static_cast<std::uint32_t>(word(20, 4)),
			{word(24, 8), word(32, 8), word(40, 8)}, true};
		if ((out.scene != 8 && out.scene != 16 && out.scene != 17) || out.mask > 7 || out.gate_hold > 1 ||
			((out.scene == 16 || out.scene == 17) && out.mask != 7))
			throw std::invalid_argument("private_sync_policy");
		return out;
	}
	// Checks the file's identity (this reader's format, the one game executable, the capsule schema) and returns the
	// SHA-256 of the emulator that recorded it, which may be another build than this one (playback_refusal).
	inline std::optional<std::string> checked_identity(const json& root)
	{
		const auto& build = field(root, "build");
		if (!is(root, "format", "pbd-replay-v2") || integer(root, "version") != 2 || integer(root, "draftRevision") != 2 ||
			!is(build, "gameExecutableSha256", "b56e25e9210ed3c4fc19f3dfae686b7246aef386cdd4f88c80e0b89141ff887d") ||
			!is(build, "capsuleSchema", "pbd-game-start-capsule-draft-2"))
			return std::nullopt;
		const auto& emulator = field(build, "emulatorSha256");
		hex<32>(emulator); // 64 lower-case hex digits, or the file is refused.
		return emulator.text;
	}
	inline round_point point(const json& value, std::uint32_t kind_value, std::uint32_t ordinal,
		std::uint32_t wins, const std::vector<input>& inputs, bool complete = false)
	{
		round_point out;
		out.kind = integer(value, "initializer");
		out.ordinal = integer(value, "nativeOrdinal", 31);
		out.counter = integer(value, "frameCounter");
		out.input_offset = integer(value, "inputOffset");
		out.spawn_selector = integer(value, "spawnSelector");
		out.segment = integer(value, "sourceSegment");
		out.generators = field(value, "generatorContext");
		if (complete)
		{
			const auto& carry = field(value, "carryOver");
			auto decoded = pbd::replay_carry::decode_game_values(carry);
			if (!decoded.value || !decoded.value->history || !decoded.value->flow || !decoded.value->records ||
				!std::ranges::all_of(pbd::replay_carry::carried_fields, [&](auto name)
					{
						return carry.find(name) != nullptr;
					}))
				throw std::invalid_argument("private_round_carry");
			out.carry = std::move(*decoded.value);
		}
		if (out.kind != kind_value || out.ordinal != ordinal || out.input_offset >= inputs.size() ||
			inputs[out.input_offset].segment != out.segment || out.generators.kind != kind::object)
			throw std::invalid_argument("private_round_context");
		const auto& scores = field(value, "scoreResult");
		if (scores.kind != kind::array || scores.items.size() != 2)
			throw std::invalid_argument("private_round_score");
		for (std::size_t side = 0; side < 2; ++side)
		{
			const auto& pair = scores.items[side];
			if (pair.kind != kind::array || pair.items.size() != 2)
				throw std::invalid_argument("private_round_score");
			out.result[side][0] = static_cast<std::int16_t>(integer(pair.items[0], wins - 1));
			out.result[side][1] = pair.items[1].kind == kind::number && pair.items[1].text == "-1" ? -1 :
			                                                                                         static_cast<std::int16_t>(integer(pair.items[1], 2));
		}
		return out;
	}

	inline const char* read_inputs(game& out, const json& row, const json& timeline)
	{
		const auto& segments = field(row, "inputStream");
		if (segments.kind != kind::array || segments.items.empty() || segments.items.size() > max_inputs)
			return "private_input_segments";
		for (const auto& segment : segments.items)
		{
			const auto ticks = integer(segment, "ticks", static_cast<std::uint32_t>(max_inputs));
			const auto counter = integer(segment, "counter"), source = integer(segment, "sourceSegment");
			if (!ticks || integer(segment, "startTick") != out.inputs.size() || ticks > max_inputs - out.inputs.size() ||
				counter > ~0u - (ticks - 1))
				return "private_input_extent";
			const auto& changes = field(segment, "changes");
			if (changes.kind != kind::array || changes.items.empty() || changes.items.size() > ticks)
				return "private_input_changes";
			std::vector<input> held;
			std::vector<std::uint32_t> offsets;
			for (const auto& change : changes.items)
			{
				if (change.kind != kind::array || change.items.size() != 3)
					return "private_input_change";
				const auto offset = integer(change.items[0], ticks - 1);
				if (offsets.empty() ? offset != 0 : offset <= offsets.back())
					return "private_input_change_order";
				offsets.push_back(offset);
				held.push_back({source, 0, {static_cast<std::uint16_t>(integer(change.items[1], 65535)), static_cast<std::uint16_t>(integer(change.items[2], 65535))}});
			}
			std::size_t change = 0;
			for (std::uint32_t offset = 0; offset < ticks; ++offset)
			{
				if (change + 1 < offsets.size() && offsets[change + 1] == offset)
					++change;
				auto sample = held[change];
				sample.counter = counter + offset;
				out.inputs.push_back(sample);
			}
		}
		if (out.inputs.empty() || integer(timeline, "ticks") != out.inputs.size() ||
			integer(timeline, "endTick") != out.inputs.size())
			return "private_input_timeline";
		return nullptr;
	}

	inline const char* read_sync(game& out, const json& row)
	{
		if (const auto* sync = row.find("syncFacts"))
		{
			const auto& data = field(*sync, "data");
			if (!is(*sync, "encoding", "pbd-sync-facts-v1") || !is(*sync, "definition", "pbd-peer-sync-v3") ||
				data.kind != kind::string || data.text.size() != out.inputs.size() * 64)
				return "private_sync_identity";
			for (std::size_t i = 0; i < out.inputs.size(); ++i)
			{
				auto native = sync_point(std::string_view(data.text).substr(64 * i, 64));
				if (native.segment != out.inputs[i].segment ||
					(!out.sync.empty() && (out.sync.back().boundary == ~0u || native.boundary != out.sync.back().boundary + 1)))
					return "private_sync_sequence";
				out.sync.push_back(native);
			}
		}
		return nullptr;
	}

	inline const char* read_rounds(game& out, const json& rounds, std::uint32_t wins)
	{
		for (const auto& cap : rounds.items)
		{
			round saved;
			saved.number = integer(cap, "round", 32);
			const bool complete = is(cap, "schema", "pbd-round-start-capsule-draft-2");
			if (!saved.number || (!complete && !is(cap, "schema", "pbd-round-start-capsule-draft-1")) ||
				!is(cap, "anchor", "before-native-round-initializer") || !unqualified(cap, "qualified"))
				return "private_round_capsule";
			saved.ready = point(field(cap, "ready"), saved.number == 1 ? 4 : 5, saved.number - 1, wins, out.inputs, complete);
			const auto& cleanup = field(cap, "cleanup");
			if (saved.number == 1)
			{
				if (cleanup.kind != kind::null)
					return "private_first_round_cleanup";
			}
			else
			{
				saved.cleanup = point(cleanup, 6, saved.number - 1, wins, out.inputs, complete);
				if (saved.cleanup->input_offset >= saved.ready.input_offset)
					return "private_round_cleanup_order";
			}
			if (!out.rounds.empty() && (saved.number <= out.rounds.back().number ||
										   saved.ready.input_offset <= out.rounds.back().ready.input_offset))
				return "private_round_order";
			out.rounds.push_back(std::move(saved));
		}
		return nullptr;
	}

	inline pbd::set_channel::result<game> parse(std::string_view text)
	{
		try
		{
			if (text.empty() || text.size() > max_file_bytes)
				return {{}, "private_file_extent"};
			auto parsed = pbd::set_channel::detail::json_parser(text, max_nodes, max_file_bytes, max_sync_string_bytes).parse();
			if (!parsed.value)
				return {{}, parsed.error};
			const auto& root = *parsed.value;
			auto emulator = checked_identity(root);
			if (!emulator)
				return {{}, "private_file_identity"};
			const auto& games = field(root, "games");
			if (games.kind != kind::array || games.items.size() != 1)
				return {{}, "independent_game_required"};
			const auto& row = games.items[0];
			game out;
			out.emulator = std::move(*emulator);
			out.number = integer(row, "number", 65535);
			out.capsule = field(row, "capsule");
			const auto& match = field(row, "match");
			const auto& rounds = field(row, "roundCapsules");
			const auto& timeline = field(row, "timeline");
			if (!out.number || integer(match, "number") != out.number || !is(match, "endReason", "completed") ||
				!unqualified(match, "resultQualified") || !unqualified(out.capsule, "qualified") ||
				!unqualified(timeline, "qualified") || rounds.kind != kind::array || rounds.items.size() > 32)
				return {{}, "private_closed_game_required"};
			const auto& selection = field(out.capsule, "selection");
			const auto wins = integer(selection, "roundsToWin", 5);
			const auto& score = field(match, "score");
			const auto left = integer(score, "left", wins), right = integer(score, "right", wins);
			if (!wins || (left == wins) == (right == wins) || !is(match, "winner", left == wins ? "left" : "right") ||
				integer(match, "roundsToWin") != wins || integer(match, "stage") != integer(selection, "stage"))
				return {{}, "native_final_score_required"};
			out.final_score = {left, right};
			out.names = player_names(match);
			out.rank_screens = pbd::rank_stakes::read_values(out.capsule.find("rankScreens"));
			if (const auto* error = read_inputs(out, row, timeline))
				return {{}, error};
			if (const auto* error = read_sync(out, row))
				return {{}, error};
			if (const auto* error = read_rounds(out, rounds, wins))
				return {{}, error};
			return {std::move(out), {}};
		}
		catch (const std::exception& error)
		{
			return {{}, error.what()};
		}
	}

	// Whether a game's start record holds all the game before it left: its flow words, the fighters' records and
	// their typed values (pbd_replay_carry_codec.h carried_fields). Only later games require these fields.
	// The launcher tests the same names (replayV2File.ts).
	inline bool carries_previous_game(const json& capsule)
	{
		const auto* carry = capsule.find("carryOver");
		return carry && std::ranges::all_of(pbd::replay_carry::carried_fields, [&](std::string_view name)
							{
								return carry->find(name) != nullptr;
							});
	}

	// Every recording needs per-frame checks and a later game's carried start, regardless of the recording build.
	inline std::string playback_refusal(const pbd::set_channel::result<game>& parsed)
	{
		if (!parsed.value)
			return parsed.error;
		const auto& file = *parsed.value;
		if (file.sync.empty() || file.sync.size() != file.inputs.size())
			return "replay_sync_facts_required";
		if (file.number > 1 && !carries_previous_game(file.capsule))
			return "replay_previous_game_carry_required";
		return "";
	}

	// Where an input offset falls among a game's rounds: the round it is in (1 for the first, 0 before it) and that
	// round's first input. Round i + 1 starts at starts[i] and counts only when bit i of `captured` is set, so an offset
	// in a round never captured counts in the captured round before it.
	struct round_position
	{
		std::uint32_t round = 0, start = 0;
	};
	inline round_position recorded_round(std::uint32_t captured, std::span<const std::uint32_t> starts, std::size_t offset)
	{
		round_position out;
		for (std::uint32_t i = 0; i < starts.size() && i < 32; ++i)
			if (((captured >> i) & 1) && starts[i] <= offset)
				out = {i + 1, starts[i]};
		return out;
	}
} // namespace pbd::replay_game_file
