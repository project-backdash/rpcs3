#pragma once

#include "util/types.hpp"
#include "../PBD/pbd_practice_wakeup.h"
#include "../PBD/pbd_practice_throw_escape.h"
#include <array>
#include <span>
#include <memory>

class ppu_thread;
namespace pbd::practice::frames
{
	struct result;
	struct event;
	struct calculated;
} // namespace pbd::practice::frames
namespace pbd::practice::reference
{
	struct result;
}
namespace pbd::practice
{
	struct feedback;
}

namespace pbd::practice::native
{
	using wakeup_view = pbd::practice::pbd_wakeup_observation;
	std::optional<wakeup_view> read_wakeup() noexcept;
	// The dummy while it is held in a throw whose escape window is open (pbd::practice::throw_escape_view); nothing otherwise.
	std::optional<pbd::practice::throw_escape_view> read_throw_escape() noexcept;
	// Called at every Practice boundary with the Dummy Action the player chose for the dummy (umax when the player drives
	// the dummy or the choice cannot be read): removes the dummy's guard while it gets up from a knockdown if that action
	// never guards (pbd::practice::pbd_getup_guard).
	void getup_guard(u32 chosen_action) noexcept;
	// option_restore writes all five menu options at once (values packed by pbd::practice::pack_game_options).
	enum class operation : u8
	{
		sound,
		option_commit,
		command_list,
		character_select,
		option_restore,
		stage_load
	};
	enum class option : u8
	{
		action,
		counter,
		command_capture,
		attack_data,
		hit_analysis
	};
	enum class sound : u32
	{
		cursor = 2,
		cancel = 3,
		confirm = 6
	};
	struct request
	{
		operation kind = operation::sound;
		option choice = option::action;
		u32 value = 0;
		u64 generation = 0;
		u32 boundary = 0;
	};
	struct owner_view
	{
		u64 generation = 0;
		u32 practice = 0, initiating = 0, opponent = 0, port = 0;
		bool owned = false, pause_acknowledged = false;
	};
	struct result
	{
		u32 entry = 0;
		bool accepted = false, preserved = false, readback = false;
		bool load_started = false;
	};
	bool in_native_call() noexcept;
	bool boundary_ready(const ppu_thread& ppu) noexcept;
	bool boundary_readable(const ppu_thread& ppu) noexcept;
	bool start_models_match(const std::array<u32, 2>& rows) noexcept;
	owner_view inspect(bool owned) noexcept;
	result execute(ppu_thread& ppu, const request& command, const owner_view& owner) noexcept;
	void frame_event(ppu_thread& ppu, u32 address) noexcept;
	void frame_boundary(u32 counter, bool valid) noexcept;
	void frame_reset() noexcept;
	const std::array<pbd::practice::frames::result, 2>& frame_results() noexcept;
	// Each side's latest attack and its advantage from move data at contact (pbd::practice::frames::calculator).
	const std::array<pbd::practice::frames::calculated, 2>& frame_calculated() noexcept;
	const std::array<pbd::practice::reference::result, 2>& frame_references() noexcept;
	std::span<const pbd::practice::frames::event> frame_events() noexcept;
	void frame_events_clear() noexcept;
	struct frame_state;
	std::shared_ptr<const frame_state> frame_save() noexcept;
	void frame_restore(const std::shared_ptr<const frame_state>& saved) noexcept;
	void frame_feedback(std::array<pbd::practice::feedback, 2>& out) noexcept;
} // namespace pbd::practice::native

// Specific stock setters for match resources and local Practice loads.
namespace pbd::replay_native
{
	// Open immediately after service_owner succeeds, before any guest write; defer new scheduler work until the hook returns.
	// Nested scopes restore the previous owner without servicing between guest calls.
	class owner_guard
	{
		const ppu_thread* m_previous;

	public:
		explicit owner_guard(const ppu_thread& owner) noexcept;
		~owner_guard() noexcept;
		owner_guard(const owner_guard&) = delete;
		owner_guard& operator=(const owner_guard&) = delete;
	};
	bool service_owner(ppu_thread& owner, u64 generation, const char** refusal = nullptr) noexcept;
	struct choices
	{
		u32 left = 0, right = 0, stage = 0;
	};
	enum class resource_mode : u32
	{
		versus = 4,
		practice = 10
	};
	bool apply_handicap_options(ppu_thread& owner, u64 generation, u32 left_percent, u32 right_percent) noexcept;
	bool can_prepare_resources(ppu_thread& owner, u64 generation, const choices& selected, const char** refusal = nullptr,
		resource_mode mode = resource_mode::versus) noexcept;
	bool prepare_resources(ppu_thread& owner, u64 generation, const choices& selected, const char** refusal = nullptr,
		resource_mode mode = resource_mode::versus) noexcept;
	bool can_continue_resource_handoff(ppu_thread& owner, u64 generation, const choices& selected) noexcept;
	bool admit_match_initializer(ppu_thread& owner, u64 generation, const choices& selected, const char** refusal = nullptr,
		u32* scheduler_flags = nullptr, bool* scheduler_preemption = nullptr) noexcept;
	bool can_initialize_round(ppu_thread& owner, u64 generation, const char** refusal = nullptr) noexcept;
	bool round_spawn_available(ppu_thread& owner, u64 generation, u32 selector) noexcept;
	bool can_prepare_round_styles(ppu_thread& owner, u64 generation, u32 ordinal) noexcept;
	bool prepare_round_styles(ppu_thread& owner, u64 generation, u32 ordinal) noexcept;
	bool initialize_round(ppu_thread& owner, u64 generation, u32 kind) noexcept;
} // namespace pbd::replay_native

namespace pbd::fighter_side
{
	struct screen_sides
	{
		std::array<s32, 2> projected_x{};
		std::array<bool, 2> on_right{}; // Indexed by fighter/player port, not by screen position.
	};
	// Read on the owner frame boundary, then retain the sample alongside that frame's inputs.
	// DR's projection already includes its current camera and fighter transform. Ties/loading return false.
	bool read(const ppu_thread& owner, u64 generation, screen_sides& out) noexcept;
	// The side DR itself converts each fighter's pad directions with (see read_input): true reads Left as forward, false
	// reads Right as forward. Indexed by fighter/player port.
	struct input_sides
	{
		std::array<bool, 2> on_right{};
	};
	// Read at a frame boundary in a fight: the sides the next frame converts this boundary's words with. Plain memory reads
	// (no camera or render state), so it answers on every fight boundary; false outside a fight or on unverified code.
	bool read_input(u64 generation, input_sides& out) noexcept;
} // namespace pbd::fighter_side
