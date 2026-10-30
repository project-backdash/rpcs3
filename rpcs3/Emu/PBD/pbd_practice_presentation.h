#pragma once

#include "pbd_practice_menu_model.h"

namespace pbd::practice
{
	// Practice's in-game move list as the renderer draws it (pbd_practice_move_list.h): the rows in view, each a category
	// title or a move with its command and whether it is on, the highlighted one, and the line saying what the list is for.
	inline constexpr std::size_t move_list_lines = 14;
	struct move_list_line
	{
		std::string name, command;
		bool header = false, selectable = false, on = false, focus = false;
		// The command in arrow and button glyph tokens, drawn instead of its text when it fits.
		std::string glyphs;
	};
	struct move_list_panel
	{
		bool open = false;
		// What the moves are for ("Dummy Action 3"), how many are on, and where the highlighted move is ("12 / 140").
		std::string title, detail, position, help;
		std::array<move_list_line, move_list_lines> lines{};
		std::uint32_t count = 0;
		bool more_above = false, more_below = false;
	};

	struct snapshot
	{
		bool active = false;
		bool hud_visible = false;
		// Display Settings: each side's command history (P1 at the left edge, P2 at the right), the Attack Info and Frame Data
		// panels and the in-game control bar. Menus always draw their own control bar.
		std::array<bool, 2> command_history{true, true};
		bool attack_info = true;
		bool frame_data = true;
		bool control_hints = true;
		// The pop-up for a run of P1's wavedashes or backdash cancels once it ends.
		bool movement_streaks = true;
		// The fresh (never re-executed) boundary that published this view was about to run a Practice fight step
		// (practice_fight). False through loading, a K.O. or the game's own selection, so the panels never cover them.
		bool fight_live = false;
		bool native_menu = false;
		std::uint64_t menu_generation = 0;
		std::uint32_t mouse_context = 0, mouse_row_base = 0;
		std::uint64_t mouse_emulation = 0;
		std::uint64_t mouse_revision = 0;
		bool mouse_pages = false;
		std::string section = "Restart Settings";
		std::array<history, 2> inputs{};
		std::array<feedback, 2> fighters{};
		// Toast title and optional detail separated by a tab; both may carry {glyph} tokens.
		std::string toast;
		std::string dummy_status;
		// Pause menu: the categories (left rail), the one in use, the header text and the control bar for the focused row.
		// A decision layout presents the rows as a centred dialog instead of a list. An unavailable category is dimmed with
		// its note ("Coming soon") at the rail's right edge; a category without rows shows list_note in place of its list.
		std::array<std::string, max_pages> pages{};
		std::array<std::string, max_pages> page_notes{};
		std::array<bool, max_pages> page_unavailable{};
		std::uint32_t page = 0;
		std::string list_note;
		// The rail has focus: its current item carries the focus bar and the help line describes that category. With the
		// list focused, the current item keeps only the gold underline.
		bool rail_focus = false;
		std::string rail_help;
		std::string header;
		menu_layout layout = menu_layout::list;
		// The dialog's message, centred above its options.
		std::string dialog_title;
		// Dialog detail lines are separated by '\n' and may carry {glyph} tokens.
		std::string dialog_detail;
		prompt_bar menu_prompts{};
		dropdown menu_dropdown{};
		// A Yes / No question stacked over the list (Restore Defaults, Character Selection, Stage Selection, Exit Practice).
		confirm_dialog confirm{};
		// The move list, open over the menu to choose a Dummy Action's or a reaction's moves.
		move_list_panel move_list{};
		// Hotkey legend drawn as one line of glyph groups under the list (Restart Settings' in-game restart chords).
		prompt_bar list_legend{};
		// In-game control bar and the dummy's recording or playback state for the top-centre band.
		prompt_bar prompts{};
		dummy_phase dummy = dummy_phase::idle;
		std::uint32_t dummy_frames = 0, dummy_limit = 1800;
		std::string dummy_slot;
	};

	struct shared_state
	{
		std::mutex mutex;
		snapshot current;
	};

	inline shared_state& state()
	{
		static shared_state value;
		return value;
	}

	inline void publish(const snapshot& value)
	{
		auto& shared = state();
		std::lock_guard lock(shared.mutex);
		shared.current = value;
	}

	inline snapshot load()
	{
		auto& shared = state();
		std::lock_guard lock(shared.mutex);
		return shared.current;
	}

	inline bool requested() noexcept
	{
		const auto* value = pbd::env::find("PBD_PRACTICE_REWIND");
		return value && value[0] == '1' && !value[1];
	}

