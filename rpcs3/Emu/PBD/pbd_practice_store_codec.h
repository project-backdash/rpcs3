#pragma once

#include "pbd_practice_store_model.h"

namespace pbd::practice_store
{
	inline std::string default_slot_name(u32 index)
	{
		return "Slot " + std::to_string(index + 1);
	}
	inline recordings empty_recordings(u32 character)
	{
		recordings out;
		out.character = character;
		for (u32 i = 0; i < slot_count; ++i)
			out.slots[i].name = default_slot_name(i);
		return out;
	}

	// The take's words as base64url without padding, each word little-endian.
	inline std::string encode_words(const std::vector<u16>& words)
	{
		constexpr char alphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
		std::string text;
		text.reserve((words.size() * 16 + 5) / 6);
		u32 bits = 0, count = 0;
		const auto put = [&](u32 byte)
		{
			bits = (bits << 8) | byte;
			for (count += 8; count >= 6; count -= 6)
				text += alphabet[(bits >> (count - 6)) & 63];
		};
		for (const u16 word : words)
		{
			put(word & 0xffu);
			put(word >> 8);
		}
		if (count)
			text += alphabet[(bits << (6 - count)) & 63];
		return text;
	}

	// Refuses any other character, a length that is not whole words, and padding bits that are not zero.
	inline std::optional<std::vector<u16>> decode_words(std::string_view text)
	{
		std::vector<std::uint8_t> bytes;
		bytes.reserve(text.size() * 6 / 8);
		u32 bits = 0, count = 0;
		for (const unsigned char c : text)
		{
			const int value = c >= 'A' && c <= 'Z' ? c - 'A' : c >= 'a' && c <= 'z' ? c - 'a' + 26 :
			                                               c >= '0' && c <= '9'     ? c - '0' + 52 :
			                                               c == '-'                 ? 62 :
			                                               c == '_'                 ? 63 :
			                                                                          -1;
			if (value < 0)
				return std::nullopt;
			bits = (bits << 6) | static_cast<u32>(value);
			count += 6;
			if (count >= 8)
			{
				count -= 8;
				bytes.push_back(static_cast<std::uint8_t>(bits >> count));
			}
		}
		if (count >= 6 || (bits & ((1u << count) - 1u)) || bytes.size() % 2)
			return std::nullopt;
		std::vector<u16> words(bytes.size() / 2);
		for (std::size_t i = 0; i < words.size(); ++i)
			words[i] = static_cast<u16>(bytes[2 * i] | (bytes[2 * i + 1] << 8));
		return words;
	}

	namespace detail
	{
		using pbd::json::json_kind;
		using pbd::json::json_value;

		// A number within 0..2^53 clamped into [low, high]; anything else (absent, a string, a negative) is the fallback.
		inline u32 clamped(const json_value* value, u32 low, u32 high, u32 fallback)
		{
			std::uint64_t read = 0;
			if (!pbd::json::detail::number(value, 0, pbd::json::max_safe_integer, read))
				return fallback;
			return read < low ? low : read > high ? high :
			                                        static_cast<u32>(read);
		}
		inline bool flag(const json_value* value, bool fallback)
		{
			bool read = fallback;
			return pbd::json::detail::boolean(value, read) ? read : fallback;
		}
		inline std::optional<u32> exact(const json_value* value, u32 high)
		{
			u32 read = 0;
			if (!pbd::json::detail::number32(value, 0, high, read))
				return std::nullopt;
			return read;
		}
	} // namespace detail

	inline display read_display(const pbd::json::json_value* value)
	{
		display out;
		if (!value || value->kind != pbd::json::json_kind::object)
			return out;
		out.command_history = {detail::flag(value->find("p1History"), true), detail::flag(value->find("p2History"), true)};
		out.attack_info = detail::flag(value->find("attackInfo"), true);
		out.frame_data = detail::flag(value->find("frameData"), true);
		out.control_hints = detail::flag(value->find("controlHints"), true);
		out.movement_streaks = detail::flag(value->find("movementStreaks"), true);
		return out;
	}

	inline constexpr std::array<std::string_view, pbd::practice::game_option_count> game_option_names{"dummyAction", "counterHit",
		"inputDisplay", "attackData", "hitAnalysis"};

