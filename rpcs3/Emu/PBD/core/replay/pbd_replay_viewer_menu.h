#pragma once

#include "pbd_replay_viewer_presentation.h"

namespace pbd::replay_viewer
{
	// Presentation model for the overlay. Row n of a page is controller choice n (plus `first` on the round list), so a
	// drawn row and the pad choice it represents can never disagree.
	enum class tone : std::uint8_t
	{
		plain,
		default_value,
		changed
	};
	struct menu_row
	{
		std::string label, value, help;
		tone value_tone = tone::plain;
	};
	struct menu_view
	{
		enum class shape : std::uint8_t
		{
			none,
			list,
			dialog
		};
		shape kind = shape::none;
		std::string title;                  // list: the page title; dialog: the question
		std::array<std::string, 2> lines{}; // dialog body lines
		std::array<menu_row, max_menu_rows> rows{};
		std::uint32_t count = 0, focus = 0, first = 0;
		std::string note; // drawn in place of rows on a page that has none
	};
	inline std::string take_prompt_title(std::uint32_t port, std::string_view character)
	{
		if (character.empty() || character.size() > 64 ||
			std::any_of(character.begin(), character.end(), [](unsigned char c)
				{
					return c < 0x20 || c > 0x7e;
				}))
			return std::string("Enable controls for the ") + (port == 2 ? "right" : "left") + "-side character?";
		return "Take control of " + std::string(character) + (port == 2 ? " (Right)?" : " (Left)?");
	}
	inline void add_menu_row(menu_view& v, std::string label, std::string value = {}, tone value_tone = tone::plain, std::string help = {})
	{
		if (v.count < v.rows.size())
			v.rows[v.count++] = {std::move(label), std::move(value), std::move(help), value_tone};
	}

	inline std::string display_state_text(bool on)
	{
		return std::string(on ? "On" : "Off");
	}

	inline bool takeover_dialog(menu_view& v, const snapshot& s)
	{
		if (s.takeover == pbd::takeover_phase::enable_prompt)
		{
			v.kind = menu_view::shape::dialog;
			const char* character = pbd::env::find(s.port == 2 ? "PBD_REPLAY_RIGHT_CHARACTER" : "PBD_REPLAY_LEFT_CHARACTER");
			v.title = take_prompt_title(s.port, character ? character : "");
			v.lines = {"Left / Right: choose a side. Your opponent follows the replay.", "Yes starts a 10-second attempt."};
			add_menu_row(v, "Yes");
			add_menu_row(v, "No");
			v.focus = s.take_choice;
			return true;
		}
		if (s.takeover == pbd::takeover_phase::end_prompt)
		{
			v.kind = menu_view::shape::dialog;
			v.title = "Try this moment again?";
			v.lines[0] = s.bookmarked == 1 ? "Saving replay bookmark..." :
			             s.bookmarked == 2 ? "Replay bookmark saved." :
			             s.bookmarked == 3 ? "Could not save the bookmark. Try again." :
			                                 "Saving this situation to Practice is not available yet.";
			add_menu_row(v, "Try Again");
			add_menu_row(v, "Return to Replay");
			add_menu_row(v, "Bookmark This Moment");
			v.focus = s.take_choice;
			return true;
		}
		return false;
	}

