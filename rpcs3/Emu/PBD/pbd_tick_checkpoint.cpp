#include "stdafx.h"
#include "../Cell/pbd_rollback_session_boundary.h"

void pbd::rollback_session::tick_dispatch::capture_tick_checkpoint(tick_sample& tick)
{
	if (!data.checkpoint.ring.empty() && (!data.roundtrip.repeat_active || data.correction.active))
	{
		// Checkpoint this boundary into the ring (no restore). The capture is timed per tick.
		const auto ring_begin = session_clock::now();
		pbd::stall_metrics::scope measure_capture("checkpoint_capture", data.checkpoint.checkpoint_pages.size() * 4096ull);
		auto& entry = data.checkpoint.ring[index % data.checkpoint.ring.size()];
		entry.voice_checkpoint_token = pbd::fn_hook::voice_checkpoint_token();
		bool ok = true;
		u32 calls = 0;
		if (data.checkpoint.capture_pool)
		{
			ok = data.checkpoint.capture_pool->run(entry.pages.data());
			calls = static_cast<u32>(data.checkpoint.capture_pool->threads.size()) + 1;
		}
		else if (!data.checkpoint.checkpoint_runs.empty())
		{
			for (const auto& run : data.checkpoint.checkpoint_runs)
			{
				ok = vm::try_read_stream(data.checkpoint.checkpoint_pages[run.first], entry.pages.data() + u64{run.first} * 4096, run.second * 4096u);
				calls++;
				if (!ok)
					break;
			}
		}
		else
		{
			for (usz i = 0; i < data.checkpoint.checkpoint_pages.size() && ok; i++)
			{
				ok = vm::try_read_stream(data.checkpoint.checkpoint_pages[i], entry.pages.data() + i * 4096, 4096);
				calls++;
			}
		}
		const auto ring_pages_done = session_clock::now();
		entry.valid = ok;
		entry.index = index;
		entry.counter = row.before_counter;
		entry.consumer_offset = data.consumer_offset;
		entry.last_fight_index = data.last_fight_index;
		const auto ring_context_begin = session_clock::now();
		entry.context = read_context(ppu);
		const auto ring_context_done = session_clock::now();
		entry.previous_held = data.previous_held;
		tick.ring_captured = ok;
		tick.ring_capture_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(session_clock::now() - ring_begin).count();
		tick.ring_pages_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(ring_pages_done - ring_begin).count();
		tick.ring_context_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(ring_context_done - ring_context_begin).count();
		tick.ring_calls = calls;
		tick.ring_bytes = data.checkpoint.checkpoint_pages.size() * 4096ull;
		arm_tick_checkpoint(tick, entry);
	}
}

void pbd::rollback_session::tick_dispatch::reset_undo_journal(ring_entry& entry)
{
	entry.undo_pages.clear();
	entry.undo_pcs.clear();
	entry.undo_lrs.clear();
	entry.skip_pages.clear();
	entry.skip_pcs.clear();
	entry.undo_bytes.clear();
	entry.undo_overflow = false;
	for (u32 p : data.undo.marked)
	{
		data.undo.page_bits[p >> 6] &= ~(u64{1} << (p & 63));
		g_pbd_undo_skip_bits[p >> 6] &= ~(u64{1} << (p & 63));
	}
	data.undo.marked.clear();
	entry.foreign_pages.clear();
}

void pbd::rollback_session::tick_dispatch::arm_tick_checkpoint(tick_sample& tick, ring_entry& entry)
{
	// A new tick starts: its undo list is rebuilt from the first out-of-set main-PPU stores.
	reset_undo_journal(entry);
	data.undo.tick = index;
	data.undo.ready = data.correction.active;
	if (data.fault.capture && data.undo.ready)
	{
		tick.faults = static_cast<u32>(data.fault.events - data.fault.events_mark);
		tick.fault_ns = data.fault.capture_ns - data.fault.capture_ns_mark;
		data.fault.events_mark = data.fault.events;
		data.fault.capture_ns_mark = data.fault.capture_ns;
		if (!data.fault.armed_all)
			session_runtime.fault_arm_all();
		else
			session_runtime.fault_rearm();
		if (!data.fault.log.empty())
		{
			std::vector<std::string> rows;
			{
				pbd_fault_guard guard(data.fault.lock);
				rows.swap(data.fault.log);
			}
			for (const auto& text : rows)
				data.correction.file.write(text.data(), text.size());
		}
	}
}

void pbd::rollback_session::tick_dispatch::capture_replay_checkpoints(tick_sample& tick)
{
	// Finish the requested takeover anchor at the same point as the production rollback checkpoint.
	// Its since-anchor write journal is armed immediately below, before guest execution resumes.
	if (data.takeover.capture_pending && data.takeover.capture_index == index)
	{
		if (!capture_takeover(index) && data.practice.rewind)
		{
			const auto text = fmt::format("{\"type\":\"practice_remember_fallback\",\"boundary\":%u,\"reason\":\"%s\"}\n",
				index, data.takeover.refusal);
			data.replay.control_file.write(text.data(), text.size());
			data.practice.view.toast = "Position remembered\tThe full moment couldn't be saved";
			data.practice.menu.toast_until = session_clock::now() + std::chrono::seconds(3);
		}
	}
	// A replay rewind checkpoint asked for at this boundary (rewind_schedule); its journal is armed just below too.
	if (data.rewind.capture_pending && data.rewind.capture_index == index)
		rewind_capture(index);
	if (data.replay.takeover_fast)
	{
		data.undo.tick = index;
		const bool keep_armed = data.session.in_match &&
		                        (data.takeover.fault_armed || tick.fight || data.takeover.keyframe.valid || !data.rewind.points.empty());
		if (!keep_armed)
			rewind_clear();
		data.undo.ready = keep_armed;
		if (keep_armed)
		{
			tick.faults = static_cast<u32>(data.fault.events - data.fault.events_mark);
			tick.fault_ns = data.fault.capture_ns - data.fault.capture_ns_mark;
			data.fault.events_mark = data.fault.events;
			data.fault.capture_ns_mark = data.fault.capture_ns;
			if (!data.fault.armed_all)
				session_runtime.fault_arm_all();
			else
				session_runtime.fault_rearm();
			data.takeover.fault_armed = true;
		}
		else if (data.fault.armed_all)
		{
			session_runtime.fault_disarm_all();
			data.takeover.fault_armed = false;
			data.takeover.keyframe.valid = false;
			g_pbd_takeover_anchor_live.store(false, std::memory_order_release);
		}
		if (!data.fault.log.empty())
		{
			std::vector<std::string> rows;
			{
				pbd_fault_guard guard(data.fault.lock);
				rows.swap(data.fault.log);
			}
			for (const auto& text : rows)
				data.replay.control_file.write(text.data(), text.size());
		}
	}
}
