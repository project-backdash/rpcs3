#pragma once

#include "../PBD/pbd_env.h"
#include "../PBD/pbd_controller_settings.h"

#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <optional>
#include <span>
#include <string_view>

namespace pbd::practice::controls
{
	inline bool touchpad_enabled() noexcept
	{
		const char* practice = pbd::env::find("PBD_PRACTICE_RESET");
		return practice && std::strcmp(practice, "1") == 0;
	}

	// What a touchpad click is in Practice, from the launcher's Controller setting (PBD_TOUCHPAD_CLICK): select makes every
	// click SELECT; start makes every click START; split, the default, makes the left half SELECT and the right half START on
	// a pad that reports a finger, and SELECT on one that does not. Arcade sticks with a touchpad button and pads in
	// a simple report mode never report a finger.
	enum class touchpad_mode : std::uint8_t
	{
		select,
		start,
		split
	};
	inline touchpad_mode parse_touchpad_mode(std::string_view text) noexcept
	{
		const auto is = [&](std::string_view word)
		{
			return text.size() == word.size() && std::equal(text.begin(), text.end(), word.begin(),
													 [](char a, char b)
													 {
														 return (a >= 'A' && a <= 'Z' ? static_cast<char>(a - 'A' + 'a') : a) == b;
													 });
		};
		return is("start") ? touchpad_mode::start : is("select") ? touchpad_mode::select :
		                                                           touchpad_mode::split;
	}
	inline touchpad_mode configured_touchpad_mode() noexcept
	{
		static const touchpad_mode mode = []
		{
			const char* value = pbd::env::find("PBD_TOUCHPAD_CLICK");
			return parse_touchpad_mode(value ? std::string_view(value) : std::string_view());
		}();
		return mode;
	}

	// The click's button is picked at click-down and kept until release: a drag or lost contact during a click must not turn a
	// Restart modifier into a pause request. In split, a click whose report carries no usable contact is SELECT, the Practice
	// modifier.
	struct touchpad_click
	{
		static constexpr std::uint16_t select = 0x0100, start = 0x0800;
		touchpad_mode mode = configured_touchpad_mode();
		bool down = false;
		bool placed = false; // the click-down report had a usable contact
		std::uint16_t held = 0;

		std::uint16_t update(bool clicked, std::optional<float> x, bool enabled) noexcept
		{
			if (!enabled || !clicked)
			{
				down = false;
				held = 0;
				return 0;
			}
			if (!down)
			{
				down = true;
				placed = x && std::isfinite(*x) && *x >= 0.f && *x <= 1.f;
				held = mode == touchpad_mode::start || (mode == touchpad_mode::split && placed && *x >= .5f) ? start : select;
			}
			return held;
		}

		// What the click became, for the input log.
		const char* describe() const noexcept
		{
			return held == start ? "START" : mode != touchpad_mode::split || placed ? "SELECT" :
			                                                                          "SELECT (no touch position)";
		}
	};

	// SELECT is a modifier in the Practice fight, never game input. While it is held the live controller's word is
	// neutral, so a SELECT chord cannot also press an attack or move the fighter. A button that was held under SELECT
	// stays withheld after SELECT lifts until that button is released: lifting SELECT first must not turn a chord's
	// button into a fresh press. A button released while SELECT is still down is done with, so a press of it on the
	// boundary SELECT lifts is a new press. Directions are held states rather than presses, so they return as soon as
	// SELECT is up. Nothing is delayed while SELECT is not involved.
	struct select_modifier
	{
		// L3, R3 (SELECT+R3 restarts at the remembered position; an action on R3 must not follow it) and every digital2 button.
		static constexpr std::uint16_t select = 0x0100, buttons = 0x06ff;
		std::uint16_t withheld = 0;

		// word: what would reach the game this frame. still_held: every other reading of the controller this frame, so a
		// button is only let through again once no reading still shows it down.
		std::uint16_t filter(std::uint16_t word, bool select_down, std::uint16_t still_held) noexcept
		{
			if (select_down)
			{
				withheld = static_cast<std::uint16_t>((word | still_held) & buttons);
				return 0;
			}
			withheld = static_cast<std::uint16_t>(withheld & (word | still_held));
			return static_cast<std::uint16_t>(word & ~withheld);
		}
	};

