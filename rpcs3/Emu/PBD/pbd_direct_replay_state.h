#pragma once

#include "../Cell/pbd_direct_match.h"
#include "../Cell/pbd_replay_game_file.h"
#include "../Cell/pbd_fn_hook.h"
#include "pbd_character_rule_native.h"
#include "pbd_character_rule.h"
#include "pbd_replay_difference.h"
#include "pbd_env.h"
#include "pbd_owner_flags.h"
#include "pbd_t5dr_memory.h"
#include "../Cell/pbd_rollback_session.h"
#include "../Cell/pbd_practice_native.h"
#include "Emu/PBD/pbd_anchor_dump.h"
#include "Emu/PBD/pbd_replay_carry.h"
#include "Emu/PBD/pbd_replay_standby.h"
#include "Emu/PBD/pbd_replay_carry_codec.h"
#include "Emu/PBD/pbd_replay_capsule_start.h"
#include "Emu/PBD/pbd_replay_restore.h"
#include "../Cell/pbd_set_channel.h"
#include "Emu/PBD/pbd_settings_runtime.h"
#include "../Cell/PPUThread.h"
#include "Emu/System.h"
#include "Emu/system_config.h"
#include "Emu/RSX/pbd_replay_pacing.h"
#include "Emu/Memory/vm.h"
#include <atomic>
#include <chrono>
#include <charconv>
#include <fstream>
#include <iterator>
#include <stdexcept>
#include <string_view>
#include <vector>

LOG_CHANNEL(pbd_replay_log, "PBD_REPLAY");

namespace pbd::direct_replay
{
	using json = pbd::set_channel::json_value;
	using kind = pbd::set_channel::json_kind;
	extern std::atomic<pbd_direct_match*> active;
	std::filesystem::path game_path();
	std::filesystem::path request_path();
	using pbd::replay_game_file::field;
	using pbd::replay_game_file::hex;
	using pbd::replay_game_file::integer;
	using pbd::replay_restore::generators;
	using pbd::replay_restore::round_styles_bound;
	using pbd::replay_restore::store;
	using pbd::replay_restore::store_word;
	using pbd::replay_restore::word;
} // namespace pbd::direct_replay
using namespace pbd::direct_replay;

