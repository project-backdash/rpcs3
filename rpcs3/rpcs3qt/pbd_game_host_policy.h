#pragma once

#include <algorithm>
#include <array>
#include <charconv>
#include <cstdint>
#include <string>
#include <string_view>

namespace pbd::game_host
{
	inline bool fullscreen_key(bool f11, bool modified)
	{
		return f11 && !modified;
	}

	// Shift+F11 is the player's own way out of a game that runs while its picture does not reach the screen: the game moves
	// into a window of its own, or, already there, the launcher offers to show it again or close it. Ctrl and Alt with F11
	// stay with recorders and overlays.
	inline bool escape_key(bool f11, bool shift, bool other_modifiers)
	{
		return f11 && shift && !other_modifiers;
	}

	inline bool can_show_frame(bool first_frame, bool presentation_allowed)
	{
		return first_frame && presentation_allowed;
	}

	struct window_visibility
	{
		bool exists = false;
		bool visible = false;
		bool minimized = false;
		bool cloak_known = false;
		bool cloaked = false;
		bool on_screen = false;
		bool foreground = false;

		bool verified() const
		{
			return exists && visible && !minimized && cloak_known && !cloaked && on_screen && foreground;
		}
	};

	// The visible answer. A game not yet confirmed says whether the player is in front of it (its window, or the launcher it
	// is drawn in or owned by): the launcher, not focused while a game's own window is in front, offers its way out only to a
	// player who is looking. A game whose on-screen check has not decided yet in its own window, shown and in front, says so
	// (screen_check_pending): the launcher waits for the check then. Older launchers read any answer but OK and NO as a
	// refusal, which they treat as NO.
	inline std::string_view control_response(int action, bool accepted, bool verified_visible = false, bool player_in_front = false,
		bool check_pending = false)
	{
		if (!accepted)
			return "ERR\n";
		if (action != 5 || verified_visible)
			return "OK\n";
		if (check_pending)
			return "NO PENDING\n";
		return player_in_front ? "NO FRONT\n" : "NO\n";
	}

	// A recovery the launcher asks for while the on-screen check runs on a window that is shown, in front and on a monitor
	// does nothing: the check decides then, and a repair would restore, move and shrink a launcher window that is fine.
	inline bool recovery_skipped(bool screen_checking, bool window_verified)
	{
		return screen_checking && window_verified;
	}

	// How a recovery puts the launcher window back: restored only when minimized or hidden, moved only when no monitor shows
	// it, and never moved or resized when maximized or full screen.
	struct window_repair
	{
		bool restore = false;
		bool reposition = false;
	};

	inline window_repair plan_window_repair(bool minimized, bool hidden, bool on_a_monitor, bool maximized, bool full_screen)
	{
		return {minimized || hidden, !on_a_monitor && !maximized && !full_screen};
	}

	// When a game in its own window takes the keyboard back from the launcher that owns it. Taking it activates the game's
	// window, which cancels a drag or resize of the launcher by its frame and closes its window menu; so it waits while the
	// launcher's thread moves, sizes or shows a menu and while a mouse button is down.
	enum class hand_back
	{
		none, // the game is in front already, or the player is in another program
		now,
		later, // asked again on the window thread's next pass
	};

	inline hand_back plan_hand_back(bool game_in_front, bool launcher_in_front, bool launcher_busy, bool button_down)
	{
		if (game_in_front || !launcher_in_front)
			return hand_back::none;
		return launcher_busy || button_down ? hand_back::later : hand_back::now;
	}

