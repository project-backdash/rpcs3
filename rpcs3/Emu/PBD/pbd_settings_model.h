#pragma once

// In-game Settings: Esc in the game window, or Settings in the Practice menu, in Practice, replays, spectating and
// offline play. It never changes the guest's speed. Online sets offer only local controls and keep playing while the
// player edits them; this player's fighter stands still meanwhile (pbd_rollback_session_peer.cpp filter_local_sample).
//
// Where the launcher names a navigate request file (PBD_NAVIGATE_REQUEST), Esc opens the menu on a pause menu's top level:
// Return to Game, Settings (the page below), Start recording or Stop recording where clips can record on this PC, Return to
// Launcher and Exit Game. The last two write the request; the launcher then ends this run as its own End or Stop would and
// either comes to the front on its main menu or closes.
//
// The game window's GUI thread owns the menu. It reads this PC's controller on its own timer, so the menu answers while the
// game waits or is paused, applies each change and publishes a copy for the RSX overlay to draw. Nothing here reads or writes
// guest memory: the update latch and cellPad only read claimed() to keep this controller's input from the game while the
// menu is open. Changes the player makes are also written to a small JSON file, so the launcher can keep them for the next
// session (it rewrites the emulator config from its own settings before every start).

#include "pbd_env.h"
#include "pbd_json.h"
#include "pbd_controller_settings.h"
#include "pbd_controller_display.h"
#include <algorithm>
#include <array>
#include <atomic>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "Emu/PBD/pbd_practice_presentation.h"
#include "../pbd_session_mode.h"

namespace pbd::settings_menu
{
	// This PC's controller as the game's 16-bit word (digital1 << 8 | digital2), as pbd_local_pad_word() returns it.
	using word = std::uint16_t;
	inline constexpr word start = 0x0800, up = 0x1000, right = 0x2000, down = 0x4000, left = 0x8000, cross = 0x0040, circle = 0x0020;
	inline constexpr word select = 0x0100, l3 = 0x0200, r3 = 0x0400;
	// The buttons a Button Layout row can give its action to (pbd::controllers::settings::slot_bits): digital2's eight and the
	// two stick clicks.
	inline constexpr word slot_buttons = 0x06ff;
	using layout = decltype(pbd::controllers::settings::values::mapping);

	// Windows' default output (audio_device_enumerator::DEFAULT_DEV_ID): the game follows whatever device Windows uses.
	inline constexpr std::string_view default_audio_output = "@@@default@@@";

	enum class row : std::uint32_t
	{
		volume,
		resolution,
		window,
		vsync,
		audio_output,
		display,
		controls,
		close
	};
	inline constexpr std::uint32_t row_count = 8;
	// The Controls page, the launcher's Button Layout as Tekken 8 draws it: the controller as the panel's header, the game's
	// actions (assignments 1 to 15, so a row's index is its assignment's) in one column, then Reset to default and Back.
	inline constexpr std::uint32_t controls_count = 18, controls_reset = 16, controls_back = 17;
	// A slot button held this long while a row waits for its button cancels the wait.
	inline constexpr std::uint64_t hold_cancel_ms = 700;

	// The top level's rows: those that stay in the game, then the two that end this run. The recording row shows only where
	// clips can record on this PC (shown_entries), and says Stop recording while a recording runs (entry_label).
	enum class entry : std::uint32_t
	{
		resume,
		settings,
		record,
		launcher,
		quit
	};
	inline constexpr std::uint32_t entry_count = 5;
	inline constexpr std::array<std::string_view, entry_count> entry_labels{"Return to Game", "Settings", "Start recording", "Return to Launcher", "Exit Game"};
	// The launcher's names for what the leaving rows ask of it (inGameRequests.ts NAVIGATE_TARGETS): "launcher" opens its main
	// menu, "quit" closes Project Backdash. Empty for the rows that stay in the game.
	inline constexpr std::array<std::string_view, entry_count> entry_targets{"", "", "", "launcher", "quit"};
	// The first row that ends this run; it and the rows after it sit apart from those above (entry_top).
	inline constexpr std::uint32_t first_leaving_entry = static_cast<std::uint32_t>(entry::launcher);
	inline constexpr std::uint32_t leaving_entries = entry_count - first_leaving_entry;

