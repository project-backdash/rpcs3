#pragma once
#include <array>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include "util/types.hpp"
#include "pbd_game_profile.h"
#include "../PBD/pbd_custom_wire.h"
#include "../PBD/pbd_throw_break.h"

class ppu_thread;
namespace pbd::lobby_set
{
	struct sync_facts;
}

// Live semantic records, never a memory archive. Final packaging consumes these records, not an earlier game's trace.
class pbd_live_replay
{
	struct state;
	std::unique_ptr<state> data;

public:
	struct player
	{
		std::string id, name;
	};
	// The PLAYER DATA numbers the bars show now (left, right); none for a side without a record.
	using names_source = std::function<std::array<std::string, 2>()>;
	using records_source = std::function<std::array<std::optional<pbd::game_profile::record>, 2>()>;
	using looks_source = std::function<pbd::custom_wire::pair()>;
	// The throw break rule's setting from the owner's tick on, at a game start (false) or a round start (true)
	// (rollback_session::throw_break_round_start); nothing before the delay is applied, or at a round start while the rule
	// still tracks a press it moved.
	using throw_break_source = std::function<std::optional<pbd::throw_break::lookahead_setting>(bool)>;
	pbd_live_replay() noexcept;
	~pbd_live_replay();
	static bool requested() noexcept;
	bool enabled() const noexcept;
	// The canonical game_start captured at the initializer, shared immutably with the local set channel.
	std::shared_ptr<const std::string> pbd_game_start_record(u64 generation) const noexcept;
	// Where each game's start takes the PLAYER DATA numbers it records, asked on the owner thread at the initializer. Without one,
	// a start records none.
	void pbd_names(names_source source) noexcept;
	void pbd_records(records_source source) noexcept;
	void pbd_looks(looks_source source) noexcept;
	// Where each game start and round start takes the rule's setting it records, asked on the owner thread at the initializer.
	// Without one, they record none.
	void pbd_throw_break(throw_break_source source) noexcept;
	bool configure(u64 generation, const std::filesystem::path& run, bool session, bool round_lifecycle,
		bool playback_or_practice, u32 seed, std::array<player, 2> players, bool checkpoint_source = false) noexcept;
	// Called only from the latch's confirmed prefix. Non-executed alignment gaps do not become inputs.
	bool confirmed(u32 boundary, u32 segment, u32 counter, std::array<u16, 2> input,
		u32 scene, bool executed, const pbd::lobby_set::sync_facts& sync) noexcept;
	void observe_boundary(u32 boundary, u32 scene) noexcept;
	void close_incomplete(const char* reason) noexcept;
	void drain() const noexcept;
	static bool function_event(ppu_thread&, u32 address) noexcept;
};
