// Golden bytes of the datagrams two peers exchange during a session: the version-11 input message with no inputs
// and with the full window of 16, and the end message. A receiver accepts only this exact layout, so a changed byte
// splits players on different builds. The bytes are written out here field by field; the codec in pbd_peer_message.h,
// which the sender, the receive thread and peer_poll all use, must read them back field by field and write exactly
// them.
#include "pbd_test.h"
#include "Emu/PBD/pbd_peer_input_trace.h"

#include "Emu/PBD/pbd_peer_message.h"

#include <array>
#include <cstdint>
#include <cstring>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

using pbd::test::check;

namespace
{
	namespace message = pbd::peer_message;
	using message::input_check;
	using u8 = std::uint8_t;
	using u16 = std::uint16_t;
	using u32 = std::uint32_t;
	using u64 = std::uint64_t;
	using bytes = std::vector<u8>;

	constexpr u32 session_tag = 0x1a2b3c4d;

	// One field per line, at the offset in its comment; spaces split an input entry into its parts.
	constexpr const char* empty_input_hex[] = {
		"5042442d50454552",                 // 0 magic "PBD-PEER"
		"0b000000",                         // 8 version
		"00000000",                         // 12 sender role
		"ffffffff",                         // 16 newest confirmed tick (-1: none)
		"0000000000000000",                 // 20 its combat hash 0
		"0000000000000000",                 // 28 its combat hash 1
		"0000000000000000",                 // 36 its combat hash 2
		"00000000",                         // 44 input count
		"4d3c2b1a",                         // 48 session tag
		"ce984ab500000000",                 // 52 send time, us
		"0000000000000000",                 // 60 the other peer's newest send time, echoed
		"00000000",                         // 68 how long that echo was held, us
		"00000000",                         // 72 boundary
		"03000000",                         // 76 announced input delay + 1 (role 0 only)
		"06ffffff",                         // 80 frame advantage, milliticks
		"00000000",                         // 84 advantage valid
		"01000000",                         // 88 session segment
		"14000000",                         // 92 its first index
		"0a000500",                         // 96 its scene
		"04000000",                         // 100 previous segment's first index
		"07000300",                         // 104 previous segment's scene
		"00010000",                         // 108 segment counter
		"ff000000",                         // 112 previous segment counter
		"01000000",                         // 116 exit gate segment
		"15000000",                         // 120 its first ready index + 1
		"13000000",                         // 124 readiness recorded through
		"01000000",                         // 128 stage digest match
		"b1675616",                         // 132 stage digest
		"01000000",                         // 136 options sequence
		"2febd427",                         // 140 options digest
		"01000000",                         // 144 button map valid
		"404142434445464748494a4b4c4d4e4f", // 148 button map, bytes 0-15
		"505152535455565758595a5b5c5d5e5f", // 164 button map, bytes 16-31
		"01000000",                         // 180 state digest segment
		"4786c861",                         // 184 state digest
		"00000080",                         // 188 the segment's rank plan
		"00000000",                         // 192 the previous segment's rank plan
	};