	// The page in view.
	enum class page : std::uint8_t
	{
		top,
		settings,
		controls
	};

	// When a change takes effect. Volume at once. The video rows when the menu closes: each rebuilds the swapchain or the render
	// targets (a short hitch), so they are applied together, once. The audio device and the monitor at the next session:
	// switching the device rebuilds the game's audio with up to 0.9 s of silence, and the game window has no code to move to
	// another monitor while it runs.
	enum class timing : std::uint8_t
	{
		now,
		on_close,
		next_session,
		none
	};
	inline constexpr timing when(row r) noexcept
	{
		switch (r)
		{
		case row::volume: return timing::now;
		case row::resolution:
		case row::window:
		case row::vsync: return timing::on_close;
		case row::audio_output:
		case row::display: return timing::next_session;
		case row::controls:
		case row::close: break;
		}
		return timing::none;
	}

	// The launcher's render sizes: render scale percent and the size it draws a 1920 x 1080 game at.
	inline constexpr std::array<std::uint32_t, 4> resolution_scales{67, 100, 133, 200};
	inline constexpr std::array<std::string_view, 4> resolution_labels{"1280 x 720", "1920 x 1080", "2560 x 1440", "3840 x 2160"};
	inline constexpr int volume_step = 5;
	// A dropdown fits the panel's list area (pbd::practice::dropdown_top) at 30 units per option.
	inline constexpr std::size_t max_options = 14;

	struct option
	{
		std::string id;
		std::string label;
	};

	struct values
	{
		int volume = 100; // 0..100, the launcher's range
		std::uint32_t resolution = 100;
		bool fullscreen = true;
		bool vsync = false;
		std::string audio_output{default_audio_output};
		// "monitor:<identity>" as the launcher stores it (tools/game_displays.py); empty when the window's monitor is unknown.
		std::string display;
		pbd::controllers::settings::values input;
	};

	struct model
	{
		// What this session started with: the changes file holds only what differs from it.
		values start;
		// What is in effect now: a video row the player changed stays pending until the menu closes.
		values applied;
		// The menu's choices.
		values current;
		// The launcher's Borderless draws through the same fullscreen window without exclusive mode, which cannot change while the
		// game runs. Fullscreen keeps that name and that saved value.
		bool borderless = false;
		// Windows' default output first.
		std::vector<option> audio_outputs;
		std::vector<option> displays;
		std::vector<pbd::controllers::settings::device> controllers;
		bool online = false;
		// Clips can record on this PC (its hardware video encoder is ready), and a recording runs now (set_clips).
		bool clips = false;
		bool recording = false;
	};

	// Clips' state as the window last read it; true when the top level's rows or the recording row's words change.
	inline bool set_clips(model& m, bool available, bool recording) noexcept
	{
		recording = available && recording;
		if (m.clips == available && m.recording == recording)
			return false;
		m.clips = available;
		m.recording = recording;
		return true;
	}

	// Every row but the recording row always shows.
	inline bool shows(const model& m, entry e) noexcept
	{
		return e != entry::record || m.clips;
	}

	// The top level's rows as shown, in order.
	inline std::vector<entry> shown_entries(const model& m)
	{
		std::vector<entry> out;
		for (std::uint32_t i = 0; i < entry_count; ++i)
			if (shows(m, static_cast<entry>(i)))
				out.push_back(static_cast<entry>(i));
		return out;
	}

	inline std::string_view entry_label(entry e, bool recording) noexcept
	{
		if (e == entry::record && recording)
			return "Stop recording";
		return entry_labels[std::min(static_cast<std::uint32_t>(e), entry_count - 1)];
	}

	namespace detail
	{
		inline bool same_device(std::string_view a, std::string_view b) noexcept
		{
			// Endpoint IDs are GUIDs; Windows and the launcher's saved copy may differ in letter case.
			return a.size() == b.size() && std::equal(a.begin(), a.end(), b.begin(), [](char x, char y)
											   {
												   const auto lower = [](char c)
												   {
													   return c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c;
												   };
												   return lower(x) == lower(y);
											   });
		}

