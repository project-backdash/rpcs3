#pragma once

#include "pbd_controller_mapper.h"
#include <mutex>

namespace pbd::controllers
{
	struct display
	{
		bool active = false;
		std::string family = "playstation";
		std::string device;
		std::array<std::string, count> keys{};
		std::array<std::string, count> bindings{};
		std::string button_type;
		// This controller's clip button as the prompts name it ("Share", "{l3}"; pbd::clips::prompt_name), empty without one.
		std::string clip_button;
	};
	inline display device_display(std::string device, bool active)
	{
		display result;
		result.active = active;
		result.family = device == "keyboard" ? "keyboard" : "playstation";
		result.device = std::move(device);
		return result;
	}
	inline std::mutex display_mutex;
	inline display current_display;
	inline void publish(display value)
	{
		std::lock_guard lock(display_mutex);
		current_display = std::move(value);
	}
	inline display read_display()
	{
		std::lock_guard lock(display_mutex);
		return current_display;
	}
	// Offline play's two players, each as its own controller shows its buttons (inactive without one), so Versus prompts
	// each player in the glyphs or keys of the controller that player holds (pbd_controller_backend.cpp).
	inline std::array<display, 2> current_players{};
	inline void publish_players(std::array<display, 2> value)
	{
		std::lock_guard lock(display_mutex);
		current_players = std::move(value);
	}
	inline std::array<display, 2> read_players()
	{
		std::lock_guard lock(display_mutex);
		return current_players;
	}
	// Prompts describe the physical input before gameplay attack remaps. A taught input consumes its old job, just as
	// the mapper does; unknown raw controls retain their button, axis or hat number instead of claiming a face glyph.
	inline std::array<std::string, count> prompt_bindings(const layout& defaults, const layout& taught)
	{
		std::array<std::string, count> result{};
		for (unsigned i = 0; i < count; ++i)
		{
			if (taught[i])
			{
				const auto& input = *taught[i];
				result[i] = input.type == binding::button ? "key:Button " + std::to_string(input.index + 1) :
				            input.type == binding::axis   ? "key:Axis " + std::to_string(input.index + 1) + (input.direction > 0 ? "+" : "-") :
				                                            "key:Hat " + std::to_string(input.index + 1) + ":" + std::to_string(input.direction);
				for (unsigned physical = 0; physical < count; ++physical)
					if (defaults[physical] && same_input(input, *defaults[physical]))
					{
						result[i] = names[physical];
						break;
					}
			}
			else if (defaults[i])
			{
				for (const auto& assigned : taught)
					if (assigned && same_input(*assigned, *defaults[i]))
						result[i] = "key:Unbound";
			}
			else if (i < up)
				result[i] = "key:Unbound";
		}
		return result;
	}

	inline std::vector<std::string> prompt_tokens(std::string_view token, const display& d)
	{
		if (!d.active || d.family == "keyboard")
			return {std::string(token)};
		const auto physical = [&](unsigned a)
		{
			if (!d.bindings[a].empty())
				return d.bindings[a];
			return std::string(names[a]);
		};
		if (token.starts_with("dpad:"))
		{
			std::vector<std::string> parts;
			bool changed = false;
			for (const char c : token.substr(5))
			{
				const unsigned a = c == 'u' ? up : c == 'd' ? down :
				                               c == 'l'     ? left :
				                                              right;
				changed |= !d.bindings[a].empty();
				parts.push_back(physical(a));
			}
			if (changed)
				return parts;
		}
		for (unsigned i = 0; i < count; ++i)
			if (token == names[i])
				return {physical(i)};
		return {std::string(token)};
	}

	// The label a glyph token takes on `d` (a key, a letter plate), or nothing where the PlayStation sprite stands.
	inline std::string glyph_label(std::string_view token, const display& d)
	{
		if (!d.active)
			return {};
		if (token.starts_with("key:"))
			return std::string(token.substr(4));
		if (token == "touchpad" && d.family != "keyboard")
			return "Touchpad";
		if (token == "home" && d.family != "keyboard")
			return d.family == "xbox" ? "Guide" : "PS";
		if (token == "start" && (d.button_type == "ps4" || d.button_type == "ps5"))
			return "OPTIONS";
		if (token == "select" && d.button_type == "ps4")
			return "SHARE";
		if (token == "select" && d.button_type == "ps5")
			return "CREATE";
		if (d.family != "keyboard" && (d.family == "playstation" || d.button_type == "ps3" || d.button_type == "arcade"))
		{
			if (token == "start")
				return "START";
			if (token == "select")
				return "SELECT";
		}
		if (d.family == "xbox")
		{
			if (token == "start")
				return "Menu";
			if (token == "select")
				return "View";
		}
		if (d.family == "playstation")
			return {};
		if (d.family == "keyboard")
		{
			for (unsigned i = 0; i < count; ++i)
				if (token == names[i])
					return d.keys[i].empty() ? "Unbound" : d.keys[i] == "Return" ? "Enter" :
					                                                               d.keys[i];
			if (token.starts_with("dpad:"))
			{
				std::string text;
				for (const char c : token.substr(5))
				{
					const unsigned a = c == 'u' ? up : c == 'd' ? down :
					                               c == 'l'     ? left :
					                                              right;
					if (!text.empty())
						text += '/';
					text += d.keys[a].empty() ? "Unbound" : d.keys[a];
				}
				return text;
			}
		}
		const bool nintendo = d.family == "switch";
		if (d.family == "numbers")
		{
			if (token == "square")
				return "1";
			if (token == "triangle")
				return "2";
			if (token == "cross")
				return "3";
			if (token == "circle")
				return "4";
		}
		if (nintendo || d.family == "steamdeck")
		{
			if (token == "cross")
				return nintendo ? "B" : "A";
			if (token == "circle")
				return nintendo ? "A" : "B";
			if (token == "square")
				return nintendo ? "Y" : "X";
			if (token == "triangle")
				return nintendo ? "X" : "Y";
			if (token == "select")
				return nintendo ? "-" : "View";
			if (token == "start")
				return nintendo ? "+" : "Menu";
			if (token == "l1")
				return nintendo ? "L" : "L1";
			if (token == "r1")
				return nintendo ? "R" : "R1";
			if (token == "l2")
				return nintendo ? "ZL" : "L2";
			if (token == "r2")
				return nintendo ? "ZR" : "R2";
			if (token == "l3")
				return "L3";
			if (token == "r3")
				return "R3";
		}
		return {};
	}
} // namespace pbd::controllers