	inline void playback_rows(menu_view& v, const snapshot& s, page menu)
	{
		v.title = menu == page::playback ? "REPLAY" : "TAKE CONTROL";
		const auto rows = options_for(menu, s.takeover);
		for (std::uint32_t i = 0; i < rows.count; ++i)
		{
			switch (rows.rows[i])
			{
			case option::resume: add_menu_row(v, "Resume", {}, tone::plain, "Close the menu and play."); break;
			case option::take_left: add_menu_row(v, "Take Control: Player 1", {}, tone::plain, "Play Player 1 from here. Player 2 follows the replay."); break;
			case option::take_right: add_menu_row(v, "Take Control: Player 2", {}, tone::plain, "Play Player 2 from here. Player 1 follows the replay."); break;
			case option::speed:
				add_menu_row(v, "Playback Speed", speed_text(s.speed), s.speed == 2 ? tone::default_value : tone::changed, "Choose the playback speed.");
				break;
			case option::rounds: add_menu_row(v, "Round Select", {}, tone::plain, "Jump to a round of this match."); break;
			case option::display: add_menu_row(v, "Display", {}, tone::plain, "Command history and frame data."); break;
			case option::retry: add_menu_row(v, "Try Again", {}, tone::plain, "Restart from the moment you took control."); break;
			case option::return_replay: add_menu_row(v, "Return to Replay", {}, tone::plain, "Give control back and watch on."); break;
			case option::launcher: add_menu_row(v, "Return to Launcher", {}, tone::plain, "Stop the replay and go back to the launcher."); break;
			case option::quit: add_menu_row(v, "Exit Game", {}, tone::plain, "Stop the replay and close Project Backdash."); break;
			}
		}
	}

	inline void playback_speed_rows(menu_view& v, const snapshot& s)
	{
		v.title = "PLAYBACK SPEED";
		for (std::uint32_t i = 0; i < 5; ++i)
			add_menu_row(v, speed_text(i), i == s.speed ? "Current" : i == 2 ? "Default" :
																			   "",
				i == s.speed ? tone::changed : tone::default_value,
				"Playback speed for this replay.");
	}

	inline void round_rows(menu_view& v, const snapshot& s)
	{
		v.title = "ROUND SELECT";
		if (!s.count)
			v.note = "No recorded round boundaries yet.";
		else
		{
			v.first = s.choice > 5 ? s.choice - 5 : 0;
			for (std::uint32_t i = v.first; i < std::min(s.count, v.first + 8); ++i)
				add_menu_row(v, "Round " + std::to_string(s.rounds[i]));
		}
	}

	inline void display_rows(menu_view& v, const snapshot& s)
	{
		v.title = "DISPLAY";
		for (std::uint32_t side = 0; side < 2; ++side)
		{
			const bool on = history_visible(s.flags, side);
			add_menu_row(v, side ? "P2 Command History" : "P1 Command History", display_state_text(on), on ? tone::changed : tone::default_value,
				side ? "Show Player 2's inputs." : "Show Player 1's inputs.");
		}
		const bool frames = s.flags & 2;
		add_menu_row(v, "Frame Data", display_state_text(frames), frames ? tone::changed : tone::default_value,
			"Startup and frame advantage of each side's attacks.");
	}

	inline menu_view menu_layout(const snapshot& s)
	{
		menu_view v;
		if (s.fast)
			return v;
		if (takeover_dialog(v, s))
			return v;
		const auto menu = static_cast<page>(s.menu);
		if (menu == page::none)
			return v;
		v.kind = menu_view::shape::list;
		v.focus = s.choice;
		switch (menu)
		{
		case page::takeover_options:
		case page::playback: playback_rows(v, s, menu); break;
		case page::speed: playback_speed_rows(v, s); break;
		case page::rounds: round_rows(v, s); break;
		case page::display: display_rows(v, s); break;
		default: break;
		}
		return v;
	}

