#include "pbd_test.h"

#include "Emu/PBD/pbd_peer_message.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <limits>
#include <span>
#include <type_traits>
#include <vector>

namespace
{
	namespace message = pbd::peer_message;
	using pbd::test::check;
	using bytes = std::vector<std::uint8_t>;
	constexpr auto largest = std::numeric_limits<std::uint32_t>::max();

	message::input_message sample(std::uint32_t role, std::uint32_t count)
	{
		message::input_message m;
		m.role = role;
		m.count = count;
		m.confirmed = std::numeric_limits<std::int32_t>::min();
		m.combat = {0, 0xffffffffffffffffULL, 0x0123456789abcdefULL};
		for (std::uint32_t k = 0; k < count; k++)
			m.inputs[k] = {largest - k, static_cast<std::uint16_t>(0xffff - k)};
		m.sync = {largest, 0xffffffffffffffffULL, 0x0123456789abcdefULL, largest, largest, 1,
			std::numeric_limits<std::int32_t>::min(), 1};
		m.segment = {1, 2, 3, 4, 5, 6, largest, 0x8000ffff, largest};
		m.gate = {7, 8, largest};
		m.digests = {9, 10, 11, 12, 1, {}, 13, largest};
		for (std::size_t k = 0; k < m.digests.map.size(); k++)
			m.digests.map[k] = static_cast<std::uint8_t>(0x80 + k);
		return m;
	}

	bytes encode(const message::input_message& m)
	{
		std::array<std::uint8_t, message::max_input_bytes> out;
		out.fill(0xa5);
		const auto size = message::encode_input(m, out);
		check(size == message::input_bytes(m.count), "the production encoder writes the exact datagram");
		check(std::all_of(out.begin() + size, out.end(), [](auto byte)
				  {
					  return byte == 0xa5;
				  }),
			"nothing past the datagram is written");
		return bytes(out.begin(), out.begin() + size);
	}

	void refuse_input(std::span<const std::uint8_t> wire, const message::input_message& expected)
	{
		auto out = expected;
		check(message::read_input(wire, expected.role ^ 1, expected.sync.session, out) != message::input_check::valid,
			"a malformed datagram is refused");
		check(out == expected, "refusal leaves every output field untouched");
	}
} // namespace

PBD_TEST(PbdPeerConformance, BothRolesRoundTripEveryWindowSizeAndExtremeFields)
{
	for (std::uint32_t role : {0u, 1u})
	{
		for (std::uint32_t count = 0; count <= message::max_inputs; count++)
		{
			const auto expected = sample(role, count);
			const auto wire = encode(expected);
			message::input_message actual;
			check(message::read_input(wire, role ^ 1, largest, actual) == message::input_check::valid);
			check(actual == expected, "all signed, unsigned, array and tail fields survive the codec");
			for (std::uint32_t k = 0; k < count; k++)
				check(wire[54 + 8 * k] == 0 && wire[55 + 8 * k] == 0, "entry padding is zero");
		}
	}
}

PBD_TEST(PbdPeerConformance, EmptyInputsAndExtremeEndFieldsHaveGoldenBytes)
{
	const std::array<std::uint8_t, 196> empty{
		0x50, 0x42, 0x44, 0x2d, 0x50, 0x45, 0x45, 0x52,
		0x0b, 0, 0, 0, 0, 0, 0, 0, 0xff, 0xff, 0xff, 0xff};
	const auto written = encode({});
	check(std::equal(empty.begin(), empty.end(), written.begin(), written.end()));
	message::input_message actual;
	check(message::read_input(empty, 1, 0, actual) == message::input_check::valid);
	check(actual == message::input_message{});
	const std::array<std::uint8_t, 32> end{
		0x50, 0x42, 0x44, 0x2d, 0x45, 0x4e, 0x44, 0x21,
		1, 0, 0, 0, 0, 0, 0, 0, 0xff, 0xff, 0xff, 0xff,
		0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0, 0, 0, 0};
	const message::end_message expected{0, largest, largest, largest};
	check(message::encode_end(expected) == end);
	check(message::read_end(end, 1, largest) == expected);
}

PBD_TEST(PbdPeerConformance, EveryInputWindowRejectsAllTruncationsAndTrailingBytes)
{
	for (std::uint32_t count = 0; count <= message::max_inputs; count++)
	{
		const auto expected = sample(count % 2, count);
		auto wire = encode(expected);
		for (std::size_t size = 0; size < wire.size(); size++)
			refuse_input(std::span(wire).first(size), expected);
		wire.push_back(0);
		refuse_input(wire, expected);
		wire.resize(65507, 0);
		refuse_input(wire, expected);
	}
}

PBD_TEST(PbdPeerConformance, EncodingRefusesShortBuffersAndExcessCountsWithoutWriting)
{
	// The encoder takes a buffer of exactly max_input_bytes, so a shorter or unsized one does not compile.
	static_assert(!std::is_convertible_v<std::span<std::uint8_t, message::max_input_bytes - 1>, std::span<std::uint8_t, message::max_input_bytes>>);
	static_assert(!std::is_convertible_v<bytes&, std::span<std::uint8_t, message::max_input_bytes>>);
	for (auto count : {17u, largest})
	{
		auto invalid = sample(0, 0);
		invalid.count = count;
		std::array<std::uint8_t, message::max_input_bytes> out;
		out.fill(0xa5);
		check(message::encode_input(invalid, out) == 0);
		check(std::all_of(out.begin(), out.end(), [](auto byte)
			{
				return byte == 0xa5;
			}));
	}
}

PBD_TEST(PbdPeerConformance, BothInputMagicsEnforceVersionCountRoleAndSession)
{
	const auto expected = sample(1, 1);
	for (const auto& magic : {message::input_magic, message::old_input_magic})
	{
		auto wire = encode(expected);
		std::copy(magic.begin(), magic.end(), wire.begin());
		message::input_message actual;
		check(message::read_input(wire, 0, largest, actual) == message::input_check::valid);
		check(actual == expected);
		check(message::check_input(wire, 1, largest) == message::input_check::bad_message);
		check(message::check_input(wire, 0, 0) == message::input_check::other_session);
		for (auto offset : {8u, 12u, 44u})
		{
			auto bad = wire;
			bad[offset] = 0xff;
			refuse_input(bad, expected);
		}
	}
}

PBD_TEST(PbdPeerConformance, BothEndMagicsRefuseEveryTruncationVersionRoleAndSession)
{
	for (auto role : {0u, 1u})
	{
		const message::end_message expected{role, largest, largest, largest};
		for (const auto& magic : {message::end_magic, message::old_end_magic})
		{
			auto wire = message::encode_end(expected);
			std::copy(magic.begin(), magic.end(), wire.begin());
			check(message::read_end(wire, role ^ 1, largest) == expected);
			for (std::size_t size = 0; size < wire.size(); size++)
				check(!message::read_end(std::span(wire).first(size), role ^ 1, largest));
			check(!message::read_end(wire, role, largest));
			check(!message::read_end(wire, role ^ 1, 0));
			for (auto format : {0u, 2u, 0xffu})
			{
				auto bad = wire;
				bad[8] = static_cast<std::uint8_t>(format);
				check(!message::read_end(bad, role ^ 1, largest));
			}
			bytes oversized(wire.begin(), wire.end());
			oversized.push_back(0);
			check(!message::read_end(oversized, role ^ 1, largest));
		}
	}
}