	constexpr const char* full_input_hex[] = {
		"5042442d50454552",                 // 0 magic "PBD-PEER"
		"0b000000",                         // 8 version
		"01000000",                         // 12 sender role
		"f1030000",                         // 16 newest confirmed tick (-1: none)
		"efcdab8967452301",                 // 20 its combat hash 0
		"1032547698badcfe",                 // 28 its combat hash 1
		"78695a4b3c2d1e0f",                 // 36 its combat hash 2
		"10000000",                         // 44 input count
		"e8030000 4080 0000",               // 48 tick 1000, button word 0x8040, 2 zero bytes
		"e9030000 4181 0000",               // 56 tick 1001, button word 0x8141, 2 zero bytes
		"ea030000 4282 0000",               // 64 tick 1002, button word 0x8242, 2 zero bytes
		"eb030000 4383 0000",               // 72 tick 1003, button word 0x8343, 2 zero bytes
		"ec030000 4484 0000",               // 80 tick 1004, button word 0x8444, 2 zero bytes
		"ed030000 4585 0000",               // 88 tick 1005, button word 0x8545, 2 zero bytes
		"ee030000 4686 0000",               // 96 tick 1006, button word 0x8646, 2 zero bytes
		"ef030000 4787 0000",               // 104 tick 1007, button word 0x8747, 2 zero bytes
		"f0030000 4888 0000",               // 112 tick 1008, button word 0x8848, 2 zero bytes
		"f1030000 4989 0000",               // 120 tick 1009, button word 0x8949, 2 zero bytes
		"f2030000 4a8a 0000",               // 128 tick 1010, button word 0x8a4a, 2 zero bytes
		"f3030000 4b8b 0000",               // 136 tick 1011, button word 0x8b4b, 2 zero bytes
		"f4030000 4c8c 0000",               // 144 tick 1012, button word 0x8c4c, 2 zero bytes
		"f5030000 4d8d 0000",               // 152 tick 1013, button word 0x8d4d, 2 zero bytes
		"f6030000 4e8e 0000",               // 160 tick 1014, button word 0x8e4e, 2 zero bytes
		"f7030000 4f8f 0000",               // 168 tick 1015, button word 0x8f4f, 2 zero bytes
		"4d3c2b1a",                         // 176 session tag
		"9078563412000000",                 // 180 send time, us
		"0000503412000000",                 // 188 the other peer's newest send time, echoed
		"dc050000",                         // 196 how long that echo was held, us
		"f4030000",                         // 200 boundary
		"00000000",                         // 204 announced input delay + 1 (role 0 only)
		"c4090000",                         // 208 frame advantage, milliticks
		"01000000",                         // 212 advantage valid
		"07000000",                         // 216 session segment
		"d4030000",                         // 220 its first index
		"2c001100",                         // 224 its scene
		"84030000",                         // 228 previous segment's first index
		"2c001000",                         // 232 previous segment's scene
		"e0040000",                         // 236 segment counter
		"df040000",                         // 240 previous segment counter
		"07000000",                         // 244 exit gate segment
		"e9030000",                         // 248 its first ready index + 1
		"f2030000",                         // 252 readiness recorded through
		"03000000",                         // 256 stage digest match
		"b979379e",                         // 260 stage digest
		"02000000",                         // 264 options sequence
		"6bcaeb85",                         // 268 options digest
		"01000000",                         // 272 button map valid
		"a0a1a2a3a4a5a6a7a8a9aaabacadaeaf", // 276 button map, bytes 0-15
		"b0b1b2b3b4b5b6b7b8b9babbbcbdbebf", // 292 button map, bytes 16-31
		"06000000",                         // 308 state digest segment
		"35aeb2c2",                         // 312 state digest
		"d9000080",                         // 316 the segment's rank plan
		"00000080",                         // 320 the previous segment's rank plan
	};

	constexpr const char* end_hex[] = {
		"5042442d454e4421", // 0 magic "PBD-END!"
		"01000000",         // 8 format
		"01000000",         // 12 sender role
		"4d3c2b1a",         // 16 session tag
		"0b000000",         // 20 finish reason
		"e1100000",         // 24 tick
		"00000000",         // 28 zero
	};

	u8 nibble(char c)
	{
		return static_cast<u8>(c <= '9' ? c - '0' : c - 'a' + 10);
	}

	bytes from_hex(std::span<const char* const> parts)
	{
		bytes out;
		bool valid = true;
		for (const std::string_view part : parts)
		{
			std::string digits;
			for (const char c : part)
				if (c != ' ')
					digits += c;
			valid = valid && digits.size() % 2 == 0 && digits.find_first_not_of("0123456789abcdef") == digits.npos;
			for (std::size_t i = 0; valid && i < digits.size(); i += 2)
				out.push_back(static_cast<u8>(nibble(digits[i]) << 4 | nibble(digits[i + 1])));
		}
		check(valid, "golden bytes are whole lower-case hex bytes");
		return out;
	}

	std::array<u8, 32> map_from(u8 first)
	{
		std::array<u8, 32> map{};
		for (u32 i = 0; i < map.size(); i++)
			map[i] = static_cast<u8>(first + i);
		return map;
	}

