#include "stdafx.h"
#include "pbd_direct_replay_state.h"

void pbd_direct_match::poll_round_request_file() noexcept
{
	auto& s = *data;
	if (!s.request_file.empty())
	{
		try
		{
			std::ifstream file(s.request_file, std::ios::binary);
			std::array<char, 1025> bytes{};
			file.read(bytes.data(), bytes.size());
			const auto size = file.gcount();
			if (size > 0 && size <= 1024)
			{
				auto parsed = pbd::set_channel::detail::json_parser(
					std::string_view(bytes.data(), static_cast<usz>(size)), 64)
				                  .parse();
				if (parsed.value)
				{
					const auto& value = *parsed.value;
					const u32 id = integer(value, "requestId");
					if (id && id > s.file_request_seen &&
						pbd::set_channel::detail::is(value.find("protocol"), "pbd-replay-v2-loader-draft-1"))
					{
						const auto& generation = field(value, "runtimeGeneration");
						if (generation.kind != kind::number)
							throw std::invalid_argument("request_generation");
						u64 token = 0;
						const auto number = std::from_chars(generation.text.data(), generation.text.data() + generation.text.size(), token);
						const auto& target = field(value, "target");
						const u32 game = integer(target, "game"), round = integer(target, "round", 32);
						const auto& mode = field(value, "continuation");
						const auto continuation = mode.text == "take-left"  ? pbd_replay_continuation::take_left :
						                          mode.text == "take-right" ? pbd_replay_continuation::take_right :
						                                                      pbd_replay_continuation::recorded;
						if (number.ec == std::errc{} && number.ptr == generation.text.data() + generation.text.size() &&
							mode.kind == kind::string && (mode.text == "recorded" || mode.text == "take-left" || mode.text == "take-right"))
						{
							s.file_request_seen = id;
							const bool accepted = game == s.game && request_round(token, round, id, continuation);
							const auto prior = std::atomic_load(&s.round_reply);
							const char* dependency = accepted ? "" : token != s.generation                                                          ? "native_round_generation" :
							                                     game != s.game                                                                     ? "native_round_game" :
							                                     !round                                                                             ? "native_round_number" :
							                                     !(s.captured_round_mask.load(std::memory_order_acquire) & (u32{1} << (round - 1))) ? "native_round_not_captured" :
							                                     prior && prior->state == pbd_replay_round_result::status::refused                  ? prior->dependency :
							                                     prior && id <= prior->request_id                                                   ? "native_round_request_id" :
							                                                                                                                          "native_round_request_busy";
							s.event(accepted ? "round_request_queued" : "round_request_not_queued", dependency,
								{{"requestId", json::number(id)}, {"round", json::number(round)}});
						}
					}
				}
			}
		}
		catch (...)
		{
		} // A partial/malformed local request never mutates the running game.
	}
}

const char* pbd_direct_match::state::round_context(ppu_thread& owner, const saved_round& target, u32 round)
{
	const char* context_refusal = nullptr;
	if (!target.ready || !target.ready->rng)
		context_refusal = "native_round_ready_context_not_captured";
	else if (!target.ready->carry)
		context_refusal = "native_round_retained_values_not_recorded";
	else if (!target.ready->pbd_entry_counter)
		context_refusal = "native_round_entry_counter_not_recorded";
	else if (!target.ready->rng->style_readback())
		context_refusal = "native_round_ready_style_readback";
	else if (round > 1 && (!target.cleanup || !target.cleanup->rng))
		context_refusal = "native_round_cleanup_context_not_captured";
	else if (round > 1 && !target.cleanup->carry)
		context_refusal = "native_round_retained_values_not_recorded";
	else if (round > 1 && !target.cleanup->rng->style_readback())
		context_refusal = "native_round_cleanup_style_readback";
	else if (!pbd::replay_native::service_owner(owner, generation, &context_refusal) && !context_refusal)
		context_refusal = "native_round_scheduler";
	return context_refusal;
}

