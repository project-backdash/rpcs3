// practice_controls_at() and its first steps: the frame's pause and history, the requested reset, and the pause
// menu's native calls, walls, restart and panel.
#include "stdafx.h"
#include "pbd_rollback_session_boundary.h"
#include "../PBD/pbd_dummy_move_runtime.h"
#include "../PBD/pbd_practice_lesson_channel.h"
#include "../PBD/pbd_practice_frames.h"
#include "../PBD/pbd_practice_start.h"
#include "../PBD/pbd_practice_paint.h"

namespace category = pbd::practice::categories;

replay_control_result pbd::rollback_session::boundary_call::practice_controls_at(u32 index)
{
	bool restored = false;
	practice_steps steps{*this, index, restored};
	if (const auto opening = steps.opening_request())
		return *opening;
	if (const auto load = steps.observe_stage_load())
		return *load;
	steps.observe_frame();
	steps.scripted_recording();
	steps.read_pad();
	// Offense with a Movement: the dummy moves on its own, all the time (Action After a Hit or Block: None) or for a
	// moment once it has recovered from the counted hits or blocks (Movement). With a Dummy Action chosen there, that
	// action plays once at that moment instead, the Movement row aside.
	if (!restored)
		practice_offense_begin(index);
	steps.publish();
	if (restored)
		return replay_control_result::restored;
	if (const auto result = steps.apply_reset())
		return *result;
	bool guest_called = false;
	if (const auto stopped = steps.native_calls(guest_called))
		return *stopped;
	// The game's own functions called above run inside this boundary, before the game's frame. A counter one of them
	// moved is not a frame that ran: read as one, the open menu would ask DR to pause again and that START would resume it.
	if (guest_called)
	{
		const auto after = read_practice_pause();
		data.practice.boundary_counter_valid = after.valid;
		data.practice.boundary_counter = after.counter;
	}
	steps.practice_wall_report();
	steps.lesson_boundary();
	steps.update_dummy_moves();
	if (data.practice.dummy.audio_muted)
	{
		pbd::player_volume::restore();
		data.practice.dummy.audio_muted = false;
	}
	if (pbd_emulation_stopping(ppu) || data.session.pending_end == user_stopped)
		return replay_control_result::stopped;
	return replay_control_result::go;
}

void pbd::rollback_session::boundary_call::log_practice_select(u32 index)
{
	// Practice's own select scenes (mode 10, scenes 1-3): a line whenever either select record (handler 0xb0cf0, records
	// at 0x10093f58 + 0x78 per side) or the stage list changes, to map the game's flow from state 3 to the stage list (13).
	if (u32 select_scene = 0, select_subscene = 0, select_mode = 0; read_word(pbd::t5dr::scene_address, select_scene) && select_scene >= 1 && select_scene <= 3 &&
																	read_word(pbd::t5dr::game_mode_address, select_mode) && select_mode == 10 && read_word(pbd::t5dr::subscene_address, select_subscene))
	{
		u32 grid_mode = umax, unlock_high = 0, unlock_low = 0;
		read_word(0x10094048 + 0xf8, grid_mode);
		read_word(0x100934a0, unlock_high);
		read_word(0x100934a4, unlock_low);
		std::string line = fmt::format("\"scene\":%u,\"subscene\":%u,\"mode\":%u,\"grid_mode\":%u,\"unlock\":\"%08x%08x\",\"records\":[",
			select_scene, select_subscene, select_mode, grid_mode, unlock_high, unlock_low);
		bool stage_list = false;
		for (u32 side = 0; side < 2; ++side)
		{
			const u32 base = 0x10093f58 + side * 0x78;
			u32 states = umax, cursor = umax, character = umax, chosen = umax;
			read_word(base + 0x20, states);
			read_word(base, cursor);
			read_word(base + 8, character);
			read_word(base + 0x48, chosen);
			stage_list = stage_list || (states >> 24) == 13;
			line += fmt::format("%s{\"state\":%u,\"b21\":%u,\"b22\":%u,\"port\":%u,\"cursor\":%u,\"character\":%u,\"row\":%u}",
				side ? "," : "", states >> 24, (states >> 16) & 0xff, (states >> 8) & 0xff, states & 0xff, cursor, character, chosen);
		}
		line += "]";
		if (stage_list)
		{
			u32 owner = umax, cursor = umax, list = umax, fresh = umax, old = umax;
			read_word(0x10094048 + 0xf0, owner);
			read_word(0x10094048 + 0xfc, cursor);
			read_word(0x10094048 + 0x104, list);
			read_word(0x10094048 + 0x110, fresh);
			read_word(0x10094048 + 0x114, old);
			line += fmt::format(",\"stage\":{\"owner\":%u,\"cursor\":%u,\"list\":%u,\"new\":%u,\"old\":%u}", owner, cursor, list, fresh, old);
		}
		if (line != data.practice.selection_state_record)
		{
			practice_dummy_log(fmt::format("{\"type\":\"practice_select_probe\",\"boundary\":%u,%s}\n", index, line));
			data.practice.selection_state_record = std::move(line);
		}
	}
}