	// Control bar entries. Keys use the overlay's glyph tokens ("{a}+{b}" is a chord, "{a}/{b}" a pair); only actions that
	// work in the current state are listed. Entries with a pointer target are clickable.
	struct prompt
	{
		std::string keys, label;
		std::uint32_t pointer = pointer_none;
	};
	struct prompt_list
	{
		std::array<prompt, 8> items{};
		std::uint32_t count = 0;
		void add(std::string keys, std::string label, std::uint32_t pointer = pointer_none)
		{
			if (count < items.size())
				items[count++] = {std::move(keys), std::move(label), pointer};
		}
	};
	inline prompt_list prompts(const snapshot& s)
	{
		prompt_list p;
		const auto leave = [&]()
		{
			p.add("{start}+{select}", "Exit (Hold)", pointer_exit);
		};
		if (s.fast)
		{
			leave();
			return p;
		}
		if (s.takeover == pbd::takeover_phase::end_prompt || s.takeover == pbd::takeover_phase::enable_prompt)
		{
			p.add("{cross}", "Confirm");
			p.add("{circle}", s.takeover == pbd::takeover_phase::end_prompt ? "Return to Replay" : "Cancel");
			p.add("{dpad:ud}", "Move");
			leave();
			return p;
		}
		const auto view = menu_layout(s);
		if (view.kind == menu_view::shape::list)
		{
			const auto row = view.focus - view.first;
			if (view.focus >= view.first && row < view.count)
				p.add("{cross}", "Confirm");
			p.add("{circle}", "Back", pointer_options);
			if (view.count > 1)
				p.add("{dpad:ud}", "Move");
			leave();
			return p;
		}
		// The control bar's entries in order. The bar stops at the first entry that no longer fits, so the hold-to-leave entry (also
		// Return to Launcher in the menu) comes last.
		if (s.takeover == pbd::takeover_phase::armed || s.takeover == pbd::takeover_phase::controlling)
		{
			p.add("{start}", "Menu", pointer_options);
			if (s.takeover == pbd::takeover_phase::controlling)
			{
				p.add("{select}+{cross}", "Retry");
				p.add("{select}+{circle}", "Return to Replay");
			}
			p.add("{select}", "Display");
			leave();
			return p;
		}
		p.add("{circle}", "Pause/Play");
		p.add("{l3}/{r3}", "Take control P1/P2");
		p.add("{triangle}", "Rounds");
		p.add("{l1}/{r1}", "Prev/Next Round");
		if (s.paused)
			p.add("{l2}/{r2}", "Previous/Next Frame");
		else
			p.add("{l2}/{r2}", "Speed");
		p.add("{start}", "Menu", pointer_options);
		p.add("{dpad:lr}", "Rewind/FF");
		leave();
		return p;
	}

	// The help line above the control bar: the focused row's description, or what to do while control is armed.
	inline std::string help_line(const snapshot& s)
	{
		if (s.fast)
			return {};
		const auto view = menu_layout(s);
		if (view.kind == menu_view::shape::list)
		{
			const auto row = view.focus - view.first;
			return view.focus >= view.first && row < view.count ? view.rows[row].help : std::string{};
		}
		if (view.kind == menu_view::shape::none && s.takeover == pbd::takeover_phase::armed)
			return (s.flags & 8) ? "Press any fight input to take control for 10 seconds." : "Wait for Fight!, then press a fresh fight input to take control.";
		return {};
	}

	inline std::string seconds_text(std::uint32_t frames)
	{
		const auto tenths = (frames + 5) / 6;
		return std::to_string(tenths / 10) + "." + std::to_string(tenths % 10) + " s";
	}

	// The bottom-right readout while the replay runs ("Replay, 1x, Round 2, 0:34" with dot separators).
	inline std::string status_line(const snapshot& s)
	{
		if (s.fast)
			return "Fast-forwarding to the replay moment";
		const std::string dot = "  \xC2\xB7  ";
		const auto seconds = s.frame / 60;
		std::string text = std::string(s.paused ? "Paused" : "Replay") + dot + speed_text(s.speed);
		if (s.round)
			text += dot + "Round " + std::to_string(s.round);
		return text + dot + std::to_string(seconds / 60) + (seconds % 60 < 10 ? ":0" : ":") + std::to_string(seconds % 60);
	}

	// The take-control tag beside the controlled side's history column. Empty when no side is controlled.
	inline std::string badge_text(const snapshot& s)
	{
		if (s.fast || (s.takeover != pbd::takeover_phase::armed && s.takeover != pbd::takeover_phase::controlling))
			return {};
		const char* side = s.port == 2 ? "P2" : "P1";
		return (s.takeover == pbd::takeover_phase::controlling ? std::string("You control ") + side : std::string(side) + " ready") + "  \xC2\xB7  " + seconds_text(s.frames_left);
	}
} // namespace pbd::replay_viewer
