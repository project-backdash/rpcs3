#pragma once

#include "pbd_controller_display.h"

#include <chrono>
#include <cstdint>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

// The clip keys and the clip toast in the game's own overlay. The clips controller publishes the player's Clips settings and
// what the recorder just did; the pause menus draw the clip keys and a short toast reports a saved clip and a recording's
// start and end. Nothing shows while this PC cannot make clips.
namespace pbd::clips::hud
{
	// The Clips keys until the player's settings say otherwise: NVIDIA Instant Replay's, which most PC players already know.
	inline constexpr char default_save_key[] = "Alt+F10", default_record_key[] = "Alt+F9";

	struct settings
	{
		bool available = false;  // the hardware encoder runs, so the clip keys do something
		bool background = false; // Background recording: the Save clip key has a last part to save
		unsigned seconds = 30;   // the length a saved clip keeps
		std::string save_key = default_save_key, record_key = default_record_key;
		bool operator==(const settings&) const = default;
	};

	// One action's prompt: keyboard keys ("Alt+F10"), or the controller's clip button by name ("Share") or as a glyph token
	// ("{l3}"). A held pad button reads "Hold" first. Empty when the action has no prompt.
	struct keys
	{
		bool pad = false;
		bool hold = false;
		std::string text;
		bool operator==(const keys&) const = default;
	};

	struct legend
	{
		bool shown = false;
		keys save, record;               // the keyboard keys
		keys save_button, record_button; // the clip button of the pad in use: a tap saves, a one-second hold records
		bool operator==(const legend&) const = default;
	};

	// The keyboard keys whatever the device in use, since most players clip from the keyboard, and the controller's clip
	// button while a pad that has one sent the last input. Save clip needs Background recording; Record works without it.
	// Nothing while clips cannot record here.
	inline legend legend_for(const settings& clips, const controllers::display& device)
	{
		if (!clips.available)
			return {};
		legend out{true, {false, false, clips.save_key}, {false, false, clips.record_key}};
		if (device.active && device.family != "keyboard" && !device.clip_button.empty())
		{
			out.save_button = {true, false, device.clip_button};
			out.record_button = {true, true, device.clip_button};
		}
		if (!clips.background)
		{
			out.save = {};
			out.save_button = {};
		}
		return out;
	}

	// What stops a recording on the device in use: the pad's clip button while a pad that has one is in use, otherwise the
	// keyboard keys.
	inline const keys& stop_keys(const legend& shown)
	{
		return shown.record_button.text.empty() ? shown.record : shown.record_button;
	}

	// The keycaps of a keyboard shortcut: "Ctrl+Shift+F11" is Ctrl, Shift and F11.
	inline std::vector<std::string_view> key_parts(std::string_view text)
	{
		std::vector<std::string_view> out;
		while (!text.empty())
		{
			const auto plus = text.find('+');
			if (plus)
				out.push_back(text.substr(0, plus));
			if (plus == std::string_view::npos)
				break;
			text.remove_prefix(plus + 1);
		}
		return out;
	}

	// A clip length as the Clips settings name it: "15 s", "30 s", "1 min", "2 min", "5 min".
	inline std::string length_text(unsigned seconds)
	{
		if (seconds >= 60 && seconds % 60 == 0)
			return std::to_string(seconds / 60) + " min";
		return std::to_string(seconds) + " s";
	}

	// A recording's length as minutes and seconds ("1:42", "60:00"), rounded down to the second.
	inline std::string duration_text(std::uint64_t ms)
	{
		const std::uint64_t seconds = ms / 1000;
		const std::uint64_t part = seconds % 60;
		return std::to_string(seconds / 60) + (part < 10 ? ":0" : ":") + std::to_string(part);
	}

	enum class toast_kind : std::uint8_t
	{
		none,
		clip_saved,
		recording,
		recording_saved
	};

	inline std::string_view title(toast_kind kind)
	{
		switch (kind)
		{
		case toast_kind::clip_saved: return "CLIP SAVED";
		case toast_kind::recording: return "RECORDING";
		case toast_kind::recording_saved: return "RECORDING SAVED";
		case toast_kind::none: break;
		}
		return {};
	}