void pbd::rollback_session::practice_steps::trace_pad(u16 host, u16 pad, u16 pressed)
{
	if (!data.practice.rewind_quiet && (host != data.practice.host_pad_prev || pad != data.practice.dummy.pad_prev))
		practice_dummy_log(pbd::practice::pad::trace({index, host, pad, pressed, static_cast<u32>(data.practice.dummy.state),
			data.practice.menu.open, pbd::practice::input_claim.load(std::memory_order_acquire), data.game_overlay.claim,
			data.practice.dummy.ignore_until_release, data.takeover.ignore_pad_until_release,
			data.takeover.keyframe.valid, data.session.in_match}));
	data.practice.host_pad_prev = host;
}

void pbd::rollback_session::practice_steps::log_health()
{
	if (session_clock::now() < data.practice.health_next || data.practice.rewind_quiet)
		return;
	data.practice.health_next = session_clock::now() + std::chrono::seconds(1);
	const auto observed = read_practice_pause();
	u32 mode = 0, battle_counter = 0;
	read_word(pbd::t5dr::game_mode_address, mode);
	read_word(battle_counter_addresses[1], battle_counter);
	practice_dummy_log(fmt::format("{\"type\":\"practice_health\",\"boundary\":%u,\"pause_word\":%u,\"pause_read\":%s,\"session_mode\":%u,\"native_pause_state\":%u,\"native_pause_request\":%u,\"native_menu_state\":%u,\"practice_counter\":%u,\"battle_counter\":%u,\"in_match\":%s,\"menu_page\":%u,\"dummy_state\":%u,\"dummy_pending_frames\":%u,\"input_leaks\":%u,\"frame_advantage\":\"Unknown\",\"hit_block\":\"Unknown\",\"history\":[%u,%u]}\n",
		index, observed.paused ? 1u : 0u, json_bool(observed.valid), mode, observed.blocked, observed.request, observed.menu, observed.counter,
		battle_counter, json_bool(data.session.in_match), data.practice.menu.page, static_cast<u32>(data.practice.dummy.state), static_cast<u32>(data.practice.dummy.pending.words.size()), data.practice.input_leaks,
		data.practice.view.inputs[0].count, data.practice.view.inputs[1].count));
}

void pbd::rollback_session::practice_steps::close_panel()
{
	if (data.practice.menu.open)
		practice_dummy_log(fmt::format("{\"type\":\"practice_menu_close\",\"boundary\":%u,\"page\":%u}\n", index, data.practice.menu.page));
	data.practice.menu.open = false;
	data.practice.menu.by_escape = false;
	data.practice.menu.dropdown_open = false;
	if (session_runtime.m_practice_memory)
		session_runtime.m_practice_memory->set_editor.reset();
	// A move list still open when the menu closes another way keeps nothing: only Circle keeps its moves.
	data.practice.menu.move_list = {};
	data.practice.menu.confirm = pbd::practice::confirm_kind::none;
	data.practice.view.native_menu = false;
	pbd::practice::input_claim.store(true, std::memory_order_release);
	data.practice.dummy.ignore_until_release = true;
	pbd::game_overlay::publish({});
}

