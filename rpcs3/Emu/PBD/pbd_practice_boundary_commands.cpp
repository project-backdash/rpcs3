#include "stdafx.h"
#include "../Cell/pbd_rollback_session_boundary.h"
#include "pbd_dummy_move_runtime.h"
#include "pbd_practice_lesson_channel.h"
#include "pbd_practice_frames.h"
#include "pbd_practice_start.h"

namespace start = pbd::practice::start;

std::optional<replay_control_result> pbd::rollback_session::practice_steps::observe_stage_load()
{
	const auto load = pbd::practice::commands::observe_load(index);
	if (load == pbd::practice::stage::progress::waiting)
	{
		if (pbd::practice::lesson::current.play.active())
			pbd::practice::lesson::current.finish("not_ready");
		data.practice.dummy.mask_ui = true;
		pbd::practice::input_claim.store(true, std::memory_order_release);
		trace_pad(pbd_local_pad_word(), 0, 0);
		data.practice.dummy.pad = data.practice.dummy.pad_prev = 0;
		pbd::practice::publish({});
		return replay_control_result::go;
	}
	if (load != pbd::practice::stage::progress::none)
		practice_dummy_log(fmt::format("{\"type\":\"practice_stage_load\",\"boundary\":%u,\"progress\":%u}\n",
			index, static_cast<u32>(load)));
	if (load == pbd::practice::stage::progress::invalid || load == pbd::practice::stage::progress::timed_out)
	{
		pbd_session_log.error("Practice stage load did not reach the requested fight.");
		Emu.CallFromMainThread([]()
			{
				Emu.Kill(true, false);
			});
		return replay_control_result::stopped;
	}
	if (load == pbd::practice::stage::progress::complete)
	{
		data.practice.native.screen = 0;
		data.practice.dummy.ignore_until_release = true;
	}

	return std::nullopt;
}

void pbd::rollback_session::practice_steps::scripted_recording()
{
	const u32 offset = data.session.in_match ? index - data.session.match_start : 0;
	if (data.practice.dummy.enabled && data.session.in_match && data.practice.native.pause_valid && data.practice.dummy.test.start != umax)
	{
		if (!data.practice.dummy.test.started && offset >= data.practice.dummy.test.start)
		{
			data.practice.dummy.test.started = true;
			practice_dummy_start_record(index);
			data.practice.dummy.ignore_until_release = false;
		}
		if (data.practice.dummy.test.started && !data.practice.dummy.test.stopped && offset >= data.practice.dummy.test.stop)
		{
			data.practice.dummy.test.stopped = true;
			practice_dummy_stop_record(index, "scripted");
			practice_dummy_accept(index);
			data.practice.dummy.ignore_until_release = false;
		}
		if (!data.practice.dummy.test.complete && offset >= data.practice.dummy.test.end)
		{
			data.practice.dummy.test.complete = true;
			const auto& slot = data.practice.dummy.slots[0];
			practice_dummy_log(fmt::format("{\"type\":\"dummy_test_complete\",\"boundary\":%u,\"frames\":%u,\"cycles\":%u}\n", index, slot.frames.size(), data.practice.dummy.cycles));
		}
	}
	if (data.practice.dummy.enabled && data.practice.dummy.state == dummy_state::recording && data.practice.dummy.pending.words.size() >= 1800)
		practice_dummy_stop_record(index, "cap");
}

void pbd::rollback_session::practice_steps::update_dummy_moves()
{
	{
		const auto native = pbd::practice::native::inspect(true);
		auto* memory = session_runtime.m_practice_memory.get();
		pbd::practice::dummy_move::current_gate = {data.practice.offense.counted, data.practice.offense.times};
		pbd::practice::dummy_move::boundary(ppu,
			data.practice.dummy.enabled && data.practice.view.fight_live && !data.peer.enabled && native.practice,
			// Moves are Offense's Action After a Hit or Block: they play only in Offense, and SELECT+Circle turns them off.
			(data.practice.dummy.state == dummy_state::idle ||
				(data.practice.dummy.state == dummy_state::playing && !data.practice.dummy.preview)) &&
				!pbd::practice::lesson::controls_dummy() &&
				data.practice.training_mode == pbd::practice::training_mode::offense && !data.practice.offense.movement_off,
			data.practice.native.paused || data.practice.menu.open || data.game_overlay.claim ||
				data.practice.restart.reset_request.has_value() || !data.practice.native.pause_valid,
			native.port ^ 1u, data.practice.boundary_counter, data.practice.frame_advanced, memory ? &memory->files : nullptr,
			[&](u32 slot) -> std::optional<std::vector<u16>>
			{
				if (slot >= data.practice.dummy.slots.size())
					return {};
				const auto input_sequence = practice_slot_frames(data.practice.dummy.slots[slot]);
				if (input_sequence.empty())
					return {};
				return std::vector<u16>(input_sequence.begin(), input_sequence.end());
			});
		// Moves set or cleared from the launcher's move picker change Offense's Action After a Hit or Block: the row is Moves
		// exactly while moves are set.
		if (const bool moves_set = pbd::practice::dummy_move::current_settings.enabled();
			moves_set != (data.practice.offense.after == pbd::practice::after_contact::moves))
		{
			data.practice.offense.after = moves_set ? pbd::practice::after_contact::moves : pbd::practice::after_contact::none;
			data.practice.offense.movement_off = false;
			data.practice.contact.watching = false;
			data.practice.contact.ready = false;
			data.practice.contact.reaction_count.reset();
			practice_movement_refresh();
			practice_dummy_log(fmt::format("{\"type\":\"dummy_offense_moves\",\"boundary\":%u,\"set\":%s}\n", index, json_bool(moves_set)));
		}
		if (practice_move_reactions_on() && data.practice.dummy.state == dummy_state::playing && !data.practice.dummy.preview)
			practice_dummy_stop_playback(index, "move_reactions");
		// Slots with moves read their next move from the dummy's list once it is loaded (again).
		practice_slot_moves_refresh();
		auto& moves = pbd::practice::dummy_move::current;
		for (u32 at = 0; at < moves.event_count; ++at)
		{
			const auto& event = moves.events[at];
			practice_dummy_log(fmt::format("{\"type\":\"dummy_move\",\"event\":%u,\"counter\":%u,\"id\":%u,\"variant\":%u,\"value\":%u}\n",
				event.kind, event.counter, event.id, event.variant, event.value));
		}
		moves.event_count = 0;
	}
}
