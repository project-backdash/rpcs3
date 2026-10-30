// Replay rewind: the in-memory checkpoints a watched replay keeps (pbd_replay_rewind.h), their capture at the production
// checkpoint point, and the rewinds, fast forwards and round jumps built on them. A restore goes through restore_anchor, the
// same path Take Control's Retry and Return use; a forward move runs the recorded inputs at the fast vblank rate with the
// sound off, which never changes what the game computes.
#include "stdafx.h"
#include "pbd_rollback_session_boundary.h"
#include "Emu/PBD/pbd_replay_control_buttons.h"
#include "Emu/PBD/pbd_replay_rewind.h"
#include "Emu/RSX/Overlays/overlay_message.h"

namespace
{
	template <typename Storage>
	std::vector<pbd::replay_rewind::point> rewind_points_of(const Storage& data)
	{
		std::vector<pbd::replay_rewind::point> out;
		out.reserve(data.rewind.points.size());
		for (const auto& p : data.rewind.points)
			out.push_back({p->anchor.index, p->anchor.match, p->anchor.frame, p->round, p->pinned, p->bytes});
		return out;
	}

	// What one checkpoint holds in memory: its static pages, its journal and the journal's index.
	template <typename Anchor>
	u64 anchor_bytes(const Anchor& a)
	{
		return a.checkpoint.pages.capacity() + a.journal_bytes.capacity() +
		       a.journal_pages.capacity() * sizeof(u32) + a.journal_index.size() * 48 + 4096;
	}

	template <typename Point>
	bool materialize(Point& point, bool static_pages = true)
	{
		if (!point.sealed)
			return true;
		point.sealed->wait();
		if (!point.journal_loaded)
		{
			std::vector<u8> journal;
			if (!point.sealed->journal.expand(journal))
				return false;
			point.anchor.journal_bytes = std::move(journal);
			point.sealed->journal.bytes = std::vector<u8>{};
			point.journal_loaded = true;
		}
		if (static_pages)
		{
			std::vector<u8> pages;
			if (!point.sealed->pages.expand(pages))
				return false;
			point.anchor.checkpoint.pages = std::move(pages);
			point.sealed.reset();
		}
		point.bytes = anchor_bytes(point.anchor) + (point.sealed ? point.sealed->bytes() : 0);
		return true;
	}

	template <typename Anchor>
	void clear_journal(Anchor& a)
	{
		a.journal_pages = {};
		a.journal_bytes = {};
		a.journal_index = {};
		a.overflow = false;
	}

	// A restore lands on its checkpoint at normal speed (restore_anchor resets the vblank rate and the sound); a fast forward
	// of an earlier target must not carry on from there.
	template <typename Storage>
	void drop_fast(Storage& data)
	{
		data.replay.fast.active = false;
		data.replay.fast.to_match = 0;
		data.replay.fast.to_frame = 0;
		data.replay.fast.until_match = 0;
		data.takeover.next_round = 0;
		data.takeover.next_match = 0;
	}
} // namespace

// At a first dispatch, after the round tracking: decide whether this boundary takes a checkpoint. The host slot saved at
// the start of this dispatch, before the round tracking, travels with it (a Take Control round-start anchor saves its slot
// after the tracking; restoring that one would count the round twice).
void pbd::rollback_session::boundary_call::rewind_schedule(u32 index, bool fighting, bool round_start)
{
	if (!data.rewind.enabled)
		return;
	if (data.rewind.capture_pending && data.rewind.capture_index != index)
		data.rewind.capture_pending = false;
	if (data.rewind.capture_pending)
		return;
	if (!data.rewind.points.empty() && data.rewind.points.back()->anchor.match != data.session.matches)
		rewind_clear();
	const bool watching = data.session.in_match && data.takeover.phase == pbd::takeover_phase::watching && data.replay.control_port == 0;
	const bool have = !data.rewind.points.empty();
	const u32 newest = have ? data.rewind.points.back()->anchor.index : 0;
	// A round start seen again after a rewind to it already has its pinned checkpoint.
	const bool pinned = round_start && !pbd::replay_rewind::round_choice(rewind_points_of(data), data.session.matches, data.replay.round);
	if (!pbd::replay_rewind::capture_due(watching, fighting, pinned, have, newest, index))
		return;
	data.rewind.capture_pending = true;
	data.rewind.capture_index = index;
	data.rewind.capture_pinned = pinned;
	data.rewind.capture_round = data.replay.round;
	data.rewind.capture_host = data.rewind.dispatch_host;
}