bool pbd_direct_match::state::round_resources(ppu_thread& owner, const saved_round& target, u32 round, const char*& context_refusal)
{
	if (!pbd::replay_native::can_initialize_round(owner, generation, &context_refusal) && !context_refusal)
		context_refusal = "native_round_context";
	if (context_refusal)
	{
		finish_round(pbd_replay_round_result::status::unavailable, context_refusal);
		event("round_request_unavailable", context_refusal,
			{{"schedulerFlags", json::number(static_cast<u32>(owner.state.load()))},
				{"schedulerPreemption", json::boolean(owner.ack_suspend)},
				{"pc", json::number(owner.cia)}, {"caller", json::number(owner.lr)},
				{"scene", json::number(word(pbd::t5dr::scene_address))}});
		return false;
	}
	if (!pbd::replay_native::round_spawn_available(owner, generation, target.ready->spawn_selector) ||
		(round > 1 && !pbd::replay_native::round_spawn_available(owner, generation, target.cleanup->spawn_selector)))
	{
		finish_round(pbd_replay_round_result::status::unavailable, "native_round_spawn_selector");
		event("round_request_unavailable", "native_round_spawn_selector");
		return false;
	}
	if (!pbd::replay_native::can_prepare_round_styles(owner, generation, target.ready->ordinal))
	{
		finish_round(pbd_replay_round_result::status::unavailable, "native_mokujin_round_resource_rebind");
		event("round_request_unavailable", "native_mokujin_round_resource_rebind");
		return false;
	}
	return true;
}

bool pbd_direct_match::round_boundary(ppu_thread& owner, u32 boundary, bool* reconstructed) noexcept
{
	if (reconstructed)
		*reconstructed = false;
	if (!data)
		return true;
	auto& s = *data;
	poll_round_request_file();
	// A file can supply a later round before resources load. Keep that request until direct entry has an input anchor.
	if (!s.redirected || s.anchor == ~0u || boundary < s.anchor)
		return true;
	const u32 round = s.requested_round.exchange(0);
	if (!round)
		return true;
	try
	{
		if (s.failed || !s.redirected || s.generation != static_cast<u64>(Emu.GetEmulationIdentifier()) ||
			pbd::fn_hook::current_pass.load() || round > s.rounds.size() ||
			!(s.captured_round_mask.load(std::memory_order_acquire) & (u32{1} << (round - 1))))
		{
			s.finish_round(pbd_replay_round_result::status::unavailable, "native_round_generation_or_pass");
			return true;
		}
		const auto& target = s.rounds[round - 1];
		const char* context_refusal = s.round_context(owner, target, round);
		if (context_refusal)
		{
			s.finish_round(pbd_replay_round_result::status::unavailable, context_refusal);
			s.event("round_request_unavailable", context_refusal,
				{{"cpuFlags", json::number(static_cast<u32>(owner.state.load()))},
					{"schedulerPreemption", json::boolean(owner.ack_suspend)}});
			return true;
		}
		const state::pbd_owner_run admitted(s, owner);
		if (!s.round_resources(owner, target, round, context_refusal))
			return true;
		const auto begin = std::chrono::steady_clock::now();
		if (!pbd::replay_native::prepare_round_styles(owner, s.generation, target.ready->ordinal) ||
			!round_styles_bound(target.ready->ordinal))
			return s.refuse("native_mokujin_round_resource_rebind");
		if (round > 1 && (!s.apply_round(owner, *target.cleanup) ||
							 !pbd::replay_native::initialize_round(owner, s.generation, 6)))
			return s.refuse("native_round_cleanup");
		if (!s.apply_round(owner, *target.ready) ||
			!pbd::replay_native::initialize_round(owner, s.generation, target.ready->kind))
			return s.refuse("native_round_initializer");
		// The capsule precedes initialization; playback resumes at its recorded first input boundary.
		if (!store_word(0x1008f07c, *target.ready->pbd_entry_counter))
			return s.refuse("native_round_entry_counter_write");
		for (u32 side = 0; side < 2; ++side)
		{
			be_t<s16> score{};
			if (!vm::try_access(side ? 0x100909a0 : 0x100900d0, &score, sizeof(score), false) ||
				score != target.ready->result[side][0])
				return s.refuse("native_round_score_readback");
		}
		s.input_base = target.ready->input_offset;
		s.anchor = boundary;
		s.logical_position.store(s.input_base, std::memory_order_release);
		pbd::fn_hook::keyed_segment.store(target.ready->keyed_segment);
		const auto request = std::atomic_load(&s.round_reply);
		s.continuation.store(request->continuation, std::memory_order_release);
		const auto native_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
			std::chrono::steady_clock::now() - begin)
		                           .count();
		// The rebuilt starting state must match before Take Control can execute its first input.
		s.round_validation_ns = static_cast<u64>(native_ns);
		if (reconstructed)
			*reconstructed = true;
		return true;
	}
	catch (...)
	{
		return s.refuse("native_round_exception");
	}
}
