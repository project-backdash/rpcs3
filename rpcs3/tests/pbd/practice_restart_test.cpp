#include "pbd_test.h"

#include "Emu/PBD/pbd_practice_presentation.h"
#include "Emu/PBD/pbd_practice_menu.h"
#include "Emu/PBD/pbd_practice_position.h"
#include "Emu/PBD/pbd_practice_store.h"
#include "practice_wall_model.h"

#include <bit>
#include <cmath>
#include <numbers>
#include <string>
#include <string_view>

namespace
{
	using pbd::game_lifecycle::actor_placement;
	using pair = std::array<actor_placement, 2>;

	using pbd::test::check;

	// The Centre pair exactly as the restart writes it: P1 at x -1000 facing +X, the dummy at x +1000 facing -X.
	const pair centre{actor_placement{static_cast<s32>(0xc47a0000u), 0, 0, 0x4000, 0x3fc90fdbu},
		actor_placement{static_cast<s32>(0x447a0000u), 0, 0, -0x4000, 0xbfc90fdbu}};
	// A pair as Remember keeps it: anywhere on the stage, neither fighter facing the other.
	const pair remembered{pbd::practice::facing_placement({300.f, -200.f}, {.39f, .92f}),
		pbd::practice::facing_placement({-500.f, 900.f}, {-.72f, -.69f})};

	bool same(const actor_placement& a, const actor_placement& b)
	{
		return a.x == b.x && a.y == b.y && a.z == b.z && a.facing == b.facing && a.yaw_bits == b.yaw_bits;
	}
	float coordinate(s32 bits)
	{
		return std::bit_cast<float>(static_cast<u32>(bits));
	}
	// The angle between a fighter's yaw and the direction to the other, as the dummy's own facing check reads it.
	std::optional<float> error_degrees(const actor_placement& from, const actor_placement& to)
	{
		return pbd::practice::movement::facing_error_degrees(std::bit_cast<float>(from.yaw_bits), coordinate(from.x), coordinate(from.z),
			coordinate(to.x), coordinate(to.z));
	}
	pbd::practice::movement::facing facing_of(const actor_placement& from, const actor_placement& to)
	{
		return pbd::practice::movement::facing_verdict(error_degrees(from, to));
	}
	// The 16-bit facing agrees with the yaw to within one step of its scale.
	bool facings_agree(const actor_placement& value)
	{
		const float from_facing = pbd::practice::movement::facing16_yaw(value.facing);
		const float off = std::remainder(from_facing - std::bit_cast<float>(value.yaw_bits), 2.f * std::numbers::pi_v<float>);
		return std::abs(off) < std::numbers::pi_v<float> / 32768.f;
	}
	pair wall_pair(const pbd::practice::walls::pair_placement& at)
	{
		return {pbd::practice::facing_placement(at.actors[0], at.forward),
			pbd::practice::facing_placement(at.actors[1], {-at.forward.x, -at.forward.z})};
	}
} // namespace

PBD_TEST(PbdPracticeRestart, TheDefaultVariantWritesThePairAsBefore)
{
	const auto written = pbd::practice::arranged(centre, {});
	check(same(written[0], centre[0]) && same(written[1], centre[1]), "Front and Don't Switch Position change nothing");
	// The facing the wall and random restarts compute is the native layout's: yaw 0 faces +Z, +X is a quarter turn.
	check(same(pbd::practice::facing_placement({-1000.f, 0.f}, {1.f, 0.f}), centre[0]),
		"a fighter facing +X is written as the Centre restart writes P1");
	check(same(pbd::practice::facing_placement({1000.f, 0.f}, {-1.f, 0.f}), centre[1]), "and facing -X as it writes the dummy");
}

PBD_TEST(PbdPracticeRestart, SwitchPositionTradesPlacesAndKeepsBothFacingEachOther)
{
	using pbd::practice::movement::facing;
	const auto written = pbd::practice::arranged(centre, {.switched = true});
	check(same(written[0], centre[1]) && same(written[1], centre[0]), "each fighter takes the other's spot and facing");
	check(facing_of(written[0], written[1]) == facing::toward && facing_of(written[1], written[0]) == facing::toward,
		"both still face each other");
	const auto traded = pbd::practice::arranged(remembered, {.switched = true});
	check(same(traded[0], remembered[1]) && same(traded[1], remembered[0]), "a remembered pair trades places whole");
}

PBD_TEST(PbdPracticeRestart, SwitchPositionAtTheLeftWallCornersTheDummyAtThatWall)
{
	namespace geometry = pbd::practice::walls;
	using pbd::practice::movement::facing;
	const auto stage30 = geometry::survey(wall_model::stage30);
	check(stage30.path != geometry::wall_path::none, "the captured stage geometry is supported");
	const auto left = geometry::place(stage30.arena, geometry::position::p1_back, 750.f, 2000.f);
	check(left.has_value(), "Left Wall is offered on the captured stage");
	const auto switched = pbd::practice::arranged(wall_pair(*left), {.switched = true});
	check(same(switched[1], pbd::practice::facing_placement(left->actors[0], left->forward)),
		"the dummy stands at the wall's spot with its back to the wall");
	check(same(switched[0], pbd::practice::facing_placement(left->actors[1], {-left->forward.x, -left->forward.z})),
		"P1 stands where the dummy would, facing the wall");
	check(facing_of(switched[0], switched[1]) == facing::toward && facing_of(switched[1], switched[0]) == facing::toward,
		"the two still face each other");
}