// At the production checkpoint point of the boundary rewind_schedule chose: the new checkpoint becomes the newest, so the
// write-fault handler fills its journal from here on. Sealed predecessors go to the bounded compression worker; only images
// still over budget after compression need thinning.
void pbd::rollback_session::boundary_call::rewind_capture(u32 index)
{
	data.rewind.capture_pending = false;
	std::unique_ptr<rewind_point> point;
	try
	{
		point = std::make_unique<rewind_point>();
	}
	catch (...)
	{
		return;
	}
	if (!capture_anchor(point->anchor, index, data.rewind.capture_host, false))
	{
		const auto text = fmt::format("{\"type\":\"rewind_capture_refused\",\"boundary\":%u,\"reason\":\"%s\"}\n", index,
			data.takeover.refusal);
		data.replay.control_file.write(text.data(), text.size());
		return;
	}
	point->round = data.rewind.capture_round;
	point->pinned = data.rewind.capture_pinned;
	point->bytes = anchor_bytes(point->anchor);
	try
	{
		data.rewind.points.push_back(std::move(point));
	}
	catch (...)
	{
		return;
	}
	data.rewind.captures++;
	g_pbd_takeover_anchor_live.store(true, std::memory_order_release);
	compress_rewind_points();
	u64 total = 0;
	for (const auto& p : data.rewind.points)
		total += p->bytes;
	total = trim_rewind_points(total);
	data.rewind.bytes = total;
	if (data.rewind.captures % 30 == 1 || data.rewind.points.back()->pinned)
	{
		const auto text = fmt::format("{\"type\":\"rewind_point\",\"boundary\":%u,\"frame\":%u,\"round\":%u,\"pinned\":%s,\"points\":%u,\"mib\":%u,\"evictions\":%u}\n",
			index, data.rewind.points.back()->anchor.frame, data.rewind.points.back()->round,
			json_bool(data.rewind.points.back()->pinned), data.rewind.points.size(), total >> 20, data.rewind.evictions);
		data.replay.control_file.write(text.data(), text.size());
	}
}

void pbd::rollback_session::boundary_call::rewind_clear()
{
	data.rewind.capture_pending = false;
	if (data.rewind.points.empty())
		return;
	data.rewind.points.clear();
	data.rewind.bytes = 0;
}

// A Take Control bookmark at boundary `index` was restored: later checkpoints describe moments this replay no longer
// holds. The newest one kept stays valid; its journal already holds every page first written since it.
void pbd::rollback_session::boundary_call::rewind_forget_after(u32 index)
{
	while (!data.rewind.points.empty() && data.rewind.points.back()->anchor.index > index)
	{
		data.rewind.bytes -= std::min(data.rewind.bytes, data.rewind.points.back()->bytes);
		data.rewind.points.pop_back();
	}
	if (!data.rewind.points.empty() && !materialize(*data.rewind.points.back()))
		data.rewind.points.back()->anchor.overflow = true;
}

// Restore the checkpoint for `frame` of `match` (with `round`, that round's pinned start), from boundary `from`. A
// checkpoint a guard refuses (a lifecycle call, a changed scene since it) gives way to the next later one, which is still
// nearer to the moment asked for than staying put. Refusals after the guest was touched end the attempt.
bool pbd::rollback_session::boundary_call::rewind_restore(u32 match, u32 frame, u32 round, u32 from, bool exact)
{
	if (!data.rewind.enabled || data.rewind.points.empty())
		return false;
	const auto points = rewind_points_of(data);
	auto choice = round ? pbd::replay_rewind::round_choice(points, match, round) : std::nullopt;
	if (!choice)
		choice = pbd::replay_rewind::restore_choice(points, match, frame, exact);
	if (!choice)
		return false;
	for (usz k = *choice, tries = 0; k < data.rewind.points.size() && tries < 8; k++, tries++)
	{
		auto& target = data.rewind.points[k]->anchor;
		if (target.index >= from || (exact && target.frame > frame))
			break;
		std::vector<takeover_anchor*> journals;
		pbd::replay_rewind::journal_coverage coverage;
		for (usz j = k; j < data.rewind.points.size(); j++)
		{
			auto& point = *data.rewind.points[j];
			const bool needed = coverage.include(point.anchor.journal_pages);
			if (j != k && !needed && !point.anchor.overflow)
				continue;
			if (!materialize(point, j == k))
			{
				data.takeover.refusal = "checkpoint_decode_failed";
				return false;
			}
			journals.push_back(&data.rewind.points[j]->anchor);
		}
		if (!restore_anchor(target, journals, "rewind", 0, from))
		{
			if (data.session.pending_end == guard_failed)
				return false;
			const std::string_view reason = data.takeover.refusal;
			const bool guard = reason == "session_changed" || reason == "battle_clock_changed" || reason == "mapping_changed" ||
			                   reason == "lifecycle_syscall" || reason == "generator_epoch_changed" || reason == "journal_full" ||
			                   reason == "journal_page_unmapped" || reason == "direct_replay_cursor_unavailable" ||
			                   reason == "voice_history_changed";
			if (!guard)
				return false;
			continue;
		}
		while (data.rewind.points.size() > k + 1)
			data.rewind.points.pop_back();
		clear_journal(target);
		data.rewind.points[k]->bytes = anchor_bytes(target);
		u64 total = 0;
		for (const auto& p : data.rewind.points)
			total += p->bytes;
		data.rewind.bytes = total;
		// Take Control's anchor described a moment after this one, and its journal the pages written since.
		clear_journal(data.takeover.keyframe);
		data.takeover.keyframe.valid = false;
		const auto text = fmt::format("{\"type\":\"rewind_restore\",\"to\":%u,\"frame\":%u,\"from\":%u,\"asked_frame\":%u,\"asked_round\":%u,\"points\":%u,\"tries\":%u}\n",
			target.index, target.frame, from, frame, round, data.rewind.points.size(), tries + 1);
		data.replay.control_file.write(text.data(), text.size());
		return true;
	}
	return false;
}