	// Cross pressed a moment before SELECT still means SELECT+Cross. Cross goes down first on many real presses of the
	// chord; those first frames have already reached the game, but the restart that follows replaces whatever they began.
	inline constexpr std::uint32_t late_select_frames = 2;
	struct chord_window
	{
		static constexpr std::uint16_t select = 0x0100, cross = 0x0040;
		std::uint32_t cross_down_at = ~0u;

		// pad and pressed: the controller and its new presses at this boundary. Returns the presses the chord checks see.
		std::uint16_t update(std::uint16_t pad, std::uint16_t pressed, std::uint32_t boundary) noexcept
		{
			// A Cross pressed under SELECT is already a chord, and a Cross paired once is spent: a SELECT released and pressed
			// again (or a touchpad click that bounces) must not pair the same Cross a second time and restart twice.
			if (!(pad & cross))
				cross_down_at = ~0u;
			else if (pressed & cross)
				cross_down_at = (pad & select) ? ~0u : boundary;
			if ((pressed & select) && (pad & cross) && !(pressed & cross) && cross_down_at != ~0u &&
				boundary >= cross_down_at && boundary - cross_down_at <= late_select_frames)
			{
				cross_down_at = ~0u;
				return static_cast<std::uint16_t>(pressed | cross);
			}
			return pressed;
		}
	};

	// While a take is being recorded, SELECT+Circle ends it as START does, in either order: SELECT does nothing else then
	// (restarts wait for the take, and SELECT+START is the Exit hold), so Circle pressed under SELECT, or SELECT pressed
	// with Circle down, is the stop whatever the dummy is doing. A Circle that went down at most late_select_frames before
	// SELECT belongs to the chord: the frames recorded since it went down are the chord's tail, which playback leaves out
	// (take_body). A Circle held longer was part of the take and stays in it.
	struct record_stop
	{
		static constexpr std::uint16_t select = 0x0100, circle = 0x0020;
		std::uint32_t circle_down_at = ~0u;
		std::size_t circle_from = 0;

		// pad and pressed: the controller and its new presses at this boundary. recorded: the frames the take holds so far.
		// Returns true at the boundary where the chord completes; tail receives the frames its early Circle reached.
		bool update(std::uint16_t pad, std::uint16_t pressed, std::uint32_t boundary, std::size_t recorded, std::size_t& tail) noexcept
		{
			tail = 0;
			if (!(pad & circle))
				circle_down_at = ~0u;
			else if (pressed & circle)
			{
				circle_down_at = (pad & select) ? ~0u : boundary;
				circle_from = recorded;
			}
			// Both down, and one of them new at this boundary: a chord held from before is not pressed again.
			if (!(pad & select) || !(pad & circle) || !(pressed & (select | circle)))
				return false;
			if ((pressed & select) && !(pressed & circle) && circle_down_at != ~0u && boundary >= circle_down_at &&
				boundary - circle_down_at <= late_select_frames && recorded >= circle_from)
				tail = recorded - circle_from;
			return true;
		}
	};

	// The take without the stop chord's tail (record_stop). Playback and the neutral measures use this body; the take
	// itself keeps every frame it recorded.
	inline std::span<const std::uint16_t> take_body(std::span<const std::uint16_t> original, std::size_t chord_tail) noexcept
	{
		return original.first(original.size() - (std::min)(chord_tail, original.size()));
	}

	// START opens the Practice menu on its press. A press Practice cannot act on at its own boundary (a Restart still
	// pending, the game's pause state not readable yet, or SELECT still held as the restart chord's modifier) is kept while
	// START stays held and acts at the first boundary that can take it, so a press right after a restart is never lost.
	// SELECT+START from neutral stays the Exit hold: it never opens the menu, even when SELECT lifts before START. A press
	// that arrives while the menu, a game screen or the record question owns START (idle false) is theirs, so a START that
	// closed the menu cannot reopen it.
	struct menu_start
	{
		bool armed = false;

		// start_down, start_pressed: START held, and newly pressed at this boundary. select_down: SELECT held. restart_select:
		// SELECT has been held since a restart chord. idle: nothing else owns START. ready: the menu could act now. Returns
		// true at the one boundary where the press acts.
		bool update(bool start_down, bool start_pressed, bool select_down, bool restart_select, bool idle, bool ready) noexcept
		{
			if (!start_down || !idle || (select_down && !restart_select))
			{
				armed = false;
				return false;
			}
			if (start_pressed)
				armed = true;
			if (!armed || !ready)
				return false;
			armed = false;
			return true;
		}
	};

