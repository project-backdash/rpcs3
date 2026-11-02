#pragma once

#include "pbd_controller_mapper.h"

#include <algorithm>
#include <charconv>
#include <cstdint>
#include <deque>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

namespace pbd::clips
{
	template <typename Exists>
	std::optional<std::string> available_filename(const std::string& base, Exists exists)
	{
		for (unsigned copy = 1; copy < 1000; ++copy)
		{
			auto name = base + (copy == 1 ? "" : " (" + std::to_string(copy) + ")") + ".mp4";
			if (!exists(name) && !exists(name + ".part"))
				return name;
		}
		return {};
	}
	struct packet
	{
		std::vector<std::uint8_t> bytes;
		std::int64_t time_us = 0;
		std::int64_t duration_us = 0;
		bool audio = false;
		bool keyframe = false;
	};
	using packet_ptr = std::shared_ptr<const packet>;

	// Owned by the encoder thread. Saving shares encoded bytes; it never encodes them again.
	class buffer
	{
		std::deque<packet_ptr> m_packets;
		std::size_t m_bytes = 0;
		std::int64_t m_span_us = 30'000'000;
		static constexpr std::size_t byte_limit = 384 * 1024 * 1024;

	public:
		void clear()
		{
			m_packets.clear();
			m_bytes = 0;
		}
		void seconds(unsigned value)
		{
			m_span_us = std::clamp(value, 15u, 300u) * 1'000'000ll;
		}
		std::size_t bytes() const
		{
			return m_bytes;
		}
		void push(packet_ptr value)
		{
			m_bytes += value->bytes.size();
			m_packets.push_back(std::move(value));
			const auto oldest = m_packets.back()->time_us - m_span_us - 1'000'000;
			while (!m_packets.empty() && (m_packets.front()->time_us < oldest || m_bytes > byte_limit))
			{
				m_bytes -= m_packets.front()->bytes.size();
				m_packets.pop_front();
			}
		}
		std::vector<packet_ptr> snapshot() const
		{
			if (m_packets.empty())
				return {};
			const auto cut = m_packets.back()->time_us - m_span_us;
			std::int64_t start = -1;
			for (const auto& value : m_packets)
				if (!value->audio && value->keyframe)
				{
					if (start < 0 || value->time_us <= cut)
						start = value->time_us;
					if (value->time_us > cut)
						break;
				}
			if (start < 0)
				return {};
			std::vector<packet_ptr> result;
			for (const auto& value : m_packets)
				if (value->time_us >= start)
					result.push_back(value);
			std::stable_sort(result.begin(), result.end(), [](const auto& a, const auto& b)
				{
					if (a->time_us != b->time_us)
						return a->time_us < b->time_us;
					return a->audio < b->audio;
				});
			return result;
		}
	};

	enum class action
	{
		none,
		save,
		record
	};
	// Presses of the controller's clip button (pad_button), which the game never reads: a tap saves once, on release; a
	// one-second hold starts or stops recording and saves nothing.
	struct gesture
	{
		bool armed = false, down = false, held = false;
		std::uint64_t since = 0;
		action update(bool pressed, std::uint64_t now_ms)
		{
			if (!armed)
			{
				armed = !pressed;
				return action::none;
			}
			if (pressed && !down)
			{
				down = true;
				held = false;
				since = now_ms;
			}
			if (pressed && !held && now_ms >= since && now_ms - since >= 1000)
			{
				held = true;
				return action::record;
			}
			if (!pressed && down)
			{
				down = false;
				return held ? action::none : action::save;
			}
			return action::none;
		}
	};

	// The controller's clip button, from the launcher's Clips setting "padButton". It is never a button the game reads, so
	// a press saves a clip and does nothing in the game:
	//   home      PS / Home / Guide, which then opens no menu; never in a match, where it is the game's Start.
	//   button-N  raw button N, unless the pad's layout makes it Select or Start (reaches_select_or_start).
	// Any other value ("create", the default, and the older "share" and "touchpad" among them) is the pad's own capture
	// button: SDL's MISC1, which is an Xbox Share, a Switch Capture, a DualSense Mute or a Steam Deck Quick Access button.
	// The touchpad click is never a clip button: the game gets it as Select or Start. A pad without MISC1 has no clip
	// button; the clip keys still save.
	enum class pad_source
	{
		capture,
		home,
		raw
	};
	struct pad_button
	{
		pad_source source = pad_source::capture;
		unsigned index = 0;
	};
	inline pad_button parse_pad_button(std::string_view text)
	{
		if (text == "home")
			return {pad_source::home};
		constexpr std::string_view raw = "button-";
		if (text.starts_with(raw) && text.size() > raw.size())
		{
			unsigned index = 0;
			const auto* end = text.data() + text.size();
			const auto [stop, error] = std::from_chars(text.data() + raw.size(), end, index);
			if (error == std::errc{} && stop == end)
				return {pad_source::raw, index};
		}
		return {};
	}

	// Whether raw button `index` reaches the game as Select or Start: as either, as the touchpad click (the game gets it as
	// one of them) or, in a match, as Home (the game's Start there). A button keeps its default job (SDL's mapping or the
	// Sony layout) unless the player taught it another job or taught that job to another button, as apply_device_layout
	// and controllers::map do.
	inline bool reaches_select_or_start(const controllers::layout& defaults, const controllers::layout& taught, unsigned index, bool versus)
	{
		const controllers::binding button{controllers::binding::button, index};
		const auto is_button = [&](const std::optional<controllers::binding>& value)
		{
			return value && controllers::same_input(*value, button);
		};
		const bool retaught = std::any_of(taught.begin(), taught.end(), is_button);
		for (const unsigned job : {controllers::select, controllers::start, controllers::touchpad, controllers::home})
			if ((job != controllers::home || versus) && (taught[job] ? is_button(taught[job]) : !retaught && is_button(defaults[job])))
				return true;
		return false;
	}