	// The options/map barrier packet: role 0 has not sampled a controller yet, so it has no inputs and no confirmed
	// tick (whose combat hashes stay zero), and nothing to echo.
	message::input_message empty_input()
	{
		message::input_message m;
		m.role = 0;
		m.confirmed = -1;
		m.sync = {session_tag, 0xb54a98ce, 0, 0, 0, 3, -250, 0};
		m.segment = {1, 20, 0x0005000a, 4, 0x00030007, 0x100, 0xff, 0x80000000, 0};
		m.gate = {1, 21, 19};
		m.digests = {1, 0x165667b1, 1, 0x27d4eb2f, 1, map_from(0x40), 1, 0x61c88647};
		return m;
	}

	// Role 1 with the full window of 16 scripted inputs and every field set; only role 0 announces the input delay.
	message::input_message full_input()
	{
		message::input_message m;
		m.role = 1;
		m.confirmed = 1009;
		m.combat = {0x0123456789abcdef, 0xfedcba9876543210, 0x0f1e2d3c4b5a6978};
		for (u32 k = 0; k < message::max_inputs; k++)
			m.inputs[m.count++] = {1000 + k, static_cast<u16>(0x8040 + 0x0101 * k)};
		m.sync = {session_tag, 0x1234567890, 0x1234500000, 1500, 1012, 0, 2500, 1};
		m.segment = {7, 980, 0x0011002c, 900, 0x0010002c, 0x4e0, 0x4df, 0x800000d9, 0x80000000};
		m.gate = {7, 1001, 1010};
		m.digests = {3, 0x9e3779b9, 2, 0x85ebca6b, 1, map_from(0xa0), 6, 0xc2b2ae35};
		return m;
	}

	// Role 1 ends the session at tick 4321 because the lobby ended the set (finish reason 11).
	constexpr message::end_message expected_end{1, session_tag, 11, 4321};

	// The bytes with a little-endian word written over those at `at`.
	bytes with32(bytes in, std::size_t at, u32 value)
	{
		for (u32 i = 0; i < 4; i++)
			in[at + i] = static_cast<u8>(value >> (8 * i));
		return in;
	}

	bytes with_magic(bytes in, const message::magic& value)
	{
		std::memcpy(in.data(), value.data(), value.size());
		return in;
	}

	bytes encode(const message::input_message& m)
	{
		std::array<u8, message::max_input_bytes> out{};
		const std::size_t size = message::encode_input(m, out);
		return bytes(out.begin(), out.begin() + size);
	}

	// What role `receiver` reads from `in` in a session: the fields of a valid message, nothing for anything else.
	std::optional<message::input_message> read(std::span<const u8> in, u32 receiver, u32 session = session_tag)
	{
		message::input_message m;
		if (message::read_input(in, receiver, session, m) != input_check::valid)
			return std::nullopt;
		return m;
	}

	input_check status(std::span<const u8> in, u32 receiver = 0)
	{
		return message::check_input(in, receiver, session_tag);
	}

	// A fixed stream of numbers for the round trips (xorshift64), so every run checks the same messages.
	struct numbers
	{
		u64 state = 0x9e3779b97f4a7c15;
		u64 next()
		{
			state ^= state << 13;
			state ^= state >> 7;
			state ^= state << 17;
			return state;
		}
		u32 word()
		{
			return static_cast<u32>(next());
		}
	};

	message::input_message random_input(numbers& n)
	{
		message::input_message m;
		m.role = n.word() & 1;
		m.confirmed = static_cast<std::int32_t>(n.word());
		for (auto& hash : m.combat)
			hash = n.next();
		m.count = n.word() % (message::max_inputs + 1);
		for (u32 k = 0; k < m.count; k++)
			m.inputs[k] = {n.word(), static_cast<u16>(n.word())};
		m.sync = {n.word(), n.next(), n.next(), n.word(), n.word(), n.word(), static_cast<std::int32_t>(n.word()), n.word()};
		m.segment = {n.word(), n.word(), n.word(), n.word(), n.word(), n.word(), n.word(), n.word(), n.word()};
		m.gate = {n.word(), n.word(), n.word()};
		m.digests = {n.word(), n.word(), n.word(), n.word(), n.word(), {}, n.word(), n.word()};
		for (auto& b : m.digests.map)
			b = static_cast<u8>(n.word());
		return m;
	}
} // namespace

