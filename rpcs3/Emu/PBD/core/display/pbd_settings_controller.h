#pragma once

#include "pbd_settings_model.h"

namespace pbd::settings_menu
{
	inline void show_controller(model& m, std::string_view device)
	{
		m.current.input.device = std::string(device);
		for (const auto& d : m.controllers)
			if (d.id == device && d.mapped)
				m.current.input.mapping = d.mapping;
	}

	struct controller
	{
		bool open = false;
		// The page in view. The top level exists only when the menu opened on it (has_top); the settings page then goes back to it
		// instead of closing.
		page level = page::settings;
		bool has_top = false;
		// The focused top-level row as an entry value, not its position among the rows shown.
		std::uint32_t top_focus = 0;
		// Return to Launcher or Exit Game was chosen: the menu stays up saying so and takes no more input until this run ends.
		bool leaving = false;
		// A line for the help area in place of the focused row's help (the launcher could not be asked); cleared by the next press.
		std::string notice;
		std::uint32_t focus = 0;
		// The Controls page's focus.
		std::uint32_t controls_focus = 0;
		bool controls_only = false;
		// A Button Layout row waiting for its button (poll_capture): the buttons that were down when it started, each of which
		// counts again only once it is let go (so the press that started it never counts, and a button held on another
		// controller never holds up the wait); whether two buttons pressed together must be let go before the next choice; and
		// the slot button held now, with the time it went down.
		struct capture_state
		{
			bool active = false, settle = false;
			word masked = 0, button = 0;
			std::uint64_t since = 0;
		};
		capture_state capture;
		bool dropdown = false;
		std::uint32_t option_focus = 0;
		// The press that opened the menu (Cross on Practice's Settings row) never also acts in it.
		bool release_required = false;
		word previous = 0;
		// A held direction repeats after 400 ms, every 90 ms, so Volume can sweep its range.
		word repeat = 0;
		std::uint64_t repeat_at = 0;
		// Bumped on every visible change, so the overlay rebuilds only when something moved.
		std::uint64_t revision = 1;

		row focused() const noexcept
		{
			return static_cast<row>(std::min(focus, row_count - 1));
		}
		entry chosen() const noexcept
		{
			return static_cast<entry>(std::min(top_focus, entry_count - 1));
		}
		// The focused top-level row as shown: a focus left on the recording row after clips stopped being able to record is
		// on Settings, the row above it.
		entry focused_entry(const model& m) const noexcept
		{
			const entry e = chosen();
			return shows(m, e) ? e : entry::settings;
		}

		// `top`: open on the top level. It always opens on Return to Game, so a quick second press never leaves the run.
		void show(word held, bool top = false) noexcept
		{
			open = true;
			dropdown = false;
			release_required = held != 0;
			previous = held;
			repeat = 0;
			++revision;
			has_top = top;
			level = top ? page::top : page::settings;
			leaving = false;
			notice.clear();
			capture = {};
			if (top)
				top_focus = 0;
		}

		void hide() noexcept
		{
			open = false;
			dropdown = false;
			repeat = 0;
			leaving = false;
			capture = {};
			++revision;
		}

		// The launcher could not be asked: the top level takes input again, on the row that was chosen, and says why.
		void cancel_leave(std::string why)
		{
			leaving = false;
			notice = std::move(why);
			++revision;
		}

		effect poll(model& m, word held, std::uint64_t now_ms)
		{
			if (!open)
				return effect::none;
			if (release_required)
			{
				previous = held;
				if (!held)
					release_required = false;
				return effect::none;
			}
			if (capture.active)
				return poll_capture(m, held, now_ms);
			const word pressed = static_cast<word>(held & ~previous);
			previous = held;
			word act = pressed;
			constexpr word arrows = up | down | left | right;
			if (pressed & arrows)
			{
				repeat = static_cast<word>(pressed & arrows);
				repeat_at = now_ms + 400;
			}
			else if (repeat && (held & repeat) == repeat)
			{
				if (now_ms >= repeat_at)
				{
					act |= repeat;
					repeat_at = now_ms + 90;
				}
			}
			else
				repeat = 0;
			return press(m, act);
		}