	// A Restart the game refused before entering its reset changed nothing, so it stays pending and runs at the first
	// boundary that admits it. The bound is ten seconds, longer than any throw, knockdown or cinematic throw lasts, so a
	// restart pressed during one lands when it ends instead of being dropped part-way; a request still refused after that is
	// reported to the player, never dropped silently. The refusals below
	// cannot clear by waiting: the request or the placement it names no longer matches this fight, or the game's code is not
	// the qualified build.
	inline constexpr std::uint32_t restart_retry_frames = 600;
	inline bool restart_retryable(std::string_view reason) noexcept
	{
		constexpr std::array<std::string_view, 7> final_reasons{"missing_request", "stale_scope", "placement_unqualified",
			"stale_placement", "native_body_mismatch", "native_pose_body_mismatch", "native_reset_exception"};
		return std::find(final_reasons.begin(), final_reasons.end(), reason) == final_reasons.end();
	}

	// DR admits an injected START only as a fresh press at its pause step (0x1D0F4), subject to the mode and round-phase
	// flags at 0x1000126C, and shows the pause at the next boundary. Retry after pause_retry_frames with neutral frames
	// between presses, or after rewinding before injection; injected_at == ~0u marks acknowledgement.
	inline constexpr std::uint32_t pause_retry_frames = 4;
	inline bool pause_press_lost(bool injected, std::uint32_t injected_at, std::uint32_t boundary, bool readable, bool paused) noexcept
	{
		if (!injected || injected_at == ~0u || !readable || paused)
			return false;
		return boundary < injected_at || boundary - injected_at >= pause_retry_frames;
	}

	// DR's pause as Practice reads it. DR's own flag (pause manager +0 / +0xC) can still read set for several frames after the
	// game has resumed, so the flag alone cannot tell a paused game from a running one. The frame counter at 0x1008f05c moves
	// only on frames that ran: a flag read on a boundary whose previous frame ran is stale.
	inline bool frame_ran(bool before_valid, std::uint32_t before, bool now_valid, std::uint32_t now) noexcept
	{
		return before_valid && now_valid && now != before;
	}
	inline bool native_paused(bool flag_paused, bool ran) noexcept
	{
		return flag_paused && !ran;
	}

	// The resume START Practice injects to close DR's pause. Until DR is seen running the press is in flight and the pause is
	// still Practice's: the menu does not reopen over a game that may be about to run, and no second START follows (back-to-back
	// STARTs read as one press). DR ignores a START that lands while its own pause is still opening, so a press not answered
	// within resume_retry_frames boundaries is sent again while the resume is still wanted, resume_presses times at most; after
	// that DR's pause is taken over (pause_unowned). A boundary before the press (a rewind took it away) gives it up.
	inline constexpr std::uint32_t resume_retry_frames = 8, resume_presses = 3;
	struct resume_watch
	{
		enum class outcome
		{
			none,
			waiting,
			confirmed,
			resend,
			given_up
		};
		std::uint32_t sent_at = ~0u, presses = 0;

		bool in_flight() const noexcept
		{
			return sent_at != ~0u;
		}
		void sent(std::uint32_t boundary) noexcept
		{
			sent_at = boundary;
			++presses;
		}
		void clear() noexcept
		{
			sent_at = ~0u;
			presses = 0;
		}
		// Once per boundary with DR's pause as read there (native_paused). wanted: Practice still wants the resume (its menu has not
		// reopened and no new pause was asked for). On resend the caller injects the press again and reports it with sent().
		outcome update(std::uint32_t boundary, bool readable, bool paused, bool wanted) noexcept
		{
			if (!in_flight())
				return outcome::none;
			if (readable && !paused)
			{
				clear();
				return outcome::confirmed;
			}
			if (boundary >= sent_at && boundary - sent_at < resume_retry_frames)
				return outcome::waiting;
			const bool again = wanted && readable && boundary >= sent_at && presses < resume_presses;
			const std::uint32_t made = presses;
			clear();
			if (!again)
				return outcome::given_up;
			presses = made;
			return outcome::resend;
		}
	};