PBD_TEST(PbdPeerMessage, TheGoldenInputMessagesReadBackFieldByField)
{
	const bytes empty = from_hex(empty_input_hex), full = from_hex(full_input_hex);
	check(empty.size() == 196 && full.size() == 324, "input messages are 196 bytes plus 8 per input");
	check(read(empty, 1) == empty_input(), "the empty message reads back field by field");
	check(read(full, 0) == full_input(), "the full message reads back field by field");
}

PBD_TEST(PbdPeerMessage, TheEncoderWritesExactlyTheGoldenInputBytes)
{
	const bytes empty = from_hex(empty_input_hex), full = from_hex(full_input_hex);
	check(encode(empty_input()) == empty, "the empty message is written byte for byte");
	check(encode(full_input()) == full, "the full message is written byte for byte");
	auto too_many = full_input();
	too_many.count = message::max_inputs + 1;
	std::array<u8, message::max_input_bytes> out{};
	check(message::encode_input(too_many, out) == 0, "more than 16 inputs are never written");
}

PBD_TEST(PbdPeerMessage, ReadingThenWritingAnyMessageGivesItsBytesBack)
{
	for (const bytes& golden : {from_hex(empty_input_hex), from_hex(full_input_hex)})
	{
		const auto m = read(golden, golden[12] ^ 1);
		check(m && encode(*m) == golden, "a golden message survives a read and a write unchanged");
	}
	numbers n;
	for (u32 round = 0; round < 2000; round++)
	{
		const auto m = random_input(n);
		const bytes encoded = encode(m);
		check(encoded.size() == message::input_bytes(m.count), "the size follows the entry count");
		check(read(encoded, m.role ^ 1, m.sync.session) == m, "every field reads back as it was written");
	}
}

PBD_TEST(PbdPeerMessage, AnInputMessageOutsideTheLayoutIsRejectedByTheRuleItBreaks)
{
	const bytes full = from_hex(full_input_hex);
	check(status(full) == input_check::valid, "the golden message is valid for role 0");
	check(status(with32(full, 8, 10)) == input_check::bad_message && status(with32(full, 8, 12)) == input_check::bad_message,
		"another version is a bad message");
	check(status(full, 1) == input_check::bad_message, "a message from the receiver's own role is a bad message");
	bytes renamed = full;
	renamed[7] = 'S';
	check(status(renamed) == input_check::bad_message, "another magic is a bad message");
	check(status(with32(full, 44, 15)) == input_check::bad_length && status(with32(full, 44, 17)) == input_check::bad_length,
		"a count that disagrees with the length is a bad length");
	check(status(with32(full, 44, 0xffffffff)) == input_check::bad_length, "a huge count is a bad length");
	bytes longer = full;
	longer.push_back(0);
	check(status(longer) == input_check::bad_length, "a byte past the last field is a bad length");
	check(status(with32(full, 176, session_tag + 2)) == input_check::other_session, "another session's tag is named as such");
	message::input_message untouched = empty_input();
	check(message::read_input(with32(full, 176, 0), 0, session_tag, untouched) == input_check::other_session &&
			  untouched == empty_input(),
		"a rejected message leaves the fields it was to fill as they were");
}

PBD_TEST(PbdPeerMessage, EveryTruncatedInputMessageIsRejected)
{
	const bytes full = from_hex(full_input_hex);
	for (std::size_t size = 0; size < full.size(); size++)
	{
		const std::span<const u8> cut(full.data(), size);
		const auto expected = size < message::input_fixed_bytes ? input_check::bad_message : input_check::bad_length;
		check(status(cut) == expected && !read(cut, 0), "a message cut short is never read");
	}
}

