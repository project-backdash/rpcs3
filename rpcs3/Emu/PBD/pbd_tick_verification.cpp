#include "stdafx.h"
#include "../Cell/pbd_rollback_session_boundary.h"

void pbd::rollback_session::tick_dispatch::capture_held_checkpoint(const char* phase)
{
	if (!data.checkpoint.capture_tick_state)
		return;
	try
	{
		const auto begun = session_clock::now();
		std::vector<u8> bytes(data.checkpoint.checkpoint_pages.size() * 4096ull);
		u32 unreadable = 0;
		for (usz i = 0; i < data.checkpoint.checkpoint_pages.size(); i++)
			unreadable += !vm::try_access(data.checkpoint.checkpoint_pages[i], bytes.data() + i * 4096, 4096, false);
		std::string lwmutexes;
		for (u32 address : sampled_lwmutexes)
		{
			std::array<be_t<u32>, 5> w{};
			const bool read = vm::try_access(address, w.data(), sizeof(w), false);
			if (!lwmutexes.empty())
				lwmutexes += ',';
			lwmutexes += fmt::format("[%u,%s,%u,%u,%u,%u,%u]", address, json_bool(read), u32{w[0]}, u32{w[1]}, u32{w[2]}, u32{w[3]}, u32{w[4]});
		}
		bool mutex_read = false, mutex_waiters = false;
		u32 mutex_owner = 0, mutex_locks = 0;
		if (auto* mutex = idm::check_unlocked<lv2_obj, lv2_mutex>(sampled_mutex_id))
		{
			std::shared_lock object_lock(mutex->mutex, std::try_to_lock);
			if (object_lock.owns_lock())
			{
				const auto control = mutex->control.load();
				mutex_owner = control.owner;
				mutex_waiters = control.sq != nullptr;
				mutex_locks = mutex->lock_count.load();
				mutex_read = true;
			}
		}
		const u64 capture_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(session_clock::now() - begun).count();
		const auto stem = data.checkpoint.capture_directory / fmt::format("tick-probe-%u-%s", index, phase);
		const auto bin_utf8 = std::filesystem::path(stem.wstring() + L".bin").u8string();
		fs::file out(std::string(bin_utf8.begin(), bin_utf8.end()), fs::write_new + fs::isfile);
		bool written = !!out;
		const u32 count = static_cast<u32>(data.checkpoint.checkpoint_pages.size());
		const u32 cr = ppu.cr.pack();
		const std::array<u8, 4> xer{u8{ppu.xer.so}, u8{ppu.xer.ov}, u8{ppu.xer.ca}, ppu.xer.cnt};
		written = written && out.write("DRTPB001", 8) == 8 && out.write(&index, 4) == 4 && out.write(&count, 4) == 4 &&
		          out.write(data.checkpoint.checkpoint_pages.data(), count * 4ull) == count * 4ull && out.write(bytes.data(), bytes.size()) == bytes.size() &&
		          out.write(ppu.gpr, sizeof(ppu.gpr)) == sizeof(ppu.gpr) && out.write(ppu.fpr, sizeof(ppu.fpr)) == sizeof(ppu.fpr) &&
		          out.write(ppu.vr, sizeof(ppu.vr)) == sizeof(ppu.vr) && out.write(&cr, 4) == 4 && out.write(&ppu.lr, 8) == 8 &&
		          out.write(&ppu.ctr, 8) == 8 && out.write(xer.data(), 4) == 4 && out.write(&ppu.cia, 4) == 4;
		const auto row = fmt::format(
			"{\"type\":\"probe\",\"index\":%u,\"phase\":\"%s\",\"pages\":%u,\"unreadable_pages\":%u,\"capture_ns\":%u,\"written\":%s,"
			"\"lwmutex\":[%s],\"mutex\":{\"id\":%u,\"read\":%s,\"owner\":%u,\"waiters\":%s,\"lock_count\":%u},\"owner_id\":%u,\"cia\":%u}\n",
			index, phase, count, unreadable, capture_ns, json_bool(written), lwmutexes, sampled_mutex_id, json_bool(mutex_read),
			mutex_owner, json_bool(mutex_waiters), mutex_locks, ppu.id, ppu.cia);
		const auto json_utf8 = std::filesystem::path(stem.wstring() + L".json").u8string();
		fs::file json(std::string(json_utf8.begin(), json_utf8.end()), fs::write_new + fs::isfile);
		if (json)
			json.write(row.data(), row.size());
	}
	catch (...)
	{
		pbd_session_log.error("Tick probe capture failed at %u (%s).", index, phase);
	}
}

