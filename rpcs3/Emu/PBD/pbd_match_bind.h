#pragma once

#include "pbd_json_reader.h"
#include <optional>

namespace pbd::match_bind
{
	struct identity
	{
		std::uint64_t generation = 0;
		std::string checkpoint, emulator, recipe;
	};
	struct configuration
	{
		std::string set_id, address;
		std::uint32_t seed = 0, role = 0, session = 0, local_port = 0, remote_port = 0;
		// peer.route "relay": the session runs through the relay (the connection line's Relay tag). Optional: a launcher
		// that learned routeAtBind from ready_text sends "relay" or "direct", an older one sends nothing.
		bool relayed = false;
		bool activate = false, spectator_from_start = false;
		std::optional<std::uint32_t> prediction_bound, input_delay;
	};
	inline json::result<configuration> parse(std::string_view text, const identity& expected)
	{
		using namespace json;
		if (text.size() > 8192)
			return {{}, "match_bind_too_large"};
		auto parsed = detail::json_parser(text).parse();
		if (!parsed.value)
			return {{}, "match_bind_invalid_json"};
		const auto& value = *parsed.value;
		const auto* peer = value.find("peer");
		const auto* route = peer && peer->kind == json_kind::object ? peer->find("route") : nullptr;
		const auto* stream = value.find("spectatorFromStart");
		const auto* delay = peer && peer->kind == json_kind::object ? peer->find("inputDelay") : nullptr;
		const auto* prediction = peer && peer->kind == json_kind::object ? peer->find("predictionBound") : nullptr;
		configuration out;
		out.relayed = detail::is(route, "relay");
		out.activate = stream != nullptr;
		if (value.kind != json_kind::object || value.fields.size() != (stream ? 9u : 8u) ||
			(stream && stream->kind != json_kind::boolean) ||
			!detail::is(value.find("schema"), "pbd-match-bind-1") ||
			!detail::is(value.find("nativeGeneration"), std::to_string(expected.generation)) ||
			!detail::is(value.find("checkpointSha256"), expected.checkpoint) ||
			!detail::is(value.find("emulatorSha256"), expected.emulator) ||
			!detail::is(value.find("recipeSha256"), expected.recipe) ||
			!detail::identifier(value.find("setId")) || !detail::number32(value.find("seed"), 1, 0xffffffffu, out.seed) ||
			!peer || peer->kind != json_kind::object || peer->fields.size() != 5u + (route ? 1u : 0u) + (delay || prediction ? 2u : 0u) ||
			(route && !out.relayed && !detail::is(route, "direct")) ||
			!detail::number32(peer->find("role"), 0, 1, out.role) ||
			!detail::number32(peer->find("session"), 1, 0xffffffffu, out.session) ||
			!detail::number32(peer->find("localPort"), 1024, 65535, out.local_port) ||
			!detail::number32(peer->find("remotePort"), 1024, 65535, out.remote_port) ||
			!detail::plain_text(peer->find("address"), 2, 63))
			return {{}, "match_bind_identity_or_peer_invalid"};
		if (delay || prediction)
		{
			std::uint32_t limit = 0, frames = 0;
			if (!stream || !detail::number32(prediction, 2, 8, limit) ||
				(!detail::is(delay, "auto") && !detail::number32(delay, 0, 5, frames)))
				return {{}, "match_bind_delay_invalid"};
			out.prediction_bound = limit;
			if (!detail::is(delay, "auto")) out.input_delay = frames;
		}
		out.set_id = value.find("setId")->text;
		out.spectator_from_start = stream && stream->truth;
		out.address = peer->find("address")->text;
		// Only a literal address reaches the socket parser. Names, scope IDs and control text have no interpretation here.
		if (out.address.find_first_not_of("0123456789abcdefABCDEF:.") != out.address.npos)
			return {{}, "match_bind_address_invalid"};
		return {std::move(out), {}};
	}
	inline std::string ready_text(const identity& value, bool assigned_side_at_bind = false)
	{
		return "{\"schema\":\"pbd-match-standby-1\",\"nativeGeneration\":" + json::quoted(std::to_string(value.generation)) +
		       ",\"checkpointSha256\":" + json::quoted(value.checkpoint) + ",\"emulatorSha256\":" + json::quoted(value.emulator) +
		       ",\"recipeSha256\":" + json::quoted(value.recipe) + ",\"origin\":0,\"scene\":3,\"subscene\":44,\"assignedSideAtBind\":" +
		       (assigned_side_at_bind ? "true" : "false") + ",\"routeAtBind\":true,\"activationAtBind\":true,\"delayAtBind\":true,\"returnAtEnd\":true,\"watchAtBind\":true}\n";
	}
	inline std::string bound_text(const identity& value, std::string_view set, const configuration* peer = nullptr)
	{
		return "{\"schema\":\"pbd-match-bound-1\",\"setId\":" + json::quoted(set) +
		       ",\"nativeGeneration\":" + json::quoted(std::to_string(value.generation)) +
		       ",\"checkpointSha256\":" + json::quoted(value.checkpoint) + ",\"emulatorSha256\":" + json::quoted(value.emulator) +
		       ",\"recipeSha256\":" + json::quoted(value.recipe) +
		       (peer && peer->prediction_bound ? ",\"inputDelay\":" + (peer->input_delay ? std::to_string(*peer->input_delay) : "\"auto\"") +
		         ",\"predictionBound\":" + std::to_string(*peer->prediction_bound) : "") + "}\n";
	}
	inline bool activates(std::string_view text, const identity& expected, std::string_view set)
	{
		if (text.size() > 2048) return false;
		const auto parsed = json::detail::json_parser(text).parse();
		if (!parsed.value) return false;
		const auto& value = *parsed.value;
		return value.kind == json::json_kind::object && value.fields.size() == 4 &&
		       json::detail::is(value.find("schema"), "pbd-match-activate-1") &&
		       json::detail::is(value.find("setId"), set) &&
		       json::detail::is(value.find("nativeGeneration"), std::to_string(expected.generation)) &&
		       json::detail::is(value.find("checkpointSha256"), expected.checkpoint);
	}
} // namespace pbd::match_bind
