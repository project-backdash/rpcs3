// Practice's menu as the overlay draws it: the view published each frame and the native menu's rows.
#include "stdafx.h"
#include "pbd_rollback_session_boundary.h"
#include "../PBD/pbd_dummy_move_runtime.h"
#include "../PBD/pbd_practice_paint.h"

namespace category = pbd::practice::categories;

void pbd::rollback_session::practice_steps::publish_menu_identity()
{
	const bool previous_menu = data.practice.view.native_menu;
	const u32 previous_context = data.practice.view.mouse_context;
	data.practice.view.native_menu = (data.practice.menu.open || data.practice.dummy.state == dummy_state::confirm) &&
	                                 data.practice.native.pause_valid && data.practice.native.paused && !data.practice_resume_settling();
	// Preview can return to the same recording decision without opening a new START menu. Give
	// each visible visit an identity so a queued action from the earlier visit cannot survive it.
	if (data.practice.view.native_menu && (!previous_menu || previous_context != pointer_context()))
		++data.practice.menu.mouse_revision;
	if (data.practice.view.native_menu && !data.practice.menu.generation)
		++data.practice.menu.generation;
	data.practice.view.menu_generation = data.practice.menu.generation;
	data.practice.view.mouse_context = pointer_context();
	data.practice.view.mouse_emulation = static_cast<u64>(Emu.GetEmulationIdentifier());
	data.practice.view.mouse_revision = data.practice.menu.mouse_revision;
	data.practice.view.mouse_row_base = 0;
	data.practice.view.mouse_pages = data.practice.menu.open && data.practice.dummy.state != dummy_state::confirm &&
	                                 !data.practice.menu.dropdown_open && !data.practice.menu.move_list.open &&
	                                 data.practice.menu.confirm == pbd::practice::confirm_kind::none;
	const auto pause_scene = data.practice_pause_scene();
	pbd::practice::input_claim.store(pbd::practice::controls::claims_input(pause_scene) || data.practice.dummy.ignore_until_release ||
										 data.practice.restart.reset_request.has_value(),
		std::memory_order_release);
	pbd::practice::paint::claim_paint(pbd::practice::controls::claims_paint(pause_scene, data.practice.view.native_menu),
		static_cast<u64>(Emu.GetEmulationIdentifier()));
	data.practice.view.hud_visible = data.session.in_match && !data.practice.native.paused;
	if (data.practice.menu.hint_pending && data.session.in_match)
	{
		data.practice.menu.hint_pending = false;
		data.practice.view.toast = "{start} Practice Menu\tHold {start}+{select} for 2 seconds to exit";
		data.practice.menu.toast_until = session_clock::now() + std::chrono::seconds(8);
	}
	// Another pad took Player 1 over (pbd_controller_takeover.h): Practice says which, as its own toast.
	if (auto controller = pbd::controller_takeover::take_new(data.practice.controller_notice))
	{
		data.practice.view.toast = std::move(*controller);
		data.practice.menu.toast_until = session_clock::now() + std::chrono::milliseconds(pbd::controller_takeover::notice_ms);
	}
}

u32 pbd::rollback_session::practice_steps::publish_dummy_state()
{
	data.practice.view.inputs = data.practice.history.inputs();
	const u32 timer_frames = data.practice.dummy.state == dummy_state::recording || data.practice.dummy.state == dummy_state::confirm || data.practice.dummy.state == dummy_state::ready || data.practice.dummy.preview ?
	                             static_cast<u32>(data.practice.dummy.pending.words.size()) :
	                             data.practice.dummy.play_pos;
	data.practice.view.dummy_status = data.practice.dummy.state == dummy_state::recording ?
	                                      fmt::format("Recording %s  %.1f / 30.0 s   %s Stop",
											  data.practice.dummy.slots[data.practice.dummy.slot_index].name, timer_frames / 60.0,
											  data.practice.glyphs == pbd::game_overlay::glyph_family::xbox ? "MENU" : "START") :
	                                  data.practice.dummy.state == dummy_state::playing ? (data.practice.dummy.preview ? "Preview" : fmt::format("Playing %s", data.practice.dummy.have_play_slot ? data.practice.dummy.slots[data.practice.dummy.play_slot].name : std::string(practice_movement_label()))) :
	                                                                                      "";
	// Structured dummy state for the top-centre band; the player-facing slot names follow the menu ("Dummy Action N").
	data.practice.view.dummy = data.practice.dummy.state == dummy_state::recording ? pbd::practice::dummy_phase::recording :
	                           data.practice.dummy.state == dummy_state::ready     ? pbd::practice::dummy_phase::ready :
	                           data.practice.dummy.state == dummy_state::playing   ? (data.practice.dummy.preview ? pbd::practice::dummy_phase::preview : pbd::practice::dummy_phase::playing) :
	                                                                                 pbd::practice::dummy_phase::idle;
	data.practice.view.dummy_frames = timer_frames;
	data.practice.view.dummy_limit = 1800;
	// Playback of the movement alone (no action chosen) shows the movement's name.
	data.practice.view.dummy_slot = data.practice.dummy.state == dummy_state::playing && !data.practice.dummy.preview ?
	                                    (data.practice.dummy.have_play_slot ? practice_slot_name(data.practice.dummy.play_slot) : std::string(practice_movement_label())) :
	                                    practice_record_name();
	return timer_frames;
}

