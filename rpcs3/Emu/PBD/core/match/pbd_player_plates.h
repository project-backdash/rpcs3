#pragma once

#include "pbd_set_channel_model.h"

// Display-only friend profiles. Both peers compare the selected ranks and names at the existing scene barrier.
namespace pbd::player_plates
{
	struct document
	{
		std::array<set_channel::fighter, 2> players;
		std::vector<std::string> ignored;
	};

	struct plan
	{
		std::array<std::string, 2> names;
		std::array<std::uint32_t, 2> ranks{};
		bool operator==(const plan&) const = default;
	};

	inline std::optional<document> parse(std::string_view text)
	{
		using namespace set_channel;
		auto parsed = detail::json_parser(text, max_json_nodes, max_file_bytes, max_file_bytes).parse();
		if (!parsed.value || !detail::is(parsed.value->find("schema"), "pbd-player-plates-1"))
			return {};
		document out;
		for (std::size_t side = 0; side < 2; ++side)
		{
			const auto key = side == 0 ? "left" : "right";
			const auto* value = parsed.value->find(key);
			if (!value)
				continue;
			if (value->kind != json_kind::object)
			{
				out.ignored.push_back(key);
				continue;
			}
			auto& player = out.players[side];
			if (const auto* name = value->find("name"))
			{
				if (detail::plain_text(name, 1, 16) && std::all_of(name->text.begin(), name->text.end(), [](unsigned char c)
														   {
															   return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == ' ' || c == '_' || c == '.' || c == '-';
														   }))
					player.name = name->text;
				else
					out.ignored.push_back(std::string(key) + ".name");
			}
			if (const auto* ranks = value->find("ranks"))
			{
				player.ranks = read_rank_table(ranks);
				if (!player.ranks)
					out.ignored.push_back(std::string(key) + ".ranks");
			}
		}
		return out;
	}

	inline plan selected(const document& profiles, const std::array<std::uint32_t, 2>& rows)
	{
		return {{profiles.players[0].name, profiles.players[1].name},
			{set_channel::shown_rank(profiles.players[0], rows[0]), set_channel::shown_rank(profiles.players[1], rows[1])}};
	}

	// Capsules require nonempty labels. Unknown names retain the recording format's existing generic player names.
	inline std::array<std::string, 2> replay_names(const plan& value)
	{
		return {value.names[0].empty() ? "Player 1" : value.names[0], value.names[1].empty() ? "Player 2" : value.names[1]};
	}

	inline std::uint32_t word(const plan& value)
	{
		std::uint32_t hash = 0x811c9dc5u;
		const auto mix = [&](std::uint8_t byte)
		{
			hash = (hash ^ byte) * 0x01000193u;
		};
		for (std::size_t side = 0; side < 2; ++side)
		{
			mix(static_cast<std::uint8_t>(value.names[side].size()));
			for (const auto byte : value.names[side])
				mix(static_cast<std::uint8_t>(byte));
			mix(game_profile::rank_byte(value.ranks[side]));
		}
		return hash;
	}

	inline plan agreed(const plan& value, bool same)
	{
		return same ? value : plan{};
	}
} // namespace pbd::player_plates
