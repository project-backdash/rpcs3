#pragma once

#include "pbd_json_reader.h"
#include "pbd_practice_stage.h"
#include "../Cell/pbd_hidden_selection.h"
#include <chrono>

namespace pbd::practice::start
{
	constexpr bool selection_boundary(std::uint32_t mode, std::uint32_t scene, std::uint32_t pending,
		std::uint32_t manager_state, std::uint32_t resource_context) noexcept
	{
		return mode == 10 && scene == 3 && pending == 44 && manager_state == 6 && resource_context == 14;
	}
	struct request
	{
		std::string id;
		std::uint32_t character_row = 0, costume = 0, stage = 0;
	};
	inline json::result<request> parse(std::string_view text)
	{
		auto parsed = json::detail::json_parser(text, 32, 4096, 128).parse();
		if (!parsed.value) return {{}, parsed.error};
		const auto& value = *parsed.value;
		request out;
		if (!json::detail::is(value.find("schema"), "pbd-practice-start-1") ||
			!json::detail::identifier(value.find("id")) || !json::detail::identifier(value.find("characterId")) ||
			!json::detail::number32(value.find("characterRow"), 0, 84, out.character_row) ||
			!json::detail::number32(value.find("costume"), 0, 4, out.costume) ||
			!json::detail::number32(value.find("stage"), 0, 255, out.stage) || !stage::valid(out.stage))
			return {{}, "invalid_practice_start"};
		out.id = value.find("id")->text;
		return {std::move(out), {}};
	}
	struct selection
	{
		std::array<std::uint32_t, 2> rows{};
		std::uint32_t stage = 0;
	};
	inline thread_local bool input_held = false;
	inline bool holds_port(std::uint32_t port) noexcept
	{
		return input_held && port < 2;
	}
	inline std::optional<selection> resolve(const request& wanted, const pbd::hidden_selection::tables& tables,
		std::uint32_t default_dummy)
	{
		const auto original = pbd::hidden_selection::resolve(tables, wanted.character_row);
		if (!original || wanted.costume > 4 || !stage::valid(wanted.stage)) return {};
		// Option zero keeps the chosen roster row. Other options follow the game's verified costume list.
		std::vector<std::uint32_t> rows{wanted.character_row};
		for (const auto row : tables.costumes[original->character].rows)
			if (row != wanted.character_row) rows.push_back(row);
		if (wanted.costume >= rows.size() || !pbd::hidden_selection::resolve(tables, rows[wanted.costume])) return {};
		if (!pbd::hidden_selection::resolve(tables, default_dummy)) default_dummy = rows[wanted.costume];
		// Mirror fighters need different costume rows or the stock selector substitutes the dummy's choice.
		if (default_dummy == rows[wanted.costume])
			for (const auto row : tables.costumes[original->character].rows)
				if (row != default_dummy && pbd::hidden_selection::resolve(tables, row)) { default_dummy = row; break; }
		return selection{{rows[wanted.costume], default_dummy}, wanted.stage};
	}
	struct snapshot
	{
		std::uint32_t mode = 0, scene = 0, pending = 0, stage = 0, counter = 0;
		std::array<std::uint32_t, 2> rows{};
		bool models_ready = false;
	};
	enum class progress { waiting, ready, refused };
	struct watch
	{
		selection wanted;
		std::optional<std::uint32_t> previous;
		std::uint32_t advancing = 0;
		progress step(const snapshot& now)
		{
			if (now.mode != 10) return progress::refused;
			if (now.scene != 17 || now.pending != 17 || !now.models_ready)
			{
				previous.reset(); advancing = 0; return progress::waiting;
			}
			if (now.rows != wanted.rows || now.stage != wanted.stage) return progress::refused;
			advancing = previous && now.counter == *previous + 1 ? advancing + 1 : 0;
			previous = now.counter;
			return advancing >= 3 ? progress::ready : progress::waiting;
		}
	};
	struct state
	{
		std::filesystem::path root;
		std::uint64_t generation = 0;
		std::optional<request> wanted;
		watch loaded;
		bool inspected = false, prepared = false, load_requested = false, ready = false, published = false, released = false;
		std::string refusal;
		std::chrono::steady_clock::time_point deadline{}, failed_at{};
	};
}
