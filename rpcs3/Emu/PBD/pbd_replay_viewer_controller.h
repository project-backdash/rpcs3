#pragma once

#include "pbd_replay_viewer_model.h"

namespace pbd::replay_viewer
{
	struct controller
	{
		page menu = page::none;
		std::uint32_t choice = 0, take_choice = 0;
		// A page opened from an Options row goes back to that row on Circle; a page its own button opened closes.
		page parent = page::none;
		std::uint32_t parent_choice = 0;
		bool history = false, frame_panel = false, attack_panel = false;
		std::array<bool, 2> history_sides{false, false}; // history: either side's command history is shown
		bool release_required = true;                    // Also applies on initial replay open.
		word previous = 0;
		pbd::takeover_phase previous_takeover = pbd::takeover_phase::watching;
		bool menu_was_paused = false;
		bool modifier_owned = false;
		bool display_tap_pending = false;
		// A stick click down when control started (it took control) is no fight input until it is let go.
		word clicks_withheld = 0;
		pbd::replay_rewind::repeat seek_repeat{};

		void barrier(word held) noexcept
		{
			previous = held;
			release_required = held != 0;
			modifier_owned = false;
			display_tap_pending = false;
			seek_repeat.clear();
		}
		void open(page next, word held, bool paused, std::uint32_t focus = 0) noexcept
		{
			menu = next;
			choice = focus;
			parent = page::none;
			parent_choice = 0;
			menu_was_paused = paused;
			barrier(held);
		}
		void enter(page next, word held, std::uint32_t focus = 0) noexcept
		{
			parent = menu;
			parent_choice = choice;
			menu = next;
			choice = focus;
			barrier(held);
		}
		void shut(word held) noexcept
		{
			menu = page::none;
			parent = page::none;
			barrier(held);
		}
		// The whole menu closes; a replay that was playing when it opened plays again.
		action close(word held, const context& c) noexcept
		{
			const bool resume = !menu_was_paused;
			shut(held);
			return resume && c.paused ? action{command::pause} : action{};
		}
		// Circle: back to the Options row that opened this page, or out of the menu.
		action back(word held, const context& c) noexcept
		{
			if (parent == page::none)
				return close(held, c);
			menu = parent;
			choice = parent_choice;
			parent = page::none;
			barrier(held);
			return {};
		}
		action choose(option row, word held, const context& c) noexcept
		{
			switch (row)
			{
			case option::resume:
				shut(held);
				return c.paused ? action{command::pause} : action{};
			case option::take_left:
			case option::take_right:
				shut(held);
				return {command::take_over, row == option::take_right ? 2 : 1};
			case option::speed:
				enter(page::speed, held, static_cast<std::uint32_t>(std::clamp(c.speed, -2, 2) + 2));
				return {};
			case option::rounds:
				enter(page::rounds, held, round_list_focus(c));
				return {};
			case option::display:
				enter(page::display, held);
				return {};
			case option::retry:
			case option::return_replay:
				shut(held);
				return {row == option::retry ? command::retry : command::return_replay};
			case option::launcher:
			case option::quit:
				shut(held);
				return {command::leave, row == option::quit ? 2 : 1};
			}
			return {};
		}

	private:
		void observe_takeover(word held, const context& c) noexcept
		{
			if (c.takeover != previous_takeover)
			{
				const auto from = previous_takeover;
				previous_takeover = c.takeover;
				take_choice = 0;
				menu = page::none;
				parent = page::none;
				display_tap_pending = false;
				// Opening a prompt/arming on a confirm must never reuse that press for the next owner.
				// Starting control preserves the first fight input (the instant anchor's frame). A stick click that took control
				// straight from watching is withheld until it is let go; from the armed state the press that starts control is the
				// fighter's own, so its action carries on.
				if (c.takeover != pbd::takeover_phase::controlling)
					barrier(held);
				else if (from != pbd::takeover_phase::armed)
					clicks_withheld = static_cast<word>(held & (l3 | r3));
			}
			clicks_withheld = static_cast<word>(clicks_withheld & held);
		}

