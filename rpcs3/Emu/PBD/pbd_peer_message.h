#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <optional>
#include <span>

// Session datagrams carry inputs every frame or announce that one side has stopped.
// Every number is little-endian; receivers require the exact layout pinned by peer_message_test.
namespace pbd::peer_message
{
	using magic = std::array<char, 8>;

	inline constexpr magic input_magic{'P', 'B', 'D', '-', 'P', 'E', 'E', 'R'};
	inline constexpr magic end_magic{'P', 'B', 'D', '-', 'E', 'N', 'D', '!'};

	// Receivers also accept DR26PEER for inputs and DR26END! for endings.
	inline constexpr magic old_input_magic{'D', 'R', '2', '6', 'P', 'E', 'E', 'R'};
	inline constexpr magic old_end_magic{'D', 'R', '2', '6', 'E', 'N', 'D', '!'};

	inline bool starts_with(const void* data, std::size_t size, const magic& expected) noexcept
	{
		return size >= expected.size() && !std::memcmp(data, expected.data(), expected.size());
	}

	inline bool is_input(const void* data, std::size_t size) noexcept
	{
		return starts_with(data, size, input_magic) || starts_with(data, size, old_input_magic);
	}

	inline bool is_end(const void* data, std::size_t size) noexcept
	{
		return starts_with(data, size, end_magic) || starts_with(data, size, old_end_magic);
	}

	// The input message's layout. A receiver refuses any other version, so both PCs must send this one.
	inline constexpr std::uint32_t input_version = 11;
	// An input message carries at most this many of the sender's own inputs: one window of ticks, so a lost datagram's
	// inputs arrive in the next ones.
	inline constexpr std::uint32_t max_inputs = 16;

	// The head of an input message, after the 8-byte magic. Its entries start at `entries`.
	namespace input_head
	{
		inline constexpr std::size_t version = 8;
		inline constexpr std::size_t role = 12;      // the sender's
		inline constexpr std::size_t confirmed = 16; // the sender's newest confirmed tick, -1 for none
		inline constexpr std::size_t combat = 20;    // that tick's three 64-bit combat hashes (zero for none)
		inline constexpr std::size_t count = 44;     // how many entries follow
		inline constexpr std::size_t entries = 48;
	} // namespace input_head

	// One input entry: the tick, its 16-bit button word at +4 and two zero bytes.
	inline constexpr std::size_t entry_bytes = 8;

	// The fields after the entries, at these offsets from the end of the last entry.
	namespace input_tail
	{
		inline constexpr std::size_t session = 0;         // the session tag
		inline constexpr std::size_t send_us = 4;         // 64-bit send time, microseconds
		inline constexpr std::size_t echo_us = 12;        // 64-bit, the other peer's newest send time, echoed
		inline constexpr std::size_t echo_held_us = 20;   // how long that echo was held
		inline constexpr std::size_t boundary = 24;       // the sender's boundary
		inline constexpr std::size_t delay_announce = 28; // the input delay + 1 role 0 decided; 0 for none
		inline constexpr std::size_t advantage = 32;      // signed frame advantage, milliticks
		inline constexpr std::size_t advantage_valid = 36;
		inline constexpr std::size_t segment = 40; // the session segment, its first index and scene, then the one before
		inline constexpr std::size_t segment_first = 44;
		inline constexpr std::size_t segment_scene = 48;
		inline constexpr std::size_t prev_first = 52;
		inline constexpr std::size_t prev_scene = 56;
		inline constexpr std::size_t segment_counter = 60;
		inline constexpr std::size_t prev_counter = 64;
		inline constexpr std::size_t gate_segment = 68; // the exit gate
		inline constexpr std::size_t gate_ready = 72;
		inline constexpr std::size_t gate_through = 76;
		inline constexpr std::size_t stage_match = 80; // the newest match start's stage digest
		inline constexpr std::size_t stage_digest = 84;
		inline constexpr std::size_t options_seq = 88; // the newest options digest
		inline constexpr std::size_t options_digest = 92;
		inline constexpr std::size_t map_valid = 96; // the button map of the side the sender plays, 32 bytes
		inline constexpr std::size_t map = 100;
		inline constexpr std::size_t state_segment = 132; // the newest segment state digest
		inline constexpr std::size_t state_digest = 136;
		inline constexpr std::size_t rank_stakes = 140; // the Ranked stakes the sender held as the segment began, then the one before
		inline constexpr std::size_t prev_rank_stakes = 144;
		inline constexpr std::size_t size = 148;
	} // namespace input_tail

	inline constexpr std::size_t input_fixed_bytes = input_head::entries + input_tail::size;
	inline constexpr std::size_t max_input_bytes = input_fixed_bytes + entry_bytes * max_inputs;
	static_assert(input_fixed_bytes == 196 && max_input_bytes == 324);