		effect press(model& m, word act)
		{
			if (!open || !act || leaving)
				return effect::none;
			// A waiting row takes its buttons in poll_capture; only Back reaches it here (Esc, a right click), and stops the wait.
			if (capture.active)
				return act & circle ? finish_capture("Nothing changed.") : effect::none;
			if (!notice.empty())
			{
				notice.clear();
				++revision;
			}
			if (level == page::top)
				return press_top(m, act);
			if (level == page::controls)
				return press_controls(m, act);
			const row r = focused();
			if (dropdown)
			{
				const auto list = options(m, r);
				const auto count = static_cast<std::uint32_t>(list.size());
				if (!count)
				{
					dropdown = false;
					++revision;
					return effect::none;
				}
				if (act & (up | down))
				{
					option_focus = pbd::practice::wrap_step(option_focus, count, (act & down) != 0);
					++revision;
				}
				else if (act & cross)
				{
					choose(m, r, list[std::min(option_focus, count - 1)]);
					dropdown = false;
					++revision;
					return back_out();
				}
				else if (act & circle)
				{
					dropdown = false;
					++revision;
				}
				else if (act & start)
					return close();
				return effect::none;
			}
			if (act & start)
				return close();
			if (act & circle)
				return back_out();
			if (act & (up | down))
			{
				focus = pbd::practice::wrap_step(focus, row_count, (act & down) != 0);
				++revision;
				return effect::none;
			}
			if (act & (left | right))
				return step(m, r, (act & right) != 0);
			if (act & cross)
				return activate(m, r);
			return effect::none;
		}

		// The Controls page: Up/Down move through the rows and wrap at either end, Left/Right change the controller on its row,
		// Cross makes an action's row wait for its button, resets the layout, or goes back.
		effect press_controls(model& m, word act)
		{
			if (act & start)
				return close();
			if (act & circle)
				return back_out();
			if (act & (up | down))
			{
				focus_controls(pbd::practice::wrap_step(controls_focus, controls_count, (act & down) != 0));
				return effect::none;
			}
			if (controls_focus >= controls_back)
				return act & cross ? back_out() : effect::none;
			if (controls_focus == 0)
			{
				if (!(act & (left | right | cross)) || m.controllers.empty())
					return effect::none;
				const bool forward = (act & left) == 0;
				std::uint32_t at = 0;
				for (std::uint32_t i = 0; i < m.controllers.size(); ++i)
					if (m.controllers[i].id == m.current.input.device)
						at = i;
				show_controller(m, m.controllers[pbd::practice::wrap_step(at, static_cast<std::uint32_t>(m.controllers.size()), forward)].id);
				++revision;
				return effect::controls;
			}
			if (!(act & cross))
				return effect::none;
			return controls_focus == controls_reset ? reset_layout(m) : start_capture(m);
		}

		void focus_controls(std::uint32_t i) noexcept
		{
			controls_focus = std::min(i, controls_count - 1);
			++revision;
		}

		// Cross on an action: its row waits for a button (poll_capture). Without this PC's controller settings (an older input
		// handler, where the launcher writes the layout into the game's controller file) the menu cannot change the layout; on
		// the keyboard the keys are the launcher's to change.
		effect start_capture(const model& m);
		effect poll_capture(model& m, word held, std::uint64_t now_ms);
		effect settle_capture();
		effect finish_capture(std::string said, effect result = effect::none);
		effect choose_slot(model& m, std::string_view action, word bit);
		effect clear_action(model& m, std::string_view action);
		effect reset_layout(model& m);