void pbd::rollback_session::practice_steps::resume_native()
{
	close_panel();
	// Close DR's pause menu through its own START path before handing the controller to the dummy. A resume already
	// in flight is that press; a second one would pause DR again.
	if (data.practice.pause.resume_watch.in_flight())
		return;
	data.practice.pause.resume_watch.clear();
	data.practice.pause.resume_native = data.practice.native.paused;
	data.practice.pause.resume_injected = false;
}

void pbd::rollback_session::practice_steps::leave()
{
	data.session.pending_end = user_stopped;
	if (session_runtime.m_practice_memory)
		memory_save();
	close_panel();
	Emu.CallFromMainThread([]()
		{
			Emu.Kill(true, false);
		});
}

// The native calls Practice queued, run at the guarded owner boundary while DR's pause is Practice's: the menu sounds first,
// then one option commit, restore, selection or stage load. A restore of the saved game options (memory_restore) is silent:
// no sound, no toast. Stops the run when a partial stage load cannot be completed.
std::optional<replay_control_result> pbd::rollback_session::practice_steps::native_calls(bool& guest_called)
{
	const auto sound_owner = pbd::practice::native::inspect(data.practice.menu.open || data.practice.dummy.state == dummy_state::confirm ||
															data.practice.dummy.ignore_until_release || data.practice.pause.resume_native ||
															practice_memory_pause());
	if (!data.practice.sounds.empty() && (!sound_owner.practice || !sound_owner.owned ||
											 data.practice.sounds.front().generation != sound_owner.generation ||
											 (!sound_owner.pause_acknowledged && !data.practice.pause.requested)))
	{
		practice_dummy_log(fmt::format("{\"type\":\"practice_sound_scope_ended\",\"boundary\":%u,\"pending\":%u}\n",
			index, data.practice.sounds.size()));
		data.practice.sounds.clear();
	}
	// Drain only at the independently guarded owner boundary. Do not consume a waiting sound, retry an
	// executed call, or release pause before the cursor/confirm/cancel requests preceding Resume have run.
	for (u32 count = 0; count < 4 && !data.practice.sounds.empty() && sound_owner.pause_acknowledged &&
						pbd::practice::native::boundary_ready(ppu);
		++count)
	{
		guest_called = true;
		const auto command = data.practice.sounds.front();
		data.practice.sounds.pop_front();
		const auto heard = pbd::practice::native::execute(ppu, command, sound_owner);
		practice_dummy_log(fmt::format("{\"type\":\"practice_native_call\",\"boundary\":%u,\"operation\":0,\"value\":%u,\"entry\":%u,\"accepted\":%s,\"preserved\":%s,\"readback\":%s,\"pending_sounds\":%u}\n",
			index, command.value, heard.entry, json_bool(heard.accepted), json_bool(heard.preserved),
			json_bool(heard.readback), data.practice.sounds.size()));
	}
	if (!data.practice.native.request || !data.practice.sounds.empty() || !sound_owner.pause_acknowledged ||
		!pbd::practice::native::boundary_ready(ppu))
		return std::nullopt;
	// Consume once before entering guest code; a failed native call is reported and never automatically retried.
	const auto command = *data.practice.native.request;
	data.practice.native.request.reset();
	guest_called = true;
	const auto* memory = session_runtime.m_practice_memory.get();
	const bool silent = command.kind == pbd::practice::native::operation::option_restore && memory && memory->restore_queued;
	const auto owner = pbd::practice::native::inspect(sound_owner.owned);
	const auto result = pbd::practice::native::execute(ppu, command, owner);
	practice_dummy_log(fmt::format("{\"type\":\"practice_native_call\",\"boundary\":%u,\"operation\":%u,\"entry\":%u,\"accepted\":%s,\"preserved\":%s,\"readback\":%s,\"silent\":%s}\n",
		index, static_cast<u32>(command.kind), result.entry, json_bool(result.accepted), json_bool(result.preserved),
		json_bool(result.readback), json_bool(silent)));
	return native_outcome(command, result, silent);
}

