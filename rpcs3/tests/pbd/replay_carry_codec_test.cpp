#include "pbd_test.h"
#include "Emu/PBD/pbd_replay_carry_codec.h"
#include "Emu/PBD/pbd_replay_boot.h"
#include <algorithm>
#include <stdexcept>

namespace
{
	using namespace pbd::replay_carry;
	using pbd::test::check;
	// Strings may be as long as the text, as the replay file reader admits (pbd_replay_game_file.h) and unlike the
	// parser's 4096-byte default: the actors' bytes are one 9,024-digit string.
	json parse(std::string_view text)
	{
		using namespace pbd::json;
		auto result = pbd::set_channel::detail::json_parser(text, max_json_nodes, max_file_bytes, max_file_bytes).parse();
		check(result.value.has_value(), "JSON parse failed");
		return std::move(*result.value);
	}
	using codec::array;
	using codec::field;
	using codec::integer;
	using codec::pair;
	using codec::vector_bits;
	using pbd::set_channel::detail::is;
	void reaction_codes_round_trip()
	{
		constexpr auto golden =
			R"({"actorLastReactionCode":{"codes":[221,222],"schema":"pbd-actor-last-reaction-code-1"}})";
		auto result = decode_game_values(parse(golden));
		check(result.value.has_value(), "Reaction codes refused");
		check(result.value->last_reaction_code->codes == std::array<std::uint16_t, 2>{221, 222}, "Code values changed");
		const auto encoded = encode_game_values(*result.value);
		const auto& codes = field(encoded, "actorLastReactionCode");
		check(encoded.fields.size() == 1 && codes.fields.size() == 2 &&
				  is(codes.find("schema"), "pbd-actor-last-reaction-code-1") &&
				  pair<std::uint16_t>(field(codes, "codes")) == std::array<std::uint16_t, 2>{221, 222},
			"Reaction code contents changed");
		for (auto codes : {std::array<std::uint16_t, 2>{0, 0}, {0x8001, 0xffff}})
		{
			result.value->last_reaction_code->codes = codes;
			auto reread = decode_game_values(encode_game_values(*result.value));
			check(reread.value && reread.value->last_reaction_code->codes == codes, "Unsigned codes narrowed");
		}
	}
	void missing_fields_stay_missing()
	{
		auto result = decode_game_values(parse("{}"));
		check(result.value.has_value(), "Absent legacy fields refused");
		check(!result.value->last_reaction_code && !result.value->history && !result.value->reaction_conditions &&
				  !result.value->bone_segments && !result.value->reaction_result && !result.value->hit_contacts &&
				  !result.value->segment_deltas &&
				  !result.value->prior_life && !result.value->reaction_scalar && !result.value->previous_blend_weight,
			"Absent fields invented");
		check(encode_game_values(*result.value).fields.empty(), "Absent fields serialized");
		result.value->descriptor_derived_flag = std::uint8_t{0};
		auto reread = decode_game_values(encode_game_values(*result.value));
		check(reread.value && reread.value->descriptor_derived_flag.has_value(), "Captured zero flag discarded");
		check(*reread.value->descriptor_derived_flag == 0, "Zero flag changed");
	}
	void reaction_conditions_round_trip()
	{
		constexpr auto golden = R"({"actorReactionConditions":{"command11e":[0,0],"command22":[1,1],)"
								R"("schema":"pbd-actor-reaction-conditions-1"}})";
		auto result = decode_game_values(parse(golden));
		check(result.value && result.value->reaction_conditions, "Reaction conditions refused");
		const auto encoded = encode_game_values(*result.value);
		const auto& conditions = field(encoded, "actorReactionConditions");
		check(encoded.fields.size() == 1 && conditions.fields.size() == 3 &&
				  is(conditions.find("schema"), "pbd-actor-reaction-conditions-1") &&
				  pair<std::uint8_t>(field(conditions, "command11e")) == std::array<std::uint8_t, 2>{0, 0} &&
				  pair<std::uint8_t>(field(conditions, "command22")) == std::array<std::uint8_t, 2>{1, 1},
			"Condition contents changed");
		for (const auto flag : std::array<std::uint8_t, 2>{0, 1})
		{
			result.value->reaction_conditions = actor_reaction_conditions{{flag, flag}, {flag, flag}};
			auto reread = decode_game_values(encode_game_values(*result.value));
			check(reread.value && reread.value->reaction_conditions->command_11e == std::array{flag, flag} &&
					  reread.value->reaction_conditions->command_22 == std::array{flag, flag},
				"Condition flags changed");
		}
	}
	void malformed_conditions_are_refused()
	{
		for (const auto* text :
			{R"({"actorReactionConditions":{"schema":"pbd-actor-reaction-conditions-1","command11e":[0,0],"command22":[1,2]}})",
				R"({"actorReactionConditions":{"schema":"pbd-actor-reaction-conditions-1","command11e":[0,0],"command22":[1,-1]}})",
				R"({"actorReactionConditions":{"schema":"pbd-actor-reaction-conditions-1","command11e":[0.5,0],"command22":[1,1]}})",
				R"({"actorReactionConditions":{"schema":"pbd-actor-reaction-conditions-1","command11e":[0],"command22":[1,1]}})",
				R"({"actorReactionConditions":{"schema":"pbd-actor-reaction-conditions-1","command11e":[0,0]}})"})
		{
			const auto result = decode_game_values(parse(text));
			check(!result.value && result.error != decode_error::none, "Malformed condition accepted");
		}
	}
	void bone_segments_round_trip()
	{
		constexpr auto golden =
			R"({"actorBoneSegments":{"ports":[[)"
			R"({"startF32Bits":[1174427105,1146152683,3299003892],"endF32Bits":[1174429054,1145860122,3296943244]},)"
			R"({"startF32Bits":[1174073527,1147528472,3296289675],"endF32Bits":[1174429054,1145860122,3296943244]},)"
			R"({"startF32Bits":[0,0,0],"endF32Bits":[0,0,0]},{"startF32Bits":[0,0,0],"endF32Bits":[0,0,0]}],[)"
			R"({"startF32Bits":[1173928275,1147838486,3300953107],"endF32Bits":[1173928275,1147838486,3300953107]},)"
			R"({"startF32Bits":[1173482607,1141585108,3302909331],"endF32Bits":[1173588587,1125187144,3301286892]},)"
			R"({"startF32Bits":[0,0,0],"endF32Bits":[0,0,0]},{"startF32Bits":[0,0,0],"endF32Bits":[0,0,0]}]],)"
			R"("schema":"pbd-actor-bone-segments-1"}})";
		auto result = decode_game_values(parse(golden));
		check(result.value && result.value->bone_segments, "Bone segments refused");
		const auto encoded = encode_game_values(*result.value), expected = parse(golden);
		const auto& bones = field(encoded, "actorBoneSegments");
		check(encoded.fields.size() == 1 && bones.fields.size() == 2 &&
				  is(bones.find("schema"), "pbd-actor-bone-segments-1"),
			"Bone segment schema changed");
		const auto& ports = array(field(bones, "ports"), 2);
		const auto& expected_ports = array(field(field(expected, "actorBoneSegments"), "ports"), 2);
		for (std::size_t side = 0; side < ports.size(); ++side)
		{
			const auto& segments = array(ports[side], 4);
			for (std::size_t bone = 0; bone < segments.size(); ++bone)
			{
				const auto& wanted = expected_ports[side].items[bone];
				check(segments[bone].fields.size() == 2 &&
						  vector_bits(field(segments[bone], "startF32Bits")) == vector_bits(field(wanted, "startF32Bits")) &&
						  vector_bits(field(segments[bone], "endF32Bits")) == vector_bits(field(wanted, "endF32Bits")),
					"Bone segment values changed");
			}
		}
		result.value->bone_segments->ports[1][3].start_bits = {0x80000000, 0x7fc00001, 0xffffffff};
		auto reread = decode_game_values(encode_game_values(*result.value));
		check(reread.value && reread.value->bone_segments->ports == result.value->bone_segments->ports,
			"Bone coordinate bits changed");
		auto malformed = parse(golden);
		malformed.fields[0].second.fields[0].second.items[0].items[0].fields[0].second.items.pop_back();
		check(!decode_game_values(malformed).value, "Short coordinate accepted");
		malformed = parse(golden);
		malformed.fields[0].second.fields[0].second.items[0].items.pop_back();
		check(!decode_game_values(malformed).value, "Short segment array accepted");
		malformed = parse(golden);
		malformed.fields[0].second.fields[0].second.items.pop_back();
		check(!decode_game_values(malformed).value, "Missing fighter accepted");
		malformed = parse(golden);
		malformed.fields[0].second.fields[0].second.items[0].items[0].fields[0].second.items[0] =
			json::number(4294967296ULL);
		check(!decode_game_values(malformed).value, "Oversized coordinate word accepted");
	}
	void malformed_fields_are_refused()
	{
		for (const auto* text :
			{R"({"actorLastReactionCode":{"schema":"other","codes":[221,222]}})",
				R"({"actorLastReactionCode":{"schema":"pbd-actor-last-reaction-code-1","codes":[221]}})",
				R"({"actorLastReactionCode":{"schema":"pbd-actor-last-reaction-code-1","codes":[221,222,0]}})",
				R"({"actorLastReactionCode":{"schema":"pbd-actor-last-reaction-code-1","codes":[-1,222]}})",
				R"({"actorLastReactionCode":{"schema":"pbd-actor-last-reaction-code-1","codes":[65536,222]}})",
				R"({"actorLastReactionCode":{"schema":"pbd-actor-last-reaction-code-1","codes":[221.5,222]}})",
				R"({"actorLastReactionCode":{"schema":"pbd-actor-last-reaction-code-1","codes":["221",222]}})",
				R"({"actorMotionStep":{"schema":"dr-actor-motion-step-draft-1","f32Bits":[[0,0],[0,4294967296]]}})",
				R"({"actorDescriptorDeltas":{"schema":"dr-actor-descriptor-deltas-draft-1","s16Bits":[[0,0],[0,65536]]}})",
				R"({"actorReactionCursor":{"schema":"dr-actor-reaction-cursor-draft-1","ports":[{"kind":"no)"
				R"(ne","index":0},{"kind":"none"}]}})",
				R"({"actorReactionCursor":{"schema":"dr-actor-reaction-cursor-draft-1","ports":[{"kind":"re)"
				R"(cords80","index":0,"byteOffset":1,"bankStyle":0},{"kind":"none"}]}})",
				R"({"actorReactionCursor":{"schema":"dr-actor-reaction-cursor-draft-1","ports":[{"kind":"sa)"
				R"(mples16","index":0,"byteOffset":2,"bankStyle":0},{"kind":"none"}]}})"})
		{
			const auto result = decode_game_values(parse(text));
			check(!result.value && result.error != decode_error::none, "Malformed field accepted");
		}
		check(decode_game_values(parse("[]")).error == decode_error::wrong_shape, "Carry shape not checked");
	}
	void reaction_result_round_trip()
	{
		constexpr auto golden =
			R"({"actorReactionResult":{"ports":[)"
			R"({"origin":"commandGroup","index":21,"bankStyle":31},{"origin":"recordEntry","index":28,"bankStyle":16}],)"
			R"("schema":"pbd-actor-reaction-result-1"}})";
		const auto result = decode_game_values(parse(golden));
		check(result.value && result.value->reaction_result, "Reaction results refused");
		const auto encoded = encode_game_values(*result.value);
		const auto& results = field(encoded, "actorReactionResult");
		check(encoded.fields.size() == 1 && results.fields.size() == 2 &&
				  is(results.find("schema"), "pbd-actor-reaction-result-1"),
			"Reaction result schema changed");
		const auto& ports = array(field(results, "ports"), 2);
		check(ports[0].fields.size() == 3 && is(ports[0].find("origin"), "commandGroup") &&
				  integer(field(ports[0], "index")) == 21 && integer(field(ports[0], "bankStyle")) == 31 &&
				  ports[1].fields.size() == 3 && is(ports[1].find("origin"), "recordEntry") &&
				  integer(field(ports[1], "index")) == 28 && integer(field(ports[1], "bankStyle")) == 16,
			"Reaction result values changed");
		constexpr auto retained =
			R"({"actorReactionResult":{"ports":[)"
			R"({"origin":"commandGroup","index":15,"bankStyle":31},{"origin":"dataPrefix","byteOffset":48104,"bankStyle":16}],)"
			R"("schema":"pbd-actor-reaction-result-1"}})";
		const auto prefix = decode_game_values(parse(retained));
		check(prefix.value && prefix.value->reaction_result && prefix.value->reaction_result->ports[1].kind == 3,
			"Retained COM prefix refused");
		const auto prefix_encoded = encode_game_values(*prefix.value);
		const auto& prefix_results = field(prefix_encoded, "actorReactionResult");
		check(prefix_encoded.fields.size() == 1 && prefix_results.fields.size() == 2 &&
				  is(prefix_results.find("schema"), "pbd-actor-reaction-result-1"),
			"Retained COM prefix schema changed");
		const auto& prefix_ports = array(field(prefix_results, "ports"), 2);
		check(prefix_ports[0].fields.size() == 3 && is(prefix_ports[0].find("origin"), "commandGroup") &&
				  integer(field(prefix_ports[0], "index")) == 15 && integer(field(prefix_ports[0], "bankStyle")) == 31 &&
				  prefix_ports[1].fields.size() == 3 && is(prefix_ports[1].find("origin"), "dataPrefix") &&
				  integer(field(prefix_ports[1], "byteOffset")) == 48104 && integer(field(prefix_ports[1], "bankStyle")) == 16,
			"Retained COM prefix values changed");
		for (const auto* port : {R"({"origin":"other","index":21,"bankStyle":31})", R"({"origin":"none","index":0})",
				 R"({"origin":"commandGroup","index":-1,"bankStyle":31})",
				 R"({"origin":"recordEntry","index":28,"bankStyle":65536})",
				 R"({"origin":"recordEntry","index":28,"bankStyle":16,"address":560192836})",
				 R"({"origin":"dataPrefix","byteOffset":48105,"bankStyle":16})",
				 R"({"origin":"dataPrefix","index":48104,"bankStyle":16})",
				 R"({"origin":"dataPrefix","byteOffset":48104,"bankStyle":16,"address":560176476})"})
		{
			const std::string text = std::string{R"({"actorReactionResult":{"ports":[)"} + port +
			                         R"(,{"origin":"none"}],"schema":"pbd-actor-reaction-result-1"}})";
			check(!decode_game_values(parse(text)).value, "Invalid native result reference accepted");
		}
	}
	// A word left from the previous game's image keeps its slot offset, and old member forms keep their bytes.
	void bank_offsets_round_trip()
	{
		constexpr auto cursor =
			R"({"actorReactionCursor":{"ports":[)"
			R"({"kind":"bankOffset","base":"opposing","byteOffset":71612,"bankStyle":2},)"
			R"({"kind":"records80","index":110,"byteOffset":16,"bankStyle":16}],)"
			R"("schema":"dr-actor-reaction-cursor-draft-1"}})";
		const auto saved = decode_game_values(parse(cursor));
		check(saved.value && saved.value->reaction_cursor, "Bank offset cursor refused");
		const auto& left = saved.value->reaction_cursor->ports[0];
		check(left.kind == 3 && left.base == pbd::replay_bank_offset::base_kind::opposing && left.byte_offset == 0x117bc &&
				  left.bank_style == 2 && !left.index,
			"Bank offset cursor changed");
		check(saved.value->reaction_cursor->ports[1].kind == 1, "Record cursor changed form");
		const auto encoded = encode_game_values(*saved.value);
		const auto& cursors = field(encoded, "actorReactionCursor");
		check(encoded.fields.size() == 1 && cursors.fields.size() == 2 &&
				  is(cursors.find("schema"), "dr-actor-reaction-cursor-draft-1"),
			"Bank offset cursor schema changed");
		const auto& ports = array(field(cursors, "ports"), 2);
		check(ports[0].fields.size() == 4 && is(ports[0].find("kind"), "bankOffset") &&
				  is(ports[0].find("base"), "opposing") && integer(field(ports[0], "byteOffset")) == 71612 &&
				  integer(field(ports[0], "bankStyle")) == 2 && ports[1].fields.size() == 4 &&
				  is(ports[1].find("kind"), "records80") && integer(field(ports[1], "index")) == 110 &&
				  integer(field(ports[1], "byteOffset")) == 16 && integer(field(ports[1], "bankStyle")) == 16,
			"Bank offset cursor values changed");
		constexpr auto result =
			R"({"actorReactionResult":{"ports":[)"
			R"({"origin":"bankOffset","base":"own","byteOffset":48732,"bankStyle":16},)"
			R"({"origin":"dataPrefix","byteOffset":48104,"bankStyle":16}],)"
			R"("schema":"pbd-actor-reaction-result-1"}})";
		const auto retained = decode_game_values(parse(result));
		check(retained.value && retained.value->reaction_result, "Bank offset result refused");
		const auto& own = retained.value->reaction_result->ports[0];
		check(own.kind == 4 && own.base == pbd::replay_bank_offset::base_kind::own && own.index == 0xbe5c &&
				  own.bank_style == 16,
			"Bank offset result changed");
		const auto result_encoded = encode_game_values(*retained.value);
		const auto& results = field(result_encoded, "actorReactionResult");
		check(result_encoded.fields.size() == 1 && results.fields.size() == 2 &&
				  is(results.find("schema"), "pbd-actor-reaction-result-1"),
			"Bank offset result schema changed");
		const auto& result_ports = array(field(results, "ports"), 2);
		check(result_ports[0].fields.size() == 4 && is(result_ports[0].find("origin"), "bankOffset") &&
				  is(result_ports[0].find("base"), "own") && integer(field(result_ports[0], "byteOffset")) == 48732 &&
				  integer(field(result_ports[0], "bankStyle")) == 16 && result_ports[1].fields.size() == 3 &&
				  is(result_ports[1].find("origin"), "dataPrefix") && integer(field(result_ports[1], "byteOffset")) == 48104 &&
				  integer(field(result_ports[1], "bankStyle")) == 16,
			"Bank offset result values changed");
		constexpr auto table = R"({"kind":"bankOffset","base":"executable","byteOffset":12,"bankStyle":0})";
		const std::string text = std::string{R"({"actorReactionCursor":{"ports":[)"} + table +
		                         R"(,{"kind":"none"}],"schema":"dr-actor-reaction-cursor-draft-1"}})";
		const auto executable = decode_game_values(parse(text));
		check(executable.value && executable.value->reaction_cursor->ports[0].base ==
									  pbd::replay_bank_offset::base_kind::executable,
			"Executable table cursor refused");
	}
	void malformed_bank_offsets_are_refused()
	{
		for (const auto* port : {R"({"kind":"bankOffset","base":"opposing","byteOffset":71613,"bankStyle":2})",
				 R"({"kind":"bankOffset","base":"address","byteOffset":0,"bankStyle":2})",
				 R"({"kind":"bankOffset","base":1,"byteOffset":0,"bankStyle":2})",
				 R"({"kind":"bankOffset","byteOffset":0,"bankStyle":2})",
				 R"({"kind":"bankOffset","base":"opposing","byteOffset":566912,"bankStyle":2})",
				 R"({"kind":"bankOffset","base":"executable","byteOffset":32,"bankStyle":0})",
				 R"({"kind":"bankOffset","base":"executable","byteOffset":12,"bankStyle":2})",
				 R"({"kind":"bankOffset","base":"own","byteOffset":0,"bankStyle":65536})",
				 R"({"kind":"bankOffset","base":"own","index":0,"byteOffset":0,"bankStyle":2})",
				 R"({"kind":"bankOffset","base":"own","byteOffset":0,"bankStyle":2,"address":560765244})",
				 R"({"kind":"records80","index":0,"byteOffset":0,"bankStyle":2,"base":"own"})",
				 R"({"kind":"none","base":"own"})"})
		{
			const std::string text = std::string{R"({"actorReactionCursor":{"ports":[)"} + port +
			                         R"(,{"kind":"none"}],"schema":"dr-actor-reaction-cursor-draft-1"}})";
			check(!decode_game_values(parse(text)).value, "Invalid bank offset cursor accepted");
		}
		for (const auto* port : {R"({"origin":"bankOffset","base":"opposing","byteOffset":48734,"bankStyle":16})",
				 R"({"origin":"bankOffset","base":"opposing","byteOffset":48732})",
				 R"({"origin":"bankOffset","base":"opposing","index":48732,"bankStyle":16})",
				 R"({"origin":"bankOffset","base":"opposing","byteOffset":48732,"bankStyle":16,"address":1})"})
		{
			const std::string text = std::string{R"({"actorReactionResult":{"ports":[)"} + port +
			                         R"(,{"origin":"none"}],"schema":"pbd-actor-reaction-result-1"}})";
			check(!decode_game_values(parse(text)).value, "Invalid bank offset result accepted");
		}
	}
	void contact_geometry_round_trip()
	{
		constexpr auto golden =
			R"({"actorHitContacts":{"ports":[[)"
			R"({"endF32Bits":[1171722742,1144427244,3300628703],)"
			R"("deltaF32Bits":[1133968464,1135721453,1149839939],"sphereIndex":4},)"
			R"({"endF32Bits":[0,0,0],"deltaF32Bits":[0,0,0],"sphereIndex":0}],[)"
			R"({"endF32Bits":[1174060168,1151378171,3306095932],)"
			R"("deltaF32Bits":[3266091776,1134867390,3294340464],"sphereIndex":8},)"
			R"({"endF32Bits":[0,0,0],"deltaF32Bits":[0,0,0],"sphereIndex":0}]],"schema":"pbd-actor-hit-contacts-1"}})";
		auto saved = decode_game_values(parse(golden));
		check(saved.value && saved.value->hit_contacts, "Contact geometry refused");
		const auto encoded = encode_game_values(*saved.value), expected = parse(golden);
		const auto& contacts = field(encoded, "actorHitContacts");
		check(encoded.fields.size() == 1 && contacts.fields.size() == 2 &&
				  is(contacts.find("schema"), "pbd-actor-hit-contacts-1"),
			"Contact schema changed");
		const auto& ports = array(field(contacts, "ports"), 2);
		const auto& expected_ports = array(field(field(expected, "actorHitContacts"), "ports"), 2);
		for (std::size_t side = 0; side < ports.size(); ++side)
		{
			const auto& slots = array(ports[side], 2);
			for (std::size_t slot = 0; slot < slots.size(); ++slot)
			{
				const auto& wanted = expected_ports[side].items[slot];
				check(slots[slot].fields.size() == 3 &&
						  vector_bits(field(slots[slot], "endF32Bits")) == vector_bits(field(wanted, "endF32Bits")) &&
						  vector_bits(field(slots[slot], "deltaF32Bits")) == vector_bits(field(wanted, "deltaF32Bits")) &&
						  integer(field(slots[slot], "sphereIndex")) == integer(field(wanted, "sphereIndex")),
					"Contact values changed");
			}
		}
		saved.value->hit_contacts->ports[1][1].delta_bits = {0x80000000, 0x7fc00001, 0xffffffff};
		auto reread = decode_game_values(encode_game_values(*saved.value));
		check(reread.value && reread.value->hit_contacts->ports == saved.value->hit_contacts->ports,
			"Contact floating point bits changed");
		for (const auto* contact : {
				 R"({"endF32Bits":[0,0,0],"deltaF32Bits":[0,0,0],"sphereIndex":14})",
				 R"({"endF32Bits":[0,0],"deltaF32Bits":[0,0,0],"sphereIndex":0})",
				 R"({"endF32Bits":[0,0,0],"deltaF32Bits":[0,4294967296,0],"sphereIndex":0})",
				 R"({"endF32Bits":[0,0,0],"deltaF32Bits":[0,0,0],"sphereIndex":0,"address":269026152})"})
		{
			auto malformed = parse(golden);
			malformed.fields[0].second.fields[0].second.items[0].items[0] = parse(contact);
			check(!decode_game_values(malformed).value, "Invalid contact accepted");
		}
		auto malformed = parse(golden);
		malformed.fields[0].second.fields[0].second.items[0].items.pop_back();
		check(!decode_game_values(malformed).value, "Missing contact slot accepted");
	}
	void retained_vectors_round_trip()
	{
		constexpr auto golden =
			R"({"actorSegmentDeltas":{"ports":[)"
			R"({"ownF32Bits":[1072930816,3247364640,1132170048],"copiedF32Bits":[1133968464,1135721453,1149839939]},)"
			R"({"ownF32Bits":[0,0,0],"copiedF32Bits":[3266091776,1134867390,3294340464]}],"schema":"pbd-actor-segment-deltas-1"}})";
		auto saved = decode_game_values(parse(golden));
		check(saved.value && saved.value->segment_deltas, "Retained vectors refused");
		const auto encoded = encode_game_values(*saved.value), expected = parse(golden);
		const auto& deltas = field(encoded, "actorSegmentDeltas");
		check(encoded.fields.size() == 1 && deltas.fields.size() == 2 &&
				  is(deltas.find("schema"), "pbd-actor-segment-deltas-1"),
			"Retained vector schema changed");
		const auto& ports = array(field(deltas, "ports"), 2);
		const auto& expected_ports = array(field(field(expected, "actorSegmentDeltas"), "ports"), 2);
		for (std::size_t side = 0; side < ports.size(); ++side)
			check(ports[side].fields.size() == 2 &&
					  vector_bits(field(ports[side], "ownF32Bits")) == vector_bits(field(expected_ports[side], "ownF32Bits")) &&
					  vector_bits(field(ports[side], "copiedF32Bits")) == vector_bits(field(expected_ports[side], "copiedF32Bits")),
				"Retained vector values changed");
		saved.value->segment_deltas->ports[1].copied_bits = {0x80000000, 0x7fc00001, 0xffffffff};
		auto reread = decode_game_values(encode_game_values(*saved.value));
		check(reread.value && reread.value->segment_deltas->ports == saved.value->segment_deltas->ports,
			"Retained vector bits changed");
		auto malformed = parse(golden);
		malformed.fields[0].second.fields[0].second.items[0].fields.push_back({"byteOffset", json::number(0)});
		check(!decode_game_values(malformed).value, "Address-shaped vector accepted");
		malformed = parse(golden);
		malformed.fields[0].second.fields[0].second.items[0].fields[0].second.items.pop_back();
		check(!decode_game_values(malformed).value, "Short retained vector accepted");
	}
	void retained_scalars_round_trip()
	{
		constexpr auto golden =
			R"({"actorPriorLife":{"fixed16Bits":[4879155,0],"schema":"pbd-actor-prior-life-1"},)"
			R"("actorReactionScalar":{"f32Bits":[0,1112014848],"schema":"pbd-actor-reaction-scalar-1"},)"
			R"("actorPreviousBlendWeight":{"f32Bits":[1062557013,1064304640],"schema":"pbd-actor-previous-blend-weight-1"}})";
		auto saved = decode_game_values(parse(golden));
		check(saved.value && saved.value->prior_life && saved.value->reaction_scalar && saved.value->previous_blend_weight,
			"Retained scalar absent");
		const auto original = encode_game_values(*saved.value), expected = parse(golden);
		check(original.fields.size() == 3, "Retained scalar field count changed");
		for (const auto* name : {"actorPriorLife", "actorReactionScalar", "actorPreviousBlendWeight"})
		{
			const auto& scalar = field(original, name);
			const auto& wanted = field(expected, name);
			const auto* bits = std::string_view(name) == "actorPriorLife" ? "fixed16Bits" : "f32Bits";
			check(scalar.fields.size() == 2 && is(scalar.find("schema"), field(wanted, "schema").text) &&
					  pair<std::uint32_t>(field(scalar, bits)) == pair<std::uint32_t>(field(wanted, bits)),
				"Retained scalar contents changed");
		}
		saved.value->prior_life->bits = {0, 0xffffffff};
		saved.value->reaction_scalar->bits = {0x80000000, 0x7fc00001};
		saved.value->previous_blend_weight->bits = {0xffffffff, 0};
		const auto encoded = encode_game_values(*saved.value);
		auto reread = decode_game_values(encoded);
		check(reread.value && reread.value->prior_life && reread.value->reaction_scalar && reread.value->previous_blend_weight &&
				  reread.value->prior_life->bits == std::array<std::uint32_t, 2>{0, 0xffffffff} &&
				  reread.value->reaction_scalar->bits == std::array<std::uint32_t, 2>{0x80000000, 0x7fc00001} &&
				  reread.value->previous_blend_weight->bits == std::array<std::uint32_t, 2>{0xffffffff, 0},
			"Retained scalar words changed");
		for (const auto* text : {"4294967296", "-1", "0.5"})
		{
			auto malformed = parse(golden);
			malformed.fields[0].second.fields[0].second.items[0] = parse(text);
			check(!decode_game_values(malformed).value, "Invalid fixed point word accepted");
		}
	}
	void previous_game_flow_round_trip()
	{
		constexpr auto golden = R"({"previousGameFlow":{"words":[4294967291,1,0,1,1],"schema":"pbd-previous-game-flow-1"}})";
		auto saved = decode_game_values(parse(golden));
		constexpr std::array<std::uint32_t, 5> words{0xfffffffb, 1, 0, 1, 1};
		check(saved.value && saved.value->flow && saved.value->flow->words == words, "Previous game flow refused");
		const auto encoded = encode_game_values(*saved.value);
		const auto& flow = field(encoded, "previousGameFlow");
		check(encoded.fields.size() == 1 && flow.fields.size() == 2 &&
				  is(flow.find("schema"), "pbd-previous-game-flow-1"),
			"Previous game flow schema changed");
		const auto& encoded_words = array(field(flow, "words"), words.size());
		for (std::size_t index = 0; index < words.size(); ++index)
			check(integer(encoded_words[index]) == words[index], "Previous game flow word changed");
		auto malformed = parse(golden);
		malformed.fields[0].second.fields[0].second.items.pop_back();
		check(!decode_game_values(malformed).value, "Short previous game flow accepted");
		malformed = parse(golden);
		malformed.fields[0].second.fields[0].second.items[0] = parse("4294967296");
		check(!decode_game_values(malformed).value, "Out-of-range previous game flow word accepted");
		check(!decode_game_values(parse("{}")).value->flow, "Absent previous game flow invented");
	}
	std::string records_text(std::string_view hex, std::string_view schema = "pbd-actor-records-1")
	{
		return std::string{R"({"actorRecords":{"bytesHex":")"} + std::string{hex} + R"(","schema":")" + std::string{schema} +
		       R"("}})";
	}
	// Both actors travel whole as lowercase hex, two digits a byte, and only that spelling reads back.
	void actor_records_round_trip()
	{
		game_values saved;
		saved.records.emplace();
		for (std::size_t at = 0; at < saved.records->bytes.size(); ++at)
			saved.records->bytes[at] = static_cast<std::uint8_t>(at * 7 + (at >> 8));
		const auto text = pbd::set_channel::detail::serialize(encode_game_values(saved));
		const std::string hex = parse(text).fields[0].second.fields[0].second.text;
		check(hex.size() == 2 * 0x11a0 && hex.starts_with("00070e15") && hex.ends_with("636a"), "Record hex changed form");
		const auto encoded = parse(text);
		const auto& records = field(encoded, "actorRecords");
		check(encoded.fields.size() == 1 && records.fields.size() == 2 &&
				  is(records.find("schema"), "pbd-actor-records-1") && is(records.find("bytesHex"), hex),
			"Record contents changed");
		const auto reread = decode_game_values(parse(text));
		check(reread.value && reread.value->records == saved.records, "Record bytes changed");
		for (const auto& bad : {hex.substr(2), hex + "00", hex.substr(1), "A" + hex.substr(1), "g" + hex.substr(1),
				 " " + hex.substr(1)})
		{
			const auto result = decode_game_values(parse(records_text(bad)));
			check(!result.value && result.error == decode_error::wrong_shape, "Malformed record bytes accepted");
		}
		check(decode_game_values(parse(records_text(hex, "pbd-actor-records-2"))).error == decode_error::wrong_schema,
			"Other record schema accepted");
		for (const auto* shape : {R"({"actorRecords":{"bytesHex":4660,"schema":"pbd-actor-records-1"}})",
				 R"({"actorRecords":{"schema":"pbd-actor-records-1"}})"})
			check(!decode_game_values(parse(shape)).value, "Record without bytes accepted");
		auto extra = parse(text);
		extra.fields[0].second.fields.push_back({"address", json::number(0x10090040)});
		check(!decode_game_values(extra).value, "Address beside the records accepted");
		const auto absent = decode_game_values(parse("{}"));
		check(absent.value && !absent.value->records, "Absent records invented");
		check(!encode_game_values(*absent.value).find("actorRecords"), "Absent records serialized");
	}
	// Every field a recorder writes at a game start: the match history and each carried field.
	game_values every_field()
	{
		game_values out;
		out.history = vs_match_history{1, {1, 0}};
		out.flow = previous_game_flow{};
		out.planar = actor_planar_terms{};
		out.descriptor_derived_flag = std::uint8_t{0};
		out.transform_sum = actor_transform_sum{};
		out.descriptor_code = actor_descriptor_code{};
		out.descriptor_deltas = actor_descriptor_deltas{};
		out.reaction_cursor = actor_reaction_cursor{};
		out.motion_step = actor_motion_step{};
		out.last_reaction_code = actor_last_reaction_code{};
		out.reaction_conditions = actor_reaction_conditions{};
		out.bone_segments = actor_bone_segments{};
		out.reaction_result = actor_reaction_result{};
		out.hit_contacts = actor_hit_contacts{};
		out.segment_deltas = actor_segment_deltas{};
		out.prior_life = out.reaction_scalar = out.previous_blend_weight = actor_scalar_bits{};
		out.records = actor_records{};
		return out;
	}
	std::size_t carried_count(const game_values& saved)
	{
		const bool present[] = {saved.flow.has_value(), saved.planar.has_value(),
			saved.descriptor_derived_flag.has_value(), saved.transform_sum.has_value(),
			saved.descriptor_code.has_value(), saved.descriptor_deltas.has_value(), saved.reaction_cursor.has_value(),
			saved.motion_step.has_value(), saved.last_reaction_code.has_value(), saved.reaction_conditions.has_value(),
			saved.bone_segments.has_value(), saved.reaction_result.has_value(), saved.hit_contacts.has_value(),
			saved.segment_deltas.has_value(), saved.prior_life.has_value(), saved.reaction_scalar.has_value(),
			saved.previous_blend_weight.has_value(), saved.records.has_value()};
		return static_cast<std::size_t>(std::count(std::begin(present), std::end(present), true));
	}
	void carried_fields_name_every_field_but_the_history()
	{
		const auto whole = encode_game_values(every_field());
		check(whole.fields.size() == carried_fields.size() + 1 && whole.find("vsMatchHistory"), "Field count changed");
		for (const auto name : carried_fields)
			check(whole.find(name), "A carried field is not one the recorder writes");
		const auto read = decode_game_values(whole);
		check(read.value && carried_count(*read.value) == carried_fields.size(), "A written field does not read");
	}
	json& member(json& root, std::string_view name)
	{
		for (auto& [key, value] : root.fields)
			if (key == name)
				return value;
		throw std::runtime_error("Member missing");
	}
	void each_carried_field_may_be_absent_but_never_malformed()
	{
		const auto whole = encode_game_values(every_field());
		for (const auto name : carried_fields)
		{
			auto without = whole;
			std::erase_if(without.fields, [&](const auto& field)
				{
					return field.first == name;
				});
			const auto read = decode_game_values(without);
			check(read.value && read.value->history && carried_count(*read.value) == carried_fields.size() - 1,
				"An absent carried field was refused or invented");
			auto other = whole, partial = whole;
			for (auto& [key, value] : member(other, name).fields)
				if (key == "schema")
					value = json::string("other");
			auto& members = member(partial, name).fields;
			members.erase(std::ranges::find_if(members, [](const auto& field)
				{
					return field.first != "schema";
				}));
			check(decode_game_values(other).error == decode_error::wrong_schema,
				"A carried field of another schema read");
			check(!decode_game_values(partial).value, "A carried field without its values read");
		}
	}
	// A game start as a recorder from before the fighters' typed values wrote it: the history and the rules only.
	void older_recorder_start_keeps_only_what_it_holds()
	{
		constexpr auto text = R"({"nativeOrdinal":0,"vsMatchHistory":{"schema":"dr-vs-match-history-draft-1",)"
							  R"("completedGames":1,"wins":[1,0]},"appliedHandicapPercent":[100,100],)"
							  R"("nativeInputContext":{"schema":"dr-native-keyread-clock-draft-1","counter":774},)"
							  R"("nativeReplayFlag":0,"styleProducer":{"generatorContext":{},"nativeOrdinal":0,)"
							  R"("nativeReplayFlag":0,"leftRow":69,"rightRow":37},"styleProducerBoundary":403,)"
							  R"("roundOptionBytesHex":"01000101010001010001010000","roundsOptionHex":"03",)"
							  R"("gameRuleWordsHex":"00000000000000020000000300000005000000000000000000000003)"
							  R"(00000001000000010000000000000000"})";
		const auto read = decode_game_values(parse(text));
		check(read.value && carried_count(*read.value) == 0, "An older game start was refused or given carried values");
		check(read.value->history && read.value->history->completed_games == 1 &&
				  read.value->history->wins == std::array<std::uint16_t, 2>{1, 0},
			"The match history changed");
		check(encode_game_values(*read.value).fields.size() == 1, "Absent carried fields serialized");
	}
} // namespace

