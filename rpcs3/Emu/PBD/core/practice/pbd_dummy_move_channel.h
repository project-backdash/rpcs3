#pragma once

#include "pbd_dummy_move_store.h"

#include <filesystem>
#include <fstream>
#include <string>

namespace pbd::practice::dummy_move
{
	struct picker_row
	{
		u32 id = 0;
		std::string name, command;
		bool selectable = false;
		u32 variants = 0;
	};
	struct picker_catalog
	{
		std::string generation;
		u32 character = 0, row = 0;
		std::vector<picker_row> moves;
	};
	struct configure_request
	{
		std::string id, generation;
		u32 character = 0;
		settings reactions;
	};
	inline json::result<configure_request> parse_configure(std::string_view text)
	{
		auto document = json::detail::json_parser(text, 1024, 32768, 1024).parse();
		if (!document.value)
			return {{}, document.error};
		const auto& value = *document.value;
		using namespace json::detail;
		configure_request out;
		if (!is(value.find("schema"), "pbd-practice-move-command-1") || !identifier(value.find("id")) ||
			!plain_text(value.find("generation"), 1, 100) || !number32(value.find("character"), 0, 34, out.character))
			return {{}, "invalid_command"};
		auto reactions = read_settings(value.find("reactions"));
		if (!reactions)
			return {{}, "invalid_reaction"};
		out.id = value.find("id")->text;
		out.generation = value.find("generation")->text;
		out.reactions = std::move(*reactions);
		return {std::move(out), {}};
	}
	inline bool available(const settings& configured, const picker_catalog& catalog)
	{
		for (const auto* reaction : {&configured.after_guard, &configured.after_hit})
		{
			if (!valid(*reaction))
				return false;
			if (reaction->kind != source::moves)
				continue;
			for (const auto id : reaction->ids)
			{
				const auto found = std::find_if(catalog.moves.begin(), catalog.moves.end(),
					[id](const auto& row)
					{
						return row.id == id && row.selectable && row.variants;
					});
				if (found == catalog.moves.end())
					return false;
			}
		}
		return true;
	}
	struct channel
	{
		std::filesystem::path root;
		std::string generation, seen, ack, error;
		bool apply(const configure_request& request, const picker_catalog& catalog)
		{
			if (request.id == seen)
				return false;
			seen = ack = request.id;
			error = request.generation != catalog.generation || request.character != catalog.character ? "fighters_changed" :
			        !available(request.reactions, catalog)                                             ? "move_unavailable" :
			                                                                                             "";
			if (!error.empty())
				return false;
			current_settings = request.reactions;
			return true;
		}
		bool poll(const picker_catalog& catalog)
		{
			if (generation != catalog.generation)
			{
				generation = catalog.generation;
				seen.clear();
				ack.clear();
				error.clear();
			}
			std::error_code ec;
			const auto path = root / "command.json";
			const auto bytes = std::filesystem::file_size(path, ec);
			if (ec || !bytes || bytes > 32768)
				return false;
			std::string text(static_cast<std::size_t>(bytes), '\0');
			std::ifstream file(path, std::ios::binary);
			if (!file.read(text.data(), text.size()))
				return false;
			auto parsed = parse_configure(text);
			return parsed.value && apply(*parsed.value, catalog);
		}
		std::string status(const picker_catalog& catalog, u32 player_row = 0xffffffffu) const
		{
			using json::json_value;
			auto moves = json_value::array();
			for (const auto& row : catalog.moves)
				moves.items.push_back(json_value::object({{"id", json_value::number(row.id)}, {"name", json_value::string(row.name)},
					{"command", json_value::string(row.command)}, {"selectable", json_value::boolean(row.selectable)},
					{"variants", json_value::number(row.variants)}}));
			return json::detail::serialize(json_value::object({{"schema", json_value::string("pbd-practice-move-list-1")},
					   {"generation", json_value::string(catalog.generation)}, {"character", json_value::number(catalog.character)},
					   {"row", json_value::number(catalog.row)}, {"playerRow", json_value::number(player_row)},
					   {"moves", std::move(moves)}, {"reactions", encode(current_settings)},
					   {"ack", json_value::string(ack)}, {"error", json_value::string(error)}})) +
			       '\n';
		}
	};
} // namespace pbd::practice::dummy_move