	// All five options or none: a partial set cannot be restored in the game's one commit.
	inline std::optional<game_options> read_game_options(const pbd::json::json_value* value)
	{
		if (!value || value->kind != pbd::json::json_kind::object)
			return std::nullopt;
		game_options out{};
		for (std::size_t i = 0; i < out.size(); ++i)
		{
			const auto* item = value->find(game_option_names[i]);
			if (!item || item->kind != pbd::json::json_kind::number)
				return std::nullopt;
			out[i] = detail::clamped(item, 0, pbd::practice::game_option_maximum[i], 0);
		}
		return restorable_options(out);
	}

	inline pbd::practice::movement::preset read_preset(const pbd::json::json_value* value)
	{
		return static_cast<pbd::practice::movement::preset>(detail::clamped(value, 0, pbd::practice::movement::preset_count - 1, 0));
	}

	// The Action After a Hit or Block of a profile from before reactionKind (its afterContact option and Movement row).
	inline pbd::practice::contact_reaction legacy_reaction(u32 after_contact, pbd::practice::movement::preset movement)
	{
		using pbd::practice::movement::preset;
		using pbd::practice::movement::reaction_input;
		if (after_contact >= 2)
			return {pbd::practice::after_contact::action, (std::min)(after_contact - 2, pbd::practice::reaction_slots - 1)};
		if (after_contact != 1)
			return {};
		constexpr std::array<std::pair<preset, reaction_input>, 6> inputs{{{preset::backdash, reaction_input::backdash},
			{preset::sidestep_left, reaction_input::sidestep_left}, {preset::sidestep_right, reaction_input::sidestep_right},
			{preset::sidewalk_left, reaction_input::sidewalk_left}, {preset::sidewalk_right, reaction_input::sidewalk_right},
			{preset::crouch, reaction_input::crouch}}};
		for (const auto& [kind, input] : inputs)
			if (kind == movement)
				return {pbd::practice::after_contact::movement, 0, input};
		return {};
	}

	inline dummy_rows read_dummy_rows(const pbd::json::json_value& value)
	{
		using namespace pbd::practice::movement;
		dummy_rows out;
		u32 wakeup = 0;
		if (pbd::json::detail::number32(value.find("wakeup"), 0, pbd::practice::pbd_wakeup_option_count - 1, wakeup))
			out.wakeup = static_cast<pbd::practice::pbd_wakeup_option>(wakeup);
		u32 throw_escape = 0;
		if (pbd::json::detail::number32(value.find("throwEscape"), 0, pbd::practice::throw_escape_option_count - 1, throw_escape))
			out.throw_escape = static_cast<pbd::practice::throw_escape_option>(throw_escape);
		if (const auto reactions = practice::dummy_move::read_settings(value.find("reactions")))
			out.reactions = *reactions;
		if (const auto* offense = value.find("offense"); offense && offense->kind == pbd::json::json_kind::object)
		{
			out.offense.movement = read_preset(offense->find("movement"));
			out.offense.wavedash_speed = detail::clamped(offense->find("wavedashSpeed"), min_wavedash_speed, max_wavedash_speed,
				default_wavedash_speed);
			auto& reaction = out.offense.reaction;
			reaction.kind = static_cast<pbd::practice::after_contact>(
				detail::clamped(offense->find("reactionKind"), 0, pbd::practice::after_contact_kinds - 1, 0));
			reaction.input = static_cast<reaction_input>(detail::clamped(offense->find("reactionInput"), 0, reaction_input_count - 1, 0));
			reaction.slot = detail::clamped(offense->find("reactionSlot"), 0, pbd::practice::reaction_slots - 1, 0);
			// A profile written before these fields keeps its row as afterContact: 0 None, 1 the Movement row's movement for a
			// moment, 2 to 9 Dummy Action 1 to 8. A Movement that is now a plain input becomes that input; a technique (it
			// played only after contacts then) is no reaction now.
			if (!offense->find("reactionKind") && offense->find("afterContact"))
				reaction = legacy_reaction(detail::clamped(offense->find("afterContact"), 0, 9, 0), out.offense.movement);
			out.offense.counted = static_cast<pbd::practice::contact_counted>(
				detail::clamped(offense->find("counted"), 0, pbd::practice::contact_counted_count - 1, 2));
			out.offense.times = detail::clamped(offense->find("times"), pbd::practice::min_reaction_times,
				pbd::practice::max_reaction_times, pbd::practice::min_reaction_times);
		}
		if (const auto* defense = value.find("defense"); defense && defense->kind == pbd::json::json_kind::object)
		{
			out.defense.movement = read_preset(defense->find("movement"));
			out.defense.wavedash_speed = detail::clamped(defense->find("wavedashSpeed"), min_wavedash_speed, max_wavedash_speed,
				default_wavedash_speed);
			const u32 end = detail::clamped(defense->find("wavedashEnd"), 0, wavedash_end_count - 1, 0);
			out.defense.wavedash_end = static_cast<wavedash_end>(end);
			out.defense.dash_end = static_cast<dash_end>(detail::clamped(defense->find("dashEnd"), 0, dash_end_count - 1, 0));
		}
		return out;
	}

