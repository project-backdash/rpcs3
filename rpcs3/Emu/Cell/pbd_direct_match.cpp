#include "stdafx.h"
#include "../PBD/pbd_direct_replay_state.h"
#include "../PBD/pbd_replay_rewind.h"

namespace pbd::direct_replay
{
	std::atomic<pbd_direct_match*> active{nullptr};
	std::filesystem::path game_path()
	{
#ifdef _WIN32
		if (const auto path = pbd::env::find_wide("PBD_REPLAY_V2_GAME"); path && *path)
			return path;
		if (const auto path = pbd::env::find_wide("PBD_TEST_REPLAY_V2_DIRECT_GAME"); path && *path)
			return path;
#else
		if (const auto path = pbd::env::find("PBD_REPLAY_V2_GAME"); path && *path)
			return path;
		if (const auto path = pbd::env::find("PBD_TEST_REPLAY_V2_DIRECT_GAME"); path && *path)
			return path;
#endif
		return {};
	}
	std::filesystem::path request_path()
	{
#ifdef _WIN32
		if (const auto path = pbd::env::find_wide("PBD_REPLAY_V2_REQUEST"); path && *path)
			return path;
#else
		if (const auto path = pbd::env::find("PBD_REPLAY_V2_REQUEST"); path && *path)
			return path;
#endif
		return {};
	}
} // namespace pbd::direct_replay

