#pragma once

// Offline Versus keeps the stock character select: Triangle opens Controller Setup and R1 takes its costume pick.
// Locked choices and rematches request the stock resource initializer at the main frame boundary, with full handicap.
// Fights cannot pause; the losing player chooses the next game, and holding START + SELECT returns to the launcher.

#include "pbd_hidden_selection.h"
#include "pbd_stage_names.h"
#include "../PBD/pbd_rematch.h"
#include "../PBD/pbd_settings_runtime.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace pbd::versus
{
	using u16 = std::uint16_t;
	using u32 = std::uint32_t;
	using layout = pbd::settings_menu::layout;

	// The game's 16-bit pad word (digital1 << 8 | digital2).
	inline constexpr u16 up = 0x1000, right = 0x2000, down = 0x4000, left = 0x8000, start = 0x0800, select = 0x0100;
	inline constexpr u16 square = 0x80, triangle = 0x10, cross = 0x40, circle = 0x20, r1 = 0x08;
	inline constexpr unsigned sides = 2;

	// The select's per-player states (pbd::hidden_selection::observation::state) this file acts on.
	inline constexpr u32 choosing = 3, handicap = 9, stage = 13;
	// The scenes: the select, the VS load and a game.
	inline constexpr u32 select_scene = 3, select_subscene = 44, load_scene = 16, game_scene = 17;
	// Suppress START during the round intro and fight; other phases retain the game's skip action.
	inline constexpr u32 round_intro_phase = 4, fight_phase = 5;
	// A direct load that has not reached VS in this many frames gives the controllers back (30 s).
	inline constexpr u32 load_limit = 1800;
	// How long a notice stays up (4 s).
	inline constexpr u32 notice_frames = 240;
	// The menu after a game takes no input for half a second, so the presses that ended the game never choose in it.
	inline constexpr u32 menu_settle = 30;
	// START + SELECT held together this long leaves Versus (2 s); the screen says so from leave_shown on.
	inline constexpr u32 leave_frames = 120, leave_shown = 20;
	inline constexpr u16 leave_chord = start | select;

	// Controller Setup: the fifteen actions (assignments 1 to 15), then Check Button Input and the three commands.
	inline constexpr u32 action_rows = 15, setup_rows = 19;
	enum class setup_row : u32
	{
		check = action_rows,
		restore_previous,
		restore_default,
		done
	};
	inline constexpr std::array<std::string_view, setup_rows - action_rows> command_labels{
		"Check Button Input", "Restore Previous Settings", "Restore Default Settings", "Confirm and Exit"};

	inline std::string_view action_of(u32 row) noexcept
	{
		return pbd::controllers::settings::assignments[std::min(row, action_rows - 1) + 1];
	}

	inline layout default_layout()
	{
		return pbd::controllers::settings::values{}.mapping;
	}

	struct setup
	{
		bool open = false;
		u32 focus = 0;
		// Check Button Input: every button shows what it does and none changes anything; Select leaves it.
		bool checking = false;
		std::string name;
		layout current{}, previous{};
		// The buttons held at the last step: a button acts on its press, so the Triangle that opened the panel does nothing here.
		u16 held = 0;
		// A press that could not change the layout (an attack's last button), said on the help line until the next press.
		bool refused = false;
	};

	// Restore Previous Settings returns to the layout captured when the panel opens.
	inline void open_setup(setup& s, std::string name, const layout& now, u16 held)
	{
		s = {};
		s.open = true;
		s.name = std::move(name);
		s.current = s.previous = now;
		s.held = held;
	}

	// An attack keeps a button: only a combination can be left with none.
	inline bool removable(u32 row) noexcept
	{
		return row < action_rows && !pbd::settings_menu::is_attack(action_of(row));
	}

	enum class setup_effect
	{
		none,
		changed,
		closed
	};

	// Read raw buttons before their layout: action rows trade assignments, and Select can clear a combination.
	// Changes apply immediately; checking only displays input until Select returns to the rows.
	inline setup_effect step(setup& s, u16 word)
	{
		const u16 pressed = static_cast<u16>(word & ~s.held);
		s.held = word;
		if (!s.open || !pressed)
			return setup_effect::none;
		s.refused = false;
		if (s.checking)
		{
			if (pressed & select)
				s.checking = false;
			return setup_effect::none;
		}
		if (pressed & start)
		{
			s.open = false;
			return setup_effect::closed;
		}
		if (pressed & up)
			s.focus = (s.focus + setup_rows - 1) % setup_rows;
		else if (pressed & down)
			s.focus = (s.focus + 1) % setup_rows;
		if (s.focus < action_rows)
		{
			if (pressed & select)
			{
				if (!removable(s.focus))
					return setup_effect::none;
				bool changed = false;
				for (auto& job : s.current)
					if (job == action_of(s.focus))
					{
						job = "off";
						changed = true;
					}
				return changed ? setup_effect::changed : setup_effect::none;
			}
			const auto& bits = pbd::controllers::settings::slot_bits;
			for (u32 slot = 0; slot < bits.size(); ++slot)
				if (pressed & bits[slot])
				{
					const auto result = pbd::settings_menu::assign_slot(s.current, action_of(s.focus), slot);
					s.refused = result == pbd::settings_menu::assigned::last_attack;
					return result == pbd::settings_menu::assigned::changed ? setup_effect::changed : setup_effect::none;
				}
			return setup_effect::none;
		}
		if (pressed & circle)
		{
			s.open = false;
			return setup_effect::closed;
		}
		if (!(pressed & cross))
			return setup_effect::none;
		switch (static_cast<setup_row>(s.focus))
		{
		case setup_row::check: s.checking = true; return setup_effect::none;
		case setup_row::restore_previous:
			if (s.current == s.previous)
				return setup_effect::none;
			s.current = s.previous;
			return setup_effect::changed;
		case setup_row::restore_default:
			if (s.current == default_layout())
				return setup_effect::none;
			s.current = default_layout();
			return setup_effect::changed;
		case setup_row::done: s.open = false; return setup_effect::closed;
		}
		return setup_effect::none;
	}

	// The attacks (bit 0 Left Punch to bit 3 Right Kick) that `word`'s buttons do under `l`.
	inline u32 attacks_of(const layout& l, u16 word)
	{
		u32 out = 0;
		const auto& bits = pbd::controllers::settings::slot_bits;
		for (u32 slot = 0; slot < bits.size(); ++slot)
			if (word & bits[slot])
				out |= pbd::settings_menu::limbs(l[slot]);
		return out;
	}

	// After each game, on the select it returns to.
	enum class next : u32
	{
		rematch,
		rematch_random_stage,
		select_character,
		select_stage,
		new_players,
		exit_to_launcher
	};
	inline constexpr u32 next_count = 6;
	inline constexpr std::array<std::string_view, next_count> next_labels{
		"Rematch", "Rematch with Random Stage", "Select Character", "Select Stage", "New Players", "Exit to Launcher"};

	struct menu
	{
		bool open = false;
		u32 focus = 0;
		std::array<u16, sides> held{};
		unsigned player = 0;
	};

	// The losing player works the menu. A button held when it opens acts only once pressed again.
	inline std::optional<next> step(menu& m, const std::array<u16, sides>& words)
	{
		const u16 pressed = static_cast<u16>(words[m.player] & ~m.held[m.player]);
		m.held = words;
		if (!m.open)
			return {};
		if (pressed & up)
			m.focus = (m.focus + next_count - 1) % next_count;
		else if (pressed & down)
			m.focus = (m.focus + 1) % next_count;
		if (!(pressed & cross))
			return {};
		m.open = false;
		return static_cast<next>(m.focus);
	}

	inline u32 draw_stage(u32 random, std::optional<u32> avoid = {})
	{
		std::vector<u32> stages;
		for (const auto& [id, name] : pbd::stage_names::names)
			if (pbd::stage_rule::loadable(id) && (!avoid || id != *avoid))
				stages.push_back(id);
		return stages[random % stages.size()];
	}

	struct setup_view
	{
		bool open = false, checking = false, refused = false;
		u32 focus = 0;
		std::string name;
		// Each action row's buttons as glyph tokens ("{square}", "{l1}/{r2}"), or pbd::settings_menu::no_button.
		std::array<std::string, action_rows> buttons{};
		// While checking: the controller's own buttons and the attacks they do.
		u16 live = 0;
		u32 attacks = 0;
		bool operator==(const setup_view&) const = default;
	};

	// What the overlay draws.
	struct view
	{
		// The character select is up and nobody steers it: the button prompts, and a panel asking for a controller on a side
		// without one.
		bool select = false;
		std::array<bool, sides> empty{};
		std::array<setup_view, sides> setup{};
		bool menu = false;
		u32 menu_focus = 0;
		unsigned menu_player = 0;
		unsigned leave_player = sides;
		// What the select is doing for the players ("Rematch", "Random stage"), or a notice.
		std::string banner;
		bool operator==(const view&) const = default;
	};

	// One frame of the game as the director reads it.
	struct frame
	{
		// The game's VS mode (game mode 4).
		bool versus = false;
		u32 scene = 0, subscene = 0;
		// A game's round phase (the dispatcher's pending phase at 0x10093230).
		u32 phase = 0;
		// Each player's select (pbd::hidden_selection::read_native_observation), left player first.
		std::array<pbd::hidden_selection::observation, sides> players{};
		// Each player's controller: its own buttons, id and name ("" without one) and its layout as it plays now.
		std::array<u16, sides> words{};
		// VS win halfwords, read before the select clears the tournament streaks.
		std::array<u16, sides> wins{};
		std::array<std::string, sides> devices{}, names{};
		std::array<layout, sides> layouts{};
		u32 random = 0;
	};

	struct output
	{
		// The word the game gets on each port; none leaves the player's own, less `hidden` and with `added`.
		std::array<std::optional<u16>, sides> words{};
		std::array<u16, sides> hidden{}, added{};
		// A layout Controller Setup changed, for that player's controller.
		std::array<std::optional<layout>, sides> changed{};
		// START + SELECT was held for leave_frames: back to the launcher.
		bool leave = false;
		bool new_players = false;
		std::optional<pbd::rematch::selection> direct_start;
	};

	class director
	{
		struct plan
		{
			next choice = next::rematch;
			pbd::rematch::selection selected;
			bool choose_stage = false, submitted = false;
		};
		std::array<setup, sides> m_setup{};
		menu m_menu{};
		std::optional<plan> m_plan;
		bool m_played = false;
		pbd::rematch::selection m_previous;
		bool m_known = false;
		u32 m_tick = 0, m_load_from = 0;
		std::array<bool, sides> m_release{};
		std::array<u16, sides> m_last{};
		std::string m_notice;
		u32 m_notice_until = 0, m_menu_from = 0, m_leave_hold = 0;

		void release_all()
		{
			m_release.fill(true);
		}
		void start(next choice, u32 random)
		{
			release_all();
			if (choice == next::new_players)
			{
				m_known = false;
				m_previous = {};
				return;
			}
			if (choice == next::select_character || choice == next::exit_to_launcher)
				return;
			if (!m_known)
			{
				load_failed();
				return;
			}
			m_plan = plan{choice, m_previous, choice == next::select_stage};
			if (choice == next::rematch_random_stage)
				m_plan->selected.stage = draw_stage(random, m_previous.stage);
			m_load_from = m_tick;
		}
		void choose_stage(u16 pressed)
		{
			if (pressed & circle)
			{
				m_plan.reset();
				m_menu = {true, static_cast<u32>(next::select_stage), m_last, m_menu.player};
				return;
			}
			const auto& names = pbd::stage_names::names;
			const auto found = std::find_if(names.begin(), names.end(), [&](const auto& entry)
				{
					return entry.first == m_plan->selected.stage;
				});
			const auto row = static_cast<u32>(found == names.end() ? 0 : found - names.begin());
			if (pressed & (left | up))
				m_plan->selected.stage = names[(row + names.size() - 1) % names.size()].first;
			else if (pressed & (right | down))
				m_plan->selected.stage = names[(row + 1) % names.size()].first;
			if (pressed & cross)
			{
				m_plan->choose_stage = false;
				m_load_from = m_tick;
			}
		}

	public:
		void load_failed()
		{
			m_plan.reset();
			release_all();
			m_notice = "The game could not be started. Choose your characters.";
			m_notice_until = m_tick + notice_frames;
		}

		output step(const frame& f)
		{
			++m_tick;
			output out;
			const bool holding = f.versus && std::any_of(f.words.begin(), f.words.end(), [](u16 w)
												 {
													 return (w & leave_chord) == leave_chord;
												 });
			m_leave_hold = holding ? m_leave_hold + 1 : 0;
			out.leave = m_leave_hold == leave_frames;
			std::array<u16, sides> pressed{};
			for (unsigned i = 0; i < sides; ++i)
			{
				pressed[i] = static_cast<u16>(f.words[i] & ~m_last[i]);
				m_last[i] = f.words[i];
				if (!f.words[i])
					m_release[i] = false;
			}
			if (!f.versus)
			{
				m_setup = {};
				m_menu = {};
				m_plan.reset();
				m_played = m_known = false;
				for (unsigned i = 0; i < sides; ++i)
					if (m_release[i])
						out.words[i] = u16{0};
				return out;
			}
			if (f.scene == game_scene)
			{
				m_played = true;
				const auto& left = f.players[0];
				if (left.applied_verified)
				{
					m_previous = {{left.applied_left, left.applied_right}, left.applied_stage};
					m_known = pbd::rematch::valid(m_previous);
				}
			}
			const bool at_select = f.scene == select_scene && f.subscene == select_subscene;
			if (!at_select)
			{
				for (unsigned i = 0; i < sides; ++i)
					if (m_setup[i].open)
						m_release[i] = true;
				m_setup = {};
				if (m_plan && (f.scene == load_scene || f.scene == game_scene))
				{
					m_plan.reset();
					release_all();
				}
			}
			if (m_plan)
			{
				out.words = {u16{0}, u16{0}};
				if (m_plan->choose_stage)
					choose_stage(pressed[m_menu.player]);
				else if (m_tick - m_load_from > load_limit)
					load_failed();
				else if (at_select && !m_plan->submitted)
				{
					out.direct_start = m_plan->selected;
					m_plan->submitted = true;
				}
				return out;
			}
			if (!at_select)
			{
				if (f.scene == game_scene)
					out.hidden.fill(f.phase == round_intro_phase || f.phase == fight_phase ? leave_chord : versus::select);
				for (unsigned i = 0; i < sides; ++i)
					if (m_release[i])
						out.words[i] = u16{0};
				return out;
			}
			if (m_played)
			{
				m_played = false;
				m_menu = {true, 0, f.words, f.wins[0] ? 1u : 0u};
				m_menu_from = m_tick + menu_settle;
				m_setup = {};
			}
			if (m_menu.open)
			{
				out.words = {u16{0}, u16{0}};
				if (m_tick < m_menu_from)
				{
					m_menu.held = f.words;
					return out;
				}
				if (const auto choice = versus::step(m_menu, f.words))
				{
					out.leave = *choice == next::exit_to_launcher;
					out.new_players = *choice == next::new_players;
					start(*choice, f.random);
				}
				return out;
			}
			// Both players chose a visible character and costume. Loading skips handicap and stage navigation.
			const auto locked = [](const auto& now)
			{
				return now.verified && now.committed_row <= 84 && (now.state == handicap || now.state == stage || now.state == 14);
			};
			if (!m_setup[0].open && !m_setup[1].open && locked(f.players[0]) && locked(f.players[1]))
			{
				m_plan = plan{next::rematch_random_stage, {{f.players[0].committed_row, f.players[1].committed_row}, draw_stage(f.random)}, false, true};
				m_load_from = m_tick;
				out.words = {u16{0}, u16{0}};
				out.direct_start = m_plan->selected;
				return out;
			}
			for (unsigned i = 0; i < sides; ++i)
			{
				const auto& now = f.players[i];
				auto& panel = m_setup[i];
				if (panel.open)
				{
					const auto effect = versus::step(panel, f.words[i]);
					if (effect == setup_effect::changed)
						out.changed[i] = panel.current;
					if (effect == setup_effect::closed)
						m_release[i] = true;
					out.words[i] = u16{0};
					continue;
				}
				if ((pressed[i] & triangle) && !f.devices[i].empty() && now.verified && now.state != stage)
				{
					open_setup(panel, f.names[i], f.layouts[i], f.words[i]);
					out.words[i] = u16{0};
					continue;
				}
				if (m_release[i] || (now.verified && now.state != choosing))
				{
					out.words[i] = u16{0};
					continue;
				}
				out.hidden[i] = triangle | r1 | versus::select | ((f.words[i] & versus::select) ? versus::start : u16{0});
				if (now.verified && now.state == choosing && (f.words[i] & r1))
					out.added[i] = triangle;
			}
			return out;
		}

		view snapshot(const frame& f) const
		{
			view out;
			const bool at_select = f.versus && f.scene == select_scene && f.subscene == select_subscene;
			out.menu = m_menu.open && at_select;
			out.menu_focus = m_menu.focus;
			out.menu_player = m_menu.player;
			out.select = at_select && !out.menu && !m_plan;
			for (unsigned i = 0; i < sides; ++i)
			{
				out.empty[i] = f.devices[i].empty();
				const auto& panel = m_setup[i];
				if (!panel.open)
					continue;
				auto& shown = out.setup[i];
				shown.open = true;
				shown.checking = panel.checking;
				shown.refused = panel.refused;
				shown.focus = panel.focus;
				shown.name = panel.name;
				for (u32 row = 0; row < action_rows; ++row)
				{
					const auto tokens = pbd::settings_menu::slot_tokens(panel.current, action_of(row), false);
					shown.buttons[row] = tokens.empty() ? std::string(pbd::settings_menu::no_button) : tokens;
				}
				if (panel.checking)
				{
					shown.live = f.words[i];
					shown.attacks = attacks_of(panel.current, f.words[i]);
				}
			}
			if (m_plan && at_select)
			{
				out.banner = m_plan->choose_stage ? "Select Stage: " + std::string(pbd::stage_names::name(m_plan->selected.stage)) +
				                                        "  Left/Right: Choose | Cross: Start | Circle: Back" :
				                                    std::string(next_labels[static_cast<u32>(m_plan->choice)]);
			}
			if (m_tick < m_notice_until && at_select)
				out.banner = m_notice;
			if (m_leave_hold >= leave_shown)
			{
				out.banner = "Leaving Versus";
				for (unsigned i = 0; i < sides; ++i)
					if ((f.words[i] & leave_chord) == leave_chord)
						out.leave_player = i;
			}
			return out;
		}
	};

	// The director's view for the overlay to draw (pbd_settings_overlay.cpp), with a revision that changes with it.
	namespace detail
	{
		inline std::mutex mutex;
		inline view shown;
		inline std::uint64_t revision = 0;
	} // namespace detail

	inline void publish(const view& value)
	{
		std::lock_guard lock(detail::mutex);
		if (value == detail::shown)
			return;
		detail::shown = value;
		++detail::revision;
	}

	inline std::pair<view, std::uint64_t> read()
	{
		std::lock_guard lock(detail::mutex);
		return {detail::shown, detail::revision};
	}
} // namespace pbd::versus