	// The on-screen check (pbd_game_host_window.h). After each show the window thread reads the composed desktop at 16
	// points of the game's client area and the colours of the image the game presented there, overlays included
	// (VKPresent.cpp). Window state alone cannot tell a shown game from one composed under the launcher's own page, so only
	// this check lets the visible answer say OK. Only positive evidence fails it; anything short of that is inconclusive.
	constexpr std::uint32_t screen_points = 16;
	// The launcher window's background (launcherWindow.ts), shown where neither its page nor the game is drawn.
	constexpr std::uint32_t launcher_background = 0x140608;
	// Two reads of the same colour from the desktop: dithering and colour management move a channel by a few steps.
	constexpr int screen_same = 6;
	// A presented pixel against the desktop read of it: colour conversions on the way, and a frame a little older or newer
	// than the one on screen at a point with a soft edge.
	constexpr int screen_match = 20;
	// Two colours count as different beyond what the same picture read twice could explain.
	constexpr int clearly_different = 2 * screen_same;
	// Points that must agree before a hidden verdict, and before the screen counts as showing the game or its change.
	constexpr std::uint32_t screen_quorum = 10, screen_witnesses = 4;
	// A presented image this many pixels off the window's client size is still the same surface: a one-pixel surface
	// refresh, or a resize the renderer has not caught up with yet. The points are fractions of each size.
	constexpr long frame_size_slack = 2;
	// Timing of a check, in milliseconds. Its first sample comes this long after the show, then one every sample interval
	// while nothing is decided; after `screen_undecided_ms` from the first sample it passes as inconclusive, and a slow
	// re-check samples every `screen_recheck_sample_ms` for `screen_recheck_ms` more, as the launcher hides its page then
	// and a game composed under it shows the launcher's background from then on.
	constexpr std::uint64_t screen_first_sample_ms = 300, screen_sample_ms = 500, screen_undecided_ms = 6500;
	constexpr std::uint64_t screen_recheck_sample_ms = 2000, screen_recheck_ms = 20000;
	// The renderer answers a sample's request with its next presented image; past this, the screen is judged alone.
	constexpr std::uint64_t screen_answer_ms = 250;
	// A sample waits for the player to be in front of the game and for a surface refresh to settle, for as long as those
	// last. In the game's own window a check still undecided this long after its show passes as inconclusive however its
	// samples went, as one whose samples ran out does (they end by about then when nothing holds them): nothing follows from
	// a verdict there but the log. In the launcher window the check waits for its samples, as a picture found missing there
	// moves into a window of its own.
	constexpr std::uint64_t screen_decision_ms = 8000;

	inline bool screen_check_overdue(bool own_window, std::uint64_t since_show, bool passed)
	{
		return own_window && !passed && since_show >= screen_decision_ms;
	}

	// The game's own window is shown, in front and on a monitor, and its on-screen check has not decided yet: the visible
	// answer says so, and the launcher waits for the check, which decides within screen_decision_ms of the show, rather than
	// offer its way out to a player who may be playing.
	inline bool screen_check_pending(bool own_window, bool window_verified, bool screen_passed)
	{
		return own_window && window_verified && !screen_passed;
	}

	struct screen_sample
	{
		std::array<std::uint32_t, screen_points> screen{}; // 0xRRGGBB read from the composed desktop
		std::array<std::uint32_t, screen_points> frame{};  // 0xRRGGBB the game presented there, when `framed`
		std::uint32_t usable = 0;                          // a bit per point: the window there is the game's or the launcher's
		bool framed = false;
		// The frame's image is not the client size (frame_fits_window), and nothing reports how it reaches the window, so
		// its points are read twice (pbd::screen_check::client_x): in `screen` where the image shown at its own size from the
		// client's top left has them, and in `stretched` where a stretch over the client area has them. Either reading can
		// confirm the game. As the mapping is assumed, such a sample never counts toward a frozen screen, and toward the
		// launcher still shown only in the launcher window (screen_shows_launcher).
		bool scaled = false;
		std::array<std::uint32_t, screen_points> stretched{};
	};

	enum class screen_verdict
	{
		undecided,           // no evidence either way: passes as inconclusive once the samples run out
		confirmed,           // the screen shows the game's frame
		launcher_background, // the launcher window's background shows where the game draws
		launcher_unchanged,  // the screen still shows what the launcher showed before the game was shown
		frozen,              // the screen stays the same while the game's frames change
		forced_hidden,       // a hardware test asked for the hidden verdict (the contract's screenCheck)
	};

	inline bool screen_hidden(screen_verdict verdict)
	{
		return verdict == screen_verdict::launcher_background || verdict == screen_verdict::launcher_unchanged ||
		       verdict == screen_verdict::frozen || verdict == screen_verdict::forced_hidden;
	}

	inline std::string_view screen_verdict_name(screen_verdict verdict)
	{
		switch (verdict)
		{
		case screen_verdict::confirmed: return "confirmed";
		case screen_verdict::launcher_background: return "hidden (launcher background)";
		case screen_verdict::launcher_unchanged: return "hidden (launcher still shown)";
		case screen_verdict::frozen: return "hidden (screen frozen while the game moves)";
		case screen_verdict::forced_hidden: return "hidden (forced for a test)";
		default: return "inconclusive";
		}
	}