		action poll_prompt(word pressed, const context& c) noexcept
		{
			const std::uint32_t count = c.takeover == pbd::takeover_phase::end_prompt ? 3 : 2;
			if (pressed & up)
				take_choice = (take_choice + count - 1) % count;
			else if (pressed & down)
				take_choice = (take_choice + 1) % count;
			else if (pressed & circle)
				return action{c.takeover == pbd::takeover_phase::end_prompt ? command::return_replay : command::cancel_arm};
			else if (pressed & cross)
			{
				if (c.takeover == pbd::takeover_phase::enable_prompt)
					return action{take_choice == 0 ? command::arm : command::cancel_arm};
				return action{take_choice == 0 ? command::retry : take_choice == 1 ? command::return_replay :
																					 command::bookmark};
			}
			return {};
		}

		action poll_menu(word held, word pressed, const context& c) noexcept
		{
			const auto count = row_count(menu, c);
			if (pressed & up)
			{
				if (count)
					choice = (choice + count - 1) % count;
			}
			else if (pressed & down)
			{
				if (count)
					choice = (choice + 1) % count;
			}
			else if (pressed & start)
				return close(held, c); // Options closes the menu from any page.
			else if (pressed & circle)
				return back(held, c);
			else if ((pressed & cross) && choice < count)
			{
				if (menu == page::playback || menu == page::takeover_options)
					return choose(options_for(menu, c.takeover).rows[choice], held, c);
				if (menu == page::speed)
				{
					const auto picked = static_cast<std::int32_t>(choice) - 2;
					back(held, c);
					return action{command::speed, picked};
				}
				if (menu == page::rounds)
				{
					auto picked = action{command::round, static_cast<std::int32_t>(choice)};
					picked.resume = !menu_was_paused;
					shut(held);
					return picked;
				}
				if (menu == page::display)
				{
					if (choice < 2)
					{
						history_sides[choice] = !history_sides[choice];
						history = history_sides[0] || history_sides[1];
					}
					else if (choice == static_cast<std::uint32_t>(display_row::frame_data))
						frame_panel = attack_panel = !frame_panel;
				}
			}
			return {};
		}

		action poll_controlled(word held, word pressed, const context& c) noexcept
		{
			const word fight = static_cast<word>(ordinary | (c.clicks & ~clicks_withheld));
			// Options owns any simultaneous fight input, but closing it restores the ordinary game bindings.
			if (pressed & start)
			{
				open(page::takeover_options, held, c.paused);
				return c.paused ? action{} : action{command::pause};
			}
			if (held & select)
			{
				if (pressed & select)
					display_tap_pending = held == select;
				if (held != select)
					display_tap_pending = false;
				modifier_owned = true;
				if (c.takeover == pbd::takeover_phase::controlling && (pressed & cross))
					return action{command::retry};
				if (c.takeover == pbd::takeover_phase::controlling && (pressed & circle))
					return action{command::return_replay};
				return {}; // Modifier owns the entire word; a chord never starts armed control.
			}
			if (modifier_owned)
			{
				const bool display_tap = display_tap_pending && !held;
				if (display_tap)
				{
					open(page::display, held, c.paused);
					return c.paused ? action{} : action{command::pause};
				}
				barrier(held);
				return {};
			}
			if (c.takeover == pbd::takeover_phase::armed)
			{
				if (c.in_match && c.fighting && (pressed & fight))
					return action{command::start_take, 0, static_cast<word>(held & fight)};
				return {};
			}
			return action{command::none, 0, static_cast<word>(held & fight)};
		}

