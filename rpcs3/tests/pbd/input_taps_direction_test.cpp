#include "pbd_test.h"
#include "Emu/PBD/pbd_input_taps.h"

#include <array>

namespace
{
	using namespace pbd::input_taps;
	using pbd::test::check;
	constexpr std::uint16_t left = 0x8000, right = 0x2000, up = 0x1000, down = 0x4000, attack = 0x0080;
} // namespace

PBD_TEST(PbdInputDirections, SequentialOppositesKeepTheLatestDirectionAndAttack)
{
	for (const auto pair : std::array{std::array{left, right}, std::array{right, left}, std::array{up, down}, std::array{down, up}})
	{
		tap_buffer buffer;
		buffer.sample(0, 0);
		buffer.read(reader::game, 0);
		buffer.sample(pair[0], 2'000);
		buffer.sample(pair[1] | attack, 8'000);
		check(buffer.peek(reader::game, 9'000) == (pair[1] | attack), "an older tap cannot oppose the direction held now");
		buffer.sample(0, 12'000);
		check(buffer.read(reader::game, 33'334) == (pair[1] | attack), "a late read keeps the latest direction and the attack");
		check(buffer.read(reader::game, 33'335) == 0, "the buffered word is consumed once");
		check(buffer.read(reader::practice, 33'336) == (pair[1] | attack), "each reader retains its own tap");
	}
}

PBD_TEST(PbdInputDirections, PhysicalOppositesAndDiagonalsArePreserved)
{
	for (const auto directions : std::array<std::uint16_t, 4>{left | right, up | down, up | right, down | left})
	{
		tap_buffer buffer;
		buffer.sample(directions | attack, 2'000);
		check(buffer.read(reader::game, 3'000) == (directions | attack), "a simultaneous physical report passes unchanged");
		buffer.sample(0, 4'000);
		check(buffer.read(reader::practice, 5'000) == (directions | attack), "the complete simultaneous tap survives release");
	}
}

PBD_TEST(PbdInputDirections, OvertakenRingSlotsCannotCreateOpposites)
{
	for (const auto pair : std::array{std::array{left, right}, std::array{right, left}, std::array{up, down}, std::array{down, up}})
	{
		ring_reader ring;
		std::array<std::uint16_t, 16> slots{};
		ring.read(slots, 1, 0);
		slots[1] = pair[0];
		slots[2] = pair[1] | attack;
		slots[3] = 0;
		check(ring.read(slots, 4, 0) == (pair[1] | attack), "a late update keeps taps without inventing opposite directions");
		check(ring.read(slots, 4, 0) == 0, "catch-up without a new slot cannot repeat a released tap");
	}
	ring_reader ring;
	std::array<std::uint16_t, 16> slots{};
	ring.read(slots, 1, 0);
	slots[1] = left;
	slots[2] = right | attack;
	check(ring.read(slots, 3, left) == (right | attack), "the newer completed slot wins over the game's older word");
	slots[3] = left | right;
	check(ring.read(slots, 4, slots[3]) == (left | right), "physical opposites in the newest slot remain physical opposites");
}

PBD_TEST(PbdInputDirections, LateReadsRetainButtonsWithinTheExistingHistoryLimits)
{
	tap_buffer buffer;
	buffer.sample(attack, 2'000);
	buffer.sample(0, 8'000);
	check(buffer.read(reader::game, 33'334) == attack, "a frame-long stall retains an isolated short attack");
	check(buffer.read(reader::practice, 102'001) == 0, "an unread tap expires after the existing 100 ms limit");
	ring_reader ring;
	std::array<std::uint16_t, 16> slots{};
	ring.read(slots, 1, 0);
	slots[1] = attack;
	check(ring.read(slots, 17, 0) == 0, "a full ring overrun cannot recover overwritten history");
}

PBD_TEST(PbdInputDirections, EveryPhysicalWordPassesUnchanged)
{
	for (std::uint32_t word = 0; word <= 0xffff; ++word)
	{
		tap_buffer buffer;
		buffer.sample(static_cast<std::uint16_t>(word), 1);
		check(buffer.read(reader::game, 2) == word, "a physical word retains every direction, attack and system button");
		buffer.sample(0, 3);
		check(buffer.read(reader::practice, 4) == word, "an isolated tap retains the complete physical word");
	}
}