	// The size of an input message with `count` entries.
	constexpr std::size_t input_bytes(std::uint32_t count) noexcept
	{
		return input_fixed_bytes + entry_bytes * count;
	}

	// The end message, after its magic; the last four of its 32 bytes are zero.
	inline constexpr std::uint32_t end_format = 1;
	inline constexpr std::size_t end_bytes = 32;
	namespace end_field
	{
		inline constexpr std::size_t format = 8;
		inline constexpr std::size_t role = 12; // the sender's
		inline constexpr std::size_t session = 16;
		inline constexpr std::size_t reason = 20; // the sender's finish reason
		inline constexpr std::size_t tick = 24;   // and the tick it stopped at
	} // namespace end_field

	struct input_entry
	{
		std::uint32_t tick = 0;
		std::uint16_t value = 0;
		bool operator==(const input_entry&) const = default;
	};

	// The clock echo for the round trip, plus the input delay and frame advantage the sender reports.
	struct sync_fields
	{
		std::uint32_t session = 0;
		std::uint64_t send_us = 0, echo_us = 0;
		std::uint32_t echo_held_us = 0, boundary = 0, delay_announce = 0;
		std::int32_t advantage_milli = 0;
		std::uint32_t advantage_valid = 0;
		bool operator==(const sync_fields&) const = default;
	};

	// The sender's session segment and the one before it: a peer can pass a one-boundary segment unseen. The rank stakes are
	// the Ranked set's stakes the sender's launcher had given it as each segment began (pbd::rank_stakes::stakes_word).
	struct segment_fields
	{
		std::uint32_t number = 0, first_index = 0, scene = 0;
		std::uint32_t prev_first_index = 0, prev_scene = 0, counter = 0, prev_counter = 0;
		std::uint32_t rank_stakes = 0, prev_rank_stakes = 0;
		bool operator==(const segment_fields&) const = default;
	};

	// The exit gate: the gated segment, its first ready index + 1, and the index below which readiness is recorded.
	struct gate_fields
	{
		std::uint32_t segment = 0, ready = 0, through = 0;
		bool operator==(const gate_fields&) const = default;
	};

	struct digest_fields
	{
		std::uint32_t stage_match = 0, stage = 0, options_seq = 0, options = 0, map_valid = 0;
		std::array<std::uint8_t, 32> map{};
		std::uint32_t state_segment = 0, state = 0;
		bool operator==(const digest_fields&) const = default;
	};

	struct input_message
	{
		std::uint32_t role = 0;
		std::int32_t confirmed = -1;
		std::array<std::uint64_t, 3> combat{};
		std::uint32_t count = 0;
		std::array<input_entry, max_inputs> inputs{}; // the first `count` are sent
		sync_fields sync;
		segment_fields segment;
		gate_fields gate;
		digest_fields digests;
		bool operator==(const input_message&) const = default;
	};

	struct end_message
	{
		std::uint32_t role = 0, session = 0, reason = 0, tick = 0;
		bool operator==(const end_message&) const = default;
	};

	namespace detail
	{
		template <typename Value>
		Value load(std::span<const std::uint8_t> in, std::size_t at) noexcept
		{
			Value value = 0;
			for (std::size_t i = 0; i < sizeof(Value); i++)
				value |= static_cast<Value>(Value{in[at + i]} << (8 * i));
			return value;
		}

		template <typename Value>
		void store(std::span<std::uint8_t> out, std::size_t at, Value value) noexcept
		{
			for (std::size_t i = 0; i < sizeof(Value); i++)
				out[at + i] = static_cast<std::uint8_t>(value >> (8 * i));
		}

		inline void store_tail(const input_message& m, std::span<std::uint8_t> out, std::size_t tail) noexcept
		{
			namespace at = input_tail;
			const auto put32 = [&](std::size_t offset, std::uint32_t value)
			{
				store(out, tail + offset, value);
			};
			put32(at::session, m.sync.session);
			store(out, tail + at::send_us, m.sync.send_us);
			store(out, tail + at::echo_us, m.sync.echo_us);
			put32(at::echo_held_us, m.sync.echo_held_us);
			put32(at::boundary, m.sync.boundary);
			put32(at::delay_announce, m.sync.delay_announce);
			put32(at::advantage, static_cast<std::uint32_t>(m.sync.advantage_milli));
			put32(at::advantage_valid, m.sync.advantage_valid);
			put32(at::segment, m.segment.number);
			put32(at::segment_first, m.segment.first_index);
			put32(at::segment_scene, m.segment.scene);
			put32(at::prev_first, m.segment.prev_first_index);
			put32(at::prev_scene, m.segment.prev_scene);
			put32(at::segment_counter, m.segment.counter);
			put32(at::prev_counter, m.segment.prev_counter);
			put32(at::gate_segment, m.gate.segment);
			put32(at::gate_ready, m.gate.ready);
			put32(at::gate_through, m.gate.through);
			put32(at::stage_match, m.digests.stage_match);
			put32(at::stage_digest, m.digests.stage);
			put32(at::options_seq, m.digests.options_seq);
			put32(at::options_digest, m.digests.options);
			put32(at::map_valid, m.digests.map_valid);
			std::memcpy(out.data() + tail + at::map, m.digests.map.data(), m.digests.map.size());
			put32(at::state_segment, m.digests.state_segment);
			put32(at::state_digest, m.digests.state);
			put32(at::rank_stakes, m.segment.rank_stakes);
			put32(at::prev_rank_stakes, m.segment.prev_rank_stakes);
		}

