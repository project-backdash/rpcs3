#pragma once

#include "pbd_settings_input.h"

namespace pbd::settings_menu
{
	// What the overlay draws. A copy: the overlay never sees the model or the controller.
	struct view_row
	{
		std::string label;
		std::string value;
		timing when = timing::none;
		bool pending = false;
	};

	struct prompt
	{
		std::string keys;
		std::string label;
	};

	struct view
	{
		bool open = false;
		std::uint64_t revision = 0;
		std::string context;
		// The top level is in view (entries, top_focus); the settings rows and the list are not drawn then. has_top: the
		// settings page goes back to it, so Circle and Esc go back rather than close.
		bool top = false;
		bool has_top = false;
		bool controls = false;
		// A Controls page row waits for its button: the focused row says so.
		bool capturing = false;
		std::uint32_t row_count = pbd::settings_menu::row_count;
		bool leaving = false;
		// The top level's rows as shown, in order, and the focused one's position among them.
		std::vector<std::string> entries;
		std::uint32_t top_focus = 0;
		std::array<view_row, controls_count> rows{};
		std::uint32_t focus = 0;
		bool dropdown = false;
		float stack_top = 0.f;
		std::uint32_t option_focus = 0, option_current = 0;
		std::vector<option> options;
		std::string help;
		std::vector<prompt> prompts;
	};

	inline constexpr std::array<std::string_view, row_count> labels{"Volume", "Resolution", "Window", "VSync", "Audio Output", "Display", "Controller / Button assignments", "Close Settings"};

	inline constexpr std::array<std::string_view, row_count> helps{
		"The game's volume. You hear a change at once, and it is kept for your next session.",
		"The size the game is drawn at. Applies when you close Settings; larger sizes need a faster graphics card.",
		"Fullscreen or a window. Applies when you close Settings.",
		"Matches frames to the screen's refresh to stop tearing; it can add a little delay. Applies when you close Settings.",
		"Where the game's sound plays. Windows default follows the device Windows uses. A new choice applies next session.",
		"The screen the game opens on. A new choice applies next session.",
		"Choose your controller and change button assignments without leaving the game.",
		"Back to the game. Your changes are kept for your next session."};

	// The settings page's last row under the top level: it goes back up, and the video rows apply as they do on a close.
	inline constexpr std::string_view back_label = "Back";
	inline constexpr std::string_view back_help = "Back to the menu. Changes to Resolution, Window and VSync apply now; all are kept for your next session.";

	// What leaving the game does to the run in view, in the menu's context words (context()).
	inline std::string leave_words(std::string_view context)
	{
		if (context == "Practice")
			return "End Practice";
		if (context == "Spectating")
			return "Stop watching";
		if (context == "Offline")
			return "Quit the game";
		return "Stop the replay";
	}

	inline std::string entry_help(entry e, std::string_view context, bool recording = false)
	{
		const std::string leave = leave_words(context);
		switch (e)
		{
		case entry::resume: return "Close the menu and carry on.";
		case entry::settings: return "Volume, resolution, window, VSync, audio output and display.";
		case entry::record:
			return recording ? "Stop and save the recording to your Clips folder." : "Record until you stop it, up to 60 minutes. Saved to your Clips folder.";
		case entry::launcher: return leave + " and go back to the launcher's main menu.";
		case entry::quit: return leave + " and close Project Backdash.";
		}
		return {};
	}

	// The line the top level shows while the launcher ends this run for the chosen row.
	inline std::string_view leaving_help(entry e) noexcept
	{
		return e == entry::quit ? "Closing Project Backdash..." : "Returning to the launcher...";
	}

	// The top level's side note: what both leaving rows do to the run in view.
	inline std::string leave_note(std::string_view context)
	{
		std::string words = leave_words(context);
		if (words.front() >= 'A' && words.front() <= 'Z')
			words.front() = static_cast<char>(words.front() - 'A' + 'a');
		return "Return to Launcher and Exit Game " + words + ".";
	}

	// A row's tag. A change still waiting says so in full ("Applies next session").
	inline std::string_view timing_text(timing t, bool pending = false) noexcept
	{
		switch (t)
		{
		case timing::now: return "Now";
		case timing::on_close: return pending ? "Applies on close" : "On close";
		case timing::next_session: return pending ? "Applies next session" : "Next session";
		case timing::none: break;
		}
		return {};
	}

