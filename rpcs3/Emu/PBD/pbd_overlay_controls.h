#pragma once

#include <type_traits>

template <typename KeepPeer>
struct pbd::rollback_session::overlay_controls : boundary_call
{
	const u32& index;
	const KeepPeer& keep_peer;
	bool practice_menu = false;
	session_clock::time_point chord_from{};

	replay_control_result run()
	{
		if (!data.game_overlay.enabled)
			return replay_control_result::go;
		practice_menu = data.practice.menu.open || data.practice.dummy.state == dummy_state::confirm;

		// Also at every boundary, so the mute ends with the selection even when no local sample follows it at once.
		lobby_selection_audio();

		// A hidden selection owns this PC's input while it runs: physical presses do not navigate the loading card, and the claim
		// stays off so the selector's already published owned input reaches the stock handlers. A repick shows no card: a panel
		// shown over its open select takes the controller as in any in-game selection, and pauses its steering meanwhile.

		// Between games and on the post-set panel no sample is admitted, so the leave chord never reaches filter_local_sample and
		// no completed row checks the stop file. A lobby set reads both here instead: Start + Select held for the same 2 s, or the
		// launcher's stop file, ends the session as user_stopped, as either does during a game. Nothing read here reaches the game.

		poll_command(false);
		if (practice_menu && !match_prompt(data.game_overlay.panel) && !data.game_overlay.claim && !data.game_overlay.ignore_until_release)
			return replay_control_result::go;
		read_pad();
		bool held = false;
		for (u32 polls = 0; data.game_overlay.panel.visible || (data.lobby.set && data.lobby.set->complete() && data.lobby.set->healthy()); polls++)
		{
			if (!session_runtime.m_active.load())
				return replay_control_result::stopped;
			if (!held)
			{
				held = true;
				vm::temporary_unlock(ppu);
			}
			if (!data.game_overlay.audio_muted)
			{
				pbd::player_volume::mute();
				data.game_overlay.audio_muted = true;
			}
			if (polls % 20 == 0)
				rsx::set_native_ui_flip();
			if (pbd_emulation_stopping(ppu))
				return replay_control_result::stopped;
			if (!keep_peer())
				return replay_control_result::stopped;
			// The caller's own finish after a stopped result then changes nothing: the first end keeps its reason.
			if (lobby_leave_asked(polls))
			{
				session_runtime.finish(user_stopped);
				return replay_control_result::stopped;
			}
			std::this_thread::sleep_for(std::chrono::milliseconds(5));
			poll_command(false);
			read_pad();
			data.overlay_poll_ns.store(data.now_ns() | 1);
		}
		// A hidden selection still running keeps the game muted; lobby_selection_audio restores it when the selection ends.
		if (data.game_overlay.audio_muted && !data.lobby.selection_audio_muted)
		{
			pbd::player_volume::restore();
			data.game_overlay.audio_muted = false;
		}
		// The panel is hidden here. The press that closed it keeps the claim until released, except while a hidden selection
		// (begun by the command that ended this hold) owns the input.
		data.game_overlay.claim = data.game_overlay.ignore_until_release && !selection_owns_input();
		if (!session_runtime.m_active.load())
			return replay_control_result::stopped;
		return replay_control_result::go;
	}

	bool match_prompt(const pbd::game_overlay::snapshot& panel)
	{
		return pbd::practice::match_prompt::interrupts(panel, data.practice.rewind, static_cast<bool>(data.peer.enabled));
	}

	void poll_command(bool force)
	{
		const auto now = session_clock::now();
		if (!force && now < data.game_overlay.next_poll)
			return;
		// Poll inside one 60 Hz input frame so opening or replacing a panel cannot expose a stale press to the guest.
		data.game_overlay.next_poll = now + std::chrono::milliseconds(8);
		lobby_poll_command();
		if (const auto refused = pbd::search_badge::poll(data.game_overlay.search, std::chrono::steady_clock::now()); !refused.empty())
			pbd_session_log.error("Search badge file refused: %s.", refused);
		// The launcher publishes by renaming a new file over menu.pbd, and Windows refuses that rename while another process has
		// the old file open, which fails the launcher's menu. A file whose write time has not changed since it last parsed is
		// therefore not opened again. Every publish writes a new file, so its write time differs from the one it replaces.
		std::error_code written_error;
		const auto written = std::filesystem::last_write_time(data.game_overlay.command, written_error);
		if (!written_error && data.game_overlay.read_valid && written == data.game_overlay.read_written)
			return;
		auto parsed = pbd::game_overlay::read(data.game_overlay.command);
		if (!parsed.value)
		{
			if (parsed.error != "unavailable" && parsed.error != data.game_overlay.last_error)
				pbd_session_log.error("In-game menu command refused: %s.", parsed.error);
			data.game_overlay.last_error = parsed.error;
			return;
		}
		if (practice_menu && parsed.value->visible && !match_prompt(*parsed.value))
			return;
		// Taken before the read: a file replaced in between is newer than this stamp and is read again on the next poll.
		data.game_overlay.read_valid = !written_error;
		data.game_overlay.read_written = written;
		data.game_overlay.last_error.clear();
		if (parsed.value->generation <= data.game_overlay.panel.generation)
			return;
		apply_panel(std::move(*parsed.value));
	}

