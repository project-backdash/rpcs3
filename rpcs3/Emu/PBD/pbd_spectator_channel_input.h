#pragma once

#include "pbd_spectator_channel_controls.h"

namespace pbd::spectator
{
	inline bool local_channel::decode_frames(const std::uint8_t* bytes, u32 count, std::size_t stride, bool applied, std::deque<frame>& incoming)
	{
		for (u32 i = 0; i < count; ++i)
		{
			const auto* data = bytes + header_bytes + i * stride;
			frame item;
			for (u32 port = 0; port < 2; ++port)
			{
				item.pads[port] = static_cast<std::uint16_t>(data[8 * port] | (u32{data[8 * port + 1]} << 8));
				for (u32 j = 2; j < 8; ++j)
					if (data[8 * port + j])
						return refuse("noncanonical_pad");
			}
			item.boundary = le32(data + 16);
			item.segment = le32(data + 20);
			item.counter = le32(data + 24);
			item.scene = le32(data + 28);
			item.mask = le32(data + 32);
			item.gate_hold = le32(data + 36);
			for (u32 region = 0; region < 3; ++region)
				item.combat[region] = le64(data + 40 + 8 * region);
			if (applied)
			{
				item.applied_verified = true;
				for (u32 port = 0; port < 2; ++port)
				{
					item.applied[port] = static_cast<std::uint16_t>(data[64 + 8 * port] | (u32{data[65 + 8 * port]} << 8));
					for (u32 j = 2; j < 8; ++j)
						if (data[64 + 8 * port + j])
							return refuse("noncanonical_applied_pad");
				}
			}
			if (item.boundary > maximum_boundary || item.boundary < m_origin || item.segment > 255 || item.scene > 32 || item.mask > 7 || item.gate_hold > 1 ||
				(!incoming.empty() && item.boundary <= incoming.back().boundary))
				return refuse("sync_facts_invalid");
			incoming.push_back(item);
		}
		return true;
	}

	template <typename Hash>
	inline bool local_channel::poll_input(Hash hash, std::chrono::steady_clock::time_point now)
	{
		if (!healthy() || !m_prepared || !game_active() || !m_frames.empty() || m_reserved)
			return healthy();
		const auto path = m_directory / ("spectator-input-" + std::to_string(m_packet_serial + 1) + ".bin");
		std::error_code ec;
		if (!std::filesystem::exists(path, ec) && !ec)
		{
			m_input_read_denied.reset();
			return true;
		}
		if (ec || !std::filesystem::is_regular_file(path, ec) || std::filesystem::is_symlink(path, ec) || ec)
			return refuse("input_file_invalid");
		std::array<std::uint8_t, maximum_packet_bytes + 1> bytes{};
		std::ifstream stream(path, std::ios::binary);
		if (!stream)
		{
			// A published packet can still be held by its writer on Windows. Poll it without advancing the receipt.
			if (!m_input_read_denied)
				m_input_read_denied = now;
			return now - *m_input_read_denied < std::chrono::seconds(1) || refuse("input_read_failed");
		}
		m_input_read_denied.reset();
		stream.read(reinterpret_cast<char*>(bytes.data()), bytes.size());
		const auto size = static_cast<std::size_t>(stream.gcount());
		constexpr std::array<std::uint8_t, 8> magic{'P', 'B', 'D', 'S', 'P', 'C', '1', 0};
		if (stream.bad() || size < header_bytes || size > maximum_packet_bytes || !std::equal(magic.begin(), magic.end(), bytes.begin()))
			return refuse("packet_invalid");
		const auto count = u32{bytes[24]} | (u32{bytes[25]} << 8), first = le32(bytes.data() + 20);
		const bool applied = (bytes[26] & 1u) != 0;
		const auto stride = applied ? applied_frame_bytes : frame_bytes;
		digest binding{}, payload{};
		std::copy_n(bytes.data() + 32, 32, binding.begin());
		std::copy_n(bytes.data() + 64, 32, payload.begin());
		if (le64(bytes.data() + 8) != m_identity.generation || le32(bytes.data() + 16) != m_game || first != m_receipt ||
			!count || count > packet_frames || bytes[26] > 1 || bytes[27] || (m_identity.applied && !applied) || le32(bytes.data() + 28) != m_packet_serial + 1 ||
			binding != m_identity.binding || size != header_bytes + count * stride || first + count > maximum_frames ||
			(m_end && first + count > *m_end) || hash(bytes.data() + header_bytes, count * stride) != payload)
			return refuse("packet_binding_or_sequence_mismatch");
		std::deque<frame> incoming;
		if (!decode_frames(bytes.data(), count, stride, applied, incoming))
			return false;
		m_frames = std::move(incoming);
		m_receipt += count;
		++m_packet_serial;
		m_packet_through = m_receipt;
		m_packet_digest = payload;
		if (!publish("received", value::object({{"chunkSerial", value::number(m_packet_serial)}, {"chunkDigest", value::string(hex(payload))},
									 {"through", value::number(m_receipt)}, {"frameCount", value::number(count)}})))
			return refuse("report_publish_failed");
		return true;
	}

	inline bool local_channel::verify_state(u32 boundary, u32 segment, u32 scene, u32 counter, u32 mask, const std::array<u64, 3>& combat)
	{
		if (!healthy() || !m_reserved || m_frames.empty())
			return refuse("state_without_input");
		const auto& item = m_frames.front();
		if (item.boundary != pbd_source_boundary(boundary) || item.segment != segment || item.scene != scene || item.counter != counter || item.mask != mask)
			return refuse("native_coordinate_mismatch");
		for (u32 i = 0; i < 3; ++i)
			if ((mask & (1u << i)) && item.combat[i] != combat[i])
				return refuse("peer_sync_hash_mismatch");
		m_checked = m_consumed + 1;
		return true;
	}

