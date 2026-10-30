#pragma once

// Named recording positions share the autosave tape format. The catalog is read at launch; menu operations use
// these host copies and the store's background writer, never filesystem reads or waits on an emulation boundary.

#include "pbd_practice_store.h"
#include <stdexcept>

namespace pbd::practice_store
{
	inline constexpr u32 recording_set_count = 10;
	inline constexpr std::size_t max_set_name_bytes = 32;
	inline constexpr std::size_t max_set_bytes = 65536;

	struct set_content
	{
		recordings actions;
		defense_rows playback;
		bool operator==(const set_content&) const = default;
	};
	struct recording_set
	{
		set_content content;
		std::string saved_at;
		std::string name;
	};
	inline bool valid_set_name(std::string_view text)
	{
		const auto value = pbd::json::json_value::string(std::string(text));
		return pbd::json::detail::plain_text(&value, 1, max_set_name_bytes) && text.front() != ' ' && text.back() != ' ';
	}
	inline std::string set_name(const recording_set* value, u32 position)
	{
		return value && !value->name.empty() ? value->name : "Set " + std::to_string(position + 1);
	}

	// Name entry uses the same option list as the set chooser, including its pad and mouse controls.
	struct recording_name_editor
	{
		u32 character = 0, position = 0, group = 0, focus = 0;
		std::string text;
		inline static constexpr std::array<std::string_view, 7> letters{
			"ABCDEFGHIJ", "KLMNOPQRST", "UVWXYZ", "abcdefghij", "klmnopqrst", "uvwxyz", "0123456789"};
		enum class result
		{
			editing,
			save,
			cancel
		};
		std::string name() const
		{
			const auto end = text.find_last_not_of(' ');
			return end == std::string::npos ? std::string{} : text.substr(0, end + 1);
		}
		std::vector<pbd::practice::menu_option> choices() const
		{
			std::vector<pbd::practice::menu_option> out;
			const auto add = [&](std::string label, bool enabled = true)
			{
				pbd::practice::menu_option option;
				option.text = std::move(label);
				option.help = "Name: " + text + " | " + std::to_string(text.size()) + "/32 characters";
				option.enabled = enabled;
				out.push_back(std::move(option));
			};
			if (group)
			{
				add("Back to name");
				for (const char letter : letters[group - 1])
					add(std::string(1, letter), text.size() < max_set_name_bytes);
				return out;
			}
			add("Save recording set", valid_set_name(name()));
			for (const auto group_letters : letters)
				add(std::string(1, group_letters.front()) + " - " + group_letters.back());
			add("Space", !text.empty() && text.size() < max_set_name_bytes);
			add("Delete last character", !text.empty());
			add("Clear name", !text.empty());
			add("Cancel");
			return out;
		}
		result choose(u32 option)
		{
			const auto offered = choices();
			if (option >= offered.size() || !offered[option].enabled)
				return result::editing;
			focus = option;
			if (group)
			{
				if (option == 0)
					back();
				else
					text += letters[group - 1][option - 1];
				return result::editing;
			}
			if (option == 0)
				return result::save;
			if (option <= letters.size())
			{
				group = option;
				focus = 1;
			}
			else if (option == 8)
				text += ' ';
			else if (option == 9)
			{
				// Saved names may contain UTF-8 even though the option list inserts Latin letters and digits.
				auto last = text.size() - 1;
				while (last && (static_cast<unsigned char>(text[last]) & 0xc0) == 0x80)
					--last;
				text.erase(last);
			}
			else if (option == 10)
				text.clear();
			else
				return result::cancel;
			return result::editing;
		}
		bool back()
		{
			if (!group)
				return false;
			focus = std::exchange(group, 0);
			return true;
		}
	};
	static_assert(recording_name_editor::letters.size() + 5 <= pbd::practice::max_options);
	inline bool valid_set_position(u32 character, u32 position)
	{
		return character < base_characters && position < recording_set_count;
	}
	inline std::filesystem::path set_path(const std::filesystem::path& root, u32 character, u32 position)
	{
		return root / "recordings" / std::to_string(character) / ("set-" + std::to_string(position + 1) + ".json");
	}
	inline u32 action_count(const set_content& value)
	{
		return static_cast<u32>(std::count_if(value.actions.slots.begin(), value.actions.slots.end(), [](const slot& each)
			{
				return !each.words.empty() || !each.moves.empty();
			}));
	}
	inline bool valid_saved_at(std::string_view text)
	{
		if ((text.size() != 20 && text.size() != 24) || text[4] != '-' || text[7] != '-' || text[10] != 'T' || text[13] != ':' ||
			text[16] != ':' || text.back() != 'Z' || (text.size() == 24 && text[19] != '.'))
			return false;
		for (std::size_t i = 0; i + 1 < text.size(); ++i)
			if (i != 4 && i != 7 && i != 10 && i != 13 && i != 16 && i != 19 && (text[i] < '0' || text[i] > '9'))
				return false;
		const auto number = [text](std::size_t first, std::size_t count)
		{
			u32 out = 0;
			for (std::size_t i = first; i < first + count; ++i)
				out = out * 10 + text[i] - '0';
			return out;
		};
		const std::chrono::year_month_day date{std::chrono::year{static_cast<int>(number(0, 4))},
			std::chrono::month{number(5, 2)}, std::chrono::day{number(8, 2)}};
		return date.ok() && number(11, 2) < 24 && number(14, 2) < 60 && number(17, 2) < 60;
	}
	inline std::string serialize_set(const recording_set& value)
	{
		auto out = recordings_json(value.content.actions, value.saved_at);
		detail::put(out, "defense", defense_json(value.content.playback));
		if (!value.name.empty())
		{
			if (!valid_set_name(value.name))
				throw std::runtime_error("Recording set name is invalid");
			detail::put(out, "name", pbd::json::json_value::string(value.name));
		}
		return pbd::json::detail::serialize(out) + '\n';
	}
	inline pbd::json::result<recording_set> parse_set(std::string_view text, u32 character)
	{
		if (text.size() > max_set_bytes)
			return {{}, "file_too_large"};
		auto parsed = pbd::json::detail::json_parser(text, max_nodes, max_set_bytes, max_string_bytes).parse();
		if (!parsed.value)
			return {{}, parsed.error};
		const auto& value = *parsed.value;
		auto actions = read_recordings(value, character);
		if (!actions.value)
			return {{}, actions.error};
		const auto* saved_at = value.find("savedAt");
		if (!saved_at || saved_at->kind != pbd::json::json_kind::string || !valid_saved_at(saved_at->text))
			return {{}, "bad_saved_at"};
		const auto* name = value.find("name");
		if (name && (name->kind != pbd::json::json_kind::string || !valid_set_name(name->text)))
			return {{}, "bad_set_name"};
		// Existing autosaves have no Defense movement rows; their playback uses the menu defaults.
		const auto rows = read_dummy_rows(value);
		return {recording_set{{std::move(*actions.value), rows.defense}, saved_at->text, name ? name->text : std::string{}}, {}};
	}
	inline bool valid_content(const set_content& value)
	{
		if (value.actions.character >= base_characters || value.actions.interval > max_interval)
			return false;
		for (const auto& each : value.actions.slots)
		{
			if (each.words.size() > max_take_words || each.name.empty() || each.name.size() > max_slot_name_bytes)
				return false;
			const auto name = pbd::json::json_value::string(each.name);
			if (!pbd::json::detail::plain_text(&name, 1, max_slot_name_bytes))
				return false;
			const auto size = each.words.size();
			if (each.stop_chord > size || each.leading_neutral > size - each.stop_chord ||
				each.trailing_neutral > size - each.stop_chord - each.leading_neutral ||
				each.frequency < pbd::practice::min_frequency || each.frequency > pbd::practice::max_frequency || each.interval > max_interval)
				return false;
		}
		using namespace pbd::practice::movement;
		return static_cast<u32>(value.playback.movement) < preset_count &&
		       value.playback.wavedash_speed >= min_wavedash_speed && value.playback.wavedash_speed <= max_wavedash_speed &&
		       static_cast<u32>(value.playback.wavedash_end) < wavedash_end_count &&
		       static_cast<u32>(value.playback.dash_end) < dash_end_count;
	}
	inline loaded<recording_set> load_set(const std::filesystem::path& root, u32 character, u32 position)
	{
		if (!valid_set_position(character, position))
			return {load_state::absent, {}, "bad_set_position"};
		return load_file<recording_set>(set_path(root, character, position), [character](std::string_view text)
			{
				return parse_set(text, character);
			},
			recording_set{});
	}