// What an executed native call leaves behind: its toast and confirm sound (none for a silent restore), the screen a selection
// or the Command List opens, or the end of the run after a partial stage load.
std::optional<replay_control_result> pbd::rollback_session::practice_steps::native_outcome(const pbd::practice::native::request& command,
	const pbd::practice::native::result& result, bool silent)
{
	if (!result.accepted)
	{
		if (!silent)
		{
			data.practice.view.toast = "The game didn't accept that\tRestart Practice to try again";
			data.practice.menu.toast_until = session_clock::now() + std::chrono::seconds(4);
		}
		if (result.load_started)
		{
			pbd_session_log.error("Practice stage load stopped after a partial resource request.");
			close_panel();
			Emu.CallFromMainThread([]()
				{
					Emu.Kill(true, false);
				});
			return replay_control_result::stopped;
		}
	}
	else if (command.kind == pbd::practice::native::operation::command_list ||
			 command.kind == pbd::practice::native::operation::character_select ||
			 command.kind == pbd::practice::native::operation::stage_load)
	{
		close_panel();
		data.practice.native.screen = command.kind == pbd::practice::native::operation::command_list ? 1 :
		                              command.kind == pbd::practice::native::operation::stage_load   ? 3 :
		                                                                                               2;
		pbd::practice::paint::claim_paint(false, static_cast<u64>(Emu.GetEmulationIdentifier()));
	}
	else if (!silent && (command.kind == pbd::practice::native::operation::option_commit ||
							command.kind == pbd::practice::native::operation::option_restore))
	{
		data.practice.sounds.push_back(pbd::practice::native::request{pbd::practice::native::operation::sound,
			pbd::practice::native::option::action, static_cast<u32>(pbd::practice::native::sound::confirm), command.generation});
		if (command.kind == pbd::practice::native::operation::option_restore)
		{
			data.practice.view.toast = "Defaults restored";
			data.practice.menu.toast_until = session_clock::now() + std::chrono::seconds(3);
		}
	}
	return std::nullopt;
}

void pbd::rollback_session::practice_steps::queue_native(pbd::practice::native::operation kind, u32 value,
	pbd::practice::native::option choice)
{
	const pbd::practice::native::request command{kind, choice, value,
		static_cast<u64>(Emu.GetEmulationIdentifier()), index};
	if (kind == pbd::practice::native::operation::sound)
	{
		// Rapid input remains audible when an asynchronous PPU flag postpones guest entry. Each edge
		// keeps its own sound request; the bounded queue belongs to this paused Practice scope only.
		if (data.practice.sounds.size() >= 128)
		{
			practice_dummy_log(fmt::format("{\"type\":\"practice_sound_overflow\",\"boundary\":%u}\n", index));
			return;
		}
		data.practice.sounds.push_back(command);
		practice_dummy_log(fmt::format("{\"type\":\"practice_sound_queued\",\"boundary\":%u,\"sound\":%u,\"pending\":%u}\n",
			index, value, data.practice.sounds.size()));
	}
	else
		data.practice.native.request = command;
}

void pbd::rollback_session::practice_steps::queue_sound(pbd::practice::native::sound value)
{
	queue_native(pbd::practice::native::operation::sound, static_cast<u32>(value));
}

u32 pbd::rollback_session::practice_steps::pointer_context()
{
	// An open dropdown is its own pointer context, so a row click queued before it opened cannot pick an option.
	// A question over the list is another, so a click queued before it opened cannot answer it. A dropdown's own list (Recording's
	// Dummy Actions) is another again.
	if (data.practice.dummy.state == dummy_state::confirm)
		return 48u;
	if (data.practice.menu.confirm != pbd::practice::confirm_kind::none)
		return 256u + data.practice.menu.page;
	// The move list, open over the menu, is one more.
	if (data.practice.menu.move_list.open)
		return 320u;
	if (const auto* memory = session_runtime.m_practice_memory.get(); memory && memory->set_editor)
		return 384u + memory->set_editor->group;
	return data.practice.menu.page + (data.practice.menu.dropdown_open ? (data.practice.menu.dropdown_sub ? 192u : 128u) : 0u);
}

