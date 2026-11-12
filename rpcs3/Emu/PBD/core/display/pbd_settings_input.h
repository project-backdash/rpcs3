#pragma once

#include "pbd_settings_controller.h"

namespace pbd::settings_menu
{
	inline effect controller::start_capture(const model& m)
	{
		notice.clear();
		if (m.controllers.empty())
			notice = "Change the Button Layout in the launcher: Settings, Controls.";
		else if (keyboard_in_use())
			notice = std::string(keyboard_notice);
		else
			capture = {true, false, previous, 0, 0};
		++revision;
		return effect::none;
	}

	// While a row waits: a slot button acts when it is let go, so Circle, which is also Back, can be given a job; held for
	// hold_cancel_ms it cancels, as Start or a direction does at once. Select leaves a combination with no button. A button
	// that was down when the row started waiting counts only once it has been let go, and the release that ends the wait
	// never acts in the rows (`previous` holds it).
	inline effect controller::poll_capture(model& m, word held, std::uint64_t now_ms)
	{
		capture.masked = static_cast<word>(capture.masked & held);
		const word pressed = static_cast<word>(held & ~previous & ~capture.masked);
		previous = held;
		// A key pressed while the row waits (the keyboard took over from the controller) is never a button's choice.
		if (pressed && keyboard_in_use())
			return finish_capture(std::string(keyboard_notice));
		const std::string_view action = pbd::controllers::settings::assignments[std::clamp(controls_focus, 1u, controls_reset - 1)];
		if (pressed & (start | up | down | left | right))
			return finish_capture("Nothing changed.");
		if (pressed & select)
		{
			if (!is_attack(action))
				return clear_action(m, action);
			notice = action_words(action) + " always needs a button.";
			++revision;
		}
		const word fresh = static_cast<word>(pressed & slot_buttons);
		if (capture.settle)
		{
			capture.settle = (held & slot_buttons & ~capture.masked) != 0;
			return effect::none;
		}
		if (capture.button)
		{
			if (fresh & ~capture.button)
				return settle_capture();
			if (!(held & capture.button))
				return choose_slot(m, action, capture.button);
			return now_ms - capture.since >= hold_cancel_ms ? finish_capture("Nothing changed.") : effect::none;
		}
		if (!fresh)
			return effect::none;
		if (fresh & (fresh - 1))
			return settle_capture();
		capture.button = fresh;
		capture.since = now_ms;
		return effect::none;
	}

	// Two slot buttons pressed together are no choice: the row waits until every slot button is up.
	inline effect controller::settle_capture()
	{
		capture.button = 0;
		capture.settle = true;
		notice = "Press one button at a time.";
		++revision;
		return effect::none;
	}

	// The row stops waiting; `said` takes the help line until the next press.
	inline effect controller::finish_capture(std::string said, effect result)
	{
		capture = {};
		repeat = 0;
		notice = std::move(said);
		++revision;
		return result;
	}

	// The released slot takes the row's action (assign_slot); the help line says what moved.
	inline effect controller::choose_slot(model& m, std::string_view action, word bit)
	{
		const auto& bits = pbd::controllers::settings::slot_bits;
		const auto slot = static_cast<std::uint32_t>(std::find(bits.begin(), bits.end(), bit) - bits.begin());
		if (slot >= bits.size())
			return finish_capture("Nothing changed.");
		auto& mapping = m.current.input.mapping;
		const std::string previous = mapping[slot];
		const std::string token = "{" + std::string(pbd::controllers::settings::button_slots[slot]) + "}";
		switch (assign_slot(mapping, action, slot))
		{
		case assigned::unchanged: return finish_capture(action_words(action) + " is already " + token + ".");
		case assigned::last_attack:
			return finish_capture(token + " is your only button for " + action_words(previous) + ". Give it another button first.");
		case assigned::changed: break;
		}
		// A stick click given its first action is said from the button's side, with what it stops doing.
		if (previous == "off" && !click_note(slot).empty())
			return finish_capture(token + " is now " + action_words(action) + ". " + std::string(click_note(slot)), effect::controls);
		std::string said = action_words(action) + " is now " + token + ".";
		if (previous != "off" && previous != action)
		{
			const auto moved = slot_tokens(mapping, previous, false);
			said += " " + action_words(previous) + (moved.empty() ? " has no button now." : " is now " + moved + ".");
		}
		return finish_capture(std::move(said), effect::controls);
	}

