#include "pbd_test.h"

#include "Emu/PBD/pbd_input_taps.h"
#include "Input/pbd_controller_backend.h"
#include "Input/pbd_controller_ports.h"

#include <array>
#include <cstdint>
#include <initializer_list>

namespace
{
	using pbd::input_taps::reader;
	using pbd::input_taps::ring_reader;
	using pbd::input_taps::tap_buffer;

	using pbd::test::check;

	constexpr std::uint16_t square = 0x0080, cross = 0x0040, forward = 0x2000;
	constexpr std::uint64_t frame_us = 16'667;

	struct slot_word
	{
		std::uint32_t slot;
		std::uint16_t word;
	};

	// A port's ring with these words in the slots counted `slot` (the ring keeps the low four bits).
	std::array<std::uint16_t, 16> ring_with(std::initializer_list<slot_word> words)
	{
		std::array<std::uint16_t, 16> slots{};
		for (const auto& [slot, word] : words)
			slots[slot & 15] = word;
		return slots;
	}
} // namespace

namespace pbd::test
{
	struct controller_ports_fixture : pads::controller_ports
	{
		std::array<std::shared_ptr<Pad>, 2> devices;
		controller_ports_fixture() : controller_ports(nullptr)
		{
			for (u32 i = 0; i < devices.size(); ++i)
			{
				devices[i] = std::make_shared<Pad>(pad_handler::keyboard, i, 0, 0, 0);
				bind({0, 1}, i, devices[i]);
			}
		}
		using controller_ports::apply_reports;
	};
} // namespace pbd::test

PBD_TEST(PbdInputTaps, TwoControlledPortsRetainTheirOwnTaps)
{
	namespace c = pbd::controllers;
	using pbd::test::check;
	pbd::test::controller_ports_fixture ports;
	ports.apply_reports({c::state{}, c::state{}});
	std::array<c::state, 2> reports;
	reports[0].buttons = c::bit(c::square);
	reports[1].buttons = c::bit(c::cross);
	reports[1].sticks[0] = 1.f;
	ports.apply_reports(reports);
	reports[0].buttons = reports[1].buttons = 0;
	ports.apply_reports(reports);
	const auto button_down = [](const Pad& pad, u16 mask)
	{
		return std::any_of(pad.m_buttons_external.begin(), pad.m_buttons_external.end(), [&](const auto& button)
			{
				return button.m_offset == CELL_PAD_BTN_OFFSET_DIGITAL2 && button.m_outKeyCode == mask && button.m_pressed && button.m_value == 255;
			});
	};
	check(button_down(*ports.devices[0], square) && button_down(*ports.devices[1], cross), "both guest reports retain taps released between reads");
	check(ports.devices[1]->m_sticks_external[0].m_value == 255, "latching Player 2 buttons preserves its stick report");
	check(ports.read_taps(reader::game, *ports.devices[0]) == square);
	ports.game_read(*ports.devices[0]);
	check(!button_down(*ports.devices[0], square) && button_down(*ports.devices[1], cross), "reading Player 1 leaves Player 2's tap intact");
	check(ports.read_taps(reader::game, *ports.devices[1]) == cross);
	ports.game_read(*ports.devices[1]);
	check(!button_down(*ports.devices[1], cross));
	check(ports.read_taps(reader::game, *ports.devices[0]) == 0 && ports.read_taps(reader::game, *ports.devices[1]) == 0,
		"each port is neutral on its next guest read");
	check(ports.read_taps(reader::practice, *ports.devices[1]) == cross, "the second port's other reader retains its own tap");
}