PBD_TEST(PbdPeerMessage, DuplicatesReadTheSameAndKeepEveryEntry)
{
	auto repeated = full_input();
	repeated.inputs[5] = repeated.inputs[4];
	repeated.inputs[6] = {repeated.inputs[4].tick, static_cast<u16>(repeated.inputs[4].value ^ 1)};
	const auto m = read(encode(repeated), 0);
	check(m == repeated, "a tick sent twice in one message reaches the receiver's duplicate rule twice, in order");
}

PBD_TEST(PbdPeerMessage, TheEndMessageIsWrittenAndReadAsItsGoldenBytes)
{
	const bytes end = from_hex(end_hex);
	const auto written = message::encode_end(expected_end);
	check(end.size() == message::end_bytes && bytes(written.begin(), written.end()) == end,
		"the end message is written byte for byte");
	check(message::read_end(end, 0, session_tag) == expected_end, "the end message reads back field by field");
}

PBD_TEST(PbdPeerMessage, AnEndMessageOfAnotherSessionRoleOrShapeIsIgnored)
{
	const bytes end = from_hex(end_hex), full = from_hex(full_input_hex);
	check(!message::read_end(end, 1, session_tag), "an end message from the receiver's own role is ignored");
	check(!message::read_end(end, 0, session_tag + 2), "an end message of another session is ignored");
	check(!message::read_end(with32(end, 8, 2), 0, session_tag), "an end message of another format is ignored");
	check(message::is_end_message(with32(end, 8, 2)), "but it is still never read as inputs");
	bytes longer = end;
	longer.push_back(0);
	check(!message::is_end_message(longer) && !message::read_end(longer, 0, session_tag),
		"an end message longer than 32 bytes is ignored");
	check(status(end) == input_check::bad_message && !message::is_end_message(full), "end and input messages never mix");
}

// Legacy input and end magics obey the same layout and remain distinct message types.
PBD_TEST(PbdPeerMessage, TheOlderMagicsReadTheSameButNeverMix)
{
	const bytes full = from_hex(full_input_hex), end = from_hex(end_hex);
	check(std::string_view(message::old_input_magic.data(), 8) == "DR26PEER" &&
			  std::string_view(message::old_end_magic.data(), 8) == "DR26END!",
		"the older magics are the bytes builds before the rename sent");
	check(read(with_magic(full, message::old_input_magic), 0) == full_input(),
		"an input message with the older magic reads the same");
	check(message::read_end(with_magic(end, message::old_end_magic), 0, session_tag) == expected_end,
		"an end message with the older magic reads the same");
	check(status(with_magic(full, message::old_end_magic)) == input_check::bad_message &&
			  !message::read_end(with_magic(end, message::old_input_magic), 0, session_tag),
		"the older magics never mix either");
}

PBD_TEST(PbdPeerInputTrace, BoundedTraceKeepsOrderAndCountsLoss)
{
	pbd::peer_input_trace::queue<2> queue;
	check(!queue.pop());
	queue.push({pbd::peer_input_trace::kind::receive, 8, 6, 17});
	queue.push({pbd::peer_input_trace::kind::fill, 9, 7, 18});
	queue.push({pbd::peer_input_trace::kind::assign, 10, 8, 19});
	check(queue.dropped() == 1);
	check(queue.pop()->value == 17);
	queue.push({pbd::peer_input_trace::kind::assign, 11, 9, 20});
	check(queue.pop()->value == 18);
	check(queue.pop()->value == 20);
	check(!queue.pop() && queue.dropped() == 1);
}