	// Keys are base characters in decimal; any other key, and any entry that is not an object, is left out.
	inline std::map<u32, dummy_rows> read_dummies(const pbd::json::json_value* value)
	{
		std::map<u32, dummy_rows> out;
		if (!value || value->kind != pbd::json::json_kind::object)
			return out;
		for (const auto& [key, item] : value->fields)
		{
			u32 character = 0;
			const auto [end, error] = std::from_chars(key.data(), key.data() + key.size(), character);
			if (error != std::errc{} || end != key.data() + key.size() || key.empty() || (key.size() > 1 && key[0] == '0') ||
				character >= base_characters || item.kind != pbd::json::json_kind::object)
				continue;
			out[character] = read_dummy_rows(item);
		}
		return out;
	}

	// An unreadable document or another schema is refused (the caller sets the file aside); values are clamped.
	inline pbd::json::result<profile> parse_profile(std::string_view text)
	{
		auto parsed = pbd::json::detail::json_parser(text, max_nodes, pbd::json::max_file_bytes, max_string_bytes).parse();
		if (!parsed.value)
			return {{}, parsed.error};
		const auto& value = *parsed.value;
		if (value.kind != pbd::json::json_kind::object || !pbd::json::detail::is(value.find("schema"), profile_schema))
			return {{}, "bad_schema"};
		profile out;
		out.mode = pbd::json::detail::is(value.find("trainingMode"), "defense") ? pbd::practice::training_mode::defense :
		                                                                          pbd::practice::training_mode::offense;
		out.view = read_display(value.find("display"));
		for (std::size_t i = 0; i < restart_names.size(); ++i)
			if (pbd::json::detail::is(value.find("restartPosition"), restart_names[i]))
				out.restart = static_cast<restart_position>(i);
		out.dummy_back = pbd::json::detail::is(value.find("dummyDirection"), "back");
		out.switch_positions = detail::flag(value.find("switchPositions"), false);
		out.input_delay = detail::clamped(value.find("inputDelay"), 0, pbd::practice::max_input_delay, 0);
		out.options = read_game_options(value.find("gameOptions"));
		u32 guard = 0;
		if (pbd::json::detail::number32(value.find("dummyGuard"), 0, pbd::practice::pbd_guard_option_count - 1, guard))
			out.guard = static_cast<pbd::practice::pbd_guard_option>(guard);
		out.counter_hit_random = detail::flag(value.find("counterHitRandom"), false);
		out.dummies = read_dummies(value.find("dummies"));
		if (const auto* pick = value.find("lastPick"))
		{
			last_pick saved;
			if (pbd::json::detail::number32(pick->find("playerRow"), 0, max_character_row, saved.player_row) &&
				pbd::json::detail::number32(pick->find("dummyRow"), 0, max_character_row, saved.dummy_row) &&
				pbd::json::detail::number32(pick->find("stage"), 0, 255, saved.stage))
				out.pick = saved;
		}
		return {std::move(out), {}};
	}

	// The counts are cut to fit the take, in the order playback removes them: the stop chord's tail, then the leading and
	// the trailing neutral frames.
	inline void fit_counts(slot& out)
	{
		const u32 size = static_cast<u32>(out.words.size());
		out.stop_chord = std::min(out.stop_chord, size);
		out.leading_neutral = std::min(out.leading_neutral, size - out.stop_chord);
		out.trailing_neutral = std::min(out.trailing_neutral, size - out.stop_chord - out.leading_neutral);
	}

