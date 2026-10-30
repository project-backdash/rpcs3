#pragma once

#include <array>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <deque>
#include <mutex>
#include <optional>

namespace pbd::practice::mouse
{
	// The renderer supplies its actual hit rectangles. Pointer actions retain that menu's identity until the
	// input boundary handles them, so an old click cannot choose a row on a replacement page or recording.
	struct scope
	{
		std::uint64_t generation = 0;
		std::uint32_t context = 0;
		std::uint64_t emulation = 0;
		std::uint64_t revision = 0;
		bool operator==(const scope&) const = default;
	};
	// Options belong to an open dropdown; dismiss is a click outside it; frequency carries 0 for L1 / the left chevron
	// and 1 for the right chevron / R1 on the focused dummy slot. A prompt click carries the control-bar group's target
	// value (pbd::practice::prompt_action, plus prompt_second_key for the second key of a pair).
	enum class action : std::uint8_t
	{
		hover_row,
		choose_row,
		choose_page,
		scroll,
		hover_option,
		choose_option,
		dismiss,
		frequency,
		prompt,
		back
	};
	struct command
	{
		scope owner;
		action kind = action::hover_row;
		std::uint32_t value = 0;
	};
	struct rectangle
	{
		float x = 0, y = 0, width = 0, height = 0;
		std::uint32_t value = 0;
		bool contains(float px, float py) const noexcept
		{
			return width > 0 && height > 0 && px >= x && py >= y && px < x + width && py < y + height;
		}
	};
	// A clickable control-bar group. A pair can publish one target per key; both name the same group for hover.
	struct prompt_target
	{
		rectangle area;
		std::uint32_t group = 0;
	};
	struct layout
	{
		scope owner;
		bool active = false;
		float width = 1280, height = 720;
		std::uint32_t row_count = 0, page_count = 0, option_count = 0, stepper_count = 0, prompt_count = 0;
		// While a dropdown is open only its options and the control bar answer the pointer, as only they do the pad.
		bool modal = false;
		// One rectangle per row a native Practice page can show (pbd::game_overlay::max_rows) and per rail category.
		std::array<rectangle, 15> rows{};
		std::array<rectangle, 8> pages{};
		// One per option an open dropdown can show (pbd::practice::max_options).
		std::array<rectangle, 15> options{};
		std::array<rectangle, 2> steppers{};
		// Up to eight control-bar groups, with room for split pairs.
		std::array<prompt_target, 12> prompts{};
	};
	struct shared_state
	{
		std::mutex mutex;
		layout current;
		std::deque<command> pending;
		int hovered_page = -1;
		int hovered_prompt = -1;
	};
	inline shared_state& state()
	{
		static shared_state value;
		return value;
	}
	// A click that resumes/closes the menu owns its release too. Neither release nor intervening motion
	// should enter a mouse-bound attack or gyro after the emulation boundary has already resumed DR.
	inline std::atomic<bool> captured_buttons{false};

	inline void publish(const layout& next)
	{
		auto& shared = state();
		std::lock_guard lock(shared.mutex);
		if (!next.active || !(next.owner == shared.current.owner))
		{
			shared.pending.clear();
			shared.hovered_page = -1;
			shared.hovered_prompt = -1;
		}
		shared.current = next;
	}
	inline void enqueue(shared_state& shared, const command& next)
	{
		const bool hover = next.kind == action::hover_row || next.kind == action::hover_option;
		if (hover && !shared.pending.empty() && shared.pending.back().kind == next.kind && shared.pending.back().owner == next.owner)
			shared.pending.back() = next;
		else if (shared.pending.size() < 32)
			shared.pending.push_back(next);
	}
	inline bool pointer(float x, float y, float width, float height, bool click, float left = 0, float top = 0)
	{
		auto& shared = state();
		std::lock_guard lock(shared.mutex);
		shared.hovered_page = -1;
		shared.hovered_prompt = -1;
		if (!std::isfinite(x) || !std::isfinite(y) || !std::isfinite(width) || !std::isfinite(height) ||
			!std::isfinite(left) || !std::isfinite(top) ||
			width <= 0 || height <= 0 || x < left || y < top || x >= left + width || y >= top + height)
			return false;
		const auto& view = shared.current;
		if (!view.active || !view.owner.generation || !std::isfinite(view.width) || !std::isfinite(view.height) ||
			view.width <= 0 || view.height <= 0)
			return false;
		const float px = (x - left) * view.width / width, py = (y - top) * view.height / height;
		// The control bar sits below every panel and dropdown, so it is checked first and answers in every state.
		for (std::uint32_t i = 0; i < view.prompt_count && i < view.prompts.size(); ++i)
		{
			if (!view.prompts[i].area.contains(px, py))
				continue;
			shared.hovered_prompt = static_cast<int>(view.prompts[i].group);
			if (click)
				enqueue(shared, {view.owner, action::prompt, view.prompts[i].area.value});
			return true;
		}
		if (view.modal)
		{
			for (std::uint32_t i = 0; i < view.option_count && i < view.options.size(); ++i)
			{
				if (!view.options[i].contains(px, py))
					continue;
				enqueue(shared, {view.owner, click ? action::choose_option : action::hover_option, view.options[i].value});
				return true;
			}
			if (click)
				enqueue(shared, {view.owner, action::dismiss, 0});
			return click;
		}
		for (std::uint32_t i = 0; i < view.stepper_count && i < view.steppers.size(); ++i)
		{
			if (!view.steppers[i].contains(px, py))
				continue;
			if (click)
				enqueue(shared, {view.owner, action::frequency, view.steppers[i].value});
			return true;
		}
		for (std::uint32_t i = 0; i < view.page_count && i < view.pages.size(); ++i)
		{
			if (!view.pages[i].contains(px, py))
				continue;
			shared.hovered_page = static_cast<int>(view.pages[i].value);
			if (click)
				enqueue(shared, {view.owner, action::choose_page, view.pages[i].value});
			return true;
		}
		for (std::uint32_t i = 0; i < view.row_count && i < view.rows.size(); ++i)
		{
			if (!view.rows[i].contains(px, py))
				continue;
			enqueue(shared, {view.owner, click ? action::choose_row : action::hover_row, view.rows[i].value});
			return true;
		}
		return false;
	}
	inline void wheel(bool previous)
	{
		auto& shared = state();
		std::lock_guard lock(shared.mutex);
		if (shared.current.active && shared.current.owner.generation)
			enqueue(shared, {shared.current.owner, action::scroll, previous ? 0u : 1u});
	}
	inline void back()
	{
		auto& shared = state();
		std::lock_guard lock(shared.mutex);
		if (shared.current.active && shared.current.owner.generation) enqueue(shared, {shared.current.owner, action::back, 0});
	}
	inline int hovered_page(scope owner)
	{
		auto& shared = state();
		std::lock_guard lock(shared.mutex);
		return shared.current.owner == owner && shared.current.active ? shared.hovered_page : -1;
	}
	inline int hovered_prompt(scope owner)
	{
		auto& shared = state();
		std::lock_guard lock(shared.mutex);
		return shared.current.owner == owner && shared.current.active ? shared.hovered_prompt : -1;
	}
	inline std::optional<command> take(scope owner)
	{
		auto& shared = state();
		std::lock_guard lock(shared.mutex);
		while (!shared.pending.empty())
		{
			const auto next = shared.pending.front();
			shared.pending.pop_front();
			if (next.owner == owner && owner.generation)
				return next;
		}
		return {};
	}
} // namespace pbd::practice::mouse