void pbd::rollback_session::practice_steps::publish_categories()
{
	// The category rail.
	data.practice.menu.page = std::min<u32>(data.practice.menu.page, category::count - 1);
	data.practice.view.pages = {"Practice Settings", "Restart Settings", "Display Settings", "Move List",
		"Character Selection", "Stage Selection", "Settings", "Exit Practice"};
	data.practice.view.page_notes = {};
	data.practice.view.page_unavailable = {};
	data.practice.view.page = data.practice.menu.page;
	data.practice.view.rail_focus = data.practice.menu.open && data.practice.menu.rail_focus &&
	                                data.practice.dummy.state != dummy_state::confirm &&
	                                !data.practice.menu.dropdown_open;
	constexpr std::array<const char*, category::count> rail_help{
		"Choose the training mode: the game's own dummy options, or actions you record for the dummy.",
		"Restart the fight and choose where both fighters start.",
		"Choose what Practice shows on screen.",
		"Open your character's command list. Choose the dummy's reaction moves from its Dummy Action or Action After a Hit or Block.",
		"Pick new characters without leaving Practice.",
		"Pick another stage without leaving Practice.",
		"Volume, resolution, window, VSync, audio output and display.",
		"Leave Practice and return to the launcher."};
	data.practice.view.rail_help = rail_help[data.practice.view.page];
	data.practice.view.section =
		data.practice.dummy.state == dummy_state::confirm ? "Defense" : data.practice.view.pages[data.practice.view.page];
}

void pbd::rollback_session::practice_steps::publish_control_hints(bool situation_ready, bool remembered_ready, bool r3_remembers)
{
	// In-game control bar: only chords the input owner above acts on in the current state.
	{
		auto& bar = data.practice.view.prompts;
		bar = {};
		if (data.practice.dummy.state == dummy_state::recording)
		{
			bar.add("{start}", "Stop Recording");
			bar.add("{select}+{circle}", "Stop Recording");
		}
		else if (data.practice.dummy.state == dummy_state::ready)
		{
			// Ready to record: the player controls the dummy; START goes back to the menu without a take.
			bar.add("{select}+{circle}", "Start Recording");
			bar.add("{start}", "Cancel");
			if (situation_ready)
			{
				bar.add("{select}+{cross}", "Restart");
				if (r3_remembers)
					bar.add("{r3}", "Remember this moment");
			}
		}
		else
		{
			bar.add("{start}", "Menu");
			// Keep the current dummy action beside Menu so longer restart hints cannot push its control off the bar.
			if (data.practice.dummy.enabled)
			{
				// Offense with a Movement Technique: the chord turns the movement off and on, whether or not it plays now.
				// An Action After a Hit or Block is a reaction the chord turns off and on too.
				if (data.practice.training_mode == pbd::practice::training_mode::offense &&
					data.practice.offense.after != pbd::practice::after_contact::none)
					bar.add("{select}+{circle}", data.practice.offense.movement_off ? "Start Reaction" : "Stop Reaction");
				else if (data.practice.training_mode == pbd::practice::training_mode::offense && !data.practice.movement.input_sequence.empty())
					bar.add("{select}+{circle}", data.practice.offense.movement_off ? "Start Movement" : "Stop Movement");
				else if (data.practice.dummy.state == dummy_state::playing)
					bar.add("{select}+{circle}", "Stop Dummy");
				else if (data.practice.training_mode == pbd::practice::training_mode::defense &&
						 (practice_dummy_available() || practice_movement_alone()))
					bar.add("{select}+{circle}", "Play Dummy");
			}
			if (situation_ready)
			{
				bar.add("{select}+{cross}", "Restart");
				if (r3_remembers)
					bar.add("{r3}", "Remember this moment");
				if (remembered_ready)
					bar.add("{select}+{r3}", "Back to it");
				bar.add("{select}+{dpad:udlr}", "Wall Restart");
			}
			bar.add("{start}+{select}", "Exit (Hold)");
		}
		// Control Hints = Hide clears the bar during the fight. Ready to record and recording keep theirs: the bar
		// is the only place those states show how to start and stop the take.
		if (!data.practice.view.control_hints && data.practice.dummy.state != dummy_state::recording &&
			data.practice.dummy.state != dummy_state::ready)
			bar = {};
	}
}