// The fast forward to `frame` of `match`: the recorded inputs run at the fast vblank rate with the sound off.
void pbd::rollback_session::replay_steps::begin_fast_to(u32 match, u32 frame)
{
	set_pause(false);
	set_speed(0);
	data.replay.fast.to_match = match;
	data.replay.fast.to_frame = frame;
	g_cfg.video.vblank_rate.set(data.replay.fast.rate);
	pbd::replay_pacing::vblank_request.store(data.replay.fast.rate, std::memory_order_release);
	pbd::player_volume::mute();
	data.replay.fast.active = true;
}

// Show `frame` of the current match: forward by running fast to it, backward by restoring the checkpoint at or before it and
// running fast from there. The newest target replaces any earlier one; a paused replay is paused again on arrival.
void pbd::rollback_session::replay_steps::seek_to(u32 frame, bool exact)
{
	if (!data.session.in_match || !data.rewind.enabled)
	{
		rsx::overlays::queue_message(std::string(data.session.in_match ?
													 "Seeking is unavailable here. Use frame steps or reopen the replay." :
													 "Seeking is available once the replay reaches the fight."),
			3'000'000);
		return;
	}
	if (data.takeover.phase != pbd::takeover_phase::watching || data.replay.control_port)
		return;
	const u32 match = data.session.matches;
	const u32 now = match_frame(index);
	const bool paused_after = data.rewind.seeking ? data.rewind.pause_after : data.replay.paused;
	if (frame == now && !data.rewind.seeking)
		return;
	if (frame < now)
	{
		// The restore resets the controller to wait for a release; a held Left must keep repeating instead.
		const u16 held = data.replay.pad_prev;
		const auto repeat = data.replay.viewer.seek_repeat;
		// The restore bridge executes its checkpoint's input once before returning to controls. Start strictly before an
		// exact target, so a back step onto a checkpoint cannot overshoot it by one frame.
		if (!rewind_restore(match, exact && frame ? frame - 1 : frame, 0, index, exact))
		{
			if (data.session.pending_end == guard_failed)
				return;
			log_event("seek_refused", static_cast<s32>(frame));
			rsx::overlays::queue_message(std::string("That moment cannot be restored. Try a later point or reopen the replay."), 3'000'000);
			return;
		}
		drop_fast(data);
		data.replay.pad_prev = held;
		data.replay.viewer.previous = held;
		data.replay.viewer.release_required = false;
		data.replay.viewer.seek_repeat = repeat;
		const u32 landed = data.rewind.points.back()->anchor.frame;
		if (frame > landed)
			begin_fast_to(match, frame);
	}
	else
		begin_fast_to(match, frame);
	data.rewind.seeking = true;
	data.rewind.seek_match = match;
	data.rewind.seek_frame = frame;
	data.rewind.pause_after = paused_after;
	log_event("seek", static_cast<s32>(frame));
}

