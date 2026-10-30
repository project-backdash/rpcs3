#pragma once

// The steps whose return types are deduced, or that take any callable or slot, are called from several of the
// latch's source files, so every caller sees their definitions here.

inline auto pbd::rollback_session::boundary_call::peer_first_unusable(u32 now)
{
	u32 u = data.peer.scan_from;
	while (u < now && peer_usable(u, now))
		u++;
	return u;
}

// The held lease may cover a complete restore transaction, including its host cursor and renderer bridge. The core
// never reacquires it: nested acquisition would wait for the caller's own restore bit and then refuse. Its owner and
// voice-history checks still precede every write; the lease alone does not certify foreign or kernel state closure.
bool pbd::rollback_session::boundary_call::restore_checkpoint_memory_leased(const checkpoint_voice_lease& voice_lease,
	ring_entry& entry, u32 from, auto&& apply_undo, checkpoint_restore_options options,
	checkpoint_restore_result& result)
{
	if (!voice_lease.owns(ppu))
	{
		result.voice_refused = true;
		return false;
	}
	if (!pbd::fn_hook::voice_restore_history_ok(entry.voice_checkpoint_token, entry.index, from))
	{
		result.voice_history_refused = true;
		return false;
	}
	const auto rewind = [&]
	{
		const auto begin = session_clock::now();
		if (options.count_present_restore && rsx::get_current_renderer() && rsx::get_current_renderer()->ctrl)
		{
			pbd::replay_pacing::restores_started.fetch_add(1, std::memory_order_acq_rel);
			result.restore_counted = true;
		}
		result.started = true;
		result.undo_ok = apply_undo();
		if (result.undo_ok)
			result.write_ok = data.checkpoint.memory.write_pages_except(data.checkpoint.checkpoint_pages, entry.pages, result.written);
		result.rewind_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(session_clock::now() - begin).count();
	};
	bool suspend = data.correction.restore_suspend;
	if (suspend && (ppu.state & cpu_flag::wait))
	{
		if (ppu.check_state())
		{
			suspend = false;
			result.suspend_skipped = "stop_pending";
		}
		else if (ppu.state & cpu_flag::wait)
		{
			suspend = false;
			result.suspend_skipped = "still_waiting";
		}
	}
	if (suspend)
	{
		const auto begin = session_clock::now();
		cpu_thread::suspend_all(&ppu, {}, rewind);
		result.suspend_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(session_clock::now() - begin).count();
	}
	else
		rewind();
	if (!result.undo_ok || !result.write_ok)
	{
		if (result.restore_counted)
			pbd::replay_pacing::restores_started.fetch_sub(1, std::memory_order_acq_rel);
		return false;
	}
	result.readback_checked = options.readback;
	if (options.readback)
	{
		std::vector<u8> bytes;
		const auto begin = session_clock::now();
		result.readback_ok = read_pages(data.checkpoint.checkpoint_pages, bytes);
		if (result.readback_ok)
			result.readback_spans = data.checkpoint.memory.byte_differences(data.checkpoint.checkpoint_pages, entry.pages, bytes, result.readback_diff, result.readback_excluded);
		result.readback_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(session_clock::now() - begin).count();
	}
	write_context(ppu, entry.context);
	return !options.strict_readback || (result.readback_ok && result.readback_spans.empty());
}

// Both correction and takeover keep their existing lease scope. Their bounded undo traversal, CPU suspension,
// static exclusions, readback and owner restoration all enter the same guarded core in the same order.
bool pbd::rollback_session::boundary_call::restore_checkpoint_memory(ring_entry& entry, u32 from, auto&& apply_undo,
	checkpoint_restore_options options, checkpoint_restore_result& result)
{
	checkpoint_voice_lease voice_lease(ppu);
	return restore_checkpoint_memory_leased(voice_lease, entry, from, apply_undo, options, result);
}

// A slot with moves from the move list plays its next move (practice_dummy_slot::move_tape); otherwise its recording.
auto pbd::rollback_session::boundary_call::practice_slot_frames(const auto& slot)
{
	if (!slot.moves.empty())
		return std::span<const u16>(slot.move_sequence);
	return pbd::practice::controls::playback_frames(pbd::practice::controls::take_body(slot.frames, slot.stop_chord_frames),
		slot.leading_neutral_frames, slot.trailing_neutral_frames, slot.trim_neutral);
}

// A slot can be scheduled as an action only with at least one input left to play.
auto pbd::rollback_session::boundary_call::practice_slot_playable(const auto& slot)
{
	return slot.enabled && pbd::practice::controls::has_input(practice_slot_frames(slot));
}