	// DR's pause in the Practice fight always belongs to Practice: DR's own pause menu is never drawn there and the game reads no
	// controller while it is up. One boundary's view of it:
	struct pause_scene
	{
		bool fight = false;     // battle scene, DR's pause readable, none of DR's own screens (Command List, selection) open
		bool flag = false;      // DR's pause flag as read, stale or not
		bool paused = false;    // native_paused
		bool dr_menu = false;   // DR's own pause menu word (manager +0xC) is set
		bool menu = false;      // Practice's menu or the take's question is open
		bool requested = false; // a pause press is wanted or in flight
		bool resuming = false;  // a resume press is waiting to be sent or in flight
	};
	// DR's pause menu up with nothing in Practice having opened, shown or left it: a resume DR never took, or a START that
	// reached DR another way. The menu word keeps any other hold of DR's (manager +0 alone) out of it.
	inline bool pause_unowned(const pause_scene& s) noexcept
	{
		return s.fight && s.paused && s.dr_menu && !s.menu && !s.requested && !s.resuming;
	}
	// DR's pause menu is not painted while Practice owns the pause or is about to. The raw flag counts here: suppressing the
	// paint costs nothing while the game runs, and the flag shows a new pause one boundary before the counter can confirm it.
	inline bool claims_paint(const pause_scene& s, bool menu_shown) noexcept
	{
		return menu_shown || s.requested || s.resuming || (s.fight && (s.flag || s.paused));
	}
	// The game reads neutral controllers while the pause is Practice's or changing hands; Practice's own presses still reach it.
	inline bool claims_input(const pause_scene& s) noexcept
	{
		return s.menu || s.requested || s.resuming || (s.fight && s.paused);
	}
	// An unowned pause is answered as a START press would be once it has held for takeover_frames boundaries in a row, so one
	// misread cannot open the menu over a running game.
	inline constexpr std::uint32_t takeover_frames = 2;
	struct pause_takeover
	{
		std::uint32_t held = 0;

		bool update(bool unowned) noexcept
		{
			held = unowned ? held + 1 : 0;
			if (held < takeover_frames)
				return false;
			held = 0;
			return true;
		}
	};

	// In the fight no controller's START is game input, whatever the menus or timing: one DR read before Practice saw it would
	// open DR's own pause menu. Only Practice's injected pause and resume presses reach DR there.
	inline constexpr std::uint32_t start_bit = 0x0800;
	inline std::uint32_t fight_word(std::uint32_t word, bool fight) noexcept
	{
		return fight ? word & ~start_bit : word;
	}

	// Apply gameplay remaps after consuming menu gestures. Directions and system buttons retain their native meanings;
	// L3 and R3 emit their assigned attacks when mapped and retain their click bits when unmapped.
	inline constexpr auto sources = pbd::controllers::settings::slot_bits;
	using button_map = std::array<std::uint16_t, sources.size()>;
	inline constexpr button_map defaults{0x80, 0x10, 0x40, 0x20, 0, 0, 0, 0, 0, 0};
	inline constexpr std::size_t r3_slot = 9;
	// PBD_PRACTICE_BUTTON_MAP: a comma-separated attack mask per slot, in `sources` order. Eight masks leave both stick clicks
	// off, as the launcher sends them while neither has an action (practiceControlMapping.ts serializePracticeButtonMapping).
	inline bool parse_mapping(std::string_view text, button_map& out)
	{
		button_map parsed{};
		std::size_t count = 0;
		for (bool more = true; more; ++count)
		{
			if (count == parsed.size())
				return false;
			const auto end = text.find(',');
			const auto part = text.substr(0, end);
			unsigned value = 0;
			const auto [tail, ec] = std::from_chars(part.data(), part.data() + part.size(), value);
			// A slot may contain any combination of the four attacks, or off.
			if (part.empty() || ec != std::errc{} || tail != part.data() + part.size() || (value & ~0xf0u))
				return false;
			parsed[count] = static_cast<std::uint16_t>(value);
			more = end != std::string_view::npos;
			if (more)
				text.remove_prefix(end + 1);
		}
		if (count != 8 && count != parsed.size())
			return false;
		out = parsed;
		return true;
	}
	// A slot's attacks: the in-game menu's live mapping once the controller has published one (the same as the launcher's
	// saved mapping until the menu changes it), else the map Practice started with.
	inline std::uint16_t slot_attacks(const button_map& map, std::size_t slot) noexcept
	{
		if (!pbd::controllers::settings::mapping_changed.load(std::memory_order_acquire))
			return map[slot];
		return pbd::controllers::settings::slot_attacks(pbd::controllers::settings::attack_masks.load(std::memory_order_acquire),
			static_cast<unsigned>(slot));
	}
	// R3 is Practice's Remember until it has an action: in the fight an action on R3 wins, and Remember Position is the
	// Restart Settings row (SELECT+R3, read before the mapping, still restarts at the remembered position).
	inline bool r3_remembers(const button_map& map) noexcept
	{
		return slot_attacks(map, r3_slot) == 0;
	}
	inline std::uint16_t map_attacks(std::uint16_t raw, const button_map& map) noexcept
	{
		std::uint16_t result = raw & 0xff00;
		for (std::size_t i = 0; i < sources.size(); ++i)
		{
			const auto attacks = slot_attacks(map, i);
			if ((sources[i] & pbd::controllers::settings::stick_clicks) && attacks)
				result = static_cast<std::uint16_t>(result & ~sources[i]);
			if (raw & sources[i])
				result |= attacks;
		}
		return result;
	}