struct pbd_direct_match::state
{
	u64 generation = 0;
	u32 seed = 0, game = 0, prior = 0, ordinal = 0, pre_ordinal = 0, frame_counter = 0;
	u32 anchor = ~0u, input_base = 0, producer_calls = 0, initializer_calls = 0;
	u32 first_battle_input = 0;
	std::array<u32, 2> final_score{};
	u32 recorded_input_counter = 0, recorded_mob_batches = 0;
	u32 input_clock_differences = 0;
	bool recorded_input_context = false;
	pbd::replay_native::choices choices;
	bool allow_jinpachi = true;
	std::array<u8, 13> options{};
	std::array<u8, 1> rounds_option{};
	std::array<u8, 44> rules{};
	std::array<u32, 2> handicaps{};
	pbd::replay_carry::game_values carry_values;
	std::unique_ptr<generators> before_styles, before_match;
	struct input
	{
		u32 segment = 0, counter = 0;
		std::array<u16, 2> words{};
	};
	std::vector<input> inputs;
	std::vector<pbd::lobby_set::sync_facts> sync;
	struct round_start
	{
		u32 kind = 0, ordinal = 0, frame_counter = 0, input_offset = 0, keyed_segment = 0, spawn_selector = 0;
		std::optional<u32> pbd_entry_counter;
		std::array<std::array<be_t<s16>, 2>, 2> result{};
		std::unique_ptr<generators> rng;
		std::optional<pbd::replay_carry::game_values> carry;
	};
	struct saved_round
	{
		std::unique_ptr<round_start> cleanup, ready;
	};
	std::array<saved_round, 32> rounds;
	std::atomic<u32> captured_round_mask{0};
	std::atomic<u32> requested_round{0}, logical_position{0};
	std::shared_ptr<const pbd_replay_round_result> round_reply;
	std::optional<u64> round_validation_ns;
	std::atomic<pbd_replay_continuation> continuation{pbd_replay_continuation::recorded};
	std::filesystem::path request_file;
	u32 file_request_seen = 0;
	bool started = false, handoff_seen = false, redirected = false, failed = false, battle_seen = false;
	bool pbd_external_inputs = false;
	std::array<std::string, 2> player_names;
	std::array<u32, 2> player_ranks{};
	std::optional<pbd::rank_stakes::game_values> rank_screens;
	std::optional<pbd::replay_notices::placements> placements;
	std::chrono::steady_clock::time_point accepted;
	s64 loading_vblank = 0;
	u32 loading_limit = 0;
	static constexpr u32 loading_rate = 600;
	void begin_loading() noexcept
	{
		// Only saved playback owns this temporary pacing. Live watching keeps its channel's pacing.
		if (pbd_external_inputs || loading_vblank)
			return;
		loading_vblank = g_cfg.video.vblank_rate.get();
		loading_limit = pbd::replay_pacing::pbd_player_rate.exchange(loading_rate, std::memory_order_acq_rel);
		g_cfg.video.vblank_rate.set(loading_rate);
		pbd::replay_pacing::vblank_request.store(loading_rate, std::memory_order_release);
	}
	void finish_loading() noexcept
	{
		if (!loading_vblank)
			return;
		// Session teardown may already have restored pacing before the loader is destroyed.
		if (g_cfg.video.vblank_rate.get() == loading_rate)
		{
			g_cfg.video.vblank_rate.set(loading_vblank);
			pbd::replay_pacing::vblank_request.store(static_cast<u32>(loading_vblank), std::memory_order_release);
		}
		auto limit = loading_rate;
		pbd::replay_pacing::pbd_player_rate.compare_exchange_strong(limit, loading_limit, std::memory_order_acq_rel);
		loading_vblank = 0;
	}
	~state()
	{
		finish_loading();
	}
	void event(const char* name, const char* dependency = "", std::vector<std::pair<std::string, json>> details = {})
	{
		const auto elapsed = started ? std::chrono::duration_cast<std::chrono::milliseconds>(
										   std::chrono::steady_clock::now() - accepted)
		                                   .count() :
		                               0;
		const auto fields = pbd::set_channel::detail::serialize(json::object(std::move(details)));
		if (*dependency)
			pbd_replay_log.error("%s: game %u boundary %u: %s %s", name, game,
				pbd::fn_hook::current_tick.load(), dependency, fields);
		else
			pbd_replay_log.notice("%s: game %u boundary %u (%d ms) %s", name, game,
				pbd::fn_hook::current_tick.load(), elapsed, fields);
	}
	struct pbd_owner_run
	{
		state& replay;
		ppu_thread& owner;
		pbd::replay_native::owner_guard owner_check;
		pbd_owner_run(state& replay, ppu_thread& owner) noexcept
			: replay(replay), owner(owner), owner_check(owner)
		{
		}
		~pbd_owner_run() noexcept
		{
			try
			{
				const auto flags = owner.state.load();
				if (flags && pbd::pbd_owner_flags_allowed(flags, true, owner.ack_suspend))
					replay.event("owner_flags_deferred", "",
						{{"cpuFlags", json::number(static_cast<u32>(flags))},
							{"schedulerPreemption", json::boolean(owner.ack_suspend)}});
			}
			catch (...)
			{
			} // A diagnostic allocation failure never changes the completed guest operation.
		}
	};
	void finish_round(pbd_replay_round_result::status status, const char* dependency = "", u64 native_ns = 0)
	{
		const auto prior = std::atomic_load(&round_reply);
		if (!prior || prior->state != pbd_replay_round_result::status::queued)
			return;
		auto reply = std::make_shared<pbd_replay_round_result>(*prior);
		reply->state = status;
		reply->dependency = dependency;
		reply->native_call_ns = native_ns;
		if (status == pbd_replay_round_result::status::reconstructed)
		{
			const auto& ready = rounds[reply->round - 1].ready;
			reply->input_offset = ready->input_offset;
			reply->native_round = ready->ordinal + 1;
			for (u32 side = 0; side < 2; ++side)
				reply->score[side] = static_cast<u16>(s16{ready->result[side][0]});
		}
		const std::shared_ptr<const pbd_replay_round_result> result{std::move(reply)};
		std::atomic_store(&round_reply, result);
		const char* state = status == pbd_replay_round_result::status::reconstructed ? "reconstructed" :
		                    status == pbd_replay_round_result::status::unavailable   ? "unavailable" :
		                                                                               "refused";
		event("round_request_result", dependency, {{"requestId", json::number(result->request_id)}, {"runtimeGeneration", json::number(result->generation)}, {"status", json::string(state)}, {"round", json::number(result->round)}, {"nativeRound", json::number(result->native_round)}, {"scoreLeft", json::number(result->score[0])}, {"scoreRight", json::number(result->score[1])}, {"inputOffset", json::number(result->input_offset)}, {"nativeCallNanoseconds", json::number(result->native_call_ns)}});
	}
	// The recorded round an input offset falls in, and that round's first input (pbd::replay_game_file::recorded_round).
	pbd::replay_game_file::round_position round_at(usz offset) const
	{
		const u32 captured = captured_round_mask.load(std::memory_order_acquire); // Each captured round has its start.
		std::array<u32, 32> starts{};
		for (u32 i = 0; i < rounds.size(); ++i)
			starts[i] = (captured >> i) & 1 ? rounds[i].ready->input_offset : 0;
		return pbd::replay_game_file::recorded_round(captured, starts, offset);
	}
	bool refuse(const char* dependency)
	{
		round_validation_ns.reset();
		finish_loading();
		finish_round(pbd_replay_round_result::status::refused, dependency);
		const auto* owner = get_current_cpu_thread<ppu_thread>();
		event("refused", dependency,
			{{"cpuFlags", json::number(owner ? static_cast<u32>(owner->state.load()) : 0)},
				{"cpuOwnerPresent", json::boolean(owner != nullptr)},
				{"schedulerPreemption", json::boolean(owner && owner->ack_suspend)}});
		failed = true;
		pbd::set_notices::publish(generation, {});
		return false;
	}
	void capture_round(ppu_thread& owner, u32 kind) noexcept
	{
		try
		{
			if (continuation.load(std::memory_order_acquire) != pbd_replay_continuation::recorded)
				return;
			const u32 ordinal = word(0x1008f0e4), boundary = pbd::fn_hook::current_tick.load();
			if (ordinal >= rounds.size() || anchor == ~0u || boundary < anchor ||
				input_base + boundary - anchor + 1 >= inputs.size())
				return;
			auto& slot = rounds[ordinal];
			auto& target = kind == 6 ? slot.cleanup : slot.ready;
			if (target)
				return; // A jump must never replace the original anchor.
			auto saved = std::make_unique<round_start>();
			saved->kind = kind;
			saved->ordinal = ordinal;
			saved->frame_counter = word(0x1008f07c);
			saved->spawn_selector = word(0x10091cf0);
			saved->input_offset = input_base + boundary - anchor + 1;
			if (kind == 4 || kind == 5)
			{
				if (saved->input_offset >= sync.size())
					return;
				saved->pbd_entry_counter = sync[saved->input_offset].counter;
			}
			saved->keyed_segment = pbd::fn_hook::keyed_segment.load();
			if (!vm::try_access(0x100900d0, saved->result[0].data(), sizeof(saved->result[0]), false) ||
				!vm::try_access(0x100909a0, saved->result[1].data(), sizeof(saved->result[1]), false))
				return;
			for (const auto& side : saved->result)
				if (side[0] < 0 || side[0] > 5 || side[1] < -1 || side[1] > 2)
					return;
			saved->rng = std::make_unique<generators>(owner);
			saved->carry = pbd::replay_carry::capture_game_values(owner, generation);
			if (!saved->carry)
				return;
			target = std::move(saved);
			if (kind == 4 || (kind == 5 && slot.cleanup))
			{
				captured_round_mask.fetch_or(u32{1} << ordinal, std::memory_order_release);
				event("round_start_captured", "", {{"round", json::number(ordinal + 1)}, {"initializer", json::number(kind)}, {"inputOffset", json::number(target->input_offset)}, {"scoreLeft", json::number(s16{target->result[0][0]})}, {"scoreRight", json::number(s16{target->result[1][0]})}});
			}
		}
		catch (...)
		{
			pbd_replay_log.error("Round snapshot unavailable; match playback continues.");
		}
	}
	bool apply_actor_terms(ppu_thread& owner)
	{
		if (carry_values.planar)
		{
			if (!pbd::replay_carry::apply_actor_planar_terms(owner, generation, *carry_values.planar))
				return refuse("native_match_actor_planar_context");
			event("native_actor_planar_terms_applied", "",
				{{"left0Bits", json::number(carry_values.planar->bits[0][0])},
					{"left1Bits", json::number(carry_values.planar->bits[0][1])},
					{"right0Bits", json::number(carry_values.planar->bits[1][0])},
					{"right1Bits", json::number(carry_values.planar->bits[1][1])}});
		}
		if (carry_values.transform_sum)
		{
			if (!pbd::replay_carry::apply_actor_transform_sum(owner, generation, *carry_values.transform_sum))
				return refuse("native_match_actor_transform_sum_context");
			event("native_actor_transform_sum_applied", "",
				{{"leftBits", json::number(carry_values.transform_sum->bits[0])},
					{"rightBits", json::number(carry_values.transform_sum->bits[1])}});
		}
		if (carry_values.descriptor_code)
		{
			if (!pbd::replay_carry::apply_actor_descriptor_code(owner, generation, *carry_values.descriptor_code))
				return refuse("native_match_actor_descriptor_code_context");
			event("native_actor_descriptor_code_applied", "",
				{{"leftCode", json::number(carry_values.descriptor_code->codes[0])},
					{"rightCode", json::number(carry_values.descriptor_code->codes[1])}});
		}
		if (carry_values.descriptor_deltas)
		{
			if (!pbd::replay_carry::apply_actor_descriptor_deltas(
					owner, generation, *carry_values.descriptor_deltas))
				return refuse("native_match_actor_descriptor_deltas_context");
			event("native_actor_descriptor_deltas_applied", "",
				{{"left0Bits", json::number(carry_values.descriptor_deltas->bits[0][0])},
					{"left1Bits", json::number(carry_values.descriptor_deltas->bits[0][1])},
					{"right0Bits", json::number(carry_values.descriptor_deltas->bits[1][0])},
					{"right1Bits", json::number(carry_values.descriptor_deltas->bits[1][1])}});
		}
		return true;
	}
	bool apply_reaction_result(ppu_thread& owner)
	{
		if (!carry_values.reaction_result)
			return true;
		if (!pbd::replay_carry::apply_actor_reaction_result(owner, generation, *carry_values.reaction_result))
			return refuse("native_match_actor_reaction_result_context");
		event("native_actor_reaction_result_applied");
		return true;
	}
	bool apply_retained_terms(ppu_thread& owner)
	{
		if (carry_values.segment_deltas &&
			!pbd::replay_carry::apply_actor_segment_deltas(owner, generation, *carry_values.segment_deltas))
			return refuse("native_match_actor_segment_deltas_context");
		if ((carry_values.prior_life || carry_values.reaction_scalar || carry_values.previous_blend_weight) &&
			!pbd::replay_carry::apply_actor_retained_scalars(owner, generation, carry_values))
			return refuse("native_match_actor_retained_scalars_context");
		if (carry_values.segment_deltas)
			event("native_actor_segment_deltas_applied");
		if (carry_values.prior_life || carry_values.reaction_scalar || carry_values.previous_blend_weight)
			event("native_actor_retained_scalars_applied");
		return true;
	}
	bool apply_hit_contacts(ppu_thread& owner)
	{
		if (!carry_values.hit_contacts)
			return true;
		if (!pbd::replay_carry::apply_actor_hit_contacts(owner, generation, *carry_values.hit_contacts))
			return refuse("native_match_actor_hit_contacts_context");
		event("native_actor_hit_contacts_applied");
		return true;
	}
	bool apply_bone_segments(ppu_thread& owner)
	{
		if (!carry_values.bone_segments)
			return true;
		if (!pbd::replay_carry::apply_actor_bone_segments(owner, generation, *carry_values.bone_segments))
			return refuse("native_match_actor_bone_segments_context");
		event("native_actor_bone_segments_applied");
		return true;
	}
	bool apply_reaction_conditions(ppu_thread& owner)
	{
		if (!carry_values.reaction_conditions)
			return true;
		const auto& saved = *carry_values.reaction_conditions;
		if (!pbd::replay_carry::apply_actor_reaction_conditions(owner, generation, saved))
			return refuse("native_match_actor_reaction_conditions_context");
		event("native_actor_reaction_conditions_applied", "",
			{{"leftCommand11e", json::number(saved.command_11e[0])},
				{"rightCommand11e", json::number(saved.command_11e[1])},
				{"leftCommand22", json::number(saved.command_22[0])},
				{"rightCommand22", json::number(saved.command_22[1])}});
		return true;
	}
	bool apply_actor_reactions(ppu_thread& owner)
	{
		if (carry_values.reaction_cursor)
		{
			if (!pbd::replay_carry::apply_actor_reaction_cursor(owner, generation, *carry_values.reaction_cursor))
				return refuse("native_match_actor_reaction_cursor_context");
			event("native_actor_reaction_cursor_applied", "",
				{{"leftKind", json::number(carry_values.reaction_cursor->ports[0].kind)},
					{"leftIndex", json::number(carry_values.reaction_cursor->ports[0].index)},
					{"rightKind", json::number(carry_values.reaction_cursor->ports[1].kind)},
					{"rightIndex", json::number(carry_values.reaction_cursor->ports[1].index)}});
		}
		if (carry_values.motion_step)
		{
			if (!pbd::replay_carry::apply_actor_motion_step(owner, generation, *carry_values.motion_step))
				return refuse("native_match_actor_motion_step_context");
			event("native_actor_motion_step_applied", "",
				{{"leftXBits", json::number(carry_values.motion_step->bits[0][0])},
					{"leftZBits", json::number(carry_values.motion_step->bits[0][1])},
					{"rightXBits", json::number(carry_values.motion_step->bits[1][0])},
					{"rightZBits", json::number(carry_values.motion_step->bits[1][1])}});
		}
		if (carry_values.last_reaction_code)
		{
			if (!pbd::replay_carry::apply_actor_last_reaction_code(owner, generation, *carry_values.last_reaction_code))
				return refuse("native_match_actor_last_reaction_code_context");
			event("native_actor_last_reaction_code_applied", "",
				{{"leftCode", json::number(carry_values.last_reaction_code->codes[0])},
					{"rightCode", json::number(carry_values.last_reaction_code->codes[1])}});
		}
		return apply_reaction_conditions(owner) && apply_reaction_result(owner) && apply_bone_segments(owner) &&
		       apply_hit_contacts(owner) && apply_retained_terms(owner);
	}
	bool apply_descriptor_flag(ppu_thread& owner)
	{
		if (carry_values.descriptor_derived_flag)
		{
			if (!pbd::replay_carry::apply_descriptor_derived_flag(
					owner, generation, *carry_values.descriptor_derived_flag))
				return refuse("native_match_descriptor_flag_context");
			event(
				"native_descriptor_flag_applied", "", {{"value", json::number(*carry_values.descriptor_derived_flag)}});
		}
		else
		{
			pbd::replay_carry::descriptor_flag flag;
			if (!pbd::replay_carry::refresh_descriptor_flag(owner, generation, flag))
				return refuse("native_match_descriptor_flag_context");
			event("native_descriptor_flag_refreshed", "",
				{{"descriptor", json::number(flag.descriptor)}, {"before", json::number(flag.before)},
					{"value", json::number(flag.value)}});
		}
		return true;
	}
	bool apply_actor_records(ppu_thread& owner)
	{
		if (!carry_values.records)
			return true;
		u32 changed = 0;
		const auto result = pbd::replay_carry::apply_actor_records(owner, generation, *carry_values.records, changed);
		if (result != pbd::replay_carry::records_refusal::none)
			return refuse(pbd::replay_carry::refusal_reason(result));
		event("native_actor_records_applied", "", {{"bytesChanged", json::number(changed)}});
		return true;
	}
	// The records go first so the typed carries after them still check, rebuild and read back their own words; written
	// last, the records would overwrite those words unchecked. Each fighter keeps its bone-root word (apply_actor_records).
	bool apply_actor_carry(ppu_thread& owner)
	{
		return apply_actor_records(owner) && apply_actor_terms(owner) && apply_actor_reactions(owner) &&
		       apply_descriptor_flag(owner);
	}
	bool load_capsule(const json& cap)
	{
		if (!game)
			return false;
		auto parsed = pbd::replay_start::pbd_read_capsule_start(cap, seed);
		if (!parsed.value)
			return false;
		auto& start = *parsed.value;
		allow_jinpachi = start.allow_jinpachi;
		choices = {start.left, start.right, start.stage};
		prior = start.prior;
		recorded_mob_batches = start.mob_batches;
		frame_counter = start.counter;
		carry_values = std::move(start.carry);
		placements = start.placements;
		player_ranks = start.ranks;
		handicaps = start.handicaps;
		recorded_input_counter = start.input_counter;
		recorded_input_context = start.input_context;
		ordinal = start.ordinal;
		pre_ordinal = start.pre_ordinal;
		options = start.options;
		rounds_option = start.rounds_option;
		rules = start.rules;
		before_styles = std::move(start.before_styles);
		before_match = std::move(start.before_match);
		return true;
	}
	void publish_notices(u32 scene, usz offset)
	{
		if (!placements || pbd_external_inputs)
			return;
		const auto read = [](u32 address, u32& out)
		{
			be_t<u32> value{};
			if (!vm::try_access(address, &value, sizeof(value), false))
				return false;
			out = value;
			return true;
		};
		u32 pending = ~0u, native_round = ~0u, mode = 0, players = 0, humans = 0;
		read(0x10093230, pending);
		read(0x1008f0e4, native_round);
		std::array<bool, 2> drawn{}, promoted{};
		if (read(pbd::t5dr::game_mode_address, mode) && read(0x1008f080, players) && read(0x1008f0b0, humans))
			for (u32 side = 0; side < 2; ++side)
				drawn[side] = pbd::game_profile::plate_drawn(mode, players, humans, side);
		for (u32 side = 0; side < 2; ++side)
		{
			u32 change = 0;
			promoted[side] = read(pbd::game_profile::change_address(side), change) && change == pbd::game_profile::change_promoted;
		}
		// The recorded first round call also excludes its reload before the next initializer increments the ordinal.
		// Both the input position and guest ordinal rewind with a scrub; no forward-only intro flag survives a seek.
		const bool first_intro = native_round == 0 && (!rounds[0].ready || offset < rounds[0].ready->input_offset);
		pbd::set_notices::publish(generation, pbd::replay_notices::compose(*placements, scene, pending, first_intro,
												  drawn, promoted, rank_screens.value_or(pbd::rank_stakes::game_values{}).stake));
	}
	bool carry(ppu_thread& owner, u32 value, const pbd::replay_carry::game_values* retained = nullptr)
	{
		return pbd::replay_native::apply_handicap_options(owner, generation, handicaps[0], handicaps[1]) &&
		       pbd::replay_carry::apply_vs_flow(owner, generation, retained ? *retained : carry_values) &&
		       store(0x1008ebb0, options) && store(0x1008ebd4, rounds_option) && store(0x1008f098, rules) &&
		       store_word(0x1008f0e4, value) && store_word(0x10425ce0, 0);
	}
	bool apply_round(ppu_thread& owner, const round_start& saved)
	{
		u32 changed = 0;
		return saved.carry && saved.carry->records && carry(owner, saved.ordinal, &*saved.carry) &&
		       pbd::replay_carry::pbd_apply_round_actor_records(owner, generation, *saved.carry->records, changed) ==
		           pbd::replay_carry::records_refusal::none &&
		       saved.rng && saved.rng->style_readback() &&
		       saved.rng->apply(owner) && store_word(0x1008f07c, saved.frame_counter) &&
		       store_word(0x10091cf0, saved.spawn_selector) &&
		       store(0x100900d0, saved.result[0]) && store(0x100909a0, saved.result[1]);
	}

	const char* round_context(ppu_thread& owner, const saved_round& target, u32 round);
	bool round_resources(ppu_thread& owner, const saved_round& target, u32 round, const char*& context_refusal);
	std::unique_ptr<round_start> restore_point(const pbd::replay_game_file::round_point& point);
	bool enter_match(ppu_thread& owner);
};
