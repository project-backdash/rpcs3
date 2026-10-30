#pragma once

#include "pbd_replay_carry_types.h"
#include "Emu/Cell/pbd_set_channel.h"
#include <array>
#include <limits>
#include <string_view>

namespace pbd::replay_carry
{
	using json = pbd::set_channel::json_value;
	enum class decode_error
	{
		none,
		wrong_schema,
		wrong_shape,
		invalid_integer,
		invalid_reference
	};
	struct decode_result
	{
		std::optional<game_values> value;
		decode_error error = decode_error::none;
	};
	// What a game start carries from the game before it: every field of game_values but the match history. A recorder
	// may predate any of them, so each decodes as absent and a present one must be whole; a later game without all of
	// them cannot be played exactly (pbd_replay_game_file.h). The launcher names the same (replayCapsuleFields.ts).
	inline constexpr std::array<std::string_view, 18> carried_fields{"actorPlanarTerms", "descriptorDerivedFlag",
		"actorTransformSum", "actorDescriptorCode", "actorDescriptorDeltas", "actorReactionCursor", "actorMotionStep",
		"actorLastReactionCode", "actorReactionConditions", "actorBoneSegments", "actorReactionResult",
		"actorHitContacts", "actorSegmentDeltas", "actorPriorLife", "actorReactionScalar", "actorPreviousBlendWeight",
		"previousGameFlow", "actorRecords"};
	namespace codec
	{
		struct field_spec
		{
			const char* name;
			const char* schema;
		};
		inline const json& field(const json& node, std::string_view name)
		{
			const auto* found = node.find(name);
			if (!found)
				throw decode_error::wrong_shape;
			return *found;
		}
		inline std::uint32_t integer(const json& node, std::uint32_t maximum = UINT32_MAX)
		{
			std::uint32_t value = 0;
			if (!pbd::set_channel::detail::number32(&node, 0, maximum, value))
				throw decode_error::invalid_integer;
			return value;
		}
		inline const std::vector<json>& array(const json& node, std::size_t size)
		{
			if (node.kind != pbd::set_channel::json_kind::array || node.items.size() != size)
				throw decode_error::wrong_shape;
			return node.items;
		}
		template <typename T>
		std::array<T, 2> pair(const json& node)
		{
			const auto& items = array(node, 2);
			return {static_cast<T>(integer(items[0], std::numeric_limits<T>::max())),
				static_cast<T>(integer(items[1], std::numeric_limits<T>::max()))};
		}
		template <typename T>
		std::array<std::array<T, 2>, 2> matrix(const json& node)
		{
			const auto& items = array(node, 2);
			return {pair<T>(items[0]), pair<T>(items[1])};
		}
		// N bytes in the form of the capsule's other byte fields (pbd_json.h parse_lower_hex): any other spelling of the
		// same bytes is refused, so a file has one reading.
		template <std::size_t N>
		std::array<std::uint8_t, N> hex_bytes(const json& node)
		{
			std::array<std::uint8_t, N> out{};
			if (node.kind != pbd::set_channel::json_kind::string || !pbd::json::parse_lower_hex(node.text, out))
				throw decode_error::wrong_shape;
			return out;
		}
		inline std::array<std::uint8_t, 2> conditions(const json& node)
		{
			const auto& items = array(node, 2);
			return {static_cast<std::uint8_t>(integer(items[0], 1)), static_cast<std::uint8_t>(integer(items[1], 1))};
		}
		template <typename T, typename Reader>
		std::optional<T> optional(const json& root, field_spec spec, Reader read)
		{
			const auto* saved = root.find(spec.name);
			if (!saved)
				return {};
			if (saved->kind != pbd::set_channel::json_kind::object ||
				!pbd::set_channel::detail::is(saved->find("schema"), spec.schema))
				throw decode_error::wrong_schema;
			return read(*saved);
		}
		// {base, byteOffset, bankStyle} of a bankOffset port; the port has exactly these fields beside its kind or origin.
		inline replay_bank_offset::reference bank_offset(const json& port, std::uint32_t alignment)
		{
			const auto* name = port.find("base");
			const auto base = name && name->kind == pbd::set_channel::json_kind::string ?
			                      replay_bank_offset::parse_base(name->text) :
			                      std::nullopt;
			if (!base || port.fields.size() != 4)
				throw decode_error::invalid_reference;
			const replay_bank_offset::reference ref{*base, integer(field(port, "byteOffset")),
				static_cast<std::uint16_t>(integer(field(port, "bankStyle"), 0xffff))};
			if (!replay_bank_offset::valid(ref, alignment))
				throw decode_error::invalid_reference;
			return ref;
		}
		inline json encode_bank_offset(const char* tag, const char* kind, replay_bank_offset::reference ref)
		{
			return json::object({{tag, json::string(kind)},
				{"base", json::string(std::string(replay_bank_offset::name(ref.base)))},
				{"byteOffset", json::number(ref.byte_offset)}, {"bankStyle", json::number(ref.bank_style)}});
		}
		inline actor_reaction_cursor cursor(const json& node)
		{
			actor_reaction_cursor out;
			const auto& ports = array(field(node, "ports"), 2);
			for (std::size_t side = 0; side < ports.size(); ++side)
			{
				const auto& port = ports[side];
				auto& ref = out.ports[side];
				// Every port has exactly the fields of its kind, so no extra word, such as an address, rides along.
				if (pbd::set_channel::detail::is(port.find("kind"), "none"))
				{
					if (port.fields.size() != 1)
						throw decode_error::invalid_reference;
					continue;
				}
				if (pbd::set_channel::detail::is(port.find("kind"), "bankOffset"))
				{
					const auto offset = bank_offset(port, replay_bank_offset::cursor_alignment);
					ref = {3, 0, offset.byte_offset, offset.bank_style, offset.base};
					continue;
				}
				if (pbd::set_channel::detail::is(port.find("kind"), "records80"))
					ref.kind = 1;
				else if (pbd::set_channel::detail::is(port.find("kind"), "samples16"))
					ref.kind = 2;
				else
					throw decode_error::invalid_reference;
				if (port.fields.size() != 4)
					throw decode_error::invalid_reference;
				ref.index = integer(field(port, "index"));
				ref.byte_offset = integer(field(port, "byteOffset"), ref.kind == 1 ? 0x4f : 0);
				ref.bank_style = static_cast<std::uint16_t>(integer(field(port, "bankStyle"), 0xffff));
				if (ref.byte_offset & 1)
					throw decode_error::invalid_reference;
			}
			return out;
		}
		inline std::array<std::uint32_t, 3> vector_bits(const json& node)
		{
			const auto& items = array(node, 3);
			return {integer(items[0]), integer(items[1]), integer(items[2])};
		}
		inline actor_bone_segments bones(const json& node)
		{
			actor_bone_segments out;
			const auto& ports = array(field(node, "ports"), 2);
			for (std::size_t side = 0; side < ports.size(); ++side)
			{
				const auto& segments = array(ports[side], 4);
				for (std::size_t index = 0; index < segments.size(); ++index)
					out.ports[side][index] = {vector_bits(field(segments[index], "startF32Bits")),
						vector_bits(field(segments[index], "endF32Bits"))};
			}
			return out;
		}
		inline json encode_bones(const actor_bone_segments& saved)
		{
			std::vector<json> ports;
			for (const auto& port : saved.ports)
			{
				std::vector<json> segments;
				for (const auto& segment : port)
				{
					const auto& start = segment.start_bits;
					const auto& end = segment.end_bits;
					segments.push_back(json::object(
						{{"startF32Bits",
							 json::array({json::number(start[0]), json::number(start[1]), json::number(start[2])})},
							{"endF32Bits",
								json::array({json::number(end[0]), json::number(end[1]), json::number(end[2])})}}));
				}
				ports.push_back(json::array(std::move(segments)));
			}
			return json::array(std::move(ports));
		}
		inline actor_segment_deltas deltas(const json& node)
		{
			actor_segment_deltas out;
			const auto& ports = array(field(node, "ports"), 2);
			for (std::size_t side = 0; side < ports.size(); ++side)
			{
				const auto& port = ports[side];
				if (port.kind != pbd::set_channel::json_kind::object || port.fields.size() != 2)
					throw decode_error::wrong_shape;
				out.ports[side] = {vector_bits(field(port, "ownF32Bits")), vector_bits(field(port, "copiedF32Bits"))};
			}
			return out;
		}
		inline json encode_deltas(const actor_segment_deltas& saved)
		{
			std::vector<json> ports;
			for (const auto& port : saved.ports)
			{
				const auto& own = port.own_bits;
				const auto& copied = port.copied_bits;
				ports.push_back(
					json::object({{"ownF32Bits", json::array({json::number(own[0]), json::number(own[1]), json::number(own[2])})},
						{"copiedF32Bits", json::array({json::number(copied[0]), json::number(copied[1]), json::number(copied[2])})}}));
			}
			return json::array(std::move(ports));
		}
		inline actor_hit_contacts contacts(const json& node)
		{
			actor_hit_contacts out;
			const auto& ports = array(field(node, "ports"), 2);
			for (std::size_t side = 0; side < ports.size(); ++side)
			{
				const auto& entries = array(ports[side], 2);
				for (std::size_t index = 0; index < entries.size(); ++index)
				{
					const auto& entry = entries[index];
					if (entry.kind != pbd::set_channel::json_kind::object || entry.fields.size() != 3)
						throw decode_error::wrong_shape;
					out.ports[side][index] = {vector_bits(field(entry, "endF32Bits")),
						vector_bits(field(entry, "deltaF32Bits")),
						static_cast<std::uint16_t>(integer(field(entry, "sphereIndex"), 13))};
				}
			}
			return out;
		}
		inline json encode_contacts(const actor_hit_contacts& saved)
		{
			std::vector<json> ports;
			for (const auto& port : saved.ports)
			{
				std::vector<json> entries;
				for (const auto& contact : port)
				{
					const auto& end = contact.end_bits;
					const auto& delta = contact.delta_bits;
					entries.push_back(
						json::object({{"endF32Bits", json::array({json::number(end[0]), json::number(end[1]), json::number(end[2])})},
							{"deltaF32Bits", json::array({json::number(delta[0]), json::number(delta[1]), json::number(delta[2])})},
							{"sphereIndex", json::number(contact.sphere_index)}}));
				}
				ports.push_back(json::array(std::move(entries)));
			}
			return json::array(std::move(ports));
		}
		inline actor_reaction_result reaction_results(const json& node)
		{
			actor_reaction_result out;
			const auto& ports = array(field(node, "ports"), 2);
			for (std::size_t side = 0; side < ports.size(); ++side)
			{
				const auto& port = ports[side];
				auto& ref = out.ports[side];
				if (pbd::set_channel::detail::is(port.find("origin"), "none"))
				{
					if (port.fields.size() != 1)
						throw decode_error::invalid_reference;
					continue;
				}
				if (pbd::set_channel::detail::is(port.find("origin"), "bankOffset"))
				{
					const auto offset = bank_offset(port, replay_bank_offset::result_alignment);
					ref = {4, offset.byte_offset, offset.bank_style, offset.base};
					continue;
				}
				if (pbd::set_channel::detail::is(port.find("origin"), "commandGroup"))
					ref.kind = 1;
				else if (pbd::set_channel::detail::is(port.find("origin"), "recordEntry"))
					ref.kind = 2;
				else if (pbd::set_channel::detail::is(port.find("origin"), "dataPrefix"))
					ref.kind = 3;
				else
					throw decode_error::invalid_reference;
				if (port.fields.size() != 3)
					throw decode_error::invalid_reference;
				ref.index = integer(field(port, ref.kind == 3 ? "byteOffset" : "index"));
				if (ref.kind == 3 && (ref.index & 3))
					throw decode_error::invalid_reference;
				ref.bank_style = static_cast<std::uint16_t>(integer(field(port, "bankStyle"), 0xffff));
			}
			return out;
		}
		inline json encode_reaction_results(const actor_reaction_result& saved)
		{
			std::vector<json> ports;
			for (const auto& ref : saved.ports)
			{
				if (!ref.kind)
					ports.push_back(json::object({{"origin", json::string("none")}}));
				else if (ref.kind == 4)
					ports.push_back(encode_bank_offset("origin", "bankOffset", {ref.base, ref.index, ref.bank_style}));
				else
					ports.push_back(
						json::object({{"origin", json::string(ref.kind == 3 ? "dataPrefix" :
															  ref.kind == 1 ? "commandGroup" :
																			  "recordEntry")},
							{ref.kind == 3 ? "byteOffset" : "index", json::number(ref.index)},
							{"bankStyle", json::number(ref.bank_style)}}));
			}
			return json::array(std::move(ports));
		}
		template <typename T>
		json encode_pair(const std::array<T, 2>& values)
		{
			return json::array({json::number(values[0]), json::number(values[1])});
		}
		template <typename T>
		json encode_matrix(const std::array<std::array<T, 2>, 2>& values)
		{
			return json::array({encode_pair(values[0]), encode_pair(values[1])});
		}
		inline json encode_cursor(const actor_reaction_cursor& cursor)
		{
			std::vector<json> ports;
			for (const auto& ref : cursor.ports)
			{
				if (!ref.kind)
					ports.push_back(json::object({{"kind", json::string("none")}}));
				else if (ref.kind == 3)
					ports.push_back(encode_bank_offset("kind", "bankOffset", {ref.base, ref.byte_offset, ref.bank_style}));
				else
					ports.push_back(json::object({{"kind", json::string(ref.kind == 1 ? "records80" : "samples16")},
						{"index", json::number(ref.index)}, {"byteOffset", json::number(ref.byte_offset)},
						{"bankStyle", json::number(ref.bank_style)}}));
			}
			return json::array(std::move(ports));
		}
		inline void add(json& root, field_spec spec, json value)
		{
			value.fields.emplace_back("schema", json::string(spec.schema));
			root.fields.emplace_back(spec.name, std::move(value));
		}
		inline void encode_reaction_fields(json& out, const game_values& saved)
		{
			if (saved.reaction_cursor)
				add(out, {"actorReactionCursor", "dr-actor-reaction-cursor-draft-1"},
					json::object({{"ports", encode_cursor(*saved.reaction_cursor)}}));
			if (saved.motion_step)
				add(out, {"actorMotionStep", "dr-actor-motion-step-draft-1"},
					json::object({{"f32Bits", encode_matrix(saved.motion_step->bits)}}));
			if (saved.last_reaction_code)
				add(out, {"actorLastReactionCode", "pbd-actor-last-reaction-code-1"},
					json::object({{"codes", encode_pair(saved.last_reaction_code->codes)}}));
			if (saved.reaction_conditions)
				add(out, {"actorReactionConditions", "pbd-actor-reaction-conditions-1"},
					json::object({{"command11e", encode_pair(saved.reaction_conditions->command_11e)},
						{"command22", encode_pair(saved.reaction_conditions->command_22)}}));
		}
		inline void read_reaction_fields(const json& root, game_values& out)
		{
			out.reaction_cursor = optional<actor_reaction_cursor>(
				root, {"actorReactionCursor", "dr-actor-reaction-cursor-draft-1"}, cursor);
			out.motion_step = optional<actor_motion_step>(root, {"actorMotionStep", "dr-actor-motion-step-draft-1"},
				[](const json& node)
				{
					return actor_motion_step{matrix<std::uint32_t>(field(node, "f32Bits"))};
				});
			out.last_reaction_code =
				optional<actor_last_reaction_code>(root, {"actorLastReactionCode", "pbd-actor-last-reaction-code-1"},
					[](const json& node)
					{
						return actor_last_reaction_code{pair<std::uint16_t>(field(node, "codes"))};
					});
			out.reaction_conditions = optional<actor_reaction_conditions>(root,
				{"actorReactionConditions", "pbd-actor-reaction-conditions-1"},
				[](const json& node)
				{
					return actor_reaction_conditions{
						conditions(field(node, "command11e")), conditions(field(node, "command22"))};
				});
		}
		inline game_values read_actor_fields(const json& root)
		{
			game_values out;
			out.planar = optional<actor_planar_terms>(root, {"actorPlanarTerms", "dr-actor-planar-terms-draft-1"},
				[](const json& node)
				{
					return actor_planar_terms{matrix<std::uint32_t>(field(node, "s32Bits"))};
				});
			out.transform_sum =
				optional<actor_transform_sum>(root, {"actorTransformSum", "dr-actor-transform-sum-draft-1"},
					[](const json& node)
					{
						return actor_transform_sum{pair<std::uint32_t>(field(node, "f32Bits"))};
					});
			out.descriptor_code =
				optional<actor_descriptor_code>(root, {"actorDescriptorCode", "dr-actor-descriptor-code-draft-1"},
					[](const json& node)
					{
						return actor_descriptor_code{pair<std::uint16_t>(field(node, "codes"))};
					});
			out.descriptor_deltas =
				optional<actor_descriptor_deltas>(root, {"actorDescriptorDeltas", "dr-actor-descriptor-deltas-draft-1"},
					[](const json& node)
					{
						return actor_descriptor_deltas{matrix<std::uint16_t>(field(node, "s16Bits"))};
					});
			read_reaction_fields(root, out);
			out.bone_segments =
				optional<actor_bone_segments>(root, {"actorBoneSegments", "pbd-actor-bone-segments-1"}, bones);
			out.reaction_result = optional<actor_reaction_result>(
				root, {"actorReactionResult", "pbd-actor-reaction-result-1"}, reaction_results);
			out.hit_contacts = optional<actor_hit_contacts>(
				root, {"actorHitContacts", "pbd-actor-hit-contacts-1"}, contacts);
			out.segment_deltas = optional<actor_segment_deltas>(
				root, {"actorSegmentDeltas", "pbd-actor-segment-deltas-1"}, deltas);
			out.prior_life = optional<actor_scalar_bits>(root, {"actorPriorLife", "pbd-actor-prior-life-1"},
				[](const json& node)
				{
					return actor_scalar_bits{pair<std::uint32_t>(field(node, "fixed16Bits"))};
				});
			out.reaction_scalar = optional<actor_scalar_bits>(root, {"actorReactionScalar", "pbd-actor-reaction-scalar-1"},
				[](const json& node)
				{
					return actor_scalar_bits{pair<std::uint32_t>(field(node, "f32Bits"))};
				});
			out.previous_blend_weight = optional<actor_scalar_bits>(
				root, {"actorPreviousBlendWeight", "pbd-actor-previous-blend-weight-1"},
				[](const json& node)
				{
					return actor_scalar_bits{pair<std::uint32_t>(field(node, "f32Bits"))};
				});
			return out;
		}
	} // namespace codec
	inline decode_result decode_game_values(const json& root)
	{
		try
		{
			if (root.kind != pbd::set_channel::json_kind::object)
				throw decode_error::wrong_shape;
			auto out = codec::read_actor_fields(root);
			out.history = codec::optional<vs_match_history>(root, {"vsMatchHistory", "dr-vs-match-history-draft-1"},
				[](const json& node)
				{
					return vs_match_history{codec::integer(codec::field(node, "completedGames")),
						codec::pair<std::uint16_t>(codec::field(node, "wins"))};
				});
			out.flow = codec::optional<previous_game_flow>(root, {"previousGameFlow", "pbd-previous-game-flow-1"},
				[](const json& node)
				{
					previous_game_flow saved;
					const auto& words = codec::array(codec::field(node, "words"), saved.words.size());
					for (std::size_t i = 0; i < saved.words.size(); ++i)
						saved.words[i] = codec::integer(words[i]);
					return saved;
				});
			out.records = codec::optional<actor_records>(root, {"actorRecords", "pbd-actor-records-1"},
				[](const json& node)
				{
					if (node.fields.size() != 2)
						throw decode_error::wrong_shape;
					return actor_records{codec::hex_bytes<replay_memory::actor_records_bytes>(codec::field(node, "bytesHex"))};
				});
			out.descriptor_derived_flag =
				codec::optional<std::uint8_t>(root, {"descriptorDerivedFlag", "dr-descriptor-derived-flag-draft-1"},
					[](const json& node)
					{
						return static_cast<std::uint8_t>(codec::integer(codec::field(node, "value"), 0xff));
					});
			return {std::move(out), decode_error::none};
		}
		catch (decode_error error)
		{
			return {{}, error};
		}
	}
	inline json encode_game_values(const game_values& saved)
	{
		json out = json::object();
		if (saved.history)
			codec::add(out, {"vsMatchHistory", "dr-vs-match-history-draft-1"},
				json::object({{"completedGames", json::number(saved.history->completed_games)},
					{"wins", codec::encode_pair(saved.history->wins)}}));
		if (saved.flow)
		{
			std::vector<json> words;
			for (const std::uint32_t word : saved.flow->words)
				words.push_back(json::number(word));
			codec::add(out, {"previousGameFlow", "pbd-previous-game-flow-1"},
				json::object({{"words", json::array(std::move(words))}}));
		}
		if (saved.records)
			codec::add(out, {"actorRecords", "pbd-actor-records-1"},
				json::object({{"bytesHex", json::string(pbd::json::lower_hex(saved.records->bytes))}}));
		if (saved.planar)
			codec::add(out, {"actorPlanarTerms", "dr-actor-planar-terms-draft-1"},
				json::object({{"s32Bits", codec::encode_matrix(saved.planar->bits)}}));
		if (saved.descriptor_derived_flag)
			codec::add(out, {"descriptorDerivedFlag", "dr-descriptor-derived-flag-draft-1"},
				json::object({{"value", json::number(*saved.descriptor_derived_flag)}}));
		if (saved.transform_sum)
			codec::add(out, {"actorTransformSum", "dr-actor-transform-sum-draft-1"},
				json::object({{"f32Bits", codec::encode_pair(saved.transform_sum->bits)}}));
		if (saved.descriptor_code)
			codec::add(out, {"actorDescriptorCode", "dr-actor-descriptor-code-draft-1"},
				json::object({{"codes", codec::encode_pair(saved.descriptor_code->codes)}}));
		if (saved.descriptor_deltas)
			codec::add(out, {"actorDescriptorDeltas", "dr-actor-descriptor-deltas-draft-1"},
				json::object({{"s16Bits", codec::encode_matrix(saved.descriptor_deltas->bits)}}));
		codec::encode_reaction_fields(out, saved);
		if (saved.bone_segments)
			codec::add(out, {"actorBoneSegments", "pbd-actor-bone-segments-1"},
				json::object({{"ports", codec::encode_bones(*saved.bone_segments)}}));
		if (saved.reaction_result)
			codec::add(out, {"actorReactionResult", "pbd-actor-reaction-result-1"},
				json::object({{"ports", codec::encode_reaction_results(*saved.reaction_result)}}));
		if (saved.hit_contacts)
			codec::add(out, {"actorHitContacts", "pbd-actor-hit-contacts-1"},
				json::object({{"ports", codec::encode_contacts(*saved.hit_contacts)}}));
		if (saved.segment_deltas)
			codec::add(out, {"actorSegmentDeltas", "pbd-actor-segment-deltas-1"},
				json::object({{"ports", codec::encode_deltas(*saved.segment_deltas)}}));
		if (saved.prior_life)
			codec::add(out, {"actorPriorLife", "pbd-actor-prior-life-1"},
				json::object({{"fixed16Bits", codec::encode_pair(saved.prior_life->bits)}}));
		if (saved.reaction_scalar)
			codec::add(out, {"actorReactionScalar", "pbd-actor-reaction-scalar-1"},
				json::object({{"f32Bits", codec::encode_pair(saved.reaction_scalar->bits)}}));
		if (saved.previous_blend_weight)
			codec::add(out, {"actorPreviousBlendWeight", "pbd-actor-previous-blend-weight-1"},
				json::object({{"f32Bits", codec::encode_pair(saved.previous_blend_weight->bits)}}));
		return out;
	}
} // namespace pbd::replay_carry