// Rewind / fast forward by `delta` frames from the pending target, so repeated presses add up. A forward step stops a second
// before the recording runs out, since the replay ends with its inputs.
void pbd::rollback_session::replay_steps::seek_by(s32 delta)
{
	if (!data.session.in_match)
	{
		rsx::overlays::queue_message(std::string("Seeking is available once the replay reaches the fight."), 3'000'000);
		return;
	}
	const u32 now = match_frame(index);
	const u32 base = data.rewind.seeking && data.rewind.seek_match == data.session.matches ? data.rewind.seek_frame : now;
	u32 last = now;
	if (data.lobby.direct_match.enabled())
	{
		const u32 count = data.lobby.direct_match.input_count(), position = data.lobby.direct_match.position();
		if (count > position + pbd::replay_rewind::spacing)
			last = now + (count - position - pbd::replay_rewind::spacing);
	}
	else if (data.extent() > index + pbd::replay_rewind::spacing)
		last = now + static_cast<u32>(data.extent() - index - pbd::replay_rewind::spacing);
	seek_to(pbd::replay_rewind::step_target(base, delta, std::max(last, base)), delta == -1);
}

// The start of `round` of the current match: rebuild from the recorded inputs, using a watched round's pinned checkpoint
// when going back. Older files do not retain every value an independent native initializer needs.
void pbd::rollback_session::replay_steps::go_to_round(u32 round)
{
	if (!data.session.in_match || !round || data.takeover.phase != pbd::takeover_phase::watching || data.replay.control_port)
		return;
	if (data.lobby.direct_match.enabled())
	{
		if (!data.lobby.direct_match.pbd_round_frame(round))
		{
			log_event("round_refused", static_cast<s32>(round));
			rsx::overlays::queue_message(std::string("This replay has no recorded start for that round."), 3'000'000);
			return;
		}
	}
	const u32 match = data.session.matches;
	const bool paused_after = data.rewind.seeking ? data.rewind.pause_after : data.replay.paused;
	if (round > data.replay.round)
	{
		if (data.replay.fast.active)
			return;
		begin_fast_to(0, 0);
		data.takeover.next_round = round;
		data.takeover.next_match = match;
		data.rewind.seeking = true;
		data.rewind.seek_match = match;
		data.rewind.seek_frame = match_frame(index);
		data.rewind.pause_after = paused_after;
		log_event("round", static_cast<s32>(round));
		return;
	}
	u32 frame = 0;
	for (const auto& r : data.replay.viewer_rounds)
		if (r.match == match && r.round == round)
			frame = r.fight;
	for (const auto& r : data.takeover.rounds)
		if (r[0] == match && r[1] == round)
			frame = r[2];
	const u16 held = data.replay.pad_prev;
	if (!rewind_restore(match, frame, round, index))
	{
		if (data.session.pending_end == guard_failed)
			return;
		log_event("round_refused", static_cast<s32>(round));
		rsx::overlays::queue_message(std::string("That round cannot be restored here. Keep watching or reopen the replay."), 3'000'000);
		return;
	}
	drop_fast(data);
	data.replay.pad_prev = held;
	data.replay.viewer.barrier(held);
	data.rewind.seeking = true;
	data.rewind.seek_match = match;
	data.rewind.seek_frame = data.rewind.points.back()->anchor.frame;
	data.rewind.pause_after = paused_after;
	log_event("round", static_cast<s32>(round));
}

// L3 / R3: take that side now. In a fight the attempt starts at this boundary, from a bookmark of it; before one (an intro)
// it starts at the round's first fight boundary.
void pbd::rollback_session::replay_steps::take_over(u32 side)
{
	if (side != 1 && side != 2)
		return;
	if (!data.replay.takeover || !data.session.in_match)
	{
		log_event("take_refused", static_cast<s32>(side));
		rsx::overlays::queue_message(std::string("Take Control is unavailable at this moment. Try once the fight starts."), 3'000'000);
		return;
	}
	data.takeover.null_input = false;
	data.rewind.seeking = false;
	if (fighting_now)
	{
		data.takeover.auto_start = false;
		start_takeover(side);
	}
	else
	{
		arm_takeover(side);
		data.takeover.auto_start = true;
	}
}