	// Sample the mapped game masks once per advancing boundary, after Practice has consumed its own chords. Repeated callbacks and
	// host pause polls do not add held frames. Restart clears both the history and boundary identity before its first live sample.
	class history_tracker
	{
	public:
		void reset() noexcept
		{
			m_inputs = {};
			m_boundary.reset();
		}

		bool sample(std::uint64_t boundary, const std::array<std::uint16_t, 2>& masks, bool advancing = true) noexcept
		{
			if (!advancing || (m_boundary && boundary <= *m_boundary))
				return false;
			m_boundary = boundary;
			for (std::size_t port = 0; port < m_inputs.size(); port++)
			{
				auto& list = m_inputs[port];
				const command next{static_cast<std::uint16_t>(masks[port] & gameplay_mask), 1};
				if (list.count && list.rows[0].mask == next.mask)
				{
					if (list.rows[0].frames != 0xffffffffu)
						list.rows[0].frames++;
					continue;
				}
				const auto count = (std::min)(list.count, static_cast<std::uint32_t>(history_size - 1));
				for (std::uint32_t row = count; row > 0; row--)
					list.rows[row] = list.rows[row - 1];
				list.rows[0] = next;
				list.count = (std::min)(list.count + 1, static_cast<std::uint32_t>(history_size));
			}
			return true;
		}

		bool sample(std::uint64_t boundary, const std::array<std::uint32_t, 2>& masks, bool advancing = true) noexcept
		{
			return sample(boundary, std::array<std::uint16_t, 2>{static_cast<std::uint16_t>(masks[0]), static_cast<std::uint16_t>(masks[1])}, advancing);
		}

		const std::array<history, 2>& inputs() const noexcept
		{
			return m_inputs;
		}

	private:
		std::optional<std::uint64_t> m_boundary;
		std::array<history, 2> m_inputs{};
	};

	inline std::string command_text(std::uint16_t mask)
	{
		mask &= gameplay_mask;
		if (!mask)
			return "N";
		std::string text;
		const auto add = [&](const char* part)
		{
			if (!text.empty())
				text += "+";
			text += part;
		};
		// Screen directions, as the stick was pushed.
		if (mask & 0x1000)
			add("U");
		if (mask & 0x4000)
			add("D");
		if (mask & 0x8000)
			add("L");
		if (mask & 0x2000)
			add("R");
		if (mask & 0x0080)
			add("1");
		if (mask & 0x0010)
			add("2");
		if (mask & 0x0040)
			add("3");
		if (mask & 0x0020)
			add("4");
		if (mask & 0x0004)
			add("L1");
		if (mask & 0x0008)
			add("R1");
		if (mask & 0x0001)
			add("L2");
		if (mask & 0x0002)
			add("R2");
		return text;
	}

	namespace detail
	{
		inline std::string panel_number(const std::optional<std::uint32_t>& field)
		{
			return field ? std::to_string(*field) : std::string(unknown_value);
		}
	} // namespace detail

	// Attack Info, in the usual order where DR has the fact. Hit Properties is the latest contact's hit level. Combo Damage
	// and Hits (the latest combo) take the places of Max Combo Damage and Recoverable Damage; DR has no recoverable health.
	// The first line is the damage of the whole session since the last restart, and says so.
	inline std::array<panel_line, attack_info_lines> attack_info_text(const feedback& value)
	{
		const std::string outcome = value.outcome == contact::hit ? "Hit" : value.outcome == contact::block ? "Block" :
		                                                                                                      std::string(unknown_value);
		return {{
			{"Session Damage", detail::panel_number(value.total_damage)},
			{"Combo Damage", detail::panel_number(value.combo_damage)},
			{"Hit Properties", value.hit_level.empty() ? std::string(unknown_value) : std::string(value.hit_level), 0, 0, "Hit Level"},
			{"Damage", detail::panel_number(value.damage)},
			{"Hits", detail::panel_number(value.hits)},
			{"Hit / Block", outcome},
		}};
	}