PBD_TEST(PbdPeerMessage, ReceiveAdmissionBoundsConfirmedProofsBeforeKeepingAnyFields)
{
	const auto packet = encode(empty_input()); // A keyed participant may send zero inputs with any claimed proof tick.
	const message::confirmed_window window{100, 1000, 200, 16};
	const auto untouched = full_input();
	for (const u32 tick : {0x7fffffffu, 0xfffffffeu, 99u, 216u, 1000u})
	{
		auto output = untouched;
		const auto incoming = with32(packet, message::input_head::confirmed, tick);
		check(message::read_session_input(incoming, 1, session_tag, window, output) == input_check::bad_confirmed);
		check(output == untouched);
	}
	for (const u32 tick : {0xffffffffu, 100u, 200u, 215u})
	{
		auto output = untouched;
		const auto incoming = with32(packet, message::input_head::confirmed, tick);
		check(message::read_session_input(incoming, 1, session_tag, window, output) == input_check::valid);
		check(static_cast<u32>(output.confirmed) == tick && output.count == 0);
	}
	// The end of a long session stays below signed arithmetic's limit, even when adding the pending window.
	auto output = untouched;
	const auto incoming = with32(packet, message::input_head::confirmed, 0x7fffffffu);
	check(message::read_session_input(incoming, 1, session_tag, {0x7ffe0000u, 0x7fff0000u, 0x7ffeffffu, 16}, output) == input_check::bad_confirmed);
	check(output == untouched);
}

PBD_TEST(PbdPeerMessage, ReceiveAdmissionAllowsTheLocallyScheduledCharacterSelectLead)
{
	auto output = full_input();
	const auto packet = encode(empty_input());
	const message::confirmed_window window{100, 1000, 200, 31};
	check(message::read_session_input(with32(packet, message::input_head::confirmed, 230), 1, session_tag, window, output) == input_check::valid);
	check(output.confirmed == 230);
	const auto untouched = output;
	check(message::read_session_input(with32(packet, message::input_head::confirmed, 231), 1, session_tag, window, output) == input_check::bad_confirmed);
	check(output == untouched);
}

PBD_TEST(PbdPeerMessage, ReceiveAdmissionBoundsProofsFromTheCurrentSegmentAlignment)
{
	auto input = empty_input();
	input.confirmed = 255;
	input.segment.number = 11;
	input.segment.first_index = 240;
	const message::confirmed_window window{100, 1000, 200, 16, 11};
	auto output = full_input();
	check(message::read_session_input(encode(input), 1, session_tag, window, output) == input_check::valid);
	check(output == input);
	const auto untouched = output;
	input.confirmed = 256;
	check(message::read_session_input(encode(input), 1, session_tag, window, output) == input_check::bad_confirmed);
	check(output == untouched);
	input.confirmed = 255;
	input.segment.number = 12;
	check(message::read_session_input(encode(input), 1, session_tag, window, output) == input_check::bad_confirmed);
	check(output == untouched);
	input.segment.number = 11;
	input.segment.first_index = 1000;
	check(message::read_session_input(encode(input), 1, session_tag, window, output) == input_check::bad_confirmed);
	check(output == untouched);
}

PBD_TEST(PbdPeerMessage, ReceiveAdmissionDiscardsUnalignedProofsWhileThePreviousLoaderFinishes)
{
	auto input = empty_input();
	input.confirmed = 15984;
	input.combat = {11, 22, 33};
	input.segment.number = 13;
	input.segment.first_index = 15778;
	input.count = 1;
	input.inputs[0] = {15984, 64};
	const message::confirmed_window window{100, 100000, 15962, 16, 14};
	auto output = full_input();
	check(message::read_session_input(encode(input), 1, session_tag, window, output) == input_check::valid);
	check(output.confirmed == -1 && output.combat == decltype(output.combat){});
	input.confirmed = -1;
	input.combat = {};
	check(output == input);
	const auto untouched = output;
	for (const u32 tick : {0x7fffffffu, 0xfffffffeu, 99u, 100000u})
	{
		input.confirmed = static_cast<std::int32_t>(tick);
		check(message::read_session_input(encode(input), 1, session_tag, window, output) == input_check::bad_confirmed);
		check(output == untouched);
	}
	input.confirmed = 15984;
	input.segment.number = 12;
	check(message::read_session_input(encode(input), 1, session_tag, window, output) == input_check::bad_confirmed);
	check(output == untouched);
	input.segment.number = 13;
	input.segment.first_index = 15963;
	check(message::read_session_input(encode(input), 1, session_tag, window, output) == input_check::bad_confirmed);
	check(output == untouched);
}
