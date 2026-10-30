#pragma once

#include "Emu/PBD/pbd_checkpoint_compatibility.h"

#include "../Cell/pbd_set_channel.h"
#include "../Cell/pbd_match_recording.h"
#include "pbd_fight_timing.h"
#include "pbd_game_overlay_model.h"
#include <array>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <utility>

// A lobby outcome consumes the same verified native phase law as a replay. Observations remain replaceable until the
// correction frontier confirms them; host timing, a disconnect and a speculative knockout cannot become a result.
namespace pbd::lobby_set
{
	using namespace pbd::set_channel;
	// show_post_set follows the service's agreement of both terminal results. Local completion alone proves no peer receipt.
	inline bool terminal_results_acknowledged(bool complete, bool terminal, command_kind applied) noexcept
	{
		return complete && terminal && applied == command_kind::show_post_set;
	}
	using u32 = std::uint32_t;
	// Short batches bound publication latency even when a source runs slowly, without changing its confirmation frontier.
	inline constexpr u32 confirmed_chunk_frames = 15;
	// Append the hook's actual policy to the existing full-width options agreement; no new peer message or mask.
	inline constexpr u32 start_policy_digest(u32 options, bool keyed_draws, bool ban_jinpachi = false)
	{
		const auto stock = (options ^ (keyed_draws ? 1u : 0u)) * 0x01000193u;
		return ban_jinpachi ? (stock ^ 0x4a494e50u) * 0x01000193u : stock;
	}
	inline constexpr const char* start_policy_refusal = "The other player uses a different game setting (keyed draws or match options).";
	// Both life bars start full in every lobby game: the applied handicap percents (left, right) must be this.
	inline constexpr u32 full_handicap = 100;
	struct context
	{
		u32 stage = 0, left_row = 0, right_row = 0, rounds = 0;
		bool verified = false;
		u32 pending_phase = 0, round_ordinal = 0;
		std::array<std::uint16_t, 2> round_random{};
		bool round_verified = false;
		// The applied handicap percents, left and right, read with the picks (verified covers them).
		std::array<u32, 2> handicap{};
		u32 time_index = 0;
	};
	struct clean_start
	{
		u32 seed = 0;
		std::uint64_t generation = 0;
		std::string recipe_sha256, title_id;
		bool verified = false, selection_admitted = false;
		std::string checkpoint_sha256;
		u32 counter = 0;
		bool natural = false;
		bool start_skip = false, exit_gate = false, phase_delay = false, keyed_draws = false;
		bool ban_jinpachi = false, direct_start = false;
		u32 battle_clock_version = 0, menu_hash_mask = 0;
		bool require_round_state = false;
		bool publish_from_start = false;
	};
	struct input_records
	{
		std::array<std::uint16_t, 2> raw{}, applied{};
		bool verified = false;
	};
	// Byte-exact peer sync facts, sampled on the owner at the input boundary and replaced by corrected execution.
	struct sync_facts
	{
		u32 boundary = 0, segment = 0, counter = 0, scene = 0, mask = 0, gate_hold = 0;
		std::array<std::uint64_t, 3> combat{};
		bool verified = false;
	};
	inline std::array<std::uint8_t, 48> sync_record(const sync_facts& facts)
	{
		std::array<std::uint8_t, 48> out{};
		const auto word = [&](std::size_t offset, std::uint64_t value, std::size_t bytes)
		{
			for (std::size_t i = 0; i < bytes; ++i)
				out[offset + i] = static_cast<std::uint8_t>(value >> (8 * i));
		};
		word(0, facts.boundary, 4);
		word(4, facts.segment, 4);
		word(8, facts.counter, 4);
		word(12, facts.scene, 4);
		word(16, facts.mask, 4);
		word(20, facts.gate_hold, 4);
		for (std::size_t i = 0; i < 3; ++i)
			word(24 + 8 * i, facts.combat[i], 8);
		return out;
	}
	// The end is an exclusive native update boundary, while final_frame counts only executed digital samples.
	// Successful publication of the final event tail is distinct from lifecycle completion inside observe().
	struct sealed_game_result
	{
		u32 game_number = 0, origin = 0, end_boundary = 0, confirmed_through = 0, final_frame = 0;
		u32 round_left = 0, round_right = 0;
		score games;
		pbd::match_recording::winner outcome = pbd::match_recording::winner::none;
		std::uint64_t generation = 0, published_event = 0;
		bool terminal = false, spectator_stream_enabled = false;
	};
	// Confirmed input buffered before a game's game_started, in frames. An in-game selection puts the character select (30 s),
	// the handicap, the stage select (15 s) and the VS load before the first round intro of every game; the countdowns bound
	// that time, and this bound only catches a selection that never ends.
	inline constexpr std::size_t presentation_frames = 9000;
	// A fixed set keeps the automatic pre-selection's bound (its own selection times out after 3,600 samples).
	inline constexpr std::size_t fixed_presentation_frames = 4096;
	// Around the two countdowns: the previous game's exit, the zero presses' cadence, the VS load and the character intro.
	inline constexpr std::size_t presentation_margin_frames = 3600;
	// The bound for one set. The manifest admits countdowns up to 99 s each (11,880 frames together), so an in-game set whose
	// countdowns need more than presentation_frames gets their length plus the margin; the default 30 s and 15 s keep 9000.
	inline std::size_t presentation_limit(const manifest& m)
	{
		if (!in_game(m))
			return fixed_presentation_frames;
		const std::size_t countdowns = (std::size_t{m.selection.character_seconds} + m.selection.stage_seconds) * 60;
		return std::max(presentation_frames, countdowns + presentation_margin_frames);
	}
	inline std::array<std::uint8_t, 8> pad_record(std::uint16_t buttons)
	{
		return {static_cast<std::uint8_t>(buttons), static_cast<std::uint8_t>(buttons >> 8), 0, 0, 0, 0, 0, 0};
	}
	inline std::string base64url(const std::vector<std::uint8_t>& bytes)
	{
		constexpr char alphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
		std::string text;
		std::uint32_t bits = 0;
		unsigned count = 0;
		for (const auto byte : bytes)
		{
			bits = (bits << 8) | byte;
			count += 8;
			while (count >= 6)
			{
				count -= 6;
				text += alphabet[(bits >> count) & 63];
			}
		}
		if (count)
			text += alphabet[(bits << (6 - count)) & 63];
		return text;
	}
	inline bool menu_applied(const command& value, std::uint64_t before, const pbd::game_overlay::snapshot& panel)
	{
		using pbd::game_overlay::panel_kind;
		if (panel.generation <= before || !value.serial || panel.command_serial != value.serial)
			return false;
		if (value.kind == command_kind::end_set)
			return !panel.visible && panel.kind == panel_kind::set_ended;
		if (!panel.visible || panel.close_on_action)
			return false;
		return (value.kind == command_kind::show_post_set && panel.kind == panel_kind::post_set) ||
		       (value.kind == command_kind::choice_rejected && panel.kind == panel_kind::choice_rejected);
	}
	// The launcher writes a show_post_set, choice_rejected or end_set command file and then the panel that carries it. The game
	// reads menu.pbd at every poll, but command files reach it through the channel's reader thread, which reads each on its own
	// 8 ms poll, so the panel can be read first; waiting for a newer one would then wait forever. A panel names its command's
	// serial, and serials only grow within one game process (a next set keeps the channel's cursors), so the panel already shown
	// when the command is taken applies it just as a newer one would. Otherwise the command waits in `pending` for a panel newer
	// than `shown` (menu_panel_applied). Returns the command to apply now.
	inline std::optional<command> take_menu_command(command value, const pbd::game_overlay::snapshot& shown, std::optional<command>& pending,
		std::uint64_t& before)
	{
		if (menu_applied(value, 0, shown))
			return value;
		before = shown.generation;
		pending = std::move(value);
		return std::nullopt;
	}
	// A newly applied panel: the pending command it applies, taken out of `pending`. Nothing waits once it has applied, so the
	// same panel written again is harmless.
	inline std::optional<command> menu_panel_applied(const pbd::game_overlay::snapshot& panel, std::optional<command>& pending,
		std::uint64_t before)
	{
		if (!pending || !menu_applied(*pending, before, panel))
			return std::nullopt;
		return std::exchange(pending, std::nullopt);
	}
	// While this PC waits at the session start for the other one, its window is still hidden and no panel is drawn, so the
	// lobby's ending cannot wait for the set_ended panel menu_applied needs: it ends the session there at once. The other
	// player left or crashed while loading, or the service was lost, and the game must not stay hidden until its start
	// timeout. Display and stream commands take effect as they are read, and every other command needs a result first.
	inline bool ends_session_start(const command& value)
	{
		return value.kind == command_kind::end_set;
	}
	// A held boundary may apply display/stream commands and the authoritative ending, in serial order.
	// Menus and continuations retain their normal boundary requirements and must not be skipped to reach an ending.
	template <typename Apply, typename End, typename Fail>
	bool poll_wait_commands(channel& source, std::optional<command>& pending, Apply&& apply, End&& end, Fail&& fail)
	{
		if (pending)
		{
			if (!ends_session_start(*pending))
				return false;
			end(*std::exchange(pending, std::nullopt));
			return true;
		}
		for (;;)
		{
			auto next = source.next_command(source.identity().game_number);
			if (!next.value)
			{
				if (next.error == "unavailable")
					return false;
				fail(next.error);
				return true;
			}
			const auto& value = *next.value;
			if (ends_session_start(value))
			{
				end(value);
				return true;
			}
			if (value.kind != command_kind::chat_overlay && value.kind != command_kind::vs_cards &&
				value.kind != command_kind::rank_stakes && value.kind != command_kind::stream_status)
				return false;
			if (!apply(value))
				return true;
		}
	}

	// A deliberate quit gives up the seat only if no earlier end or lost peer forced it.
	// Use the clock and peer status at that first request; publish after the channel's owner has drained.
	inline constexpr std::uint64_t leave_peer_silence_us = 1000000;
	struct end_facts
	{
		bool user_stop = false, window_close = false, still_collecting = false;
		bool peer_watched = false, peer_ended = false;
		// The other PC's last valid datagram and the time of this end, in microseconds of the same clock.
		std::uint64_t peer_heard_us = 0, now_us = 0;
	};
	inline bool peer_still_sending(const end_facts& facts)
	{
		if (!facts.peer_watched)
			return true;
		return !facts.peer_ended && facts.peer_heard_us &&
		       (facts.peer_heard_us >= facts.now_us || facts.now_us - facts.peer_heard_us < leave_peer_silence_us);
	}
	inline bool leave_on_end(const end_facts& facts)
	{
		return (facts.user_stop || facts.window_close) && facts.still_collecting && peer_still_sending(facts);
	}

} // namespace pbd::lobby_set