void pbd::rollback_session::practice_steps::remember_position()
{
	const bool native_call_ready = pbd::practice::native::boundary_ready(ppu);
	const auto token = pbd::game_lifecycle::capture_placement(ppu, lifecycle_scope());
	if (token)
	{
		data.practice.restart.placement = token;
		data.practice.restart.position = pbd::game_lifecycle::placement::remembered;
		if (data.practice.rewind && data.replay.takeover_fast)
		{
			data.practice.restart.moment_scope = token->lifetime;
			request_takeover_capture(index);
		}
	}
	data.practice.view.toast = token ? "Position remembered\t{select}+{r3} restarts here" : "Couldn't remember this position";
	data.practice.menu.toast_until = session_clock::now() + std::chrono::seconds(3);
	practice_dummy_log(fmt::format("{\"type\":\"practice_placement_capture\",\"boundary\":%u,\"saved\":%s,\"native_call_ready\":%s,\"stage\":%u,\"stage_generation\":%u,\"pair_generation\":%u,\"positions\":[[%u,%u,%u,%u,%u],[%u,%u,%u,%u,%u]]}\n",
		index, json_bool(token.has_value()), json_bool(native_call_ready), token ? token->lifetime.stage : umax,
		token ? token->lifetime.stage_generation : 0, token ? token->lifetime.pair_generation : 0,
		token ? static_cast<u32>(token->positions[0].x) : 0, token ? static_cast<u32>(token->positions[0].y) : 0,
		token ? static_cast<u32>(token->positions[0].z) : 0, token ? static_cast<u16>(token->positions[0].facing) : 0,
		token ? token->positions[0].yaw_bits : 0, token ? static_cast<u32>(token->positions[1].x) : 0,
		token ? static_cast<u32>(token->positions[1].y) : 0, token ? static_cast<u32>(token->positions[1].z) : 0,
		token ? static_cast<u16>(token->positions[1].facing) : 0, token ? token->positions[1].yaw_bits : 0));
	data.practice.dummy.ignore_until_release = true;
}

bool pbd::rollback_session::practice_steps::remembered_moment_ready()
{
	auto& lifetime = data.practice.restart.moment_scope;
	if (!lifetime)
		return false;
	if (!data.session.in_match || *lifetime != lifecycle_scope())
	{
		data.takeover.keyframe = {};
		data.takeover.capture_pending = false;
		data.history.practice_anchor = {};
		lifetime.reset();
		return false;
	}
	return data.takeover.keyframe.valid;
}

