#include "pbd_test.h"

#include "Emu/Cell/pbd_lobby_set_driver.h"
#include "Emu/PBD/pbd_replay_difference.h"

#include <cstdint>
#include <string_view>

namespace
{
	using ::pbd::test::check;
	using namespace ::pbd::replay_difference;

	pbd::lobby_set::sync_facts recorded()
	{
		pbd::lobby_set::sync_facts facts;
		facts.segment = 3;
		facts.scene = 16;
		facts.counter = 1200;
		facts.mask = 7;
		facts.combat = {0x11, 0x22, 0x33};
		return facts;
	}

	// The difference between the recorded facts and a frame that played as `played` says.
	difference between(const pbd::lobby_set::sync_facts& expected, const pbd::lobby_set::sync_facts& played)
	{
		return find(expected, played.segment, played.scene, played.counter, played.mask, played.combat);
	}

	bool named(const log_name& name, std::string_view event, std::string_view refusal)
	{
		return name.event == event && name.refusal == refusal;
	}

} // namespace

PBD_TEST(PbdReplayDifference, DifferenceChecks)
{
	const auto expected = recorded();
	check(!between(expected, expected) && between(expected, expected).what == kind::none, "a matching frame");

	auto played = expected;
	played.segment++;
	check(between(expected, played).what == kind::coordinate, "another segment");
	played = expected;
	played.scene = 17;
	check(between(expected, played).what == kind::coordinate, "another scene");
	played = expected;
	played.counter++;
	check(between(expected, played).what == kind::coordinate, "another frame counter");
	played = expected;
	played.mask = 3;
	check(between(expected, played).what == kind::coordinate, "another hash mask");

	played = expected;
	played.combat[1] = 0x99;
	const auto hash = between(expected, played);
	check(hash && hash.what == kind::hash && hash.region == 1, "a combat hash the mask covers");

	auto unmasked = expected;
	unmasked.mask = 5;
	played = unmasked;
	played.combat[1] = 0x99;
	check(!between(unmasked, played), "a combat hash outside the mask is not compared");

	played = expected;
	played.counter++;
	played.combat[0] = 0x99;
	check(between(expected, played).what == kind::coordinate, "the coordinates are compared before the hashes");

	played = expected;
	played.combat[1] = 0x99;
	played.combat[2] = 0x99;
	check(between(expected, played).region == 1, "the lowest differing region is the one reported");
}

PBD_TEST(PbdReplayDifference, NameChecks)
{
	// A file that must match exactly: a difference is a fault, named as the replay watch reports it and refused.
	const difference coordinate{kind::coordinate}, hash{kind::hash, 2};
	check(named(name_of(coordinate), "sync_coordinate_mismatch", "native_replay_coordinate_mismatch"),
		"an exact file's coordinate difference");
	check(named(name_of(hash), "sync_hash_mismatch", "native_replay_peer_sync_hash_mismatch"),
		"an exact file's hash difference");
}