	void apply_panel(pbd::game_overlay::snapshot&& panel)
	{
		// The notices' pictures and plates the launcher finished after this game started (pbd::notice_art::retry).
		if (const auto art = pbd::notice_art::retry(std::chrono::steady_clock::now()))
			pbd_session_log.notice("Notice pictures: %u of %u read on a later look.", art->found, pbd::notice_art::picture_count);
		const bool was_visible = data.game_overlay.panel.visible;
		// A republish of the shown panel with the same rows (a countdown ticking in a row's detail) keeps the player's
		// highlight and does not take the controller again (pbd::game_overlay::continues).
		const bool continuing = pbd::game_overlay::continues(data.game_overlay.panel, panel);
		const u32 file_selected = panel.selected;
		const u32 highlight = pbd::game_overlay::republished_highlight(data.game_overlay.panel, data.game_overlay.file_selected, panel);
		data.game_overlay.panel = std::move(panel);
		data.game_overlay.panel.selected = highlight;
		data.game_overlay.file_selected = file_selected;
		if (data.game_overlay.panel.visible && data.lobby.selection && data.lobby.selection->hidden())
		{
			data.lobby.set->refuse("selection_menu_conflict", "The lobby selection was interrupted by another menu.");
			session_runtime.finish(guard_failed);
			return;
		}
		// The parser range-checks selected= only on a shown panel (nothing can be chosen on a hidden one), so a hidden panel
		// with rows can carry any value. Bound it before it indexes the rows.
		if (data.game_overlay.panel.row_count && (data.game_overlay.panel.selected >= data.game_overlay.panel.row_count ||
													 !data.game_overlay.panel.rows[data.game_overlay.panel.selected].enabled))
		{
			for (u32 row = 0; row < data.game_overlay.panel.row_count; row++)
				if (data.game_overlay.panel.rows[row].enabled)
				{
					data.game_overlay.panel.selected = row;
					break;
				}
		}
		// A shown panel claims at once. A hidden one that replaces a shown one in a peer session keeps the hold too; the read
		// that follows (read_pad) claims only while a button is still down, and a hidden selection still owns the sample. A
		// continuing panel already holds the controller (it is shown), and the press the player is making belongs to it.
		if (!continuing && pbd::game_overlay::takes_controller(was_visible, data.game_overlay.panel.visible, data.peer.enabled))
		{
			data.game_overlay.ignore_until_release = true;
			if (data.game_overlay.panel.visible)
				data.game_overlay.claim = true;
		}
		publish_game_overlay();
		if (const auto carried = pbd::lobby_set::menu_panel_applied(data.game_overlay.panel, data.lobby.pending_command, data.lobby.menu_generation))
			lobby_menu_command(*carried);
		// A countdown republishes once a second; only a new panel is worth a line.
		if (!continuing)
			pbd_session_log.notice("In-game menu generation %llu: %s (%u row(s)).", static_cast<unsigned long long>(data.game_overlay.panel.generation),
				data.game_overlay.panel.visible ? "shown" : "hidden", data.game_overlay.panel.row_count);
	}

	bool emit(std::string_view action)
	{
		for (u32 attempt = 0; attempt < 4; attempt++)
		{
			const u64 serial = ++data.game_overlay.event_serial;
			const auto path = data.game_overlay.dir / fmt::format("event-%u-%u.pbd", data.game_overlay.panel.generation, serial);
			const auto u8path = path.u8string();
			fs::file out(std::string(u8path.begin(), u8path.end()), fs::write_new + fs::isfile);
			if (!out)
				continue;
			const auto text = pbd::game_overlay::event_text(data.game_overlay.panel.generation, serial, action,
				data.game_overlay.panel.selected);
			if (out.write(text.data(), text.size()) == text.size())
				return true;
			break;
		}
		pbd_session_log.error("In-game menu could not publish its %s choice.", action);
		return false;
	}

