#pragma once

#include "pbd_json_reader.h"
#include "pbd_replay_carry_codec.h"
#include <initializer_list>

namespace pbd::paired_start
{
	using value = pbd::json::json_value;
	using kind = pbd::json::json_kind;
	inline constexpr std::string_view schema = "pbd-paired-game-start-1";

	inline bool uint(const value& item, std::uint32_t maximum = 0xffffffffu)
	{
		std::uint32_t ignored = 0;
		return pbd::json::detail::number32(&item, 0, maximum, ignored);
	}

	// The fixed exceptions describe host-seeded ambient sound, the asynchronous key-read witness, and per-boot bone roots.
	// pbd_function_hook_state.h records ambient host seeding; pbd_tick_input.cpp records the guest clock increment/reset.
	// Neither capture can choose fields to exclude; every other byte and every field's presence must agree.
	inline bool agree(const value& left, const value& right, const std::string& path = {})
	{
		if (left.kind != right.kind)
			return false;
		if (path == "capsule.generatorContext.mt.index" || path == "capsule.carryOver.styleProducer.generatorContext.mt.index")
			return uint(left, 624) && uint(right, 624);
		if (path == "capsule.generatorContext.mt.words" || path == "capsule.carryOver.styleProducer.generatorContext.mt.words")
			return left.kind == kind::array && left.items.size() == 624 && right.items.size() == 624 &&
			       std::all_of(left.items.begin(), left.items.end(), [](const auto& item)
					   {
						   return uint(item);
					   }) &&
			       std::all_of(right.items.begin(), right.items.end(), [](const auto& item)
					   {
						   return uint(item);
					   });
		if (path == "capsule.carryOver.nativeInputContext.counter")
			return uint(left) && uint(right);
		if (path == "capsule.carryOver.actorRecords.bytesHex")
		{
			std::array<std::uint8_t, 4512> a{}, b{};
			if (left.kind != kind::string || !pbd::json::parse_lower_hex(left.text, a) || !pbd::json::parse_lower_hex(right.text, b))
				return false;
			for (std::size_t byte = 0; byte < a.size(); ++byte)
			{
				// Each 0x8d0-byte actor's +0x8c0 root links back to that actor; apply_actor_records keeps the local object.
				if ((byte >= 0x8c0 && byte < 0x8c4) || (byte >= 0x1190 && byte < 0x1194))
					continue;
				if (a[byte] != b[byte])
					return false;
			}
			return true;
		}
		if (left.kind == kind::array)
		{
			if (left.items.size() != right.items.size())
				return false;
			for (std::size_t index = 0; index < left.items.size(); ++index)
				if (!agree(left.items[index], right.items[index], path + '.' + std::to_string(index)))
					return false;
			return true;
		}
		if (left.kind == kind::object)
		{
			if (left.fields.size() != right.fields.size())
				return false;
			for (const auto& [key, item] : left.fields)
			{
				if (key.find('.') != std::string::npos)
					return false;
				const auto* other = right.find(key);
				if (!other || !agree(item, *other, path.empty() ? key : path + '.' + key))
					return false;
			}
			return true;
		}
		return left.text == right.text && left.truth == right.truth;
	}

	inline bool keys(const value& item, std::initializer_list<std::string_view> names)
	{
		return item.kind == kind::object && item.fields.size() == names.size() &&
		       std::all_of(names.begin(), names.end(), [&](const auto name)
				   {
					   return item.find(name);
				   });
	}

	// The launcher authenticates each original envelope hash; the local native reader independently checks the pair's values
	// and its selected side. It never applies the other side's state or replaces any captured witness.
	inline std::optional<std::array<std::uint8_t, 48>> read(const value& pair, const value& selected,
		std::uint32_t game, std::uint32_t boundary, std::uint32_t segment, std::uint32_t first, std::string_view emulator)
	{
		using namespace pbd::json::detail;
		if (!keys(pair, {"schema", "source", "captures", "proof"}) || !is(pair.find("schema"), schema) || !is(pair.find("source"), "left"))
			return {};
		const auto& captures = *pair.find("captures");
		if (!keys(captures, {"left", "right"}))
			return {};
		std::array<value, 2> envelopes;
		std::size_t index = 0;
		for (const auto side : {"left", "right"})
		{
			const auto& capture = *captures.find(side);
			if (!keys(capture, {"generation", "envelope", "envelopeIdentity"}))
				return {};
			const auto *generation = capture.find("generation"), *text = capture.find("envelope"), *identity = capture.find("envelopeIdentity");
			std::array<std::uint8_t, 32> hash{};
			if (generation->kind != kind::string || generation->text.empty() || generation->text.size() > 20 || generation->text.front() == '0' ||
				generation->text.find_first_not_of("0123456789") != std::string::npos || text->kind != kind::string || text->text.size() > 65536 ||
				identity->kind != kind::string || !pbd::json::parse_lower_hex(identity->text, hash))
				return {};
			auto parsed = json_parser(text->text, 16384, 65536, 65536).parse();
			if (!parsed.value || !keys(*parsed.value, {"schema", "game", "sourceBoundary", "build", "capsule"}) ||
				!is(parsed.value->find("schema"), "pbd-game-start-capsule-envelope-1"))
				return {};
			auto& envelope = *parsed.value;
			std::uint32_t source_game = 0, source_boundary = 0;
			const auto *build = envelope.find("build"), *capsule = envelope.find("capsule");
			if (!number32(envelope.find("game"), game, game, source_game) || !number32(envelope.find("sourceBoundary"), boundary, boundary, source_boundary) ||
				!build || !keys(*build, {"capsuleSchema", "emulatorSha256", "gameExecutableSha256"}) || !is(build->find("emulatorSha256"), emulator) ||
				!capsule || capsule->kind != kind::object)
				return {};
			// A continued game cannot reconstruct absent carry from a fresh boot.
			const auto* carry = capsule->find("carryOver");
			if (!carry || !carry->find("actorRecords") || !carry->find("nativeInputContext") || !carry->find("styleProducer"))
				return {};
			if (std::any_of(pbd::replay_carry::carried_fields.begin(), pbd::replay_carry::carried_fields.end(),
					[&](const auto field)
					{
						return !carry->find(field);
					}) ||
				!pbd::replay_carry::decode_game_values(*carry).value)
				return {};
			if (!index && !agree(*capsule, selected, "selected"))
				return {};
			envelopes[index++] = std::move(envelope);
		}
		if (!agree(envelopes[0], envelopes[1]))
			return {};
		const auto& proof = *pair.find("proof");
		std::uint32_t frame = 0;
		const auto* facts = proof.find("factsHex");
		std::array<std::uint8_t, 48> bytes{};
		if (!keys(proof, {"encoding", "definition", "firstFrame", "factsHex"}) ||
			!is(proof.find("encoding"), "pbd-sync-facts-v1") || !is(proof.find("definition"), "pbd-peer-sync-v3") ||
			!number32(proof.find("firstFrame"), first, first, frame) || !facts || facts->kind != kind::string || !pbd::json::parse_lower_hex(facts->text, bytes))
			return {};
		const auto word = [&](std::size_t at)
		{
			return std::uint32_t{bytes[at]} | (std::uint32_t{bytes[at + 1]} << 8) |
			       (std::uint32_t{bytes[at + 2]} << 16) | (std::uint32_t{bytes[at + 3]} << 24);
		};
		if (word(0) != boundary + 1 || word(4) != segment || word(12) > 32 || word(16) != 7 || word(20) > 1)
			return {};
		return bytes;
	}
} // namespace pbd::paired_start
