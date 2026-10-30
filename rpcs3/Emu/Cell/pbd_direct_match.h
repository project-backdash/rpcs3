#pragma once
#include "util/types.hpp"
#include "../PBD/pbd_rank_stakes.h"
#include <array>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

class ppu_thread;
enum class pbd_replay_continuation : u8
{
	recorded,
	take_left,
	take_right
};
struct pbd_replay_cursor
{
	u64 generation = 0;
	u32 anchor = ~0u, input_base = 0, position = 0;
	pbd_replay_continuation continuation = pbd_replay_continuation::recorded;
	bool valid = false, battle_seen = false;
};
struct pbd_replay_round_result
{
	// Reconstructed acknowledges native initialization and the first resumed sync check; picture/sound readiness is separate.
	enum class status : u8
	{
		none,
		queued,
		reconstructed,
		unavailable,
		refused
	};
	status state = status::none;
	u64 generation = 0, native_call_ns = 0;
	u32 request_id = 0, round = 0, native_round = 0, input_offset = 0;
	std::array<u16, 2> score{};
	pbd_replay_continuation continuation = pbd_replay_continuation::recorded;
	const char* dependency = ""; // Engine-owned literal, valid for this binary's lifetime.
};
// One independently recorded game, entered through the stock match loader without menu input.
class pbd_direct_match
{
	struct state;
	std::unique_ptr<state> data;
	void poll_round_request_file() noexcept;

public:
	pbd_direct_match() noexcept;
	~pbd_direct_match();
	static bool requested() noexcept;
	bool enabled() const noexcept
	{
		return !!data;
	}
	bool configure(u64 generation, bool isolated, u32 seed) noexcept;
	// An isolated live watcher uses the same stock initializer, then takes its inputs from the spectator channel.
	bool pbd_configure_capsule_start(u64 generation, bool isolated, u32 seed, u32 source_game, std::string_view capsule) noexcept;
	bool pbd_live_inputs() const noexcept;
	u32 pbd_start_boundary() const noexcept;
	bool pbd_configure_file(u64 generation, bool isolated, u32 seed, const std::filesystem::path& file,
		std::string_view expected_sha256) noexcept;
	bool start(ppu_thread& owner, u32 boundary) noexcept;
	// Requests are consumed at the next owned frame boundary. Callers keep this runtime alive while using its engine.
	u32 captured_rounds() const noexcept;
	std::optional<u32> pbd_round_frame(u32 round) const noexcept;
	bool request_round(u64 generation, u32 round, u32 request_id = 0,
		pbd_replay_continuation continuation = pbd_replay_continuation::recorded) noexcept;
	bool set_continuation(u64 generation, pbd_replay_continuation continuation) noexcept;
	bool verify_sync(u32 boundary, u32 segment, u32 scene, u32 counter, u32 mask,
		const std::array<u64, 3>& combat, bool verified) noexcept;
	void stop_round_request() noexcept;
	// The owner-local flag reports a reconstruction in this call, independently of asynchronous result readers.
	bool round_boundary(ppu_thread& owner, u32 boundary, bool* reconstructed = nullptr) noexcept;
	u32 position() const noexcept;
	u32 pbd_recorded_frame(u32 boundary) const noexcept;
	u32 pbd_recorded_round(u32 boundary) const noexcept;
	// The recorded game's input count: position() reaches it when the recording runs out.
	u32 input_count() const noexcept;
	// The players' names the recorded file saved (pbd::replay_game_file::player_names), left and right, which the replayed game
	// draws under the life bars; empty without a file, and for a live watcher's start, whose names come from the launcher.
	const std::array<std::string, 2>& pbd_player_names() const noexcept;
	std::array<u32, 2> pbd_player_ranks() const noexcept;
	// The values the recorded game's own Ranked screens were written with (pbd::rank_stakes::game_values), from its start;
	// none without a file or when the recording names none.
	std::optional<pbd::rank_stakes::game_values> pbd_rank_screens() const noexcept;
	// The stock match initializer has entered the recorded game (direct_match_requested, where the launcher shows the
	// game). Read on the owner thread, which sets it in function_event.
	bool begun() const noexcept;
	// Owner-thread host state kept alongside an in-memory guest checkpoint; never serialized to a replay file.
	pbd_replay_cursor pbd_capture_cursor() const noexcept;
	bool pbd_can_restore_cursor(const pbd_replay_cursor& cursor) const noexcept;
	bool pbd_restore_cursor(const pbd_replay_cursor& cursor) noexcept;
	static bool function_event(ppu_thread& owner, u32 address) noexcept;
	enum class sample_result
	{
		waiting,
		input,
		ended,
		refused
	};
	sample_result sample(u32 boundary, u32 scene, u32 counter, std::array<u16, 2>& input, u32& segment) noexcept;
};