	void move(s32 direction)
	{
		const u32 count = data.game_overlay.panel.row_count;
		if (!count)
			return;
		for (u32 n = 0, row = data.game_overlay.panel.selected; n < count; n++)
		{
			row = static_cast<u32>((static_cast<s32>(row) + direction + static_cast<s32>(count)) % static_cast<s32>(count));
			if (data.game_overlay.panel.rows[row].enabled)
			{
				data.game_overlay.panel.selected = row;
				publish_game_overlay();
				return;
			}
		}
	}

	bool selection_owns_input()
	{
		return data.lobby.selection && data.lobby.selection->hidden();
	}

	void read_pad()
	{
		const u64 now_ms = static_cast<u64>(std::chrono::duration_cast<std::chrono::milliseconds>(
			std::chrono::system_clock::now().time_since_epoch())
				.count());
		if (data.game_overlay.panel.visible && match_prompt(data.game_overlay.panel) &&
			pbd::practice::match_prompt::expired(data.game_overlay.panel, now_ms))
		{
			data.game_overlay.panel.visible = false;
			data.game_overlay.ignore_until_release = true;
			publish_game_overlay();
		}
		const u16 pad = pbd_local_pad_word();
		// The release is watched in the selection too: a press held from the panel into the next game's selection would
		// otherwise keep its hold through the whole selection and claim the controller again at the first battle frames.
		const auto claim = pbd::game_overlay::read_claim(data.game_overlay.ignore_until_release, pad, data.game_overlay.panel.visible,
			selection_owns_input());
		data.game_overlay.ignore_until_release = claim.ignore_until_release;
		data.game_overlay.claim = claim.claimed;
		data.game_overlay.hold_buttons = pbd::game_overlay::hold_buttons(data.game_overlay.hold_buttons, pad, claim);
		if (!claim.navigate)
		{
			data.game_overlay.pad_prev = pad;
			return;
		}
		const u16 pressed = static_cast<u16>((pad & ~data.game_overlay.pad_prev) |
											 (pbd::game_overlay::take_back(data.game_overlay.panel.generation) ? 0x0020 : 0));
		if (pressed & 0x1000)
			move(-1);
		else if (pressed & 0x4000)
			move(1);
		else if (pressed & 0x0040)
		{
			const bool enabled = !data.game_overlay.panel.row_count || data.game_overlay.panel.rows[data.game_overlay.panel.selected].enabled;
			if (enabled && emit("confirm") && data.game_overlay.panel.close_on_action)
			{
				data.game_overlay.panel.visible = false;
				data.game_overlay.ignore_until_release = true;
				publish_game_overlay();
			}
		}
		else if (pressed & 0x0020)
		{
			if (emit("back") && data.game_overlay.panel.close_on_action)
			{
				data.game_overlay.panel.visible = false;
				data.game_overlay.ignore_until_release = true;
				publish_game_overlay();
			}
		}
		data.game_overlay.pad_prev = pad;
	}

	bool lobby_leave_asked(u32 polls)
	{
		if (!data.lobby.set || !data.session.enabled)
			return false;
		// The chord completed on the last samples before this hold; their completed row is not reached while it holds.
		if (data.session.pending_end == user_stopped)
			return true;
		const u16 pad = pbd_local_pad_word();
		const bool chord = (pad & (session_select_bit | session_start_bit)) == (session_select_bit | session_start_bit);
		const auto now = session_clock::now();
		if (!chord)
			chord_from = {};
		else if (chord_from == session_clock::time_point{})
			chord_from = now;
		const u32 hold_ms = chord ? static_cast<u32>(std::chrono::duration_cast<std::chrono::milliseconds>(now - chord_from).count()) : 0;
		if (data.live_stats)
			data.live_pub.leave((pad & session_select_bit) != 0, std::min(hold_ms, pbd::live_stats::leave_hold_total_ms));
		if (hold_ms >= pbd::live_stats::leave_hold_total_ms && data.session.pending_end == collecting)
		{
			data.session.pending_end = user_stopped;
			pbd_session_log.warning("Start + Select held for %u ms while the in-game menu held boundary %u: the player leaves the session.", hold_ms, index);
			return true;
		}
		return !data.stop_file.empty() && polls % 200 == 0 && fs::is_file(data.stop_file);
	}
};
