#pragma once

#include "Emu/PBD/pbd_practice_presentation.h"
#include "Emu/PBD/pbd_movement_observation.h"
#include "pbd_practice_input_delay.h"
#include <vector>

namespace pbd::practice::menu
{
	struct choice_list
	{
		std::vector<pbd::practice::menu_option> options;
		std::uint32_t current = 0;
	};
	inline void append(choice_list& out, std::string text, std::string help, bool is_default = false, std::string note = {})
	{
		if (is_default && note.empty())
			note = "Default";
		out.options.push_back({std::move(text), std::move(note), std::move(help), is_default, true});
	}
	// A name cut to fit a menu value: at most limit bytes, never inside a UTF-8 sequence, with "..." when cut.
	inline std::string shortened(std::string text, std::size_t limit)
	{
		if (text.size() <= limit || limit < 4)
			return text;
		std::size_t cut = limit - 3;
		while (cut && (static_cast<unsigned char>(text[cut]) & 0xc0) == 0x80)
			--cut;
		while (cut && text[cut - 1] == ' ')
			--cut;
		text.resize(cut);
		return text + "...";
	}
	inline choice_list restart_choices(std::uint32_t current, const std::array<bool, 7>& available)
	{
		constexpr std::array labels{"Centre", "Remembered", "Left Wall", "Right Wall", "Foreground Wall", "Background Wall", "Random"};
		constexpr std::array help{
			"Both fighters start at the centre of the stage ({select}+{cross} in game).",
			"Both fighters start where you last remembered a position ({select}+{r3} in game).",
			"Both fighters start at the left wall ({select}+{dpad:l} in game).",
			"Both fighters start at the right wall ({select}+{dpad:r} in game).",
			"Both fighters start at the foreground wall ({select}+{dpad:d} in game).",
			"Both fighters start at the background wall ({select}+{dpad:u} in game).",
			"Both fighters start at a new spot, sometimes near a wall."};
		choice_list out;
		for (std::size_t i = 0; i < labels.size(); ++i)
		{
			append(out, labels[i], help[i], i == 0);
			out.options.back().enabled = available[i];
		}
		out.current = current < labels.size() ? current : 0;
		return out;
	}
	// Restart Settings' Dummy Direction, in Tekken 8's words: Front leaves the dummy as the Restart Position places it, and
	// Back starts it back-turned. A standing fighter never turns by itself, so it stays that way until something turns it.
	inline choice_list direction_choices(bool back)
	{
		choice_list out;
		append(out, "Front", "The dummy faces you when the fight restarts; at a remembered position it faces as it did.", true);
		append(out, "Back", "The dummy starts with its back to you. Its recorded actions and movement turn it around first.");
		out.current = back ? 1 : 0;
		return out;
	}
	// Restart Settings' Switch Positions, in Tekken 8's words.
	inline choice_list switch_choices(bool switched)
	{
		choice_list out;
		append(out, "Don't Switch Position", "You and the dummy start where the Restart Position puts each of you.", true);
		append(out, "Switch Position", "You start where the dummy would and the dummy where you would, at every Restart Position.");
		out.current = switched ? 1 : 0;
		return out;
	}
	// Input Delay Frames, in Tekken 8's words: Not Set, then 1 to max_input_delay frames.
	inline choice_list input_delay_choices(std::uint32_t frames)
	{
		choice_list out;
		append(out, "Not Set", "Your fighter acts on the frame you press.", true);
		for (std::uint32_t delay = 1; delay <= max_input_delay; ++delay)
			append(out, delay == 1 ? std::string("1 Frame") : std::to_string(delay) + " Frames",
				"Your fighter acts " + (delay == 1 ? std::string("1 frame") : std::to_string(delay) + " frames") +
					" after you press, as in an online match with that much input delay.");
		out.current = frames <= max_input_delay ? frames : 0;
		return out;
	}
	inline choice_list movement_choices(pbd::practice::movement::preset current,
		std::optional<std::uint32_t> character, const char* neutral_help)
	{
		namespace movement = pbd::practice::movement;
		choice_list out;
		const bool wavedash = !character || movement::wavedash_character(*character);
		// Menu order is the preset order (pbd::practice::movement::preset).
		append(out, movement::preset_name(movement::preset::none), neutral_help, true);
		append(out, movement::preset_name(movement::preset::wavedash), wavedash ? std::string("The dummy wavedashes toward you at the Wavedash Speed.") : std::string("Only Kazuya, Heihachi and Devil Jin can wavedash."), false, wavedash ? std::string{} : std::string("Mishima only"));
		out.options.back().enabled = wavedash;
		append(out, movement::preset_name(movement::preset::backdash_cancel),
			"Korean Backdash Cancel: the dummy backdash cancels away from you without stopping.");
		append(out, movement::preset_name(movement::preset::forward_dash), "The dummy dashes toward you again and again without stopping.");
		append(out, movement::preset_name(movement::preset::backdash),
			"The dummy backdashes once, straight before each action; with no action it follows Dummy Action.");
		append(out, movement::preset_name(movement::preset::sidestep_left),
			"The dummy sidesteps to its own left once, straight before each action; with no action it follows Dummy Action.");
		append(out, movement::preset_name(movement::preset::sidestep_right),
			"The dummy sidesteps to its own right once, straight before each action; with no action it follows Dummy Action.");
		append(out, movement::preset_name(movement::preset::sidewalk_left), "The dummy sidesteps to its own left and keeps walking that way.");
		append(out, movement::preset_name(movement::preset::sidewalk_right),
			"The dummy sidesteps to its own right and keeps walking that way.");
		append(out, movement::preset_name(movement::preset::crouch), "The dummy holds down and stays crouched.");
		append(out, movement::preset_name(movement::preset::jump_back), "The dummy jumps back away from you, again each time it lands.");
		out.current = std::min<std::uint32_t>(static_cast<std::uint32_t>(current), movement::preset_count - 1);
		return out;
	}
	// Offense's Movement Technique: what the dummy does on its own, the techniques only (pbd::practice::movement::technique_count),
	// in preset order so an option is its preset.
	inline choice_list technique_choices(pbd::practice::movement::preset current, std::optional<std::uint32_t> character)
	{
		namespace movement = pbd::practice::movement;
		choice_list out;
		const bool wavedash = !character || movement::wavedash_character(*character);
		append(out, movement::preset_name(movement::preset::none), "The dummy follows Dummy Action.", true);
		append(out, movement::preset_name(movement::preset::wavedash), wavedash ? std::string("The dummy wavedashes toward you at the Wavedash Speed.") : std::string("Only Kazuya, Heihachi and Devil Jin can wavedash."), false, wavedash ? std::string{} : std::string("Mishima only"));
		out.options.back().enabled = wavedash;
		append(out, movement::preset_name(movement::preset::backdash_cancel),
			"Korean Backdash Cancel: the dummy backdash cancels away from you without stopping.");
		append(out, movement::preset_name(movement::preset::forward_dash), "The dummy dashes toward you again and again without stopping.");
		append(out, movement::preset_name(movement::preset::backdash), "The dummy backdashes away from you, again each time it can.");
		out.current = movement::is_technique(current) ? static_cast<std::uint32_t>(current) : 0u;
		return out;
	}
	inline choice_list display_choices(std::uint32_t row, const pbd::practice::snapshot& view)
	{
		choice_list out;
		namespace display_rows = pbd::practice::display_rows;
		bool on = false;
		const char* shown = "";
		const char* hidden = "";
		switch (row)
		{
		case display_rows::p1_history:
			on = view.command_history[0];
			shown = "Show P1's inputs at the left edge of the screen.";
			hidden = "Hide P1's command history.";
			break;
		case display_rows::p2_history:
			on = view.command_history[1];
			shown = "Show P2's inputs at the right edge of the screen.";
			hidden = "Hide P2's command history.";
			break;
		case display_rows::attack_info:
			on = view.attack_info;
			shown = "Show each side's last hit, its latest combo's hits and damage, and the session's damage during the fight. "
					"--- marks a value not read yet.";
			hidden = "Hide the Attack Info panels.";
			break;
		case display_rows::frame_data:
			on = view.frame_data;
			shown = "Show each side's Startup and Frame Advantage during the fight, with the outcome for a launch, knockdown, wall hit or throw, "
					"and whether your attack punished one you blocked. --- marks a value not read yet.";
			hidden = "Hide the Frame Data panels.";
			break;
		case display_rows::control_hints:
			on = view.control_hints;
			shown = "Show the control bar at the bottom of the screen during the fight.";
			hidden = "Hide the control bar during the fight; menus still show theirs.";
			break;
		case display_rows::movement_streaks:
			on = view.movement_streaks;
			shown = "Grade each of your backdash cancels, and after 3 or more wavedashes or cancels in a row, show how fast.";
			hidden = "Hide the movement streak pop-up.";
			break;
		default: return out;
		}
		append(out, "Display", shown, true);
		append(out, "Hide", hidden);
		out.current = on ? 0 : 1;
		return out;
	}
} // namespace pbd::practice::menu