	// The largest difference of one channel.
	inline int colour_distance(std::uint32_t a, std::uint32_t b)
	{
		int largest = 0;
		for (int shift = 0; shift <= 16; shift += 8)
		{
			const int difference = static_cast<int>((a >> shift) & 0xff) - static_cast<int>((b >> shift) & 0xff);
			largest = std::max(largest, difference < 0 ? -difference : difference);
		}
		return largest;
	}

	inline bool colour_near(std::uint32_t a, std::uint32_t b, int tolerance)
	{
		return colour_distance(a, b) <= tolerance;
	}

	// Brighter than the launcher's dark page and near black: a colour that tells the game's picture apart.
	inline bool colour_distinct(std::uint32_t colour)
	{
		return std::max({(colour >> 16) & 0xff, (colour >> 8) & 0xff, colour & 0xff}) >= 48;
	}

	// A desktop read of content that bypasses composition (a directly flipped surface) can come back black.
	inline bool colour_black(std::uint32_t colour)
	{
		return colour_near(colour, 0, 8);
	}

	inline bool screen_usable(const screen_sample& sample, std::uint32_t point)
	{
		return (sample.usable >> point) & 1;
	}

	// The screen shows the game's frame at enough points whose colour the launcher's own view did not already have there; a
	// scaled sample in either of its readings.
	inline bool screen_confirmed(const screen_sample& sample, const screen_sample* launcher)
	{
		if (!sample.framed)
			return false;
		std::uint32_t witnesses = 0, stretched = 0;
		for (std::uint32_t i = 0; i < screen_points; ++i)
		{
			const auto colour = sample.frame[i];
			const bool launcher_had_it = launcher && screen_usable(*launcher, i) && colour_near(launcher->screen[i], colour, screen_match);
			const bool counts = screen_usable(sample, i) && colour_distinct(colour) && !launcher_had_it;
			witnesses += counts && colour_near(sample.screen[i], colour, screen_match);
			stretched += counts && sample.scaled && colour_near(sample.stretched[i], colour, screen_match);
		}
		return witnesses >= screen_witnesses || stretched >= screen_witnesses;
	}

	// The launcher's background where the game draws something else. A black frame counts as something else: the presented
	// image is what the screen would show, and black is not the background.
	inline bool screen_shows_background(const screen_sample& sample)
	{
		std::uint32_t points = 0;
		for (std::uint32_t i = 0; i < screen_points; ++i)
		{
			const bool game_draws_it = sample.framed && colour_near(sample.frame[i], launcher_background, screen_same);
			points += screen_usable(sample, i) && colour_near(sample.screen[i], launcher_background, screen_same) && !game_draws_it;
		}
		return points >= screen_quorum;
	}

	// The screen still shows the launcher's view where the game's frame differs from it. What tells is the screen unchanged
	// since before the show, wherever the window shows the frame's points, so a scaled sample counts in the launcher window,
	// where this verdict moves the game into a window of its own; in the game's own window only the log would follow.
	inline bool screen_shows_launcher(const screen_sample& launcher, const screen_sample& sample, bool own_window)
	{
		if (!sample.framed || (sample.scaled && own_window))
			return false;
		std::uint32_t still = 0, differs = 0;
		for (std::uint32_t i = 0; i < screen_points; ++i)
		{
			if (!screen_usable(launcher, i) || !screen_usable(sample, i) || colour_black(sample.screen[i]) ||
				!colour_near(sample.screen[i], launcher.screen[i], screen_same))
				continue;
			++still;
			differs += !colour_near(sample.frame[i], launcher.screen[i], clearly_different);
		}
		return still >= screen_quorum && differs >= screen_witnesses;
	}

	// The screen did not change between two samples where the game's frame did.
	inline bool screen_frozen(const screen_sample& first, const screen_sample& latest)
	{
		if (!first.framed || !latest.framed || first.scaled || latest.scaled)
			return false;
		std::uint32_t still = 0, moved = 0;
		for (std::uint32_t i = 0; i < screen_points; ++i)
		{
			if (!screen_usable(first, i) || !screen_usable(latest, i) || colour_black(latest.screen[i]) ||
				!colour_near(first.screen[i], latest.screen[i], screen_same))
				continue;
			++still;
			moved += !colour_near(first.frame[i], latest.frame[i], clearly_different);
		}
		return still >= screen_quorum && moved >= screen_witnesses;
	}