		inline void load_tail(std::span<const std::uint8_t> in, std::size_t tail, input_message& m) noexcept
		{
			namespace at = input_tail;
			const auto get32 = [&](std::size_t offset)
			{
				return load<std::uint32_t>(in, tail + offset);
			};
			m.sync = {get32(at::session), load<std::uint64_t>(in, tail + at::send_us), load<std::uint64_t>(in, tail + at::echo_us),
				get32(at::echo_held_us), get32(at::boundary), get32(at::delay_announce), static_cast<std::int32_t>(get32(at::advantage)),
				get32(at::advantage_valid)};
			m.segment = {get32(at::segment), get32(at::segment_first), get32(at::segment_scene), get32(at::prev_first),
				get32(at::prev_scene), get32(at::segment_counter), get32(at::prev_counter), get32(at::rank_stakes), get32(at::prev_rank_stakes)};
			m.gate = {get32(at::gate_segment), get32(at::gate_ready), get32(at::gate_through)};
			m.digests = {get32(at::stage_match), get32(at::stage_digest), get32(at::options_seq), get32(at::options_digest),
				get32(at::map_valid), {}, get32(at::state_segment), get32(at::state_digest)};
			std::memcpy(m.digests.map.data(), in.data() + tail + at::map, m.digests.map.size());
		}
	} // namespace detail

	// Writes `m` into the start of `out` and returns its size, input_bytes(m.count); 0 (nothing written) when m.count is
	// above max_inputs. The two bytes after each button word are zero.
	inline std::size_t encode_input(const input_message& m, std::span<std::uint8_t, max_input_bytes> out) noexcept
	{
		if (m.count > max_inputs)
			return 0;
		const std::size_t size = input_bytes(m.count);
		std::memset(out.data(), 0, size);
		std::memcpy(out.data(), input_magic.data(), input_magic.size());
		detail::store(out, input_head::version, input_version);
		detail::store(out, input_head::role, m.role);
		detail::store(out, input_head::confirmed, static_cast<std::uint32_t>(m.confirmed));
		for (std::size_t k = 0; k < m.combat.size(); k++)
			detail::store(out, input_head::combat + 8 * k, m.combat[k]);
		detail::store(out, input_head::count, m.count);
		for (std::uint32_t k = 0; k < m.count; k++)
		{
			detail::store(out, input_head::entries + entry_bytes * k, m.inputs[k].tick);
			detail::store(out, input_head::entries + entry_bytes * k + 4, m.inputs[k].value);
		}
		detail::store_tail(m, out, input_head::entries + entry_bytes * m.count);
		return size;
	}

	// How a receiver classifies a datagram as an input message, in the order it checks.
	enum class input_check
	{
		valid,
		bad_message,   // too short for the head, not an input magic, another version, or not from the other role
		bad_length,    // more than max_inputs entries, or a length that does not match the entry count
		other_session, // well formed, but tagged with another session
		bad_confirmed, // confirmed tick is outside the receiving session's retained/pending window
	};

	// Checks `in` as an input message to `receiver_role` (0 or 1) in the session tagged `session`, reading no other field.
	inline input_check check_input(std::span<const std::uint8_t> in, std::uint32_t receiver_role,
		std::uint32_t session) noexcept
	{
		if (in.size() < input_fixed_bytes || !is_input(in.data(), in.size()) ||
			detail::load<std::uint32_t>(in, input_head::version) != input_version ||
			detail::load<std::uint32_t>(in, input_head::role) != 1 - receiver_role)
			return input_check::bad_message;
		const auto count = detail::load<std::uint32_t>(in, input_head::count);
		if (count > max_inputs || in.size() != input_bytes(count))
			return input_check::bad_length;
		if (detail::load<std::uint32_t>(in, input_head::entries + entry_bytes * count + input_tail::session) != session)
			return input_check::other_session;
		return input_check::valid;
	}