	inline slot read_slot(const pbd::json::json_value& value, u32 index)
	{
		slot out;
		out.name = default_slot_name(index);
		if (value.kind != pbd::json::json_kind::object)
			return out;
		const auto* words = value.find("words");
		auto decoded = words && words->kind == pbd::json::json_kind::string ? decode_words(words->text) : std::nullopt;
		if (decoded)
			out.words = std::move(*decoded);
		if (out.words.size() > max_take_words)
			out.words.resize(max_take_words);
		out.leading_neutral = detail::clamped(value.find("leadingNeutral"), 0, max_take_words, 0);
		out.trailing_neutral = detail::clamped(value.find("trailingNeutral"), 0, max_take_words, 0);
		out.stop_chord = detail::clamped(value.find("stopChord"), 0, max_take_words, 0);
		fit_counts(out);
		out.trim = detail::flag(value.find("trim"), true);
		out.enabled = detail::flag(value.find("enabled"), true);
		if (pbd::json::detail::plain_text(value.find("name"), 1, max_slot_name_bytes))
			out.name = value.find("name")->text;
		out.frequency = detail::clamped(value.find("frequency"), pbd::practice::min_frequency, pbd::practice::max_frequency, 1);
		out.interval = detail::clamped(value.find("interval"), 0, max_interval, 1);
		// Moves are kept only as a whole valid choice (practice::dummy_move::valid): a broken list leaves the recording.
		if (const auto moves = practice::dummy_move::read_reaction(value.find("moves")); moves && moves->kind == practice::dummy_move::source::moves)
			out.moves = moves->ids;
		return out;
	}

	inline pbd::json::result<recordings> read_recordings(const pbd::json::json_value& value, u32 character)
	{
		if (value.kind != pbd::json::json_kind::object || !pbd::json::detail::is(value.find("schema"), recordings_schema))
			return {{}, "bad_schema"};
		if (detail::exact(value.find("character"), base_characters - 1) != character)
			return {{}, "bad_character"};
		auto out = empty_recordings(character);
		if (const auto* playback = value.find("playback"); playback && playback->kind == pbd::json::json_kind::object)
			out.interval = detail::clamped(playback->find("interval"), 0, max_interval, default_interval);
		if (const auto* slots = value.find("slots"); slots && slots->kind == pbd::json::json_kind::array)
			for (std::size_t i = 0; i < std::min<std::size_t>(slots->items.size(), slot_count); ++i)
				out.slots[i] = read_slot(slots->items[i], static_cast<u32>(i));
		return {std::move(out), {}};
	}

	// A file written for another character is refused like an unreadable one.
	inline pbd::json::result<recordings> parse_recordings(std::string_view text, u32 character)
	{
		auto parsed = pbd::json::detail::json_parser(text, max_nodes, pbd::json::max_file_bytes, max_string_bytes).parse();
		if (!parsed.value)
			return {{}, parsed.error};
		return read_recordings(*parsed.value, character);
	}

	// Offense's Action After a Hit or Block may name a slot only while that slot holds a take; otherwise it is None.
	inline void drop_empty_reaction(offense_rows& rows, const recordings& slots)
	{
		if (rows.reaction.kind == pbd::practice::after_contact::action &&
			(rows.reaction.slot >= slots.slots.size() || (slots.slots[rows.reaction.slot].words.empty() && slots.slots[rows.reaction.slot].moves.empty())))
			rows.reaction = {};
	}

	namespace detail
	{
		// Adds one field to an object; the writers below list a file's fields in the order they are written.
		inline void put(json_value& object, std::string key, json_value value)
		{
			object.fields.emplace_back(std::move(key), std::move(value));
		}
		inline json_value code(auto value)
		{
			return json_value::number(static_cast<u32>(value));
		}
	} // namespace detail

	inline pbd::json::json_value defense_json(const defense_rows& rows)
	{
		using detail::code;
		auto out = pbd::json::json_value::object();
		detail::put(out, "movement", code(rows.movement));
		detail::put(out, "wavedashSpeed", code(rows.wavedash_speed));
		detail::put(out, "wavedashEnd", code(rows.wavedash_end));
		detail::put(out, "dashEnd", code(rows.dash_end));
		return out;
	}

	inline pbd::json::json_value dummy_json(const dummy_rows& rows)
	{
		using detail::code;
		using detail::put;
		auto offense = pbd::json::json_value::object();
		put(offense, "movement", code(rows.offense.movement));
		put(offense, "wavedashSpeed", code(rows.offense.wavedash_speed));
		put(offense, "reactionKind", code(rows.offense.reaction.kind));
		put(offense, "reactionInput", code(rows.offense.reaction.input));
		put(offense, "reactionSlot", code(rows.offense.reaction.slot));
		put(offense, "counted", code(rows.offense.counted));
		put(offense, "times", code(rows.offense.times));
		return pbd::json::json_value::object({{"offense", std::move(offense)}, {"defense", defense_json(rows.defense)}, {"wakeup", code(rows.wakeup)},
			{"throwEscape", code(rows.throw_escape)}, {"reactions", practice::dummy_move::encode(rows.reactions)}});
	}