// Writes an accepted take to practice-dummy-slot-<n>.json:
// every recorded word (the raw take, stop chord included), the range playback uses and readable run-length steps.
// Schema 2: the words are opponent-relative (forward in the Right bit), so the steps read in Tekken notation as they
// are; "side" is the dummy's side on the take's first word. Diagnostic only; a failed write is logged and changes
// nothing else.
void pbd::rollback_session::boundary_call::practice_save_input_sequence(const auto& slot, u32 log_slot, const std::string& name,
	u32 index)
{
	if (data.checkpoint.capture_directory.empty() || data.practice.dummy.diagnostic_write_failed)
		return;
	try
	{
		namespace movement = pbd::practice::movement;
		const auto played = practice_slot_frames(slot);
		const u32 first = played.empty() ? 0u : static_cast<u32>(played.data() - slot.frames.data());
		std::string words;
		words.reserve(slot.frames.size() * 9);
		for (const u16 word : slot.frames)
		{
			if (!words.empty())
				words += ",";
			words += "\"" + movement::word_hex(word) + "\"";
		}
		const auto side = movement::screen_side::left;
		const std::string text = fmt::format("{\"schema\":2,\"slot\":%u,\"name\":\"%s\",\"boundary\":%u,\"recording\":%u,\"side\":\"%s\","
											 "\"frames\":%u,\"trim\":%s,\"leading_neutral_frames\":%u,\"trailing_neutral_frames\":%u,\"stop_chord_frames\":%u,\"playback_first\":%u,\"playback_end\":%u,"
											 "\"playback_frames\":%u,\"digest\":\"%016x\",\"steps\":%s,\"words\":[%s]}\n",
			log_slot, name, index, data.practice.dummy.recordings, movement::side_text(slot.side), static_cast<u32>(slot.frames.size()),
			json_bool(slot.trim_neutral), slot.leading_neutral_frames, slot.trailing_neutral_frames, slot.stop_chord_frames, first,
			first + static_cast<u32>(played.size()), static_cast<u32>(played.size()), practice_dummy_digest(slot.frames),
			movement::steps_json(slot.frames, side), words);
		const std::string file = fmt::format("practice-dummy-slot-%u.json", log_slot);
		const bool written = write_diagnostic_file(data.checkpoint.capture_directory / file, text);
		if (!written)
		{
			data.practice.dummy.diagnostic_write_failed = true;
			pbd_session_log.error("Dummy recording diagnostics disabled: slot %u could not be written.", log_slot);
		}
		practice_dummy_log(fmt::format("{\"type\":\"dummy_tape_saved\",\"boundary\":%u,\"slot\":%u,\"file\":\"%s\",\"written\":%s}\n",
			index, log_slot, file, json_bool(written)));
	}
	catch (...)
	{
		data.practice.dummy.diagnostic_write_failed = true;
		pbd_session_log.error("Dummy recording diagnostics disabled: slot %u publication failed.", log_slot);
	}
}

// The take in progress belongs to the action slot that chose Record. The dummy log numbers action slots from 1.
inline auto& pbd::rollback_session::boundary_call::practice_record_slot()
{
	return data.practice.dummy.slots[data.practice.dummy.slot_index];
}

inline auto pbd::rollback_session::boundary_call::practice_pending_body()
{
	return pbd::practice::controls::take_body(data.practice.dummy.pending.words, data.practice.dummy.pending.chord);
}

inline auto pbd::rollback_session::boundary_call::practice_pending_frames()
{
	return pbd::practice::controls::playback_frames(practice_pending_body(), data.practice.dummy.pending.leading,
		data.practice.dummy.pending.trailing, practice_record_slot().trim_neutral);
}

// The dummy's side, which turns relative words (a recorded take, the movement loops, a turn-around tap) into the pad
// directions DR will read as the same toward / away input. When no reading answers at this boundary the last side read
// stands in, and before any reading the right: the dummy is P2, and the round start and the Centre and wall restarts
// frame the pair from P1's side, which puts the dummy on the right.
inline auto pbd::rollback_session::boundary_call::practice_dummy_side()
{
	return practice_side_of(1).value_or(data.practice.last_sides[1].value_or(pbd::practice::movement::screen_side::right));
}

// The movement of the training mode in view: Defense's Movement Between Actions, or Offense's Movement Technique.
inline auto pbd::rollback_session::boundary_call::practice_active_movement()
{
	return data.practice.training_mode == pbd::practice::training_mode::offense ? data.practice.offense.movement : data.practice.movement.preset;
}

// The name of what the movement tape plays: Offense's reaction input when Action After a Hit or Block is one, otherwise
// the training mode's movement.
inline const char* pbd::rollback_session::boundary_call::practice_movement_label()
{
	if (data.practice.training_mode == pbd::practice::training_mode::offense && data.practice.offense.after == pbd::practice::after_contact::movement)
		return pbd::practice::movement::reaction_input_name(data.practice.offense.reaction_input);
	return pbd::practice::movement::preset_name(practice_active_movement());
}

// Offense's Action After a Hit or Block plays moves from the move list (pbd::practice::dummy_move): its own contact watch
// and playback drive the dummy, and the recorded and movement reactions stand aside.
inline bool pbd::rollback_session::boundary_call::practice_move_reactions_on()
{
	return data.practice.training_mode == pbd::practice::training_mode::offense && pbd::practice::dummy_move::current_settings.enabled();
}

// Whether the movement plays with no action to play: a continuous one, or any one played once after a contact. A
// one-shot plays only before an action, so otherwise there is nothing to play and the dummy follows the game's own
// Dummy Action.
inline auto pbd::rollback_session::boundary_call::practice_movement_alone()
{
	return !data.practice.movement.input_sequence.empty() && (data.practice.dummy.once || pbd::practice::movement::plays_alone(data.practice.movement.input_sequence));
}

// The playback in progress is Offense's Dummy Action reaction: that one slot, played once.
inline auto pbd::rollback_session::boundary_call::practice_dummy_reacting()
{
	return data.practice.dummy.once && data.practice.dummy.reaction;
}

inline auto pbd::rollback_session::practice_steps::lifecycle_scope()
{
	const u64 generation = (u64{data.session.matches} << 32) | (u64{data.session.match_start} + 1);
	return pbd::game_lifecycle::observe_scope(generation, generation);
}

inline auto pbd::rollback_session::practice_steps::practice_slot_name(u32 slot)
{
	return fmt::format("Dummy Action %u", slot + 1);
}