// While a seek runs fast: Left / Right still move its target (newest wins), so holding either keeps going. Returns true
// when a restore made at this boundary must leave the dispatch.
bool pbd::rollback_session::replay_steps::seek_poll()
{
	const auto pads = pbd_replay_pad_words();
	const u16 pad = data.game_overlay.claim || pbd::settings_menu::claimed() ? 0 :
	                                                                           static_cast<u16>(pads[0] | pads[1] | keyboard_word());
	const u16 pressed = static_cast<u16>(pad & ~data.replay.viewer.previous);
	data.replay.pad_prev = pad;
	data.replay.pad = 0;
	data.replay.viewer.previous = pad;
	constexpr u16 left = pbd::replay_viewer::left, right = pbd::replay_viewer::right;
	const u16 step = data.replay.viewer.seek_repeat.poll(pad, pressed, left | right, now_ms());
	if (step == left)
		seek_by(-static_cast<s32>(pbd::replay_rewind::step_frames));
	else if (step == right)
		seek_by(static_cast<s32>(pbd::replay_rewind::step_frames));
	return data.takeover.redispatch;
}

// The in-game Settings menu (Esc) pauses the replay while it is open, and closing it resumes. A
// time-up or confirmation prompt keeps its own pause.
void pbd::rollback_session::replay_steps::settings_pause()
{
	const bool claimed = pbd::settings_menu::claimed();
	const bool prompt = data.takeover.phase == pbd::takeover_phase::end_prompt || data.takeover.phase == pbd::takeover_phase::enable_prompt;
	if (claimed && !data.rewind.settings_paused && !data.replay.paused && !prompt)
	{
		data.rewind.settings_paused = true;
		set_pause(true);
		log_event("settings_pause", 1);
	}
	else if (!claimed && data.rewind.settings_paused)
	{
		data.rewind.settings_paused = false;
		if (data.replay.paused && !prompt)
			set_pause(false);
		log_event("settings_pause", 0);
	}
}

void pbd::rollback_session::boundary_call::compress_rewind_points()
{
	if (!data.rewind.compressor)
		try
		{
			data.rewind.compressor = std::make_unique<pbd::replay_checkpoint::compressor>();
		}
		catch (...)
		{
		}
	for (usz k = 0; k + 1 < data.rewind.points.size(); ++k)
	{
		auto& previous = *data.rewind.points[k];
		if (!previous.sealed && data.rewind.compressor)
		{
			previous.bytes = anchor_bytes(previous.anchor);
			previous.sealed = data.rewind.compressor->submit(previous.anchor.checkpoint.pages, previous.anchor.journal_bytes);
			if (previous.sealed)
				previous.journal_loaded = false;
		}
		if (previous.sealed)
		{
			if (previous.sealed->ready.load(std::memory_order_acquire))
				previous.bytes = anchor_bytes(previous.anchor) + previous.sealed->bytes();
		}
	}
}

u64 pbd::rollback_session::boundary_call::trim_rewind_points(u64 total)
{
	// Thin out: fold the chosen checkpoint's journal into the one before it, then drop it.
	while (true)
	{
		const auto gone = pbd::replay_rewind::eviction_choice(rewind_points_of(data), total, data.rewind.budget);
		if (!gone)
			break;
		// Give an unfinished image a chance to shrink at the next capture; playback never waits for compression.
		if ((data.rewind.points[*gone - 1]->sealed && !data.rewind.points[*gone - 1]->sealed->ready.load(std::memory_order_acquire)) ||
			(data.rewind.points[*gone]->sealed && !data.rewind.points[*gone]->sealed->ready.load(std::memory_order_acquire)))
			break;
		if (!materialize(*data.rewind.points[*gone - 1]) || !materialize(*data.rewind.points[*gone]))
			break;
		auto& before = data.rewind.points[*gone - 1]->anchor;
		const auto& folded = data.rewind.points[*gone]->anchor;
		try
		{
			if (folded.overflow)
				before.overflow = true;
			for (usz k = 0; !before.overflow && k < folded.journal_pages.size(); k++)
			{
				const u32 page = folded.journal_pages[k];
				if (before.journal_index.count(page))
					continue;
				const usz slot = before.journal_pages.size();
				const auto from = folded.journal_bytes.begin() + static_cast<std::ptrdiff_t>(k * 4096);
				before.journal_bytes.insert(before.journal_bytes.end(), from, from + 4096);
				before.journal_pages.push_back(page);
				before.journal_index.emplace(page, slot);
			}
			before.journal_bytes.shrink_to_fit();
		}
		catch (...)
		{
			before.overflow = true;
		}
		data.rewind.points[*gone - 1]->bytes = anchor_bytes(before);
		data.rewind.points.erase(data.rewind.points.begin() + static_cast<std::ptrdiff_t>(*gone));
		data.rewind.evictions++;
		total = 0;
		for (const auto& p : data.rewind.points)
			total += p->bytes;
	}

	return total;
}