void pbd::rollback_session::practice_steps::queue_reset(pbd::game_lifecycle::placement target,
	pbd::game_lifecycle::placement_variant variant)
{
	if (!pbd::game_lifecycle::requested() || !practice_can_change_situation())
		return;
	if (target == pbd::game_lifecycle::placement::remembered && remembered_moment_ready())
	{
		restored = restore_takeover("practice_restart", 3, index);
		if (restored)
		{
			practice_restart_after_restore(index);
			data.practice.combos = data.practice.restart.moment_combos;
			data.practice.health_previous = data.practice.restart.moment_health_previous;
			data.practice.health_valid = data.practice.restart.moment_health_valid;
			pbd::practice::native::frame_restore(data.takeover.keyframe.frame_state);
			data.practice.restart.reset_request.reset();
			data.practice.pause.requested = false;
			data.practice.pause.resume_watch.clear();
			// A moment saved from the pause menu resumes through DR's ordinary START path.
			data.practice.pause.resume_native = read_practice_pause().paused;
			close_panel();
		}
		else
		{
			data.practice.view.toast = "Couldn't return to this moment\tRemember it again with {r3}";
			data.practice.menu.toast_until = session_clock::now() + std::chrono::seconds(3);
			data.practice.dummy.ignore_until_release = true;
		}
		return;
	}
	if (!pbd::game_lifecycle::placement_available(target, lifecycle_scope(), data.practice.restart.placement))
	{
		data.practice.view.toast = target == pbd::game_lifecycle::placement::remembered ?
		                               "Remember a position first" :
		                               "This Restart Position isn't available here";
		data.practice.menu.toast_until = session_clock::now() + std::chrono::seconds(3);
		data.practice.dummy.ignore_until_release = true;
		return;
	}
	pbd::game_lifecycle::practice_reset command;
	command.request_id = ++data.practice.restart.reset_serial;
	command.lifetime = lifecycle_scope();
	command.target = target;
	command.remembered = data.practice.restart.placement;
	command.variant = variant;
	if (target == pbd::game_lifecycle::placement::random)
		command.random_seed = host_random_word();
	// A request still pending is replaced whole, with a fresh retry allowance; its resume is already under way.
	const bool replacing = data.practice.restart.reset_request.has_value();
	if (replacing)
		practice_dummy_log(fmt::format("{\"type\":\"practice_reset_replaced\",\"request_id\":%u,\"replaced\":%u,\"boundary\":%u,\"target\":%u}\n",
			command.request_id, data.practice.restart.reset_request->request_id, index, static_cast<u32>(target)));
	data.practice.restart.reset_request = command;
	// Past restart_retry_frames (ten seconds of frames), so a restart pressed during a throw or a knockdown is never
	// cut off by the clock before the frame bound.
	data.practice.restart.reset_deadline = session_clock::now() + std::chrono::seconds(12);
	data.practice.restart.reset_retry_from = umax;
	// DR must acknowledge the ordinary resume before entering the full reset. Both input ports stay claimed meanwhile.
	if (!replacing)
		resume_native();
	data.practice.dummy.mask_ui = true;
}

// SELECT with Cross, R3 or a direction. Restart owns the chord's buttons until they are released.
void pbd::rollback_session::practice_steps::restart_chord(u16 chord)
{
	data.practice.restart.modifier_held = true;
	data.practice.chord_select_held = true;
	using pbd::game_lifecycle::placement;
	const auto variant = data.practice.restart.variant;
	if (chord & 0x0400)
		queue_reset(placement::remembered, variant);
	else if (chord & 0x0040)
		queue_reset(placement::centre, variant);
	else if (chord & 0x8000)
		queue_reset(placement::left_wall, variant);
	else if (chord & 0x2000)
		queue_reset(placement::right_wall, variant);
	else if (chord & 0x4000)
		queue_reset(placement::foreground_wall, variant);
	else if (chord & 0x1000)
		queue_reset(placement::background_wall, variant);
	data.takeover.ignore_pad_until_release = true;
}

void pbd::rollback_session::practice_steps::open_panel()
{
	data.practice.menu.open = true;
	data.practice.menu.page = category::practice_settings;
	data.practice.menu.row = 0;
	data.practice.menu.rail_focus = true;
	data.practice.menu.dropdown_open = false;
	data.practice.menu.move_list = {};
	data.practice.menu.confirm = pbd::practice::confirm_kind::none;
	// Leaving the ready-to-record state returns to the Defense row of the slot that chose Record,
	// with the list focused.
	if (pbd::practice::defense_row(data.practice.training_mode, data.practice.menu.return_slot))
	{
		data.practice.menu.page = category::practice_settings;
		data.practice.menu.row = data.practice.menu.return_slot;
		data.practice.menu.rail_focus = false;
	}
	data.practice.menu.return_slot = umax;
	if (data.practice.menu.on_settings)
	{
		data.practice.menu.page = category::settings;
		data.practice.menu.row = 0;
		data.practice.menu.rail_focus = true;
	}
	data.practice.menu.by_escape = data.practice.menu.on_settings;
	data.practice.menu.on_settings = false;
	++data.practice.menu.generation;
	data.practice.dummy.ignore_until_release = data.practice.dummy.pad != 0;
	data.practice.dummy.mask_ui = true;
	data.practice.pause.requested = false;
	// The menu now owns DR's pause: a resume not sent yet, or one DR never took, must not close it from under the menu.
	data.practice.pause.resume_native = false;
	data.practice.pause.resume_injected = false;
	data.practice.pause.resume_watch.clear();
	practice_dummy_log(fmt::format("{\"type\":\"practice_menu_open\",\"boundary\":%u,\"page\":%u,\"pause\":%u,\"flag\":%u,\"ran\":%s,\"counter\":%u,\"native_owner\":true}\n",
		index, data.practice.menu.page, data.practice.native.paused ? 1u : 0u, data.practice.native.flag ? 1u : 0u,
		json_bool(data.practice.native.ran), data.practice.boundary_counter));
}