void pbd::rollback_session::tick_dispatch::write_roundtrip_record(const std::string& text)
{
	const auto path = (data.checkpoint.capture_directory / "tick-roundtrip.json").u8string();
	fs::file out(std::string(path.begin(), path.end()), fs::write_new + fs::isfile);
	if (out)
		out.write(text.data(), text.size());
}

void pbd::rollback_session::tick_dispatch::capture_roundtrip_origin()
{
	if (data.roundtrip.active && index == data.checkpoint.tick_hold_index && data.roundtrip.phase == 0)
	{
		// The verification snapshot at boundary N: tick write set, owner context and latch input state.
		data.roundtrip.snapshot_ok = read_pages(data.checkpoint.checkpoint_pages, data.roundtrip.pages_n);
		data.roundtrip.context_n = read_context(ppu);
		data.roundtrip.previous_held_n = data.previous_held;
		data.roundtrip.counter_n = row.before_counter;
		data.roundtrip.clock.clear();
		if (data.roundtrip.snapshot_ok)
		{
			// Identity restore: write the snapshot back at the same boundary, then read it back.
			std::vector<u8> readback;
			data.roundtrip.identity_ok = data.checkpoint.memory.write_pages_voice_guarded(ppu, data.checkpoint.checkpoint_pages, data.roundtrip.pages_n, data.roundtrip.identity_written) &&
			                             read_pages(data.checkpoint.checkpoint_pages, readback);
			if (data.roundtrip.identity_ok)
				static_cast<void>(data.checkpoint.memory.byte_differences(data.checkpoint.checkpoint_pages, data.roundtrip.pages_n, readback, data.roundtrip.identity_diff, data.roundtrip.identity_diff_excluded));
			data.roundtrip.identity_context_equal = context_differences(data.roundtrip.context_n, read_context(ppu)).empty();
		}
		data.roundtrip.phase = data.roundtrip.snapshot_ok ? 1 : 3;
		if (!data.roundtrip.snapshot_ok)
			write_roundtrip_record("{\"schema\":1,\"source\":\"tick_roundtrip_stage_b\",\"restored\":false,\"reason\":\"snapshot_read_failed\"}\n");
	}
}

void pbd::rollback_session::tick_dispatch::resume_roundtrip_tick()
{
	write_context(ppu, data.roundtrip.context_n);
	data.previous_held = data.roundtrip.previous_held_n;
	// Discard the undone N+1 entry; the frozen trace keeps the first row N.
	row = {};
	data.update_active = false;
	data.roundtrip.repeat_row = {};
	data.roundtrip.repeat_row.entry_seen = true;
	data.roundtrip.repeat_row.entry_r1 = ppu.gpr[1];
	data.roundtrip.repeat_row.before_read = true;
	data.roundtrip.repeat_row.before_counter = data.roundtrip.counter_n;
	data.roundtrip.repeat_index = index - 1;
	data.roundtrip.repeat_active = true;
	g_pbd_replay_timing.store(true, std::memory_order_relaxed);
	g_pbd_replay_frame.store(!data.peer.enabled || (data.correction.withhold_exact && data.correction.replay_skip_draws), std::memory_order_relaxed);
	pbd::replay_pacing::skip_draws.store(data.correction.replay_skip_draws ? 1u : 0u, std::memory_order_relaxed);
	data.stage = 1;
	pbd::ordinary_metrics::rewind_frames(data.roundtrip.counter_n);
	// The re-executed tick presents one more guest flip; keep flip indices aligned to game frames.
	pbd::frame_presentation::flip_base.fetch_sub(1, std::memory_order_release);
	pbd::frame_presentation::mark_input(data.roundtrip.counter_n);
	data.roundtrip.clock_replay = 0;
}