	// One sample of a controller, as the clip button reads it.
	struct pad_report
	{
		const controllers::raw_state& raw;
		const controllers::layout& defaults;             // SDL's mapping or the Sony layout
		const controllers::layout& taught;               // what the player taught this controller type (padLayouts)
		std::uint32_t buttons = 0;                       // the logical buttons after both
		bool misc1 = false;                              // SDL's MISC1, which no default game button uses
		bool versus = false;                             // a match, where Home is the game's Start
		std::optional<controllers::binding> misc1_input; // the pad input SDL reads as MISC1, when its mapping names one
	};
	// A MISC1 button the player taught a game job (Select on the Share button) does that job and saves nothing.
	inline bool misc1_free(const controllers::layout& taught, const std::optional<controllers::binding>& misc1_input)
	{
		return std::none_of(taught.begin(), taught.end(), [&](const std::optional<controllers::binding>& value)
			{
				return value && misc1_input && controllers::same_input(*value, *misc1_input);
			});
	}
	inline bool pressed(const pad_button& choice, const pad_report& pad)
	{
		const auto down = [&](unsigned logical)
		{
			return (pad.buttons & controllers::bit(logical)) != 0;
		};
		const bool misc1 = pad.misc1 && misc1_free(pad.taught, pad.misc1_input);
		switch (choice.source)
		{
		case pad_source::capture: return misc1;
		case pad_source::home: return !pad.versus && down(controllers::home);
		case pad_source::raw:
			return choice.index < pad.raw.buttons.size() && pad.raw.buttons[choice.index] &&
			       !reaches_select_or_start(pad.defaults, pad.taught, choice.index, pad.versus);
		}
		return false;
	}

	// The buttons a controller has for clips, as SDL reports the device.
	struct pad_buttons
	{
		std::string_view family; // the controller as detected: "playstation", "xbox", "switch", "steamdeck"
		bool misc1 = false;      // SDL's MISC1, free of any game job the player taught it (misc1_free)
	};

	// The clip button `choice` on this controller as the in-game prompts name it: the button's own name ("Share", "Capture")
	// or the glyph token of the job a raw button already has ("{l3}"). Empty when the controller has no such button, so the
	// prompts show only the keyboard keys.
	inline std::string prompt_name(const pad_button& choice, const pad_buttons& pad, const controllers::layout& defaults,
		const controllers::layout& taught, bool versus)
	{
		// MISC1 is the Share button on an Xbox pad, Capture on a Switch pad, the microphone button on a DualSense and the
		// Quick Access button on a Steam Deck, which is printed "...".
		const std::string misc1 = !pad.misc1                  ? "" :
		                          pad.family == "switch"      ? "Capture" :
		                          pad.family == "playstation" ? "Mute" :
		                          pad.family == "steamdeck"   ? "..." :
		                                                        "Share";
		const std::string home = pad.family == "playstation" ? "PS" : "Home";
		switch (choice.source)
		{
		case pad_source::capture: return misc1;
		case pad_source::home: return versus ? "" : home;
		case pad_source::raw:
		{
			if (reaches_select_or_start(defaults, taught, choice.index, versus))
				return {};
			// The job this button does on this controller, read as reaches_select_or_start reads it.
			const controllers::binding button{controllers::binding::button, choice.index};
			const auto is_button = [&](const std::optional<controllers::binding>& value)
			{
				return value && controllers::same_input(*value, button);
			};
			const bool retaught = std::any_of(taught.begin(), taught.end(), is_button);
			for (unsigned job = 0; job < controllers::count; ++job)
			{
				if (!(taught[job] ? is_button(taught[job]) : !retaught && is_button(defaults[job])))
					continue;
				if (job == controllers::home)
					return home;
				if (job >= controllers::up)
					return "{dpad:" + std::string(1, "udlr"[job - controllers::up]) + "}";
				return "{" + std::string(controllers::names[job]) + "}";
			}
			return "Button " + std::to_string(choice.index + 1);
		}
		}
		return {};
	}

	// A controller's save or record press is for the game the player sees, whether it has the focus or not: controllers
	// work while another program is in front. A held game (a standby waiting for its match, or parked) and a hidden or
	// minimized window save and record nothing.
	inline bool pad_press_counts(bool can_show, bool parked, bool window_shown)
	{
		return can_show && !parked && window_shown;
	}

	// What follows the date in a clip's file name. The launcher writes it from fixed words and the game's character names, but
	// it arrives through a file, so only ASCII letters, digits and the marks in those names ("JACK-5", "Roger Jr.") are kept.
	// Anything else, path separators and control characters among them, becomes one space; the result never starts with a dot,
	// never ends with a space or a dot (Windows drops a trailing one) and is at most 80 characters.
	inline std::string file_label(std::string_view text)
	{
		constexpr std::size_t limit = 80;
		std::string out;
		for (const char c : text)
		{
			const bool kept = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' || c == '.';
			if (kept && (c != '.' || !out.empty()))
				out += c;
			else if (!out.empty() && out.back() != ' ')
				out += ' ';
			if (out.size() >= limit)
				break;
		}
		while (!out.empty() && (out.back() == ' ' || out.back() == '.'))
			out.pop_back();
		return out;
	}
} // namespace pbd::clips