pbd_direct_match::pbd_direct_match() noexcept = default;
pbd_direct_match::~pbd_direct_match()
{
	auto* self = this;
	active.compare_exchange_strong(self, nullptr);
}
bool pbd_direct_match::requested() noexcept
{
	try
	{
		const auto* capsule = pbd::env::find("PBD_SPECTATOR_NATIVE_2");
		const auto* channel = pbd::env::find("PBD_SPECTATOR_CHANNEL");
		return !game_path().empty() || pbd::replay_standby::pbd_requested() ||
		       (capsule && std::string_view(capsule) == "1" && channel && *channel);
	}
	catch (...)
	{
		return false;
	}
}
u32 pbd_direct_match::captured_rounds() const noexcept
{
	return data ? data->captured_round_mask.load(std::memory_order_acquire) : 0;
}
std::optional<u32> pbd_direct_match::pbd_round_frame(u32 round) const noexcept
{
	if (!data || data->pbd_external_inputs || !round || round > data->rounds.size() ||
		!(captured_rounds() & (u32{1} << (round - 1))))
		return std::nullopt;
	const auto& ready = data->rounds[round - 1].ready;
	if (!ready)
		return std::nullopt;
	return pbd::replay_rewind::recorded_frame(ready->input_offset, 0, 0, data->first_battle_input);
}
u32 pbd_direct_match::position() const noexcept
{
	return data ? data->logical_position.load(std::memory_order_acquire) : 0;
}
u32 pbd_direct_match::pbd_recorded_frame(u32 boundary) const noexcept
{
	return data ? pbd::replay_rewind::recorded_frame(boundary, data->anchor, data->input_base, data->first_battle_input) : 0;
}
u32 pbd_direct_match::pbd_recorded_round(u32 boundary) const noexcept
{
	if (!data || data->pbd_external_inputs || data->anchor == ~0u || boundary < data->anchor)
		return 0;
	return data->round_at(static_cast<u64>(data->input_base) + boundary - data->anchor).round;
}
u32 pbd_direct_match::input_count() const noexcept
{
	return data ? static_cast<u32>(data->inputs.size()) : 0;
}
const std::array<std::string, 2>& pbd_direct_match::pbd_player_names() const noexcept
{
	static const std::array<std::string, 2> nobody{};
	return data ? data->player_names : nobody;
}
std::array<u32, 2> pbd_direct_match::pbd_player_ranks() const noexcept
{
	return data ? data->player_ranks : std::array<u32, 2>{};
}
std::optional<pbd::rank_stakes::game_values> pbd_direct_match::pbd_rank_screens() const noexcept
{
	return data ? data->rank_screens : std::nullopt;
}
bool pbd_direct_match::begun() const noexcept
{
	return data && data->redirected;
}
pbd_replay_cursor pbd_direct_match::pbd_capture_cursor() const noexcept
{
	const auto* owner = get_current_cpu_thread<ppu_thread>();
	if (!data || !data->redirected || data->failed || data->anchor == ~0u || !owner || owner->id != pbd::t5dr::main_ppu_id)
		return {};
	const auto& s = *data;
	return {s.generation, s.anchor, s.input_base, s.logical_position.load(std::memory_order_acquire),
		s.continuation.load(std::memory_order_acquire), true, s.battle_seen};
}
bool pbd_direct_match::pbd_can_restore_cursor(const pbd_replay_cursor& cursor) const noexcept
{
	const auto* owner = get_current_cpu_thread<ppu_thread>();
	return data && data->redirected && !data->failed && owner && owner->id == pbd::t5dr::main_ppu_id &&
	       cursor.valid && cursor.generation == data->generation &&
	       cursor.generation == static_cast<u64>(Emu.GetEmulationIdentifier()) && cursor.anchor != ~0u &&
	       cursor.input_base < data->inputs.size() && static_cast<u8>(cursor.continuation) <= 2 &&
	       (cursor.continuation != pbd_replay_continuation::recorded || cursor.position <= data->inputs.size());
}
bool pbd_direct_match::pbd_restore_cursor(const pbd_replay_cursor& cursor) noexcept
{
	if (!pbd_can_restore_cursor(cursor))
		return false;
	auto& s = *data;
	s.anchor = cursor.anchor;
	s.input_base = cursor.input_base;
	s.logical_position.store(cursor.position, std::memory_order_release);
	s.continuation.store(cursor.continuation, std::memory_order_release);
	s.battle_seen = cursor.battle_seen;
	return true;
}
bool pbd_direct_match::request_round(u64 generation, u32 round, u32 request_id,
	pbd_replay_continuation continuation) noexcept
{
	try
	{
		if (!data || generation != data->generation || static_cast<u8>(continuation) > 2 ||
			!round || round > data->rounds.size() ||
			!(captured_rounds() & (u32{1} << (round - 1))))
			return false;
		auto prior = std::atomic_load(&data->round_reply);
		if (prior && (prior->state == pbd_replay_round_result::status::queued ||
						 prior->state == pbd_replay_round_result::status::refused))
			return false;
		const u32 previous = prior ? prior->request_id : 0;
		if (previous == ~0u || (request_id && request_id <= previous))
			return false;
		auto reply = std::make_shared<pbd_replay_round_result>();
		reply->state = pbd_replay_round_result::status::queued;
		reply->generation = generation;
		reply->round = round;
		reply->request_id = request_id ? request_id : previous + 1;
		reply->continuation = continuation;
		if (!std::atomic_compare_exchange_strong(&data->round_reply, &prior,
				std::shared_ptr<const pbd_replay_round_result>{std::move(reply)}))
			return false;
		data->requested_round.store(round, std::memory_order_release);
		return true;
	}
	catch (...)
	{
		return false;
	}
}
bool pbd_direct_match::set_continuation(u64 generation, pbd_replay_continuation continuation) noexcept
{
	if (!data || generation != data->generation || static_cast<u8>(continuation) > 2)
		return false;
	const auto reply = std::atomic_load(&data->round_reply);
	if (reply && (reply->state == pbd_replay_round_result::status::queued ||
					 reply->state == pbd_replay_round_result::status::refused))
		return false;
	if (continuation == pbd_replay_continuation::recorded &&
		data->continuation.load(std::memory_order_acquire) != continuation)
		return false;
	data->continuation.store(continuation, std::memory_order_release);
	return true;
}
void pbd_direct_match::stop_round_request() noexcept
{
	if (!data)
		return;
	try
	{
		data->finish_round(pbd_replay_round_result::status::unavailable, "native_round_runtime_finished");
		data->requested_round.store(0);
	}
	catch (...)
	{
	}
}