	// Keep the original take intact. With Trim Neutral on, playback omits only the contiguous neutral frames before the
	// first input and after the last one: the wait before the player started and the wait before START ended the take.
	// Held crouch, charge, stance inputs and every neutral interval between two inputs remain part of the command.
	inline std::size_t leading_neutral_frames(std::span<const std::uint16_t> original) noexcept
	{
		std::size_t first = 0;
		while (first < original.size() && original[first] == 0)
			++first;
		return first;
	}
	inline std::size_t trailing_neutral_frames(std::span<const std::uint16_t> original) noexcept
	{
		std::size_t end = original.size();
		while (end > 0 && original[end - 1] == 0)
			--end;
		return original.size() - end;
	}
	// The counts are clamped, so stale or oversized counts can never reach outside the take. An all-neutral take trims to
	// nothing.
	inline std::span<const std::uint16_t> playback_frames(std::span<const std::uint16_t> original,
		std::size_t leading, std::size_t trailing, bool trim) noexcept
	{
		if (!trim)
			return original;
		const std::size_t first = (std::min)(leading, original.size());
		const std::size_t end = original.size() - (std::min)(trailing, original.size() - first);
		return original.subspan(first, end - first);
	}
	// An action needs at least one input. An all-neutral take (kept with Trim Neutral off) would only stand still, so it is
	// never scheduled as an action and cannot keep playback running on its own.
	inline bool has_input(std::span<const std::uint16_t> frames) noexcept
	{
		return std::any_of(frames.begin(), frames.end(), [](std::uint16_t word)
			{
				return word != 0;
			});
	}

	// Zero weights represent empty or disabled slots. One-slot playback must admit the selected slot itself.
	inline bool playback_available(const std::array<unsigned, 8>& weights, unsigned mode, unsigned selected) noexcept
	{
		if (mode == 0)
			return selected < weights.size() && weights[selected] != 0;
		if (mode != 1 && mode != 2)
			return false;
		return std::any_of(weights.begin(), weights.end(), [](unsigned weight)
			{
				return weight != 0;
			});
	}

	// Largest remainders ensure the eight displayed integer percentages sum to exactly 100, including unequal weights.
	inline std::array<unsigned, 8> percentages(const std::array<unsigned, 8>& weights)
	{
		std::array<unsigned, 8> result{}, remainder{};
		unsigned total = 0, assigned = 0;
		for (auto weight : weights)
			total += weight;
		if (!total)
			return result;
		for (std::size_t i = 0; i < result.size(); ++i)
		{
			result[i] = weights[i] * 100 / total;
			remainder[i] = weights[i] * 100 % total;
			assigned += result[i];
		}
		while (assigned < 100)
		{
			const auto best = std::max_element(remainder.begin(), remainder.end());
			++result[static_cast<std::size_t>(best - remainder.begin())];
			*best = 0;
			++assigned;
		}
		return result;
	}
} // namespace pbd::practice::controls
