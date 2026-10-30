#include "pbd_test.h"

#include "Emu/PBD/pbd_json_reader.h"

#include <array>
#include <random>
#include <string>
#include <string_view>

namespace
{
	using pbd::json::json_value;
	using pbd::json::detail::json_parser;
	using pbd::json::detail::serialize;
	using pbd::test::check;
	constexpr unsigned seed = 0x5eed1234;
	constexpr std::array<std::string_view, 6> documents{
		R"({"schema":"pbd-build-flavour-1","flavour":"tester"})",
		R"({"type":"session_connected","tick":100})",
		R"({"type":"session_progress","stage":"game_running"})",
		R"({"type":"game_closed","schema":1})",
		R"({"type":"session_end","reason":"match_ended","hash_coverage":{"checks":2}})",
		R"({"schema":"pbd-practice-profile-1","settings":{"trainingMode":"defense"},"words":[0,65535]})"};

	const std::array<std::string_view, 12> mutation_seeds{
		"null", "true", "-123.5e+2", "\"\\uD83D\\uDE00\"", "[0,1,true,null,\"text\"]",
		"{\"empty\":{},\"list\":[1,{\"text\":\"\\u0000\"}]}", "{\"same\":1,\"same\":2}",
		"\"\\uD800\"", "1e", "[", "{", "\"\\uFFFF\""};

	std::string mutate(std::string text, std::mt19937& random)
	{
		constexpr std::array<std::string_view, 12> insertions{
			"{", "}", "[", "]", "\"", "\\", ":", ",", std::string_view("\0", 1), "1e999", "\xff", "\xc3\xa9"};
		const auto at = random() % text.size();
		const auto added = insertions[random() % insertions.size()];
		switch (random() % 8)
		{
		case 0: return text.substr(0, at);
		case 1: text.erase(at, 1); break;
		case 2: text.insert(at, added); break;
		case 3: text.replace(at, 1, added); break;
		case 4: text.insert(0, added); break;
		case 5: text.append(added); break;
		case 6: return '[' + text + ',' + std::string(added) + ']';
		case 7: return " \n\t" + text + "\r ";
		}
		return text;
	}

	void check_parse(std::string_view text)
	{
		const auto first = json_parser(text).parse();
		if (!first.value)
		{
			check(!first.error.empty(), "a refusal names its reason");
			return;
		}
		check(first.error.empty(), "a successful parse has no failure reason");
		const auto encoded = serialize(*first.value);
		const auto round_trip = json_parser(encoded).parse();
		check(round_trip.value.has_value(), "an accepted value remains valid after serialization");
		check(serialize(*round_trip.value) == encoded, "the canonical representation is stable");
	}

	bool same_value(const json_value& expected, const json_value& actual)
	{
		if (expected.kind != actual.kind)
			return false;
		using enum pbd::json::json_kind;
		switch (expected.kind)
		{
		case null: return true;
		case boolean: return expected.truth == actual.truth;
		case number:
		case string: return expected.text == actual.text;
		case array:
			if (expected.items.size() != actual.items.size())
				return false;
			for (std::size_t i = 0; i < expected.items.size(); ++i)
				if (!same_value(expected.items[i], actual.items[i]))
					return false;
			return true;
		case object:
			if (expected.fields.size() != actual.fields.size())
				return false;
			for (const auto& [key, value] : expected.fields)
			{
				const auto* field = actual.find(key);
				if (!field || !same_value(value, *field))
					return false;
			}
			return true;
		}
		return false;
	}

	json_value generated_value(std::mt19937& random, unsigned depth)
	{
		const unsigned kind = random() % (depth ? 6 : 4);
		switch (kind)
		{
		case 0: return {};
		case 1: return json_value::boolean((random() & 1) != 0);
		case 2: return json_value::number(random());
		case 3:
		{
			constexpr std::array<std::string_view, 9> pieces{
				"a", "\"", "\\", "\n", std::string_view("\0", 1), "\xc3\xa9", "\xe2\x9c\x93", "\xf0\x9f\x98\x80", "/"};
			std::string text;
			for (unsigned count = random() % 12; count; count--)
				text += pieces[random() % pieces.size()];
			return json_value::string(std::move(text));
		}
		case 4:
		{
			auto value = json_value::array();
			for (unsigned count = random() % 4; count; count--)
				value.items.push_back(generated_value(random, depth - 1));
			return value;
		}
		default:
		{
			auto value = json_value::object();
			for (unsigned count = random() % 4; count; count--)
				value.fields.emplace_back("field" + std::to_string(count), generated_value(random, depth - 1));
			return value;
		}
		}
	}
} // namespace