		action poll_playback(word held, word pressed, const context& c) noexcept
		{
			if (pressed & select)
			{
				open(page::display, held, c.paused);
				return c.paused ? action{} : action{command::pause};
			}
			if (pressed & start)
			{
				open(page::playback, held, c.paused);
				return c.paused ? action{} : action{command::pause};
			}
			if (pressed & triangle)
			{
				open(page::rounds, held, c.paused, round_list_focus(c));
				return c.paused ? action{} : action{command::pause};
			}
			if (pressed & circle)
				return action{command::pause};
			const word step = seek_repeat.poll(held, pressed, left | right, c.now_ms);
			if (step == left)
				return action{command::seek_back};
			if (step == right)
				return action{command::seek_forward};
			if (pressed & l1)
				return action{command::previous_round};
			if (pressed & r1)
				return action{command::next_round};
			if (pressed & l3)
				return action{command::take_over, 1};
			if (pressed & r3)
				return action{command::take_over, 2};
			if (pressed & r2)
				return action{c.paused ? command::step : command::speed, c.speed + 1};
			if (pressed & l2)
				return action{c.paused ? command::step_back : command::speed, c.speed - 1};
			return {};
		}

	public:
		action poll(word held, const context& c) noexcept
		{
			const word pressed = static_cast<word>(held & ~previous);
			previous = held;
			observe_takeover(held, c);
			if (release_required)
			{
				if (!held)
				{
					release_required = false;
					modifier_owned = false;
				}
				return {};
			}
			// Leave holds suppress every menu action and guest input.
			if ((held & (start | select)) == (start | select))
			{
				barrier(held);
				return {};
			}
			if (c.takeover == pbd::takeover_phase::end_prompt || c.takeover == pbd::takeover_phase::enable_prompt)
				return poll_prompt(pressed, c);
			if (menu != page::none)
				return poll_menu(held, pressed, c);
			if (c.takeover == pbd::takeover_phase::armed || c.takeover == pbd::takeover_phase::controlling)
				return poll_controlled(held, pressed, c);
			return poll_playback(held, pressed, c);
		}

		// A click reuses the same menu activation as the pad. Its confirm never becomes a guest input or a second held press.
		action pointer(std::uint32_t row, const context& c, word held) noexcept
		{
			if (row == pointer_back)
			{
				controller next = *this;
				next.poll(0, c);
				const auto answer = next.poll(circle, c);
				*this = next;
				barrier(held);
				return answer;
			}
			if (row == pointer_exit)
			{
				barrier(held);
				return {command::exit};
			}
			controller next = *this;
			next.poll(0, c);
			if (row == pointer_options)
			{
				if (c.takeover == pbd::takeover_phase::end_prompt || c.takeover == pbd::takeover_phase::enable_prompt)
					return {};
				const auto answer = next.poll(next.menu == page::none ? start : circle, c);
				*this = next;
				barrier(held);
				return answer;
			}
			const auto count = c.takeover == pbd::takeover_phase::end_prompt ? 3u : c.takeover == pbd::takeover_phase::enable_prompt ? 2u :
			                                                                                                                           row_count(next.menu, c);
			if (row >= count)
				return {};
			next.choice = row;
			next.take_choice = row;
			const auto answer = next.poll(cross, c);
			*this = next;
			barrier(held);
			return {answer.kind, answer.value, 0, answer.resume};
		}
	};

	// Bits 4 and 5 hide individual histories; with neither set, bit 0 shows both sides.
	constexpr std::uint32_t display_flags(const controller& c, bool fighting) noexcept
	{
		return (c.history ? 1u : 0u) | (c.frame_panel ? 2u : 0u) | (c.attack_panel ? 4u : 0u) |
		       (fighting ? 8u : 0u) | (c.history_sides[0] ? 0u : 16u) | (c.history_sides[1] ? 0u : 32u);
	}
	constexpr bool history_visible(std::uint32_t flags, std::size_t side) noexcept
	{
		return side < 2 && (flags & 1u) && !(flags & (16u << side));
	}

} // namespace pbd::replay_viewer
