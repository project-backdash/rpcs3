#include "stdafx.h"
#include "pbd_direct_replay_state.h"

bool pbd_direct_match::state::enter_match(ppu_thread& owner)
{
	// The stock resource jobs ran every setup frame; the recording starts at the player's pacing.
	finish_loading();
	event("stock_match_initializer_observed", "", {{"pc", json::number(owner.cia)}, {"lr", json::number(owner.lr)}, {"scene", json::number(word(pbd::t5dr::scene_address))}, {"pendingScene", json::number(word(pbd::t5dr::subscene_address))}, {"actorModelLeft", json::number(word(0x100908e4))}, {"actorModelRight", json::number(word(0x100911b4))}, {"nativeInputContextCaptured", json::boolean(recorded_input_context)}, {"recordedInputCounter", json::number(recorded_input_counter)}, {"nativeInputCounter", json::number(word(0x104cce40))}});
	if (!handoff_seen || redirected || ++initializer_calls != 1 || !before_match->style_readback())
		return refuse("unexpected_native_match_initializer");
	// The replay ran this game's stage load again; any other mob batch count would have reseeded its mobs differently.
	if (!pbd::fn_hook::reseed_epoch().reached(recorded_mob_batches))
		return refuse("native_mob_batch_count");
	const char* refusal = "stock_match_initializer_or_resource_handoff_readback";
	u32 scheduler_flags = 0;
	bool scheduler_preemption = false;
	const bool entered = pbd::replay_native::admit_match_initializer(owner, generation, choices, &refusal,
		&scheduler_flags, &scheduler_preemption);
	if (scheduler_flags)
		event("native_match_scheduler_observed", "", {{"flagsBefore", json::number(scheduler_flags)}, {"flagsAfter", json::number(static_cast<u32>(owner.state.load()))}, {"schedulerPreemption", json::boolean(scheduler_preemption)}, {"entered", json::boolean(entered)}});
	if (!entered)
		return refuse(refusal);
	// The pinned ELF's only counter read is its own120740 increment;120764 writes zero asynchronously.
	// As in the ordinary round-lifecycle path, this is a local consistency witness, not an input cursor.
	// Keep recorded counters immutable and report differences; do not drive the worker to fit them.
	event("native_input_clock_policy", "", {{"policy", json::string("ordinary_native_consistency_witness")}, {"recordedContextPresent", json::boolean(recorded_input_context)}, {"recordedContextCounter", json::number(recorded_input_counter)}, {"nativeCounter", json::number(word(0x104cce40))}, {"clockWritten", json::boolean(false)}});
	// Asset loading consumes its own native draws. Restore the live pre-kind2 context here, after those jobs,
	// rather than before scene7/10. No RNG return is injected. Resource pointers come from the source only inside the
	// fighters' records, over the COM banks, styles and models this boot loaded (apply_actor_records refuses any
	// other); each fighter keeps its own bone-root object.
	if (!carry(owner, ordinal) || !before_match->apply(owner) || !store_word(0x1008f07c, frame_counter))
		return refuse("native_match_initializer_context");
	if (!apply_actor_carry(owner))
		return false;
	if (!pbd::anchor_dump::replay_anchor(game))
		return refuse("test_anchor_paste");
	event("native_handicap_options_applied", "", {{"leftPercent", json::number(handicaps[0])}, {"rightPercent", json::number(handicaps[1])}, {"vitalityWritten", json::boolean(false)}});
	if (carry_values.history)
		event("native_vs_match_history_applied", "", {{"completedGames", json::number(carry_values.history->completed_games)}, {"leftWins", json::number(carry_values.history->wins[0])}, {"rightWins", json::number(carry_values.history->wins[1])}});
	anchor = pbd::fn_hook::current_tick.load() + 1;
	redirected = true;
	event("direct_match_requested");
	event("native_match_initializer_admitted");
	return true;
}