		inline std::vector<option> capped(const std::vector<option>& list)
		{
			return {list.begin(), list.begin() + static_cast<std::ptrdiff_t>(std::min(list.size(), max_options))};
		}
	} // namespace detail

	// The choices a value row offers, in menu order.
	inline std::vector<option> options(const model& m, row r)
	{
		switch (r)
		{
		case row::resolution:
		{
			std::vector<option> out;
			for (std::size_t i = 0; i < resolution_scales.size(); ++i)
				out.push_back({std::to_string(resolution_scales[i]), std::string(resolution_labels[i])});
			return out;
		}
		case row::window: return {{"fullscreen", m.borderless ? "Borderless" : "Fullscreen"}, {"windowed", "Windowed"}};
		case row::vsync: return {{"on", "On"}, {"off", "Off"}};
		case row::audio_output: return detail::capped(m.audio_outputs);
		case row::display: return detail::capped(m.displays);
		case row::volume:
		case row::controls:
		case row::close: break;
		}
		return {};
	}

	inline std::string chosen_id(const values& v, row r)
	{
		switch (r)
		{
		case row::volume: return std::to_string(v.volume);
		case row::resolution: return std::to_string(v.resolution);
		case row::window: return v.fullscreen ? "fullscreen" : "windowed";
		case row::vsync: return v.vsync ? "on" : "off";
		case row::audio_output: return v.audio_output;
		case row::display: return v.display;
		case row::controls: return {};
		case row::close: break;
		}
		return {};
	}

	inline bool same_choice(const values& a, const values& b, row r)
	{
		return r == row::audio_output ? detail::same_device(a.audio_output, b.audio_output) : chosen_id(a, r) == chosen_id(b, r);
	}

	// The option `v` holds, or the list's size when it is not listed (a render scale the launcher does not offer, an unplugged
	// device, a monitor that went away).
	inline std::uint32_t chosen_index(const model& m, const values& v, row r)
	{
		const auto list = options(m, r);
		const auto id = chosen_id(v, r);
		for (std::uint32_t i = 0; i < list.size(); ++i)
			if (r == row::audio_output ? detail::same_device(list[i].id, id) : list[i].id == id)
				return i;
		return static_cast<std::uint32_t>(list.size());
	}

	inline void choose(model& m, row r, const option& o)
	{
		auto& v = m.current;
		switch (r)
		{
		case row::resolution:
			for (const auto scale : resolution_scales)
				if (o.id == std::to_string(scale))
					v.resolution = scale;
			break;
		case row::window: v.fullscreen = o.id == "fullscreen"; break;
		case row::vsync: v.vsync = o.id == "on"; break;
		case row::audio_output: v.audio_output = o.id; break;
		case row::display: v.display = o.id; break;
		case row::volume:
		case row::controls:
		case row::close: break;
		}
	}

	// A row waiting to take effect: a video row until the menu closes, the device and the monitor until the next session.
	inline bool pending(const model& m, row r)
	{
		switch (when(r))
		{
		case timing::on_close: return !same_choice(m.current, m.applied, r);
		case timing::next_session: return !same_choice(m.current, m.start, r);
		case timing::now:
		case timing::none: break;
		}
		return false;
	}

	inline std::string value_text(const model& m, row r)
	{
		if (r == row::volume)
			return std::to_string(m.current.volume);
		if (r == row::close)
			return {};
		const auto list = options(m, r);
		const auto i = chosen_index(m, m.current, r);
		if (i < list.size())
			return list[i].label;
		switch (r)
		{
		case row::resolution: return std::to_string(m.current.resolution) + "%";
		case row::audio_output: return "Device not connected";
		case row::display: return "Unknown";
		default: return {};
		}
	}

	// Geometry on the 1280 x 720 canvas, shared by the drawing and the pointer: the list column of the Practice menu's panel, with
	// Close set apart from the settings.
	inline constexpr float list_left = 396.f, list_width = 812.f, list_top = 160.f, row_height = 28.f, row_pitch = 30.f, close_gap = 16.f;
	inline constexpr float label_x = 415.f, timing_x = 640.f, value_centre = 1044.f, value_width = 300.f;
	inline constexpr float dropdown_left = 849.f, dropdown_width = 409.f, option_pitch = 30.f;