PBD_TEST(ReplayCarry, Codec)
{
	reaction_codes_round_trip();
	reaction_conditions_round_trip();
	malformed_conditions_are_refused();
	bone_segments_round_trip();
	reaction_result_round_trip();
	bank_offsets_round_trip();
	malformed_bank_offsets_are_refused();
	contact_geometry_round_trip();
	retained_vectors_round_trip();
	retained_scalars_round_trip();
	previous_game_flow_round_trip();
	actor_records_round_trip();
	missing_fields_stay_missing();
	malformed_fields_are_refused();
	carried_fields_name_every_field_but_the_history();
	each_carried_field_may_be_absent_but_never_malformed();
	older_recorder_start_keeps_only_what_it_holds();
}

PBD_TEST(PbdReplayBoot, FreshSelect)
{
	using namespace pbd::replay_boot;
	const observation select{4, 3, 44, 119999, true};
	check(ready(select), "Fresh VS select was refused");
	for (const observation state : {observation{0, 0, 44, 0, true}, observation{4, 12, 44, 0, true},
			 observation{4, 3, 43, 0, true}, observation{10, 3, 44, 0, true}})
	{
		check(!ready(state), "Non-VS/select start accepted");
		check(!refusal(state), "An ordinary startup screen was refused instead of waiting");
	}
}

PBD_TEST(PbdReplayBoot, RefusedStarts)
{
	using namespace pbd::replay_boot;
	for (const observation state : {observation{4, 16, 44, 0, true}, observation{4, 17, 44, 0, true},
			 observation{4, 3, 44, 120000, true}, observation{4, 3, 44, 0, false}})
	{
		check(!ready(state), "Unverified/battle/expired start accepted");
		check(refusal(state), "A refused startup has no reason");
	}
}