	inline pbd::json::json_value display_json(const display& view)
	{
		using pbd::json::json_value;
		auto out = json_value::object();
		detail::put(out, "p1History", json_value::boolean(view.command_history[0]));
		detail::put(out, "p2History", json_value::boolean(view.command_history[1]));
		detail::put(out, "attackInfo", json_value::boolean(view.attack_info));
		detail::put(out, "frameData", json_value::boolean(view.frame_data));
		detail::put(out, "controlHints", json_value::boolean(view.control_hints));
		detail::put(out, "movementStreaks", json_value::boolean(view.movement_streaks));
		return out;
	}

	inline std::string serialize(const profile& value, std::string_view saved_at)
	{
		using detail::code;
		using detail::put;
		using pbd::json::json_value;
		auto out = json_value::object();
		put(out, "schema", json_value::string(std::string(profile_schema)));
		put(out, "savedAt", json_value::string(std::string(saved_at)));
		put(out, "trainingMode", json_value::string(value.mode == pbd::practice::training_mode::defense ? "defense" : "offense"));
		put(out, "display", display_json(value.view));
		put(out, "restartPosition", json_value::string(std::string(restart_names[static_cast<std::size_t>(value.restart)])));
		put(out, "dummyDirection", json_value::string(value.dummy_back ? "back" : "front"));
		put(out, "switchPositions", json_value::boolean(value.switch_positions));
		put(out, "inputDelay", code(value.input_delay));
		if (value.options)
		{
			auto options = json_value::object();
			for (std::size_t i = 0; i < value.options->size(); ++i)
				put(options, std::string(game_option_names[i]), code((*value.options)[i]));
			put(out, "gameOptions", std::move(options));
		}
		put(out, "dummyGuard", code(value.guard));
		put(out, "counterHitRandom", json_value::boolean(value.counter_hit_random));
		auto dummies = json_value::object();
		for (const auto& [character, rows] : value.dummies)
			put(dummies, std::to_string(character), dummy_json(rows));
		put(out, "dummies", std::move(dummies));
		if (value.pick)
			put(out, "lastPick", json_value::object({{"playerRow", code(value.pick->player_row)}, {"dummyRow", code(value.pick->dummy_row)}, {"stage", code(value.pick->stage)}}));
		return pbd::json::detail::serialize(out) + '\n';
	}

	inline pbd::json::json_value slot_json(const slot& value)
	{
		using detail::code;
		using detail::put;
		using pbd::json::json_value;
		auto out = json_value::object();
		put(out, "words", json_value::string(encode_words(value.words)));
		put(out, "leadingNeutral", code(value.leading_neutral));
		put(out, "trailingNeutral", code(value.trailing_neutral));
		put(out, "stopChord", code(value.stop_chord));
		put(out, "trim", json_value::boolean(value.trim));
		put(out, "enabled", json_value::boolean(value.enabled));
		put(out, "name", json_value::string(value.name));
		put(out, "frequency", code(value.frequency));
		put(out, "interval", code(value.interval));
		if (!value.moves.empty())
			put(out, "moves", practice::dummy_move::encode(practice::dummy_move::reaction{practice::dummy_move::source::moves, 0, value.moves}));
		return out;
	}

	inline pbd::json::json_value recordings_json(const recordings& value, std::string_view saved_at)
	{
		using detail::code;
		using detail::put;
		using pbd::json::json_value;
		auto slots = json_value::array();
		for (const auto& each : value.slots)
			slots.items.push_back(slot_json(each));
		auto out = json_value::object();
		put(out, "schema", json_value::string(std::string(recordings_schema)));
		put(out, "character", code(value.character));
		put(out, "savedAt", json_value::string(std::string(saved_at)));
		put(out, "playback", json_value::object({{"interval", code(value.interval)}}));
		put(out, "slots", std::move(slots));
		return out;
	}
	inline std::string serialize(const recordings& value, std::string_view saved_at)
	{
		return pbd::json::detail::serialize(recordings_json(value, saved_at)) + '\n';
	}

} // namespace pbd::practice_store