	struct rect
	{
		float x = 0, y = 0, w = 0, h = 0;
		bool contains(float px, float py) const noexcept
		{
			return w > 0 && h > 0 && px >= x && py >= y && px < x + w && py < y + h;
		}
	};

	inline float row_top(std::uint32_t i) noexcept
	{
		return list_top + row_pitch * static_cast<float>(i) + (i >= static_cast<std::uint32_t>(row::close) ? close_gap : 0.f);
	}

	inline rect row_rect(std::uint32_t i) noexcept
	{
		return {list_left, row_top(i), list_width, row_height};
	}

	// The Controls page's panel: one column centred in the list column, about as wide as Tekken 8's. The controller is its
	// header strip; the fifteen action rows follow, each with its button in a column at the right, then Reset to default and
	// Back as command rows under a firmer rule. Rows touch, so the whole layout fits the panel without scrolling.
	inline constexpr float layout_width = 400.f, layout_left = list_left + (list_width - layout_width) / 2.f;
	inline constexpr float layout_head = 26.f, layout_pitch = 24.f, layout_rule = 2.f, layout_button_width = 112.f;

	inline rect controls_rect(std::uint32_t i) noexcept
	{
		if (i == 0)
			return {layout_left, list_top, layout_width, layout_head};
		const std::uint32_t line = std::min(i, controls_count - 1) - 1;
		const float top = list_top + layout_head + layout_pitch * static_cast<float>(line) + (i >= controls_reset ? layout_rule : 0.f);
		return {layout_left, top, layout_width, layout_pitch};
	}

	// The top level's shown rows (`count` of them) in the same list column, the two that end this run set apart below those that
	// stay in the game, as Close is on the settings page.
	inline float entry_top(std::uint32_t i, std::uint32_t count) noexcept
	{
		return list_top + row_pitch * static_cast<float>(i) + (i + leaving_entries >= count ? close_gap : 0.f);
	}

	inline rect entry_rect(std::uint32_t i, std::uint32_t count) noexcept
	{
		return {list_left, entry_top(i, count), list_width, row_height};
	}

	// The focused Volume row's < and > around its number.
	inline rect stepper_rect(bool forward) noexcept
	{
		const float top = row_top(static_cast<std::uint32_t>(row::volume));
		return forward ? rect{value_centre + 20.f, top, 34.f, row_height} : rect{value_centre - 54.f, top, 34.f, row_height};
	}

	// Top of a row's open option stack: the option in effect lines up with its row where the panel allows.
	inline float stack_top(const model& m, row r, std::uint32_t count)
	{
		const auto current = chosen_index(m, m.current, r);
		return pbd::practice::dropdown_top(row_top(static_cast<std::uint32_t>(r)), current < count ? current : 0, count);
	}

	inline rect option_rect(float top, std::uint32_t j) noexcept
	{
		return {dropdown_left, top + option_pitch * static_cast<float>(j), dropdown_width, option_pitch};
	}

	// The Button Layout's words and its rule for a press, as the launcher's (src/buttonLayout.ts).
	inline constexpr std::array<std::string_view, 4> attack_names{"Left Punch", "Right Punch", "Left Kick", "Right Kick"};

	inline bool is_attack(std::string_view assignment) noexcept
	{
		return assignment.size() == 1 && assignment[0] >= '1' && assignment[0] <= '4';
	}

	// "Left Punch (1)" for an attack, "1+2" for a combination.
	inline std::string action_words(std::string_view action)
	{
		return is_attack(action) ? std::string(attack_names[action[0] - '1']) + " (" + std::string(action) + ")" : std::string(action);
	}

	// An action's button column with no button in it.
	inline constexpr std::string_view no_button = "\xE2\x80\x94";

	// The limbs an action uses, bit 0 for 1 (Left Punch) to bit 3 for 4 (Right Kick): the lit dots of its row's limb icon.
	inline constexpr std::uint32_t limbs(std::string_view action) noexcept
	{
		std::uint32_t out = 0;
		for (const char c : action)
			if (c >= '1' && c <= '4')
				out |= 1u << (c - '1');
		return out;
	}