	// The sender's echo of the newest send time it has received from this receiver (sync_fields::echo_us), read from a message
	// check_input accepted without decoding the rest: the receive thread reads it for every input.
	inline std::uint64_t input_echo_us(std::span<const std::uint8_t> in) noexcept
	{
		const auto count = detail::load<std::uint32_t>(in, input_head::count);
		return detail::load<std::uint64_t>(in, input_head::entries + entry_bytes * count + input_tail::echo_us);
	}

	// check_input, and for a valid message its fields into `out` (left as it was otherwise).
	inline input_check read_input(std::span<const std::uint8_t> in, std::uint32_t receiver_role, std::uint32_t session,
		input_message& out) noexcept
	{
		const auto result = check_input(in, receiver_role, session);
		if (result != input_check::valid)
			return result;
		input_message m;
		m.role = detail::load<std::uint32_t>(in, input_head::role);
		m.confirmed = static_cast<std::int32_t>(detail::load<std::uint32_t>(in, input_head::confirmed));
		for (std::size_t k = 0; k < m.combat.size(); k++)
			m.combat[k] = detail::load<std::uint64_t>(in, input_head::combat + 8 * k);
		m.count = detail::load<std::uint32_t>(in, input_head::count);
		for (std::uint32_t k = 0; k < m.count; k++)
			m.inputs[k] = {detail::load<std::uint32_t>(in, input_head::entries + entry_bytes * k),
				detail::load<std::uint16_t>(in, input_head::entries + entry_bytes * k + 4)};
		detail::load_tail(in, input_head::entries + entry_bytes * m.count, m);
		out = m;
		return result;
	}

	struct confirmed_window
	{
		std::uint32_t first, end, boundary, pending;
		std::uint32_t segment = 0;
	};
	// Receive admission, after authentication and before any message field changes session state.
	// -1 is the wire sentinel; all other confirmations name retained ticks or a bounded pending proof.
	inline input_check read_session_input(std::span<const std::uint8_t> in, std::uint32_t receiver_role,
		std::uint32_t session, confirmed_window window, input_message& out) noexcept
	{
		input_message message;
		const auto status = read_input(in, receiver_role, session, message);
		if (status != input_check::valid)
			return status;
		if (message.confirmed != -1)
		{
			const auto tick = static_cast<std::uint32_t>(message.confirmed);
			// A peer may have reached this segment at a later index while this receiver is still at its barrier.
			// The announced start is the alignment frontier, bounded by the same session extent as the proof.
			const bool aligned = window.segment && message.segment.number == window.segment &&
			                     message.segment.first_index >= window.first && message.segment.first_index < window.end;
			const auto frontier = aligned ? std::max(window.boundary, message.segment.first_index) : window.boundary;
			if (message.confirmed < 0 || tick < window.first || tick >= window.end || !window.pending)
				return input_check::bad_confirmed;
			if (std::uint64_t{tick} >= std::uint64_t{frontier} + window.pending)
			{
				// While this peer waits at a barrier, the other may still execute the previous loading segment.
				// Its future proof has no local tick until alignment; keep its inputs and announcement, not that proof.
				if (window.segment <= 1 || message.segment.number != window.segment - 1 || message.segment.first_index > window.boundary)
					return input_check::bad_confirmed;
				message.confirmed = -1;
				message.combat = {};
			}
		}
		out = message;
		return input_check::valid;
	}

	inline std::array<std::uint8_t, end_bytes> encode_end(const end_message& m) noexcept
	{
		std::array<std::uint8_t, end_bytes> out{};
		std::memcpy(out.data(), end_magic.data(), end_magic.size());
		detail::store(out, end_field::format, end_format);
		detail::store(out, end_field::role, m.role);
		detail::store(out, end_field::session, m.session);
		detail::store(out, end_field::reason, m.reason);
		detail::store(out, end_field::tick, m.tick);
		return out;
	}

	// Whether `in` has the end message's size and magic. Such a datagram is never read as inputs, whatever it holds.
	inline bool is_end_message(std::span<const std::uint8_t> in) noexcept
	{
		return in.size() == end_bytes && is_end(in.data(), in.size());
	}

	// The fields of an end message from the other role in the session tagged `session`; nothing for anything else.
	inline std::optional<end_message> read_end(std::span<const std::uint8_t> in, std::uint32_t receiver_role,
		std::uint32_t session) noexcept
	{
		if (!is_end_message(in) || detail::load<std::uint32_t>(in, end_field::format) != end_format ||
			detail::load<std::uint32_t>(in, end_field::role) != 1 - receiver_role ||
			detail::load<std::uint32_t>(in, end_field::session) != session)
			return std::nullopt;
		return end_message{detail::load<std::uint32_t>(in, end_field::role), detail::load<std::uint32_t>(in, end_field::session),
			detail::load<std::uint32_t>(in, end_field::reason), detail::load<std::uint32_t>(in, end_field::tick)};
	}
} // namespace pbd::peer_message