	// Select while a combination's row waits: no button does it any more.
	inline effect controller::clear_action(model& m, std::string_view action)
	{
		bool cleared = false;
		for (auto& job : m.current.input.mapping)
		{
			if (job != action)
				continue;
			job = "off";
			cleared = true;
		}
		return finish_capture(action_words(action) + " has no button now.", cleared ? effect::controls : effect::none);
	}

	// Reset to default: the launcher's default layout, with 1+2 on L1 for the keyboard, as the launcher saves it for the
	// keyboard's O key.
	inline effect controller::reset_layout(model& m)
	{
		auto mapping = pbd::controllers::settings::values{}.mapping;
		if (m.current.input.device == "keyboard")
			mapping[4] = "1+2";
		const bool changed = mapping != m.current.input.mapping;
		m.current.input.mapping = mapping;
		notice = changed ? "Back to the default layout." : "This is the default layout.";
		++revision;
		return changed ? effect::controls : effect::none;
	}

	// Hover focuses a Controls row; clicking the header selects a controller and clicking an action starts capture.
	inline effect controller::pointer_controls(model& m, float x, float y, bool click)
	{
		// A waiting row keeps the focus; a click anywhere but on it stops the wait.
		if (capture.active)
			return click && !controls_rect(controls_focus).contains(x, y) ? finish_capture("Nothing changed.") : effect::none;
		for (std::uint32_t i = 0; i < controls_count; ++i)
			if (controls_rect(i).contains(x, y))
			{
				if (controls_focus != i)
					focus_controls(i);
				if (!click)
					return effect::none;
				// The header's left half steps back through the controllers and its right half forward, as its chevrons show.
				if (i == 0)
					return press_controls(m, x < controls_rect(0).x + controls_rect(0).w / 2.f ? left : right);
				return press_controls(m, cross);
			}
		return effect::none;
	}

	inline effect controller::pointer_top(model& m, float x, float y, bool click)
	{
		const auto rows = shown_entries(m);
		const auto count = static_cast<std::uint32_t>(rows.size());
		for (std::uint32_t i = 0; i < count; ++i)
		{
			if (!entry_rect(i, count).contains(x, y))
				continue;
			if (top_focus != static_cast<std::uint32_t>(rows[i]))
			{
				top_focus = static_cast<std::uint32_t>(rows[i]);
				++revision;
			}
			if (!click)
				return effect::none;
			if (!notice.empty())
				notice.clear();
			return pick(rows[i]);
		}
		return effect::none;
	}

	inline effect controller::pointer_dropdown(model& m, row r, float x, float y, bool click)
	{
		const auto list = options(m, r);
		const auto count = static_cast<std::uint32_t>(list.size());
		const float top = stack_top(m, r, count);
		for (std::uint32_t j = 0; j < count; ++j)
		{
			if (!option_rect(top, j).contains(x, y))
				continue;
			if (option_focus != j)
			{
				option_focus = j;
				++revision;
			}
			if (click)
			{
				choose(m, r, list[j]);
				dropdown = false;
				++revision;
				return back_out();
			}
			return effect::none;
		}
		if (click)
		{
			dropdown = false;
			++revision;
		}
		return effect::none;
	}

	inline effect controller::pointer(model& m, float x, float y, bool click)
	{
		if (!open || leaving)
			return effect::none;
		if (level == page::controls)
			return pointer_controls(m, x, y, click);
		if (level == page::top)
			return pointer_top(m, x, y, click);
		const row r = focused();
		if (dropdown)
			return pointer_dropdown(m, r, x, y, click);
		if (r == row::volume)
		{
			for (const bool forward : {false, true})
			{
				if (!stepper_rect(forward).contains(x, y))
					continue;
				return click ? step(m, row::volume, forward) : effect::none;
			}
		}
		for (std::uint32_t i = 0; i < row_count; ++i)
		{
			if (!row_rect(i).contains(x, y))
				continue;
			if (focus != i)
			{
				focus = i;
				++revision;
			}
			return click ? activate(m, static_cast<row>(i)) : effect::none;
		}
		return effect::none;
	}

} // namespace pbd::settings_menu