// Rows in the category in view. Move List, Character Selection, Settings and Exit hold one action row each.
u32 pbd::rollback_session::practice_steps::practice_row_count()
{
	switch (data.practice.menu.page)
	{
	case category::practice_settings: return pbd::practice::practice_row_count(data.practice.training_mode);
	case category::restart_settings: return pbd::practice::restart_rows::count;
	case category::display_settings: return pbd::practice::display_rows::count;
	case category::stage_selection: return pbd::practice::stage::row_count;
	default: return 1;
	}
}

// Up/Down in the list.
u32 pbd::rollback_session::practice_steps::practice_row_step(bool down)
{
	return pbd::practice::wrap_step(data.practice.menu.row, practice_row_count(), down);
}

// The Defense row (a dummy_rows value) at a row of the category in view, if it is one.
std::optional<u32> pbd::rollback_session::practice_steps::practice_defense_row(u32 row)
{
	if (data.practice.menu.page != category::practice_settings)
		return std::nullopt;
	return pbd::practice::defense_row(data.practice.training_mode, row);
}

// The action slot at the focused row, if the focused row is one of Defense's eight slots.
std::optional<u32> pbd::rollback_session::practice_steps::practice_focused_slot()
{
	const auto defense = practice_defense_row(data.practice.menu.row);
	if (!defense || *defense >= pbd::practice::dummy_rows::slot_count)
		return std::nullopt;
	return defense;
}

// True when L1/R1 set the focused row's frequency: a filled dummy slot in Defense with the list focused.
// L1/R1 do nothing anywhere else.
bool pbd::rollback_session::practice_steps::practice_frequency_row()
{
	const auto slot = practice_focused_slot();
	return !data.practice.menu.rail_focus && slot &&
	       data.practice.menu.confirm == pbd::practice::confirm_kind::none &&
	       data.practice.dummy.state != dummy_state::recording &&
	       (!data.practice.dummy.slots[*slot].frames.empty() || !data.practice.dummy.slots[*slot].moves.empty());
}

// Their current values, or nothing while the practice object cannot be read. While recording or playback routes the
// dummy (practice_dummy_route), Dummy Action reads as the player's own action, the one that will be restored. Counter Hit's
// Random reads as Off, the value choosing it commits, whatever its last draw left in the game.
std::optional<std::array<u32, pbd::practice::game_option_count>> pbd::rollback_session::practice_steps::practice_game_values()
{
	const auto native = pbd::practice::native::inspect(true);
	if (!native.practice)
		return std::nullopt;
	std::array<u32, pbd::practice::game_option_count> values{};
	for (u32 i = 0; i < values.size(); ++i)
		if (!read_word(native.practice + practice_option_offsets[i], values[i]) || values[i] > practice_option_maximum[i])
			return std::nullopt;
	if (data.practice.native.controlled && values[0] == data.practice.native.written[0])
		values[0] = data.practice.native.action[0];
	if (data.practice.reactions.counter_hit.random)
		values[1] = 0;
	return values;
}
