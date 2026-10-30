#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace pbd::controllers
{
	enum action : unsigned
	{
		cross,
		circle,
		square,
		triangle,
		l1,
		r1,
		l2,
		r2,
		select,
		start,
		l3,
		r3,
		home,
		touchpad,
		up,
		down,
		left,
		right,
		count
	};
	inline constexpr std::array<std::string_view, count> names{
		"cross", "circle", "square", "triangle", "l1", "r1", "l2", "r2", "select", "start", "l3", "r3", "home", "touchpad", "up", "down", "left", "right"};
	inline constexpr std::array<std::uint16_t, count> ps3_bits{
		0x0040, 0x0020, 0x0080, 0x0010, 0x0004, 0x0008, 0x0001, 0x0002, 0x0100, 0x0800, 0x0200, 0x0400, 0, 0, 0x1000, 0x4000, 0x8000, 0x2000};
	inline constexpr std::uint32_t bit(unsigned a)
	{
		return 1u << a;
	}

	struct state
	{
		std::uint32_t buttons = 0;
		std::array<float, 4> sticks{};
		bool connected = true;
		bool operator==(const state&) const = default;
	};

	struct raw_state
	{
		std::vector<bool> buttons;
		std::vector<float> axes;
		std::vector<unsigned> hats;
	};

	// Saved keyboard bindings contribute their logical actions independently, including simultaneous keys.
	template <typename Pressed>
	state sample_keyboard(const std::array<std::vector<std::string>, count>& keys, Pressed pressed)
	{
		state value;
		for (unsigned i = 0; i < count; ++i)
			for (const auto& key : keys[i])
				if (pressed(key))
					value.buttons |= bit(i);
		return value;
	}

	struct binding
	{
		enum kind
		{
			button,
			axis,
			hat
		} type = button;
		unsigned index = 0;
		int direction = 1;
		float rest = 0.f;
		bool operator==(const binding&) const = default;
	};
	using layout = std::array<std::optional<binding>, count>;
	inline bool same_input(const binding& a, const binding& b)
	{
		return a.type == b.type && a.index == b.index && (a.type == binding::button || a.direction == b.direction);
	}

	inline bool pressed(const raw_state& raw, const binding& b)
	{
		if (b.type == binding::button)
			return b.index < raw.buttons.size() && raw.buttons[b.index];
		if (b.type == binding::hat)
			return b.index < raw.hats.size() && (raw.hats[b.index] & b.direction) == static_cast<unsigned>(b.direction);
		return b.index < raw.axes.size() && std::isfinite(raw.axes[b.index]) &&
		       (raw.axes[b.index] - b.rest) * b.direction > .5f;
	}

	inline state map(state defaults, const raw_state& raw, const layout& overrides)
	{
		// A taught raw control has one job, even when SDL also maps it under its old name.
		for (unsigned i = 0; i < count; ++i)
			if (overrides[i])
			{
				defaults.buttons &= ~bit(i);
				if (pressed(raw, *overrides[i]))
					defaults.buttons |= bit(i);
			}
		return defaults;
	}

	inline bool neutral(const raw_state& rest, const raw_state& now)
	{
		if (std::any_of(now.buttons.begin(), now.buttons.end(), [](bool down)
				{
					return down;
				}))
			return false;
		if (std::any_of(now.hats.begin(), now.hats.end(), [](unsigned hat)
				{
					return hat != 0;
				}))
			return false;
		for (unsigned i = 0; i < now.axes.size(); ++i)
			if (!std::isfinite(now.axes[i]) || std::abs(now.axes[i] - (i < rest.axes.size() ? rest.axes[i] : 0.f)) > .20f)
				return false;
		return true;
	}

	inline std::optional<binding> learn(const raw_state& rest, const raw_state& before, const raw_state& now)
	{
		for (unsigned i = 0; i < now.buttons.size(); ++i)
			if (now.buttons[i] && (i >= before.buttons.size() || !before.buttons[i]))
				return binding{binding::button, i};
		for (unsigned i = 0; i < now.hats.size(); ++i)
			if (now.hats[i] && (now.hats[i] & (now.hats[i] - 1)) == 0 && (i >= before.hats.size() || before.hats[i] != now.hats[i]))
				return binding{binding::hat, i, static_cast<int>(now.hats[i])};
		for (unsigned i = 0; i < now.axes.size(); ++i)
		{
			const float origin = i < rest.axes.size() ? rest.axes[i] : 0.f;
			const float value = now.axes[i] - origin;
			const float previous = i < before.axes.size() ? before.axes[i] - origin : 0.f;
			if (std::isfinite(value) && std::abs(value) > .6f && (std::abs(previous) <= .6f || value * previous < 0.f))
				return binding{binding::axis, i, value > 0.f ? 1 : -1, origin};
		}
		return {};
	}

	struct source
	{
		std::string id;
		state value;
		bool keyboard = false;
	};

	// The caller replaces one complete pad report while holding the pad lock. Releases never change ownership.
	class selector
	{
		struct history
		{
			std::uint32_t buttons = 0, withheld = 0;
			std::array<bool, 4> moved{}, withheld_axes{};
		};
		std::map<std::string, history, std::less<>> m_history;
		std::string m_owner;
		std::uint64_t m_serial = 0;
		bool m_focused = false;

	public:
		explicit selector(std::string initial = "keyboard") : m_owner(std::move(initial)) {}
		const std::string& owner() const
		{
			return m_owner;
		}
		std::uint64_t serial() const
		{
			return m_serial;
		}
		state sample(std::span<const source> sources, bool focused, bool typing, std::string_view locked = {})
		{
			std::map<std::string, history, std::less<>> next;
			for (const auto& input : sources)
			{
				if (!input.value.connected)
					continue;
				const auto old = m_history.find(input.id);
				history h;
				h.buttons = input.value.buttons;
				// Controllers work without window focus. Keyboard input held across a focus change stays withheld until release.
				const bool focus_lost = input.keyboard && (!focused || !m_focused);
				h.withheld = old == m_history.end() || focus_lost ? h.buttons : old->second.withheld & h.buttons;
				bool claim = old != m_history.end() && (h.buttons & ~old->second.buttons) != 0;
				for (unsigned i = 0; i < h.moved.size(); ++i)
				{
					const bool held = old != m_history.end() && old->second.moved[i];
					const float value = std::abs(input.value.sticks[i]);
					h.moved[i] = std::isfinite(value) && value > (held ? .20f : .35f);
					h.withheld_axes[i] = (old == m_history.end() || focus_lost || old->second.withheld_axes[i]) && value > .20f;
					claim |= old != m_history.end() && h.moved[i] && !held;
				}
				if ((!input.keyboard || (focused && m_focused)) && claim && !(typing && input.keyboard) && locked.empty())
				{
					m_owner = input.id;
					++m_serial;
				}
				next.emplace(input.id, h);
			}
			m_history = std::move(next);
			m_focused = focused;
			if (!locked.empty())
				m_owner = locked;
			for (const auto& input : sources)
				if (input.id == m_owner && input.value.connected && !(typing && input.keyboard) && (focused || !input.keyboard))
				{
					auto value = input.value;
					const auto& h = m_history.at(m_owner);
					value.buttons &= ~h.withheld;
					for (unsigned i = 0; i < value.sticks.size(); ++i)
						if (h.withheld_axes[i])
							value.sticks[i] = 0.f;
					return value;
				}
			return {};
		}
	};

	// The devices a second local player picks from in offline play: every one but `first`, Player 1's device, so one
	// controller never plays both sides. A fresh press on one of them claims that player through its own selector.
	inline std::vector<source> without(std::span<const source> sources, std::string_view first)
	{
		std::vector<source> out;
		for (const auto& input : sources)
			if (input.id != first)
				out.push_back(input);
		return out;
	}

	inline std::uint16_t canonical(const state& value)
	{
		std::uint16_t word = 0;
		for (unsigned i = 0; i < count; ++i)
			if (value.buttons & bit(i))
				word |= ps3_bits[i];
		return word;
	}
} // namespace pbd::controllers