		effect step(model& m, row r, bool forward)
		{
			if (r == row::volume)
			{
				const int v = std::clamp(m.current.volume, 0, 100);
				const int next = forward ? std::min(100, v / volume_step * volume_step + volume_step) :
				                           std::max(0, (v + volume_step - 1) / volume_step * volume_step - volume_step);
				if (next == m.current.volume)
					return effect::none;
				m.current.volume = next;
				++revision;
				return effect::volume;
			}
			const auto list = options(m, r);
			if (list.empty())
				return effect::none;
			const auto count = static_cast<std::uint32_t>(list.size());
			const auto i = chosen_index(m, m.current, r);
			std::uint32_t next = 0;
			if (i >= count)
				next = forward ? 0 : count - 1;
			else if (forward ? i + 1 >= count : i == 0)
				return effect::none;
			else
				next = forward ? i + 1 : i - 1;
			choose(m, r, list[next]);
			++revision;
			return effect::none;
		}

		// The top level: Up/Down move, Cross acts on the row, Circle or START return to the game. The focus stops at either end
		// instead of wrapping: the menu opens on Return to Game, and one Up from there must not land on Exit Game, which closes
		// Project Backdash without a question.
		effect press_top(const model& m, word act)
		{
			if (act & (circle | start))
				return close();
			const auto rows = shown_entries(m);
			const auto count = static_cast<std::uint32_t>(rows.size());
			const auto at = static_cast<std::uint32_t>(std::find(rows.begin(), rows.end(), focused_entry(m)) - rows.begin());
			if (act & (up | down))
			{
				const std::uint32_t next = (act & down) != 0 ? std::min(at + 1, count - 1) : (at ? at - 1 : 0);
				if (static_cast<std::uint32_t>(rows[next]) != top_focus)
				{
					top_focus = static_cast<std::uint32_t>(rows[next]);
					++revision;
				}
				return effect::none;
			}
			if (act & cross)
				return pick(rows[at]);
			return effect::none;
		}

		// A top-level row: Return to Game closes, Settings opens the settings page, the recording row closes and starts or stops
		// a recording, the last two ask the launcher to end this run.
		effect pick(entry e)
		{
			if (e == entry::resume)
				return close();
			if (e == entry::record)
			{
				hide();
				return effect::record;
			}
			if (e == entry::settings)
			{
				level = page::settings;
				dropdown = false;
				++revision;
				return effect::none;
			}
			leaving = true;
			++revision;
			return effect::navigate;
		}

		// Leaving the settings page: up to the top level when the menu opened there (the video rows apply as on a close),
		// otherwise out of the menu.
		effect back_out() noexcept
		{
			if (level == page::controls && !controls_only)
			{
				level = page::settings;
				++revision;
				return effect::none;
			}
			if (!has_top)
				return close();
			level = page::top;
			dropdown = false;
			repeat = 0;
			++revision;
			return effect::apply;
		}

		// Cross: a value row opens its choices on the one in effect; Close (Back under the top level) leaves the page.
		effect activate(model& m, row r)
		{
			if (r == row::controls)
			{
				level = page::controls;
				controls_focus = 0;
				++revision;
				return effect::none;
			}
			if (r == row::close)
				return back_out();
			const auto list = options(m, r);
			if (list.empty())
				return effect::none;
			const auto i = chosen_index(m, m.current, r);
			dropdown = true;
			option_focus = i < list.size() ? i : 0;
			++revision;
			return effect::none;
		}

		effect close() noexcept
		{
			hide();
			return effect::close;
		}

		// The pointer in canvas units: hovering moves the focus, a click acts as Cross does (or presses the Volume arrows). While a
		// list is open only its options answer, and a click outside it closes it without a change.
		effect pointer_controls(model& m, float x, float y, bool click);
		effect pointer_top(model& m, float x, float y, bool click);
		effect pointer_dropdown(model& m, row r, float x, float y, bool click);
		effect pointer(model& m, float x, float y, bool click);

		// The wheel moves the focus (or the highlighted option) as Up/Down do.
		effect wheel(model& m, bool toward_top)
		{
			return press(m, toward_top ? up : down);
		}

		// A right click goes back as Circle does: out of an open list, then up to the top level, then out of the menu.
		effect back(model& m)
		{
			return press(m, circle);
		}
	};

} // namespace pbd::settings_menu