bool pbd_direct_match::function_event(ppu_thread& owner, u32 address) noexcept
{
	auto* loader = active.load();
	if (!loader || !loader->data || owner.id != pbd::t5dr::main_ppu_id || pbd::practice::native::in_native_call())
		return true;
	auto& s = *loader->data;
	if (!s.started)
		return true;
	// The ordinary RNG observers also report libc/module callbacks with their own TOC and CPU flags.
	// Authenticate DR's owner context only at the three sites where this adapter may mutate native state.
	const bool round_entry = address == 0x159f0 && (owner.gpr[3] == 4 || owner.gpr[3] == 5 || owner.gpr[3] == 6);
	if (address != 0x5a554 && address != 0x53ee4 && (address != 0x159f0 || owner.gpr[3] != 2) && !round_entry)
		return true;
	try
	{
		if (s.failed)
			return false;
		if (s.generation != static_cast<u64>(Emu.GetEmulationIdentifier()))
			return s.refuse("native_runtime_generation_changed");
		if (pbd::fn_hook::current_pass.load())
			return s.refuse("native_correction_pass");
		if (get_current_cpu_thread() != &owner)
			return s.refuse("native_owner_cpu_context");
		if (owner.gpr[2] != pbd::t5dr::toc)
			return s.refuse("native_DR_TOC_not_470540");
		const char* refusal = "native_owner_cpu_flags";
		if (!pbd::replay_native::service_owner(owner, s.generation, &refusal))
			return s.refuse(refusal);
		const state::pbd_owner_run admitted(s, owner);
		if (round_entry)
		{
			if (s.redirected)
				s.capture_round(owner, static_cast<u32>(owner.gpr[3]));
			return true;
		}
		if (address == 0x5a554 && !s.redirected)
		{
			if (++s.producer_calls != 1 || word(pbd::t5dr::scene_address) != 2 || !s.carry(owner, s.pre_ordinal) ||
				!s.before_styles->apply(owner))
				return s.refuse("pre_style_generator_context");
			s.event("stock_style_producer_started");
		}
		if (address == 0x53ee4 && !s.redirected)
		{
			if (s.handoff_seen || s.producer_calls != 1 || s.initializer_calls || !s.before_match->style_readback() ||
				!pbd::replay_native::can_continue_resource_handoff(owner, s.generation, s.choices))
				return s.refuse("stock_battle_resource_handoff");
			s.handoff_seen = true;
			s.event("stock_battle_resource_handoff", "", {{"pc", json::number(owner.cia)}, {"lr", json::number(owner.lr)}, {"mode", json::number(word(pbd::t5dr::game_mode_address))}, {"scene", json::number(word(pbd::t5dr::scene_address))}, {"pendingScene", json::number(word(pbd::t5dr::subscene_address))}, {"leftRow", json::number(word(0x10093f50))}, {"rightRow", json::number(word(0x10093f54))}, {"stage", json::number(word(0x1008f088))}, {"actorModelLeft", json::number(word(0x100908e4))}, {"actorModelRight", json::number(word(0x100911b4))}});
		}
		if (address == 0x159f0 && owner.gpr[3] == 2)
			return s.enter_match(owner);
		return true;
	}
	catch (...)
	{
		return s.refuse("direct_native_event_exception");
	}
}
bool pbd_direct_match::verify_sync(u32 boundary, u32 segment, u32 scene, u32 counter, u32 mask,
	const std::array<u64, 3>& combat, bool verified) noexcept
{
	if (!data)
		return true;
	auto& s = *data;
	try
	{
		if (s.sync.empty() || s.anchor == ~0u || boundary < s.anchor)
			return s.round_validation_ns ? s.refuse("native_round_sync_unavailable") : true;
		if (!s.round_validation_ns && s.continuation.load(std::memory_order_acquire) != pbd_replay_continuation::recorded)
			return true;
		const usz offset = s.input_base + boundary - s.anchor;
		if (offset >= s.inputs.size())
			return s.round_validation_ns ? s.refuse("native_round_sync_unavailable") : true;
		if (!verified)
			return s.refuse("native_replay_sync_unavailable");
		const auto& expected = s.sync[offset];
		const auto found = pbd::replay_difference::find(expected, segment, scene, counter, mask, combat);
		if (!found)
		{
			if (s.round_validation_ns)
			{
				const u64 native_ns = *s.round_validation_ns;
				s.round_validation_ns.reset();
				s.finish_round(pbd_replay_round_result::status::reconstructed, "", native_ns);
				const auto reply = std::atomic_load(&s.round_reply);
				s.event("round_reconstructed", "", {{"round", json::number(reply->round)}, {"inputOffset", json::number(s.input_base)}, {"nativeCallNanoseconds", json::number(native_ns)}});
			}
			return true;
		}
		const auto where = s.round_at(offset);
		const auto names = pbd::replay_difference::name_of(found);
		if (found.what == pbd::replay_difference::kind::coordinate)
			s.event(names.event, names.refusal, {{"inputOffset", json::number(offset)}, {"round", json::number(where.round)}, {"roundInputOffset", json::number(offset - where.start)}, {"scene", json::number(scene)}, {"expectedScene", json::number(expected.scene)}, {"frameCounter", json::number(counter)}, {"expectedFrameCounter", json::number(expected.counter)}});
		else
			s.event(names.event, names.refusal, {{"inputOffset", json::number(offset)}, {"round", json::number(where.round)}, {"roundInputOffset", json::number(offset - where.start)}, {"region", json::number(found.region)}, {"expected", json::string(fmt::format("%016x", expected.combat[found.region]))}, {"actual", json::string(fmt::format("%016x", combat[found.region]))}});
		return s.refuse(names.refusal);
	}
	catch (...)
	{
		return s.refuse("native_replay_sync_exception");
	}
}
pbd_direct_match::sample_result pbd_direct_match::sample(u32 boundary, u32 scene, u32 counter,
	std::array<u16, 2>& input, u32& segment) noexcept
{
	if (!data)
		return sample_result::waiting;
	auto& s = *data;
	// The in-game Settings menu holds this PC's controller while it is open and until its buttons are released: a taken
	// fighter stands still meanwhile. Read once per frame, so a tap shorter than a frame still reaches the fighter.
	const auto local_pad = []() -> u16
	{
		const u16 pad = pbd_local_pad_word(pbd::input_taps::reader::continuation);
		return pbd::settings_menu::claimed() ? 0 : pad;
	};
	try
	{
		input = {0, 0};
		if (s.failed)
			return sample_result::refused;
		if (s.anchor == ~0u || boundary < s.anchor)
			return sample_result::waiting;
		const usz offset = s.input_base + boundary - s.anchor;
		s.publish_notices(scene, offset);
		const auto continuation = s.continuation.load(std::memory_order_acquire);
		const bool taken = continuation != pbd_replay_continuation::recorded;
		if (taken && s.battle_seen && scene != 16 && scene != 17)
		{
			s.event("takeover_game_ended");
			return sample_result::ended;
		}
		if (offset >= s.inputs.size())
		{
			if (taken && s.battle_seen && (scene == 16 || scene == 17))
			{
				// The recorded opponent becomes neutral after its file ends; the changed fight ends natively.
				segment = s.inputs.back().segment;
				input[continuation == pbd_replay_continuation::take_left ? 0 : 1] = local_pad();
				s.logical_position.store(static_cast<u32>(offset), std::memory_order_release);
				return sample_result::input;
			}
			if (!s.battle_seen || scene == 16 || scene == 17)
			{
				s.refuse("recorded_input_end_before_native_game_end");
				return sample_result::refused;
			}
			std::array<be_t<u32>, 2> score_words{};
			const bool readable = vm::try_access(0x100900d0, &score_words[0], sizeof(score_words[0]), false) &&
			                      vm::try_access(0x100909a0, &score_words[1], sizeof(score_words[1]), false);
			const std::array<u32, 2> score{static_cast<u32>(score_words[0]) >> 16, static_cast<u32>(score_words[1]) >> 16};
			if (const auto* refusal = pbd::replay_difference::terminal_refusal(s.final_score, score, readable))
			{
				s.event("final_result_refused", refusal, {{"expectedLeft", json::number(s.final_score[0])}, {"expectedRight", json::number(s.final_score[1])}, {"scoreLeft", json::number(score[0])}, {"scoreRight", json::number(score[1])}});
				s.refuse(refusal);
				return sample_result::refused;
			}
			s.event("native_game_ended", "", {{"inputClockDifferences", json::number(s.input_clock_differences)}, {"scoreLeft", json::number(score[0])}, {"scoreRight", json::number(score[1])}});
			return sample_result::ended;
		}
		if (scene != 8 && scene != 16 && scene != 17)
		{
			s.event("input_alignment_observed", "match_input_scene_alignment",
				{{"offset", json::number(offset)}, {"scene", json::number(scene)}, {"counter", json::number(counter)},
					{"expectedCounter", json::number(s.inputs[offset].counter)}});
			s.refuse("match_input_scene_alignment");
			return sample_result::refused;
		}
		if (counter != s.inputs[offset].counter)
			++s.input_clock_differences;
		if (scene == 16 || scene == 17)
			s.battle_seen = true;
		if (!offset)
			s.event("first_match_input", "", {{"nativeCounter", json::number(counter)}, {"recordedCounter", json::number(s.inputs[offset].counter)}, {"clockWritten", json::boolean(false)}});
		input = s.inputs[offset].words;
		segment = s.inputs[offset].segment;
		if (taken)
			input[continuation == pbd_replay_continuation::take_left ? 0 : 1] = local_pad();
		s.logical_position.store(static_cast<u32>(offset), std::memory_order_release);
		return sample_result::input;
	}
	catch (...)
	{
		s.refuse("direct_input_exception");
		return sample_result::refused;
	}
}
