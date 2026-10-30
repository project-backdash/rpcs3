#include "pbd_test.h"

#include "Emu/Cell/pbd_set_channel.h"
#include "Emu/PBD/pbd_rematch_json.h"

using pbd::test::check;

using namespace pbd::rematch;
using value = pbd::set_channel::json_value;
static value parse(std::string_view text)
{
	auto parsed = pbd::set_channel::detail::json_parser(text).parse();
	check(parsed.value.has_value());
	return std::move(*parsed.value);
}
static value& field(value& root, std::string_view name)
{
	for (auto& [key, item] : root.fields)
		if (key == name)
			return item;
	::pbd::test::fail("no field " + std::string(name), __FILE__, __LINE__);
}

PBD_TEST(PbdRematchJson, Checks)
{
	const sealed_state previous{1, 5700, 0, {{12, 32}, 17}, {3, 2, {100, 100}}};
	const load requested{1, 2, 5700, 1, previous.selected, previous.rules, false};
	const auto encoded = load_json<value>(requested);
	const auto state = state_json<value>(previous);
	check(read_load(&encoded) == requested);
	check(read_state(&state) == previous);
	auto decoded = parse(pbd::set_channel::detail::serialize(encoded));
	check(read_load(&decoded) == requested);
	check(read_load(&decoded) && validate(*read_load(&decoded), previous, false) == error::none);
	for (const auto* key : {"epoch", "endFrame", "previousGame", "nextGame"})
	{
		auto changed = encoded;
		field(changed, key) = value::string("1");
		check(!read_load(&changed));
		for (const auto* number : {"-1", "1.0", "1e0", "18446744073709551616"})
		{
			field(changed, key) = value::number(1);
			field(changed, key).text = number;
			check(!read_load(&changed));
		}
	}
	auto changed = encoded;
	field(changed, "epoch") = value::number(max_serial + 1);
	check(!read_load(&changed));
	changed = encoded;
	field(changed, "endFrame") = value::number(0x100000000ull);
	check(!read_load(&changed));
	changed = encoded;
	field(changed, "randomStage") = value::number(1);
	check(!read_load(&changed));
	changed = encoded;
	changed.fields.emplace_back("guestSeed", value::number(1));
	check(!read_load(&changed));
	changed = encoded;
	field(changed, "selection").fields.emplace_back("costume", value::number(0));
	check(!read_load(&changed));
	changed = encoded;
	field(field(changed, "options"), "handicapPercent") = value::array({value::number(100)});
	check(!read_load(&changed));
	changed = encoded;
	field(changed, "schema") = value::string("pbd-rematch-state-1");
	check(!read_load(&changed));
	check(!read_load<value>(nullptr));
	// A load without a repick keeps its exact earlier form; the repick is the loser's side by name, at most once.
	check(!encoded.find("repick"));
	for (u32 side = 0; side < 2; ++side)
	{
		auto repick = requested;
		repick.repick = side;
		const auto written = load_json<value>(repick);
		check(pbd::set_channel::detail::is(written.find("repick"), side ? "right" : "left"));
		check(read_load(&written) == repick);
		auto reparsed = parse(pbd::set_channel::detail::serialize(written));
		check(read_load(&reparsed) == repick && validate(*read_load(&reparsed), previous, false) == error::none);
		changed = written;
		changed.fields.emplace_back("repick", value::string(side ? "right" : "left"));
		check(!read_load(&changed));
		changed = written;
		field(changed, "randomStage") = value::boolean(true);
		check(!read_load(&changed));
	}
	for (const auto bad : {value::string("Left"), value::string(""), value::string("both"), value::number(1), value::boolean(true), value{}})
	{
		changed = encoded;
		changed.fields.emplace_back("repick", bad);
		check(!read_load(&changed));
	}
	// The optional key never stands in for a required one.
	changed = encoded;
	changed.fields.erase(changed.fields.end() - 1);
	changed.fields.emplace_back("repick", value::string("left"));
	check(!read_load(&changed));
}