	// The verdict on two samples of one show. `launcher` is the screen read just before the show, when it could be read;
	// `own_window`: the game draws in a window of its own, not in the launcher window. Evidence that the game is shown wins
	// over evidence that it is not.
	inline screen_verdict judge_screen(const screen_sample* launcher, const screen_sample& first, const screen_sample& latest,
		bool own_window = false)
	{
		if (screen_confirmed(first, launcher) || screen_confirmed(latest, launcher))
			return screen_verdict::confirmed;
		if (screen_shows_background(first) && screen_shows_background(latest))
			return screen_verdict::launcher_background;
		if (launcher && screen_shows_launcher(*launcher, first, own_window) && screen_shows_launcher(*launcher, latest, own_window))
			return screen_verdict::launcher_unchanged;
		if (screen_frozen(first, latest))
			return screen_verdict::frozen;
		return screen_verdict::undecided;
	}

	// The verdict after the latest sample: against the show's first sample, whose long gap shows a game that moves, then
	// against the one before it, which follows the screen once the launcher has hidden its page.
	inline screen_verdict judge_screen(const screen_sample* launcher, const screen_sample& first, const screen_sample& previous,
		const screen_sample& latest, bool own_window = false)
	{
		const auto verdict = judge_screen(launcher, first, latest, own_window);
		return verdict != screen_verdict::undecided ? verdict : judge_screen(launcher, previous, latest, own_window);
	}

	// The presented image belongs to the window when its size is the client size, give or take the slack; an image of any
	// other size is compared scaled (screen_sample::scaled).
	inline bool frame_fits_window(std::uint32_t frame_width, std::uint32_t frame_height, long client_width, long client_height)
	{
		const auto near = [](long long a, long long b)
		{
			return a - b <= frame_size_slack && b - a <= frame_size_slack;
		};
		return frame_width && frame_height && near(frame_width, client_width) && near(frame_height, client_height);
	}

	// What follows a sample that decided nothing. `since_first`: ms since the check's first sample; `taken`: samples so far;
	// `passed`: the check has already passed as inconclusive and only re-checks. `next_ms` 0 ends the check.
	struct screen_schedule
	{
		bool pass = false; // pass as inconclusive now
		std::uint64_t next_ms = 0;
	};

	inline screen_schedule schedule_after_undecided(std::uint64_t since_first, int taken, bool passed)
	{
		if (passed)
			return {false, since_first + screen_recheck_sample_ms <= screen_undecided_ms + screen_recheck_ms ? screen_recheck_sample_ms : 0};
		// Two samples at least: one alone cannot show a game that moves.
		if (since_first >= screen_undecided_ms && taken >= 2)
			return {true, screen_recheck_sample_ms};
		return {false, screen_sample_ms};
	}

	// Window class and adapter names for one log line: printable ASCII without the line's own separators.
	inline std::string printable_ascii(std::wstring_view text)
	{
		std::string out;
		out.reserve(text.size());
		for (const wchar_t c : text)
			out += c >= 32 && c < 127 && c != '[' && c != ']' && c != '|' ? static_cast<char>(c) : '?';
		return out;
	}

	// Local window control has one bounded request, an exact token and no process-kill command.
	inline int parse_control(std::string_view request, std::uint32_t token)
	{
		if (!token || request.size() > 127 || !request.starts_with("PBD1 ") || !request.ends_with('\n'))
			return 0;
		request.remove_prefix(5);
		request.remove_suffix(1);
		std::uint32_t supplied = 0;
		const auto end = request.data() + request.size();
		const auto parsed = std::from_chars(request.data(), end, supplied);
		if (parsed.ec != std::errc{} || parsed.ptr == end || *parsed.ptr != ' ' || supplied != token)
			return 0;
		const std::string_view command(parsed.ptr + 1, end);
		if (command == "close")
			return 1;
		if (command == "resize")
			return 2;
		if (command == "launcher")
			return 3;
		if (command == "game")
			return 4;
		if (command == "visible")
			return 5;
		if (command == "recover")
			return 6;
		return 0;
	}
} // namespace pbd::game_host