	// Where the Attack Info and Frame Data panels sit on the 1280x720 canvas: side by side along the bottom, above the control
	// bar, Attack Info nearest the fighter's own screen edge. They keep clear of the game's own HUD (the life bars and timer
	// at the top, each side's combo counter, "N HIT COMBO" over its damage, at mid height, and the Practice prompt at the
	// bottom right) and of the command history column on their side, moving in from the screen edge while it is shown. The
	// two sides never meet in the middle.
	struct panel_rect
	{
		float x = 0.f, y = 0.f, w = 0.f, h = 0.f;
		constexpr bool overlaps(const panel_rect& other) const noexcept
		{
			return x < other.x + other.w && other.x < x + w && y < other.y + other.h && other.y < y + h;
		}
	};
	// The bottom edge clears the game's Practice prompt below (y 615 on), with the row pitch that still fits six Attack Info
	// rows under the combo counter.
	inline constexpr float panel_width = 224.f, panel_pitch = 21.f, panel_inset = 4.f, panel_gap = 6.f, panels_bottom = 606.f;
	// Replays also draw the game's WINS streak below y 550 on both sides.
	inline constexpr float replay_panels_bottom = 534.f;
	// The game's HUD and the command history column on the left (P1's side); the right side mirrors them. The top HUD runs
	// down to the Practice DAMAGE line under the name (y 130 to 152 in a 1280x720 capture).
	inline constexpr panel_rect game_top_hud{0.f, 0.f, 640.f, 160.f}, game_combo_counter{0.f, 200.f, 520.f, 270.f};
	// The game's own Practice prompt at the bottom right, "COMMAND CAPTURE" over "SELECT + O : REC" (about x 960 to 1260,
	// y 615 to 665). Both sides keep clear of it, and of its mirror, so the panels stay level.
	inline constexpr panel_rect game_capture_prompt{960.f, 612.f, 300.f, 56.f};
	inline constexpr panel_rect history_column{34.f, 150.f, 120.f, 360.f};
	// The control bar's scrim starts here.
	inline constexpr float control_bar_top = 628.f;
	inline constexpr panel_rect mirrored(const panel_rect& rect) noexcept
	{
		return {1280.f - rect.x - rect.w, rect.y, rect.w, rect.h};
	}
	inline constexpr float panel_height(std::size_t lines) noexcept
	{
		return panel_pitch * static_cast<float>(lines) + 2.f * panel_inset;
	}
	struct panel_layout
	{
		std::optional<panel_rect> attack_info, frame_data;
	};
	inline panel_layout place_panels(std::size_t side, bool history_shown, bool attack_info, bool frame_data, float bottom = panels_bottom) noexcept
	{
		panel_layout out;
		float x = history_shown ? history_column.x + history_column.w + 22.f : 56.f;
		const auto place = [&](std::size_t lines)
		{
			const float h = panel_height(lines);
			const panel_rect left{x, bottom - h, panel_width, h};
			x += panel_width + panel_gap;
			return side ? mirrored(left) : left;
		};
		if (attack_info)
			out.attack_info = place(attack_info_lines);
		if (frame_data)
			out.frame_data = place(frame_observed_lines);
		return out;
	}
	inline panel_rect panel_line_rect(const panel_rect& panel, std::size_t line, std::size_t observed, std::size_t total) noexcept
	{
		const bool columns = observed < total, reference = line >= observed;
		const auto rows = (std::max)(observed, total - observed);
		const float width = panel.w / (columns ? 2.f : 1.f);
		const float pitch = (panel.h - 2.f * panel_inset) / static_cast<float>(rows);
		return {panel.x + (reference ? width : 0.f), panel.y + panel_inset + pitch * static_cast<float>(reference ? line - observed : line), width, pitch};
	}

	// The most recent move: its startup, and its frame advantage (who acts first, and by how many frames) whether it was
	// blocked, hit or whiffed.
	// Block and hit values come from move data the moment the reaction starts (pbd::practice::frames::calculator); a whiff
	// is measured once the attacker can act. A new move clears the previous result, so at most one of the three is set.
	// A contact without a plain advantage (a launch, a knockdown, a juggle, a wall or a throw) shows its label instead. The
	// readiness text, when there is one, replaces both (it carries the label as its word). Beside "Frame Advantage" a
	// panel_width panel has room for about ten characters of the value; the readiness text runs to advantage_text_limit
	// ("+16 Crumple (-14)"), so such a value is drawn beside "Advantage", which leaves room for all of it.
	inline std::array<panel_line, frame_data_lines> frame_data_text(const feedback& value)
	{
		const auto advantage = value.on_block ? value.on_block : value.on_hit ? value.on_hit :
		                                                                        value.on_whiff;
		std::string shown = !value.advantage_text.empty() ? value.advantage_text :
		                    advantage                     ? (*advantage > 0 ? "+" : "") + std::to_string(*advantage) :
		                    !value.result_label.empty()   ? std::string(value.result_label) :
		                                                    std::string(unknown_value);
		return {{
			{"Startup", value.startup ? std::to_string(*value.startup) : std::string(unknown_value)},
			{"Frame Advantage", std::move(shown), value.advantage_marked_at, value.advantage_marked_size, "Advantage"},
		}};
	}
} // namespace pbd::practice