	inline bool local_channel::expose(u32 owner, u32 boundary, u32 stage, u32 left, u32 right, u32 rounds, bool first_intro, const native_round* actual_round)
	{
		const picks applied{stage, left, right};
		if (!healthy() || !m_reserved || m_visible || owner != pbd::t5dr::main_ppu_id || !first_intro || rounds != m_identity.rounds || !plausible(applied) ||
			(!m_identity.in_game && applied != fixed_picks()))
			return refuse("applied_intro_unverified");
		if (m_reported_picks && applied != *m_reported_picks)
			return refuse("game_picks_mismatch");
		if (m_load && !keeps(*m_load, applied))
			return refuse("rematch_picks_changed");
		if (m_round_required && !m_expected_round)
			return refuse("native_round_start_missing");
		if (m_expected_round && (!actual_round || *actual_round != *m_expected_round))
			return refuse("native_round_start_mismatch");
		m_applied_picks = applied;
		m_visible = true;
		draw();
		auto payload = value::object({{"proof", value::string("applied-intro-v1")}, {"owner", value::number(owner)}, {"cia", value::number(pbd::t5dr::boundary_pc)},
			{"origin", value::number(m_origin)}, {"through", value::number(m_consumed)}, {"introBoundary", value::number(pbd_source_boundary(boundary))},
			{"introFrame", value::number(m_consumed)}, {"stageId", value::number(stage)}, {"leftCharacterRow", value::number(left)},
			{"rightCharacterRow", value::number(right)}, {"roundsPerGame", value::number(rounds)}});
		if (m_expected_round)
			payload.fields.emplace_back("nativeRound", actual_round->json());
		if (!publish("ready", std::move(payload)))
			return refuse("report_publish_failed");
		return true;
	}

	template <typename Row>
	inline bool local_channel::commit(u32 owner, u32 pc, u32 boundary, const Row& row, const std::array<std::uint16_t, 2>& expected)
	{
		if (!healthy() || !m_reserved || m_frames.empty() || owner != pbd::t5dr::main_ppu_id || pc != 0x12efc || m_frames.front().boundary != pbd_source_boundary(boundary) ||
			m_checked != m_consumed + 1 || !row.entry_seen || !row.completed || !row.before_read || !row.after_read || row.failure ||
			(row.soft && std::string_view(row.soft) != "session_neutral_input"))
			return refuse("completed_row_unverified");
		for (u32 port = 0; port < 2; ++port)
		{
			const auto& sample = row.ports[port];
			if (m_frames.front().applied_verified && m_frames.front().applied[port] != expected[port])
				return refuse("source_applied_input_mismatch");
			if (!sample.injected || !sample.posted || !sample.stored_held_read || !sample.stored_rising_read || sample.injected_r3 != expected[port] ||
				sample.raw != expected[port] || sample.effective != expected[port] || sample.held != sample.stored_held || sample.rising != sample.stored_rising)
				return refuse("two_pad_readback_mismatch");
		}
		// History is published after every real two-port post and state check. Packet receipt never updates it.
		const auto& executed = m_frames.front();
		for (u32 port = 0; port < 2; ++port)
		{
			auto& runs = m_presentation.history[port];
			auto& count = m_presentation.history_count[port];
			if (count && runs[count - 1].held == executed.pads[port] && runs[count - 1].applied == expected[port])
				++runs[count - 1].frames;
			else
			{
				if (count == presentation::history_runs)
				{
					for (u32 i = 1; i < count; ++i)
						runs[i - 1] = runs[i];
					--count;
				}
				runs[count++] = {executed.pads[port], expected[port], 1};
			}
		}
		m_presentation.native_through = m_consumed + 1;
		m_presentation.sync_mask = executed.mask;
		++m_consumed;
		m_reserved = false;
		m_frames.pop_front();
		if (m_presentation.analyze || (m_from_start && m_consumed == 1))
			draw();
		if (m_consumed != m_packet_through)
			return true;
		if (!publish("consumed", value::object({{"proof", value::string("two-pad-post-v1")}, {"owner", value::number(owner)}, {"cia", value::number(pc)},
									 {"chunkSerial", value::number(m_packet_serial)}, {"chunkDigest", value::string(hex(m_packet_digest))}, {"through", value::number(m_consumed)},
									 {"boundaryThrough", value::number(pbd_source_boundary(boundary) + 1)}, {"hashCheckedThrough", value::number(m_checked)}})))
			return refuse("report_publish_failed");
		return true;
	}

	inline bool local_channel::end(u32 boundary, bool native_end, bool drawn)
	{
		if (!healthy() || !native_end || !m_visible || !m_end || *m_end != m_consumed || !m_frames.empty() || m_reserved)
			return refuse("natural_end_unverified");
		m_ended = true;
		m_visible = false;
		if (drawn)
		{
			m_draw_closed = true;
			m_presentation.notice = std::string(draw_notice);
		}
		draw();
		auto payload = value::object({{"proof", value::string("natural-match-end-v1")}, {"through", value::number(m_consumed)},
			{"boundaryThrough", value::number(pbd_source_boundary(boundary))}, {"hashCheckedThrough", value::number(m_checked)}});
		if (drawn)
			payload.fields.emplace_back("outcome", value::string("draw"));
		if (!publish("ended", std::move(payload)))
			return refuse("report_publish_failed");
		return true;
	}

} // namespace pbd::spectator