	// The Controls page: each action's buttons as glyph tokens in the family of the device in use (its keys on the keyboard),
	// no_button where none does it, the waiting row's prompt, and the control bar for the focused row.
	inline view controls_view(const model& m, const controller& c, view out)
	{
		out.controls = true;
		out.capturing = c.capture.active;
		out.row_count = controls_count;
		out.focus = std::min(c.controls_focus, controls_count - 1);
		out.rows[0] = {"Controller", "Not connected", timing::none, false};
		for (const auto& d : m.controllers)
			if (d.id == m.current.input.device)
				out.rows[0].value = d.label;
		const auto device = pbd::controllers::read_display();
		const bool keyboard = device.family == "keyboard";
		// The 1 / 2 / 3 / 4 icons name the face buttons by their usual attack, which this page changes: the buttons' names stand.
		const bool words = device.family == "numbers";
		for (std::uint32_t i = 1; i < controls_reset; ++i)
		{
			const auto action = pbd::controllers::settings::assignments[i];
			const auto buttons = slot_tokens(m.current.input.mapping, action, words);
			out.rows[i] = {action_words(action), buttons.empty() ? std::string(no_button) : buttons, timing::none, false};
		}
		out.rows[controls_reset] = {"Reset to default", "", timing::none, false};
		out.rows[controls_back] = {c.controls_only ? "Return to Game" : "Back", "", timing::none, false};
		const std::string_view action = pbd::controllers::settings::assignments[std::clamp(out.focus, 1u, controls_reset - 1)];
		const std::string named = action_words(action);
		// A row waits only for a controller (start_capture). Start is named as the way out, so a habit's tap of Circle (Back
		// elsewhere) is never the only one known.
		if (c.capture.active)
		{
			out.rows[out.focus].value = "Press a button";
			const std::string none = is_attack(action) ? "" : " ({select} for none)";
			out.help = "Press a button for " + named + none + ". {start} cancels.";
			if (!c.notice.empty())
				out.help = c.notice;
			out.prompts = {{"{start}", "Cancel"}};
			if (!is_attack(action))
				out.prompts.insert(out.prompts.begin(), prompt{"{select}", "None"});
			return out;
		}
		if (out.focus == 0 || out.focus >= controls_back)
			out.help = m.online ? "The game keeps playing. Your fighter stands still." :
			                      "Changes apply now and are kept for your next session. Other devices still take over when touched.";
		else if (out.focus == controls_reset)
			out.help = "Put every action back on its usual button.";
		else
			out.help = keyboard ? std::string(keyboard_notice) : "Press {cross}, then the button you want for " + named + ".";
		if (!c.notice.empty())
			out.help = c.notice;
		std::string confirm = "Change";
		if (out.focus == controls_reset)
			confirm = "Reset";
		else if (out.focus >= controls_back)
			confirm = out.rows[controls_back].label;
		if (out.focus == 0)
			out.prompts = {{"{dpad:lr}", "Change"}, {"{circle}", "Back"}, {"{dpad:ud}", "Move"}};
		else
			out.prompts = {{"{cross}", confirm}, {"{circle}", "Back"}, {"{dpad:ud}", "Move"}};
		return out;
	}

	inline view make_view(const model& m, const controller& c, std::string_view context)
	{
		view out;
		out.open = c.open;
		out.revision = c.revision;
		out.context = std::string(context);
		out.has_top = c.has_top;
		if (c.level == page::controls)
			return controls_view(m, c, std::move(out));
		if (c.level == page::top)
		{
			out.top = true;
			out.leaving = c.leaving;
			const entry e = c.focused_entry(m);
			const auto rows = shown_entries(m);
			for (std::uint32_t i = 0; i < rows.size(); ++i)
			{
				out.entries.emplace_back(entry_label(rows[i], m.recording));
				if (rows[i] == e)
					out.top_focus = i;
			}
			if (c.leaving)
				out.help = std::string(leaving_help(e));
			else
				out.help = c.notice.empty() ? entry_help(e, context, m.recording) : c.notice;
			if (!c.leaving)
				out.prompts = {{"{cross}", "Choose"}, {"{circle}", "Return to Game"}, {"{dpad:ud}", "Move"}};
			return out;
		}
		out.focus = std::min(c.focus, row_count - 1);
		for (std::uint32_t i = 0; i < row_count; ++i)
		{
			const auto r = static_cast<row>(i);
			auto& line = out.rows[i];
			line.label = std::string(labels[i]);
			line.value = value_text(m, r);
			line.when = when(r);
			line.pending = pending(m, r);
		}
		if (c.has_top)
			out.rows[static_cast<std::uint32_t>(row::close)].label = std::string(back_label);
		const row r = c.focused();
		out.help = c.has_top && r == row::close ? std::string(back_help) : std::string(helps[out.focus]);
		if (c.dropdown)
		{
			out.options = options(m, r);
			const auto count = static_cast<std::uint32_t>(out.options.size());
			out.dropdown = count != 0;
			out.stack_top = stack_top(m, r, count);
			out.option_focus = count ? std::min(c.option_focus, count - 1) : 0;
			out.option_current = chosen_index(m, m.current, r);
			out.prompts = {{"{cross}", "Choose"}, {"{circle}", "Back"}, {"{dpad:ud}", "Move"}};
			return out;
		}
		const char* leave = c.has_top ? "Back" : "Close";
		if (r == row::close)
			out.prompts.push_back({"{cross}", leave});
		else if (r != row::volume)
			out.prompts.push_back({"{cross}", "Choose"});
		if (r != row::close)
			out.prompts.push_back({"{dpad:lr}", "Change"});
		out.prompts.push_back({"{circle}", leave});
		out.prompts.push_back({"{dpad:ud}", "Move"});
		return out;
	}

	// The changes file: a launcher GameSettingsPatch (gameSettings.ts) holding only what the player changed from this session's
	// start, under a schema name so a later format can be told apart.
	//   {"schema":"pbd-game-settings-changes-1","patch":{"video":{...},"audio":{...}}}
	inline constexpr std::string_view changes_schema = "pbd-game-settings-changes-1";
	inline constexpr std::string_view changes_file_name = "game-settings-changes.json";

	// The navigate request for a top-level row, as the launcher reads it (inGameRequests.ts readNavigateRequest):
	//   {"schema":"pbd-navigate-1","target":"launcher"}
} // namespace pbd::settings_menu