	struct toast
	{
		toast_kind kind = toast_kind::none;
		std::uint64_t at_ms = 0;
		std::uint64_t length_ms = 0; // a saved recording's length
		unsigned seconds = 30;       // a saved clip's length, as set when it saved
		std::uint64_t serial = 0;
	};

	// The toast holds 2 s, fading in over 135 ms while it settles 6 units down, then fades out over 250 ms. The recording dot
	// blinks in two steps a second.
	inline constexpr std::uint64_t hold_ms = 2000, fade_in_ms = 135, fade_out_ms = 250, rise = 6;
	struct frame
	{
		bool shown = false;
		float alpha = 0.f;
		float offset = 0.f; // added to the toast's y
		bool dot = false;
	};
	inline frame frame_at(std::uint64_t elapsed_ms)
	{
		if (elapsed_ms >= hold_ms + fade_out_ms)
			return {};
		frame out{true, 1.f, 0.f, elapsed_ms % 1000 < 500};
		if (elapsed_ms < fade_in_ms)
		{
			const float t = static_cast<float>(elapsed_ms) / fade_in_ms;
			out.alpha = t;
			out.offset = -static_cast<float>(rise) * (1.f - t);
		}
		else if (elapsed_ms >= hold_ms)
			out.alpha = 1.f - static_cast<float>(elapsed_ms - hold_ms) / fade_out_ms;
		return out;
	}

	// What the recorder's status says just happened, from each status the clips controller reads: a recording started, a
	// recording ended in a saved file, or a clip saved (a new file while no recording ended). A stop that saved nothing says
	// nothing; the recorder's error says why.
	struct events
	{
		bool recording = false;
		std::string saved;
		std::uint64_t started_ms = 0;

		struct result
		{
			toast_kind kind = toast_kind::none;
			std::uint64_t length_ms = 0;
		};
		result update(bool recording_now, std::string_view saved_now, std::uint64_t now_ms)
		{
			result out;
			const bool started = recording_now && !recording, stopped = !recording_now && recording;
			const bool new_file = !saved_now.empty() && saved_now != saved;
			if (stopped && new_file)
				out = {toast_kind::recording_saved, now_ms - started_ms};
			else if (new_file)
				out = {toast_kind::clip_saved, 0};
			if (started)
			{
				started_ms = now_ms;
				out = {toast_kind::recording, 0};
			}
			recording = recording_now;
			saved = std::string(saved_now);
			return out;
		}
	};

	inline std::uint64_t steady_ms() noexcept
	{
		return static_cast<std::uint64_t>(
			std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count());
	}

	struct shared_state
	{
		std::mutex mutex;
		settings clips;
		toast latest;
	};
	inline shared_state& state()
	{
		static shared_state value;
		return value;
	}

	inline void publish(const settings& value)
	{
		auto& shared = state();
		std::lock_guard lock(shared.mutex);
		shared.clips = value;
	}
	inline settings load()
	{
		auto& shared = state();
		std::lock_guard lock(shared.mutex);
		return shared.clips;
	}

	// A new toast replaces the one showing. Only while clips can record here: nothing else reaches the recorder.
	inline void show(toast_kind kind, std::uint64_t now_ms, std::uint64_t length_ms = 0)
	{
		auto& shared = state();
		std::lock_guard lock(shared.mutex);
		if (kind == toast_kind::none || !shared.clips.available)
			return;
		shared.latest = {kind, now_ms, length_ms, shared.clips.seconds, shared.latest.serial + 1};
	}
	inline toast current()
	{
		auto& shared = state();
		std::lock_guard lock(shared.mutex);
		return shared.latest;
	}

	// The clip toast takes the Practice toast's slot while it shows.
	inline bool toast_showing(std::uint64_t now_ms)
	{
		const auto value = current();
		return value.kind != toast_kind::none && now_ms >= value.at_ms && frame_at(now_ms - value.at_ms).shown;
	}
} // namespace pbd::clips::hud