PBD_TEST(PbdPracticeRestart, DummyDirectionBackTurnsTheDummyAwayAndKeepsItsSpot)
{
	using pbd::practice::movement::facing;
	const auto written = pbd::practice::arranged(centre, {.dummy_back = true});
	check(same(written[0], centre[0]), "P1 is left as it was");
	check(written[1].x == centre[1].x && written[1].y == centre[1].y && written[1].z == centre[1].z, "the dummy keeps its spot");
	check(written[1].yaw_bits == 0x3fc90fdbu && written[1].facing == 0x4000, "at the Centre the dummy faces +X, as P1 does");
	check(facing_of(written[1], written[0]) == facing::backturned && facing_of(written[0], written[1]) == facing::toward,
		"Practice's own facing check reads the dummy as back-turned and P1 as facing it");
	// At any remembered pair, switched or not, the dummy looks straight along the line from P1 through it.
	for (const bool switched : {false, true})
	{
		const auto turned = pbd::practice::arranged(remembered, {.switched = switched, .dummy_back = true});
		check(same(turned[0], remembered[switched ? 1 : 0]), "P1 keeps its remembered placement, or takes the dummy's");
		const auto error = error_degrees(turned[1], turned[0]);
		check(error && std::abs(*error - 180.f) < .01f && facings_agree(turned[1]), "the dummy shows P1 its back exactly");
	}
	// With no line to follow (both on one spot) the dummy turns half a turn from where it faced.
	const float yaw = std::numbers::pi_v<float> / 8.f;
	const pair stacked{centre[0], actor_placement{centre[0].x, 0, centre[0].z, 0x1000, std::bit_cast<u32>(yaw)}};
	const auto half = pbd::practice::arranged(stacked, {.dummy_back = true});
	check(half[1].facing == std::bit_cast<s16>(u16{0x9000}) &&
			  std::abs(std::bit_cast<float>(half[1].yaw_bits) - (yaw - std::numbers::pi_v<float>)) < 1e-6f,
		"the 16-bit facing and the yaw both turn by half a turn");
}

PBD_TEST(PbdPracticeRestart, RestartSettingsListDummyDirectionAndSwitchPositions)
{
	namespace rows = pbd::practice::restart_rows;
	check(rows::restart == 0 && rows::position == 1 && rows::remember == 2 && rows::dummy_direction == 3 && rows::switch_positions == 4 &&
			  rows::restore == 5 && rows::count == 6,
		"Restart, Restart Position, Remember Position, Dummy Direction, Switch Positions, Restore Defaults");
	const auto front = pbd::practice::menu::direction_choices(false), back = pbd::practice::menu::direction_choices(true);
	check(front.options.size() == 2 && front.options[0].is_default &&
			  !front.options[1].is_default && front.current == 0 && back.current == 1,
		"Dummy Direction offers Front, the default, then Back");
	const auto stay = pbd::practice::menu::switch_choices(false), trade = pbd::practice::menu::switch_choices(true);
	check(stay.options.size() == 2 && stay.options[0].is_default &&
			  stay.current == 0 && trade.current == 1,
		"Switch Positions offers Don't Switch Position, the default, then Switch Position");
	for (const auto& list : {front, stay})
		for (const auto& option : list.options)
			check(option.enabled && !option.help.empty(), "every choice can be taken and says what it does");
}

PBD_TEST(PbdPracticeStore, RestartSettingsAndInputDelayAreRemembered)
{
	using namespace pbd::practice_store;
	profile value;
	value.dummy_back = true;
	value.switch_positions = true;
	value.input_delay = 4;
	const auto text = serialize(value, "x");
	const auto parsed = parse_profile(text);
	check(parsed.value && *parsed.value == value, "they come back next launch");
	const auto older = parse_profile(R"({"schema":"pbd-practice-profile-1","restartPosition":"left_wall"})");
	check(older.value && !older.value->dummy_back && !older.value->switch_positions && older.value->input_delay == 0 &&
			  older.value->restart == restart_position::left_wall,
		"a profile from before them keeps Front, Don't Switch Position and Not Set");
	const auto field = [](std::string_view name, std::string_view json)
	{
		const auto read = parse_profile(R"({"schema":"pbd-practice-profile-1",")" + std::string(name) + "\":" + std::string(json) + "}");
		check(read.value.has_value(), "the profile reads");
		return *read.value;
	};
	for (const auto invalid : {"\"BACK\"", "\"left\"", "1", "true", "null"})
		check(!field("dummyDirection", invalid).dummy_back, "only \"back\" turns the dummy");
	for (const auto invalid : {"1", "\"true\"", "null", "[]"})
		check(!field("switchPositions", invalid).switch_positions, "only true switches positions");
	check(field("inputDelay", "6").input_delay == 6 && field("inputDelay", "99").input_delay == pbd::practice::max_input_delay,
		"a delay above the menu's last choice is its last choice");
	for (const auto invalid : {"-1", "\"3\"", "null", "2.5"})
		check(field("inputDelay", invalid).input_delay == 0, "anything but a whole number of frames is Not Set");
}