bool pbd::rollback_session::tick_dispatch::restore_roundtrip_origin()
{
	if (data.roundtrip.active && index == data.checkpoint.tick_hold_index + 1 && data.roundtrip.phase == 1)
	{
		const auto restore_begin = session_clock::now();
		data.roundtrip.locks_free = roundtrip_locks_free();
		const bool s1_ok = read_pages(data.checkpoint.checkpoint_pages, data.roundtrip.pages_s1);
		data.roundtrip.context_s1 = read_context(ppu);
		data.roundtrip.saved_battle_counters = data.tick_at(index).battle_counters;
		if (!data.roundtrip.locks_free || !s1_ok || data.roundtrip.clock_error)
		{
			// Fail closed before any write: continue live without restore.
			data.roundtrip.phase = 3;
			write_roundtrip_record(fmt::format("{\"schema\":1,\"source\":\"tick_roundtrip_stage_b\",\"restored\":false,\"reason\":\"%s\"}\n",
				!s1_ok ? "s1_read_failed" : data.roundtrip.clock_error ? "clock_journal_overflow" :
																		 "locks_busy_before_restore"));
		}
		else if (!data.checkpoint.memory.write_pages_voice_guarded(ppu, data.checkpoint.checkpoint_pages, data.roundtrip.pages_n, data.roundtrip.restored_bytes))
		{
			data.roundtrip.phase = 3;
			write_roundtrip_record("{\"schema\":1,\"source\":\"tick_roundtrip_stage_b\",\"restored\":\"partial\",\"reason\":\"restore_write_failed\"}\n");
			fail("roundtrip_restore_write_failed");
			return true;
		}
		else
		{
			// Read back what was restored before releasing anything to execution.
			std::vector<u8> readback;
			data.roundtrip.readback_ok = read_pages(data.checkpoint.checkpoint_pages, readback);
			if (data.roundtrip.readback_ok)
				static_cast<void>(data.checkpoint.memory.byte_differences(data.checkpoint.checkpoint_pages, data.roundtrip.pages_n, readback, data.roundtrip.readback_diff, data.roundtrip.readback_diff_excluded));
			resume_roundtrip_tick();
			data.roundtrip.phase = 2;
			data.roundtrip.pass = data.roundtrip.branch ? 1 : 0;
		}
		data.roundtrip.restore_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(session_clock::now() - restore_begin).count();
	}
	return false;
}

bool pbd::rollback_session::tick_dispatch::repeat_branch_input()
{
	if (data.roundtrip.branch && data.roundtrip.pass == 1)
	{
		// The branch re-execution (different input) reached N+1. Record it, restore to N again
		// and re-execute tick N with the tape input (correction). Any rejection fails closed.
		const bool wrong_ok = read_pages(data.checkpoint.checkpoint_pages, data.roundtrip.pages_wrong);
		data.roundtrip.context_wrong = read_context(ppu);
		data.roundtrip.battle_counters_mismatch = data.tick_at(index).battle_counters;
		data.roundtrip.wrong_repeat_completed = data.roundtrip.repeat_completed;
		data.roundtrip.wrong_injected = {u32{data.roundtrip.repeat_row.ports[0].injected_r3}, u32{data.roundtrip.repeat_row.ports[1].injected_r3}};
		u64 second_written = 0;
		if (!wrong_ok || !roundtrip_locks_free() || data.roundtrip.clock_error || data.roundtrip.clock_replay != data.roundtrip.clock.size() ||
			!data.checkpoint.memory.write_pages_voice_guarded(ppu, data.checkpoint.checkpoint_pages, data.roundtrip.pages_n, second_written))
		{
			data.roundtrip.phase = 3;
			fail("branch_second_restore_rejected");
			return true;
		}
		resume_roundtrip_tick();
		data.roundtrip.repeat_completed = false;
		data.roundtrip.pass = 2;
		return true;
	}
	return false;
}