PBD_TEST(PbdInputTaps, TapBetweenReadsReadsOnce)
{
	tap_buffer tap_state;
	tap_state.sample(0, 0);
	check(tap_state.read(reader::game, 0) == 0, "A neutral controller reads neutral");
	tap_state.sample(square, 3'000);
	tap_state.sample(0, 9'000);
	check(tap_state.level() == 0, "The host sees the tap released before the game's next read");
	check(tap_state.read(reader::game, frame_us) == square, "A tap between two reads reads as held at the next read");
	tap_state.sample(0, frame_us + 1'000);
	check(tap_state.read(reader::game, 2 * frame_us) == 0, "The tap reads as held for one read only");

	tap_state.sample(forward, 2 * frame_us + 1'000);
	tap_state.sample(forward | cross, 2 * frame_us + 4'000);
	tap_state.sample(forward, 2 * frame_us + 6'000);
	check(tap_state.read(reader::game, 3 * frame_us) == (forward | cross), "A tap joins the buttons held at the same read");
	check(tap_state.read(reader::game, 4 * frame_us) == forward, "The held direction stays and the tap is gone at the read after");
}

PBD_TEST(PbdInputTaps, PressAndReleaseKeepTheirTiming)
{
	tap_buffer tap_state;
	tap_state.sample(0, 0);
	tap_state.read(reader::game, 0);
	tap_state.sample(cross, 10'000);
	check(tap_state.read(reader::game, 10'500) == cross, "A press reads at the first read after it");
	for (std::uint64_t frame = 1; frame <= 5; ++frame)
	{
		tap_state.sample(cross, 10'000 + frame * frame_us - 1'000);
		check(tap_state.read(reader::game, 10'000 + frame * frame_us) == cross, "A held button reads as held at every read");
	}
	tap_state.sample(0, 10'000 + 5 * frame_us + 2'000);
	check(tap_state.read(reader::game, 10'000 + 6 * frame_us) == 0, "A release of a button seen held reads at the first read after it");

	tap_state.sample(square, 200'000);
	check(tap_state.read(reader::game, 200'000 + frame_us) == square, "A button seen held at this read");
	tap_state.sample(0, 200'000 + frame_us + 2'000);
	tap_state.sample(square, 200'000 + frame_us + 6'000);
	tap_state.sample(0, 200'000 + frame_us + 9'000);
	check(tap_state.read(reader::game, 200'000 + 2 * frame_us) == square, "A release and a new tap before the next read still count the tap");
	check(tap_state.read(reader::game, 200'000 + 3 * frame_us) == 0, "Then the button reads released");
}

PBD_TEST(PbdInputTaps, ReadersAreSeparate)
{
	tap_buffer tap_state;
	tap_state.sample(0, 0);
	tap_state.sample(square, 2'000);
	tap_state.sample(0, 5'000);
	check(tap_state.peek(reader::game, 6'000) == square, "The game's next read shows the tap before it reads");
	check(tap_state.read(reader::game, 8'000) == square, "The game reads the tap");
	check(tap_state.peek(reader::game, 9'000) == 0, "The game's read took its tap");
	check(tap_state.read(reader::replay, 16'000) == square, "Replay controls still read the tap the game has already read");
	check(tap_state.read(reader::practice, 17'000) == square, "Practice reads it too");
	check(tap_state.read(reader::replay, 32'000) == 0 && tap_state.read(reader::practice, 33'000) == 0, "Each reader reads a tap once");
}

PBD_TEST(PbdInputTaps, OldTapsExpire)
{
	tap_buffer tap_state;
	tap_state.sample(cross, 1'000);
	tap_state.sample(0, 4'000);
	check(tap_state.read(reader::game, 1'000 + tap_buffer::hold_us + 1) == 0, "A reader that waited longer than hold_us gets no old tap");
	tap_state.sample(cross, 500'000);
	tap_state.sample(0, 503'000);
	check(tap_state.read(reader::continuation, 500'000 + tap_buffer::hold_us) == cross, "A tap still reads at the end of hold_us");
	tap_state.sample(cross, 600'000);
	tap_state.sample(0, 602'000);
	check(tap_state.read(reader::continuation, 2'000'000) == 0, "A reader that stopped reading does not get a tap when it reads again");
}

PBD_TEST(PbdInputTaps, RingSlotsOvertaken)
{
	ring_reader ring;
	auto slots = ring_with({{9, 0}});
	check(ring.read(slots, 10, slots[9]) == 0, "The first read reports the newest slot");
	slots = ring_with({{9, 0}, {10, square}, {11, 0}});
	check(ring.read(slots, 12, slots[11]) == square, "A tap in a slot written and overtaken since the previous read reads as held");
	slots[12] = 0;
	check(ring.read(slots, 13, slots[12]) == 0, "It reads as held once");

	slots[13] = cross;
	check(ring.read(slots, 14, slots[13]) == cross, "A press in the newest slot reads at once");
	slots[14] = 0;
	slots[15] = 0;
	check(ring.read(slots, 16, slots[15]) == 0, "A release in an overtaken slot reads at once, not stretched");
	slots[16 & 15] = forward;
	slots[17 & 15] = forward;
	check(ring.read(slots, 18, slots[17 & 15]) == forward, "A hold across slots reads as held");
	check(ring.read(slots, 18, slots[17 & 15]) == forward, "No new slot: the newest word as it is");
}

PBD_TEST(PbdInputTaps, RingHistoryLimits)
{
	ring_reader ring;
	auto slots = ring_with({{99, 0}});
	ring.read(slots, 100, 0);
	slots = ring_with({{0, square}, {1, 0}});
	check(ring.read(slots, 2, slots[1]) == 0, "A count that restarted has no history to add");
	slots = ring_with({{2, square}, {3, 0}});
	check(ring.read(slots, 4, slots[3]) == square, "History resumes from the restarted count");

	slots = ring_with({{4, cross}});
	check(ring.read(slots, 4 + ring_reader::readable + 1, 0) == 0, "Past the readable slots nothing is added");

	ring_reader game_read;
	slots = ring_with({{29, 0}});
	game_read.read(slots, 30, 0);
	slots = ring_with({{29, 0}, {30, 0}, {31, square}});
	check(game_read.read(slots, 32, slots[30 & 15]) == square, "A press newer than the game's own word reads now, never later");
	slots[32 & 15] = square;
	check(game_read.read(slots, 33, slots[31 & 15]) == square, "Then it reads as held, not as a second press");
}

// Sample every millisecond, write the pad ring at vertical blank, and skip every fourth update.
// Taps lasting 1 to 12 ms must reach exactly one update through both buffers; reading only
// the current levels loses taps between reads.
PBD_TEST(PbdInputTaps, EveryTapReachesOneUpdate)
{
	constexpr std::array<std::uint16_t, 4> buttons{square, cross, 0x0010, 0x0020};
	constexpr std::uint64_t first_tap_us = 50'000, tap_spacing_us = 100'000;
	tap_buffer tap_state;
	ring_reader update;
	std::array<std::uint16_t, 16> slots{}, level_slots{};
	std::array<unsigned, 24> seen{}, level_seen{};
	std::uint32_t written = 0;
	std::uint64_t next_vblank_us = 0;
	unsigned frame = 0;
	for (std::uint64_t now = 0; now < first_tap_us + tap_spacing_us * seen.size(); now += 1'000)
	{
		std::uint16_t word = 0;
		for (unsigned tap = 0; tap < seen.size(); ++tap)
		{
			const std::uint64_t down = first_tap_us + tap_spacing_us * tap;
			if (now >= down && now < down + 1'000 * (1 + tap % 12))
				word |= buttons[tap % 4];
		}
		tap_state.sample(word, now);
		if (now < next_vblank_us)
			continue;
		level_slots[written & 15] = tap_state.level();
		slots[written++ & 15] = tap_state.read(reader::game, now);
		next_vblank_us += frame_us;
		if (++frame % 4 == 0)
			continue;
		const std::uint16_t result = update.read(slots, written, slots[(written - 1) & 15]);
		const std::uint16_t level_result = level_slots[(written - 1) & 15];
		if (now >= first_tap_us)
		{
			const auto tap = static_cast<unsigned>((now - first_tap_us) / tap_spacing_us);
			if (tap < seen.size() && (result & buttons[tap % 4]))
				++seen[tap];
			if (tap < seen.size() && (level_result & buttons[tap % 4]))
				++level_seen[tap];
		}
	}
	for (const unsigned updates : seen)
		check(updates == 1, "Every tap shorter than a frame reaches exactly one update of the game");
	unsigned lost = 0;
	for (const unsigned updates : level_seen)
		lost += updates == 0;
	check(lost >= seen.size() / 2, "Read as levels, half of these taps or more never reach the game");
}