	// What a slot's choice did: the action moved onto it, nothing (the slot does it already), or nothing because the slot holds
	// an attack no other slot does.
	enum class assigned : std::uint8_t
	{
		changed,
		unchanged,
		last_attack
	};

	// The pressed slot takes the action. The first slot that held the action takes the pressed slot's old job, so swapping two
	// attacks loses neither, and any other slot that held it is freed: each action ends up on one button. With no slot to take
	// it the old job goes, unless it is an attack's last button. A slot that does the action already keeps it and frees the
	// others: a layout saved before each action had one button can give it two.
	inline assigned assign_slot(layout& mapping, std::string_view action, std::uint32_t slot)
	{
		const std::string previous = mapping[slot];
		if (previous == action)
		{
			bool freed = false;
			for (std::uint32_t i = 0; i < mapping.size(); ++i)
			{
				if (i == slot || mapping[i] != action)
					continue;
				mapping[i] = "off";
				freed = true;
			}
			return freed ? assigned::changed : assigned::unchanged;
		}
		bool held = false, kept = false;
		for (std::uint32_t i = 0; i < mapping.size(); ++i)
		{
			held = held || (i != slot && mapping[i] == action);
			kept = kept || (i != slot && mapping[i] == previous);
		}
		if (!held && !kept && is_attack(previous))
			return assigned::last_attack;
		bool first = true;
		for (std::uint32_t i = 0; i < mapping.size(); ++i)
		{
			if (i == slot || mapping[i] != action)
				continue;
			mapping[i] = first ? previous : "off";
			first = false;
		}
		mapping[slot] = std::string(action);
		return assigned::changed;
	}

	// The slots that do `action`, as glyph tokens for the overlay's rich text ("{square}", "{l1}/{r2}"), or by name.
	inline std::string slot_tokens(const layout& mapping, std::string_view action, bool words)
	{
		constexpr std::array<std::string_view, std::tuple_size_v<layout>> names{"Square", "Triangle", "Cross", "Circle", "L1", "R1", "L2", "R2",
			"L3", "R3"};
		std::string out;
		for (std::uint32_t i = 0; i < mapping.size(); ++i)
		{
			if (mapping[i] != action)
				continue;
			if (!out.empty())
				out += words ? ", " : "/";
			out += words ? std::string(names[i]) : "{" + std::string(pbd::controllers::settings::button_slots[i]) + "}";
		}
		return out;
	}

	// What changes for a stick click given an action (the launcher's button-layout.js BL_CLICK_NOTES). A replay's Take Control
	// and Practice's SELECT+R3 read the button itself, before the layout, and keep working; in a fight the action wins, so
	// Practice's Remember on R3 is its menu's Restart Settings row only.
	inline std::string_view click_note(std::uint32_t slot) noexcept
	{
		return pbd::controllers::settings::slot_bits[slot] == r3 ? "Remember Position moves to Restart Settings." : "";
	}

	// The device in use is the keyboard: its keys are the launcher's to change (Controller setup and the Button Layout there),
	// so a key pressed here never changes the layout every controller shares.
	inline constexpr std::string_view keyboard_notice = "Change keys in the launcher: Settings > Controls.";
	inline bool keyboard_in_use()
	{
		return pbd::controllers::read_display().family == "keyboard";
	}

	// What a press, a click or a closing request asks of the window: hear the new volume; apply the video rows (leaving the
	// settings page for the top level); apply them, write the changes file and let the game have the controller back once every
	// button is up (close); ask the launcher to end this run for the chosen row (navigate); or close as Return to Game does and
	// start or stop a recording (record), so the menu itself is never in the video.
	enum class effect : std::uint8_t
	{
		none,
		volume,
		apply,
		close,
		navigate,
		controls,
		record
	};

	// Navigation, as in the Practice menu: Up/Down move through the rows, Left/Right change a value in place, Cross opens a row's
	// choices, Circle or START close. In an open list Up/Down move, Cross chooses and Circle goes back to the rows. Opened on the
	// top level, Cross on Settings opens the settings page and Circle there goes back up; START closes from anywhere.
} // namespace pbd::settings_menu