	class recording_sets
	{
	public:
		std::vector<std::string> set_aside;

		void read(const std::filesystem::path& root)
		{
			for (u32 character = 0; character < base_characters; ++character)
				for (u32 position = 0; position < recording_set_count; ++position)
				{
					auto found = load_set(root, character, position);
					if (found.state == load_state::loaded)
						m_saved[{character, position}] = std::move(found.value);
					else if (found.state == load_state::set_aside)
						set_aside.push_back(std::to_string(character) + "/set-" + std::to_string(position + 1) + ": " + found.error);
				}
		}
		const recording_set* find(u32 character, u32 position) const
		{
			const auto found = m_saved.find({character, position});
			return found == m_saved.end() ? nullptr : &found->second;
		}
		bool pending(u32 character) const
		{
			return std::any_of(m_pending.begin(), m_pending.end(), [character](const save& each)
				{
					return each.value.content.actions.character == character;
				});
		}
		bool changed(const set_content& live) const
		{
			const auto character = live.actions.character;
			if (const auto kept = m_kept.find(character); kept != m_kept.end())
				return !kept->second.saved || live != kept->second.content;
			for (const auto& [key, value] : m_saved)
				if (key.first == character && value.content == live)
					return false;
			return live != set_content{empty_recordings(character), {}};
		}
		void begin_character(const set_content& live)
		{
			const auto character = live.actions.character;
			if (!m_kept.contains(character))
				m_kept.emplace(character, checkpoint{live, !changed(live)});
		}
		std::optional<set_content> select(u32 character, u32 position)
		{
			const auto* found = find(character, position);
			if (!found || pending(character))
				return {};
			m_kept[character] = {found->content, true};
			return found->content;
		}
		bool submit(writer& files, const std::filesystem::path& root, u32 position, set_content content, std::string saved_at, std::string name = {})
		{
			const auto character = content.actions.character;
			if (!valid_set_position(character, position) || pending(character) || !valid_content(content) || !valid_saved_at(saved_at) ||
				(!name.empty() && !valid_set_name(name)))
				return false;
			recording_set value{std::move(content), std::move(saved_at), std::move(name)};
			auto done = files.submit(set_path(root, character, position), [value]
				{
					return serialize_set(value);
				});
			m_pending.push_back({position, std::move(value), std::move(done)});
			return true;
		}
		struct completion
		{
			u32 character, position;
			bool saved;
		};
		std::vector<completion> poll()
		{
			std::vector<completion> out;
			for (auto it = m_pending.begin(); it != m_pending.end();)
			{
				if (it->done.wait_for(std::chrono::seconds(0)) != std::future_status::ready)
				{
					++it;
					continue;
				}
				const auto character = it->value.content.actions.character;
				const bool saved = it->done.get();
				if (saved)
				{
					m_kept[character] = {it->value.content, true};
					m_saved[{character, it->position}] = std::move(it->value);
				}
				out.push_back({character, it->position, saved});
				it = m_pending.erase(it);
			}
			return out;
		}

	private:
		struct checkpoint
		{
			set_content content;
			bool saved;
		};
		struct save
		{
			u32 position;
			recording_set value;
			std::future<bool> done;
		};
		std::map<std::pair<u32, u32>, recording_set> m_saved;
		std::map<u32, checkpoint> m_kept;
		std::vector<save> m_pending;
	};
} // namespace pbd::practice_store