void pbd::rollback_session::practice_steps::publish()
{
	log_health();
	if (!restored)
		practice_dummy_route(index);
	if (session_clock::now() > data.practice.menu.toast_until)
		data.practice.view.toast.clear();
	// The menu and the take's question are shown only over DR's pause: a game running under either is paused again.
	if ((data.practice.menu.open || data.practice.dummy.state == dummy_state::confirm) && data.practice.native.pause_valid && !data.practice.native.paused &&
		!data.practice.pause.requested)
	{
		data.practice.pause.requested = true;
		data.practice.pause.injected = false;
	}
	publish_menu_identity();
	const u32 timer_frames = publish_dummy_state();
	publish_categories();
	const bool situation_ready = pbd::game_lifecycle::requested() && practice_can_change_situation();
	const bool moment_ready = remembered_moment_ready();
	const bool remembered_ready = situation_ready && (moment_ready ||
														 pbd::game_lifecycle::placement_available(pbd::game_lifecycle::placement::remembered, lifecycle_scope(), data.practice.restart.placement));
	// R3 with an action is that action in the fight, and Remember Position is the Restart Settings row only.
	const bool r3_remembers = pbd::practice::controls::r3_remembers(data.practice.button_map);
	publish_control_hints(situation_ready, remembered_ready, r3_remembers);
	const bool decision = data.practice.dummy.state == dummy_state::confirm;
	data.practice.view.layout = decision ? pbd::practice::menu_layout::decision : pbd::practice::menu_layout::list;
	data.practice.view.list_note.clear();
	data.practice.view.header.clear();
	data.practice.view.dialog_title.clear();
	data.practice.view.dialog_detail.clear();
	data.practice.view.menu_prompts = {};
	data.practice.view.menu_dropdown = {};
	data.practice.view.confirm = {};
	data.practice.view.list_legend = {};
	publish_native_menu(decision, remembered_ready, situation_ready, timer_frames);
	pbd::practice::publish(data.practice.view);
	practice_dummy_publish();
}

// Offense's Action After a Hit or Block as its row shows it: the input, the recorded Dummy Action, or the chosen moves (the
// move's own name for one, otherwise how many the dummy picks from at random).
std::string pbd::rollback_session::practice_steps::practice_reaction_name()
{
	using pbd::practice::after_contact;
	switch (data.practice.offense.after)
	{
	case after_contact::none: return "None";
	case after_contact::movement: return pbd::practice::movement::reaction_input_name(data.practice.offense.reaction_input);
	case after_contact::action: return practice_slot_name(data.practice.offense.reaction_slot);
	case after_contact::moves: break;
	}
	using pbd::practice::dummy_move::source;
	const auto& settings = pbd::practice::dummy_move::current_settings;
	return practice_moves_label(settings.after_hit.kind == source::moves ? settings.after_hit.ids : settings.after_guard.ids);
}

// Moves from the move list as a menu value: the move's own name for one, otherwise how many are picked from at random.
std::string pbd::rollback_session::practice_steps::practice_moves_label(const std::vector<u32>& ids)
{
	if (ids.size() != 1)
		return fmt::format("Random: %u moves", static_cast<u32>(ids.size()));
	const auto& picker = pbd::practice::dummy_move::current.picker.moves;
	const auto found = std::find_if(picker.begin(), picker.end(), [&](const auto& row)
		{
			return row.id == ids[0];
		});
	return found == picker.end() ? std::string("1 move") : pbd::practice::menu::shortened(found->name, 26);
}
