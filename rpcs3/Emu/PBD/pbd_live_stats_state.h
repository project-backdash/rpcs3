#pragma once

// The netcode stats block behind the connection line (PBD_NET_OVERLAY=1, off by default).
//
// One block of relaxed atomics shared by the update latch (writer) and the RSX overlay (reader), on the pattern of
// Emu/RSX/pbd_replay_pacing.h. The latch's owner PPU writes it through `publisher` (finish() may store the end reason
// from the thread-state thread); the RSX thread reads it in rsx::overlays::pbd_net_overlay. Every shown number is ONE
// atomic the writer computed (averages are published as values, never divided by the
// reader), so a torn multi-field read can only put fields of adjacent events side by side for one refresh. Nothing here names
// guest memory (no vm:: API), the overlay never writes the block and never calls the latch, and with the switch off the latch
// writes nothing at all (storage::live_stats stays false). Std-only, so a standalone test compiles it without RPCS3.
#include "../PBD/pbd_env.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <string>
#include <string_view>

namespace pbd::live_stats
{
	// Text refresh period of the overlay: four refreshes a second.
	inline constexpr std::uint64_t refresh_interval_us = 250000;

	enum boundary_mode : std::uint32_t
	{
		mode_idle = 0,     // no latch configured in this process (ordinary play)
		mode_straight = 1, // a latch without rolling corrections or a peer (tape replay, one-shot correction tests)
		mode_rolling = 2,  // single-instance rolling corrections (--tick-roll-*)
		mode_peer = 3,     // two-peer fight fixture
		mode_session = 4,  // netplay session (menus and matches) with a peer
		mode_practice = 5, // local Practice; the net line stays hidden but the shared leave prompt may show
	};

	struct alignas(64) block
	{
		std::atomic<std::uint32_t> generation{0}; // latch configures seen; 0 = no latch in this process
		std::atomic<std::uint32_t> mode{mode_idle}, role{0}, session_tag{0};
		std::atomic<std::uint32_t> boundary{0};       // next_index after the newest completed live row
		std::atomic<std::uint32_t> input_delay{0};    // D: configured or negotiated input delay (rolling: roll_delay)
		std::atomic<std::uint32_t> delay_now{0};      // delay of the newest assign row + 1 (0 = no assign row yet)
		std::atomic<std::uint32_t> max_prediction{0}; // prediction bound (PBD_PEER_MAX_PREDICTION)
		std::atomic<std::uint32_t> window{0};         // rollback window (peer_delay; rolling: roll_delay)
		std::atomic<std::uint32_t> rtt_last_us{0}, rtt_avg_us{0}, rtt_jitter_us{0}, rtt_samples{0};
		std::atomic<std::int32_t> adv_milli{0}, error_milli{0};
		std::atomic<std::uint32_t> adv_valid{0};  // bit 0: advantage estimate valid; bit 1: halved-difference error valid
		std::atomic<std::uint32_t> predicting{0}; // ticks this boundary predicts (ahead0 of the hold loop)
		std::atomic<std::uint32_t> corrections{0}, depth_last{0}, depth_max{0};
		std::atomic<std::uint32_t> restore_last_us{0}, restore_avg_us{0}, restore_max_us{0};
		std::atomic<std::uint32_t> tick_last_us{0}, tick_avg_us{0}, tick_max_us{0}, ticks_replayed{0};
		std::atomic<std::uint32_t> correction_last_us{0}, correction_avg_us{0}, correction_max_us{0};
		std::atomic<std::uint32_t> hash_checks{0};
		std::atomic<std::int32_t> hash_tick{-1}, mismatch_tick{-1};
		// Net overlay (PBD_NET_OVERLAY): 1 while the newest boundary is inside a match (session: battle scene; fight
		// fixtures: always), the largest rollback depth of the last ~2-4 s, and the share (per mille, ~5 s windows) of the other
		// peer's boundaries whose first datagram never arrived.
		std::atomic<std::uint32_t> in_match{0}, depth_recent{0}, loss_permille{0};
		// 1 when the session's datagrams go through the relay (PBD_PEER_ROUTE=relay): the connection line shows a Relay tag.
		std::atomic<std::uint32_t> relayed{0};
		// Leaving a session in game: 1 while this PC's player holds Select (the game never sees it in a session), and how long
		// Start + Select have been held together (the hold that ends the session), in milliseconds.
		std::atomic<std::uint32_t> leave_select{0}, leave_hold_ms{0};
		// Replay controls (a session replay with PBD_REPLAY_CONTROLS=1): 0 = not a controlled replay, 1 = playing, 2 = paused; the
		// speed step plus 2 (0..4: 1/4, 1/2, 1, 2 and 4 times); the side this PC's controller plays (0 = none, 1 = P1, 2 = P2); the
		// match number and the frame within it (0 outside a match).
		std::atomic<std::uint32_t> replay_state{0}, replay_speed{2}, replay_control{0}, replay_match{0}, replay_frame{0};
		// While a round is being fought: its number in the match and the frames since its "Fight!" (0 = between rounds).
		std::atomic<std::uint32_t> replay_round{0}, replay_round_frame{0};
		// Replay takeover: on, its state (0 watching, 1 armed, 2 in control, 3 end prompt, 4 enable prompt), the side,
		// the frames left and the prompt's choice (0 Try again/Yes, 1 Return to replay/No).
		std::atomic<std::uint32_t> takeover_on{0}, takeover_state{0}, takeover_port{0}, takeover_frames_left{0}, takeover_choice{0};
		// Local Practice dummy: 0 ready, 1 recording, 2 accept prompt, 3 playback, 4 settings. Slot is zero-based; interval is
		// 0 short, 1 normal, 2 slow, 3 shuffle. The emulator owns the recordings; this block carries display state only.
		std::atomic<std::uint32_t> practice_dummy_on{0}, practice_dummy_state{0}, practice_dummy_slot{0}, practice_dummy_frames{0};
		std::atomic<std::uint32_t> practice_dummy_choice{0}, practice_dummy_row{0}, practice_dummy_frequency{1}, practice_dummy_interval{1};
		std::atomic<std::uint32_t> practice_dummy_slots{0};
		std::atomic<const char*> end_reason{nullptr}; // reason_text literal once the latch finished
		std::atomic<const char*> guard{nullptr};      // the failed guard's literal (fail())
	};
	static_assert(std::atomic<std::uint32_t>::is_always_lock_free && std::atomic<std::int32_t>::is_always_lock_free &&
					  std::atomic<const char*>::is_always_lock_free,
		"the stats block must be lock-free");
	static_assert(alignof(block) == 64, "the stats block shares no cache line with the pacing atomics");

	inline block stats;

} // namespace pbd::live_stats