PBD_TEST(PbdJsonReaderRandomized, GoldenDocuments)
{
	for (const auto text : documents)
	{
		const auto parsed = json_parser(text).parse();
		pbd::test::check(parsed.value.has_value());
		pbd::test::check(serialize(*parsed.value) == text, "fixture bytes stay canonical");
	}
}

PBD_TEST(PbdJsonReaderRandomized, DocumentMutations)
{
	std::mt19937 random(seed);
	for (unsigned iteration = 0; iteration < 4096; iteration++)
	{
		SCOPED_TRACE(iteration);
		const auto document = iteration % 2 ? documents[random() % documents.size()] : mutation_seeds[random() % mutation_seeds.size()];
		check_parse(mutate(std::string(document), random));
	}
}

PBD_TEST(PbdJsonReaderRandomized, ArbitraryBytes)
{
	std::mt19937 random(seed);
	for (unsigned iteration = 0; iteration < 2048; iteration++)
	{
		SCOPED_TRACE(iteration);
		std::string bytes(random() % 512, '\0');
		for (auto& byte : bytes)
			byte = static_cast<char>(random() & 0xff);
		check_parse(bytes);
		const auto budget = random() % 513;
		const auto bounded = json_parser(bytes, 1 + random() % 32, budget, random() % 33).parse();
		check(bounded.value.has_value() == bounded.error.empty());
		if (bytes.size() > budget)
			check(!bounded.value && bounded.error == "file_too_large");
	}
}

PBD_TEST(PbdJsonReaderRandomized, GeneratedTrees)
{
	std::mt19937 random(seed);
	for (unsigned iteration = 0; iteration < 1024; iteration++)
	{
		SCOPED_TRACE(iteration);
		const auto original = generated_value(random, 4);
		const auto encoded = serialize(original);
		const auto parsed = json_parser(encoded).parse();
		pbd::test::check(parsed.value.has_value(), "generated bounded trees parse");
		pbd::test::check(same_value(original, *parsed.value), "all scalar and container values round-trip");
	}
}

PBD_TEST(PbdJsonReaderRandomized, TornTrailingAndDuplicateRecords)
{
	std::mt19937 random(seed);
	for (const auto sample_document : documents)
	{
		for (unsigned iteration = 0; iteration < 64; iteration++)
		{
			pbd::test::check(!json_parser(sample_document.substr(0, random() % sample_document.size())).parse().value);
		}
		pbd::test::check(!json_parser(std::string(sample_document) + '\0').parse().value);
	}
	for (unsigned iteration = 0; iteration < 256; iteration++)
	{
		const auto key = "field" + std::to_string(random());
		const auto text = '{' + pbd::json::quoted(key) + ":0," + pbd::json::quoted(key) + ":1}";
		pbd::test::check(json_parser(text).parse().error == "duplicate_key");
	}
	pbd::test::check(json_parser(R"({"field":0,"\u0066ield":1})").parse().error == "duplicate_key");
}

PBD_TEST(PbdJsonReaderRandomized, UnicodeAndIntegerEdges)
{
	for (const std::string_view text : {R"("\ud800")", R"("\udfff")", R"("\ud800\u0061")", "\"\xff\"", "\"\xc0\x80\""})
		pbd::test::check(!json_parser(text).parse().value, "invalid Unicode never becomes a string");
	const auto scalar = json_parser(R"("\ud83d\ude00")").parse();
	pbd::test::check(scalar.value && scalar.value->text == "\xf0\x9f\x98\x80");
	for (const auto text : {"1e999", "-1", "1.0", "9007199254740992", "18446744073709551616"})
	{
		const auto parsed = json_parser(text).parse();
		pbd::test::check(parsed.value.has_value(), "the grammar preserves valid number text");
		std::uint64_t number = 0;
		pbd::test::check(!pbd::json::detail::number(&*parsed.value, 0, pbd::json::max_safe_integer, number));
	}
}

PBD_TEST(PbdJsonReaderRandomized, Budgets)
{
	for (const auto text : documents)
	{
		pbd::test::check(json_parser(text, 1).parse().error == "json_limit");
		pbd::test::check(json_parser(text, 8192, text.size() - 1).parse().error == "file_too_large");
		pbd::test::check(json_parser(text, 8192, text.size()).parse().value.has_value());
	}
	const auto nested = std::string(18, '[') + "0" + std::string(18, ']');
	pbd::test::check(json_parser(nested).parse().error == "json_limit");
	pbd::test::check(json_parser(R"("12345")", 8192, 64, 4).parse().error == "string_too_large");
	std::string flat = "[0";
	for (std::size_t i = 1; i < pbd::json::max_json_nodes; ++i)
		flat += ",0";
	flat += ']';
	pbd::test::check(json_parser(flat).parse().error == "json_limit");
	pbd::test::check(json_parser(flat, pbd::json::max_json_nodes + 1).parse().value);
}
