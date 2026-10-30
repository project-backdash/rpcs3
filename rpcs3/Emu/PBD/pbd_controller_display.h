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
		std::string button_type;
	};
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
	// The label a glyph token takes on `d` (a key, a letter plate), or nothing where the PlayStation sprite stands.
	inline std::string glyph_label(std::string_view token, const display& d)
	{
		if (!d.active)
			return {};
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
					text += d.keys[a];
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
