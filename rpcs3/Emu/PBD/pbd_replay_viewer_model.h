#pragma once

// Host-only replay controller and presentation. No guest-memory readers or restore implementation live here.
// The latch remains the sole simulation owner; its existing takeover/seek primitives execute returned commands.
#include <algorithm>
#include <array>
#include <atomic>
#include <charconv>
#include <cstdint>
#include <cstdlib>
#include <mutex>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "pbd_replay_rewind.h"
#include "pbd_replay_takeover.h"
#include "Emu/PBD/pbd_practice_presentation.h"

namespace pbd::replay_viewer
{
	using word = std::uint16_t;
	inline constexpr word select = 0x0100, start = 0x0800, cross = 0x0040, circle = 0x0020;
	inline constexpr word up = 0x1000, down = 0x4000, left = 0x8000, right = 0x2000;
	inline constexpr word l1 = 0x0004, r1 = 0x0008, l2 = 0x0001, r2 = 0x0002, l3 = 0x0200, r3 = 0x0400, triangle = 0x0010;
	inline constexpr word ordinary = 0xf0ff;
	enum class page : std::uint32_t
	{
		none,
		playback,
		speed,
		rounds,
		display,
		takeover_options
	};
	// Scrubbing restores in-memory checkpoints; round selection rebuilds admitted records in one-game recordings.
	// Forward seeks run the recorded inputs fast with the picture and sound held back.
	inline bool requested() noexcept
	{
		const auto* value = pbd::env::find("PBD_REPLAY_VIEWER");
		return value && std::string_view(value) == "1";
	}
	constexpr bool combat_fight(std::uint32_t mode, std::uint32_t scene, std::uint32_t phase) noexcept
	{
		return mode != pbd::practice::practice_mode && (scene == 16 || scene == 17) && phase == 5;
	}
	enum class command
	{
		none,
		pause,
		step,
		speed,
		seek_back,
		seek_forward,
		previous_round,
		next_round,
		round,
		arm,
		cancel_arm,
		start_take,
		retry,
		return_replay,
		bookmark,
		exit,
		take_over, // value: the side taken at once (1 = left / Player 1, 2 = right / Player 2)
		leave,     // value: 1 = Return to Launcher, 2 = Exit Game (the pause menu's two leaving rows)
		step_back
	};
	inline constexpr std::uint32_t pointer_options = 256, pointer_exit = 257, pointer_discard = 258, pointer_back = 259, pointer_none = ~0u;
	struct action
	{
		command kind = command::none;
		std::int32_t value = 0;
		word game = 0;
		// Play once the command is done: a round picked from a list that paused a playing replay plays from that round.
		bool resume = false;
	};
	struct round_row
	{
		std::uint32_t match = 0, round = 0, from = 0, fight = 0;
		std::int32_t end = -1;
	};
	// A one-game recording uses the viewer's local match number, even when its source was a later game in a set.
	// Keep observed fight boundaries; unvisited rounds start at their admitted native initializer until watched.
	template <typename Frame>
	void include_recorded_rounds(std::vector<round_row>& rows, std::uint32_t match, Frame frame)
	{
		if (!match)
			return;
		bool added = false;
		for (std::uint32_t round = 1; round <= 32; ++round)
		{
			const auto from = frame(round);
			if (!from || std::any_of(rows.begin(), rows.end(), [&](const auto& r)
							 {
								 return r.match == match && r.round == round;
							 }))
				continue;
			rows.push_back({match, round, *from, *from, -1});
			added = true;
		}
		if (added)
			std::sort(rows.begin(), rows.end(), [](const auto& a, const auto& b)
				{
					return a.match == b.match ? a.round < b.round : a.match < b.match;
				});
	}

	// Reject the whole manifest on a malformed/overlarge row: never partially trust replay metadata.
	inline bool parse_rounds(std::string_view text, std::vector<round_row>& rows)
	{
		rows.clear();
		if (text.empty())
			return true;
		if (text.size() > 16384)
			return false;
		std::vector<round_row> result;
		std::size_t at = 0;
		while (at < text.size())
		{
			std::array<std::int32_t, 5> v{};
			for (std::size_t i = 0; i < v.size(); ++i)
			{
				const auto end = text.find(i == 4 ? ';' : ':', at);
				const auto stop = end == std::string_view::npos ? text.size() : end;
				if (stop == at || (i != 4 && end == std::string_view::npos))
					return false;
				const auto parsed = std::from_chars(text.data() + at, text.data() + stop, v[i]);
				if (parsed.ec != std::errc{} || parsed.ptr != text.data() + stop)
					return false;
				at = stop + (end != std::string_view::npos);
			}
			if (v[0] < 1 || v[0] > 65535 || v[1] < 1 || v[1] > 255 || v[2] < 0 || v[3] < v[2] ||
				v[3] >= 216000 || v[4] < -1 || (v[4] != -1 && (v[4] < v[3] || v[4] >= 216000)))
				return false;
			for (const auto& r : result)
				if (r.match == static_cast<std::uint32_t>(v[0]) && r.round == static_cast<std::uint32_t>(v[1]))
					return false;
			result.push_back({static_cast<std::uint32_t>(v[0]), static_cast<std::uint32_t>(v[1]),
				static_cast<std::uint32_t>(v[2]), static_cast<std::uint32_t>(v[3]), v[4]});
			if (result.size() > 256)
				return false;
		}
		std::sort(result.begin(), result.end(), [](const auto& a, const auto& b)
			{
				return a.match == b.match ? a.round < b.round : a.match < b.match;
			});
		for (std::size_t i = 0; i < result.size(); ++i)
		{
			const auto& row = result[i];
			if (!i || row.match != result[i - 1].match)
			{
				if (row.round != 1)
					return false;
			}
			else
			{
				const auto& previous = result[i - 1];
				if (row.round != previous.round + 1 || row.from <= previous.from || row.fight <= previous.fight ||
					(previous.end != -1 && static_cast<std::uint32_t>(previous.end) > row.from))
					return false;
			}
		}
		rows = std::move(result);
		return true;
	}

	struct context
	{
		bool paused = false, in_match = false;
		std::int32_t speed = 0;
		pbd::takeover_phase takeover = pbd::takeover_phase::watching;
		std::uint32_t round_count = 0;
		bool fighting = false;
		std::uint64_t now_ms = 0;      // steady clock; drives hold-to-repeat of rewind / fast forward (0: fresh presses only)
		std::uint32_t round_focus = 0; // the current round's place in the round list, where the list opens
		// The stick clicks the player gave an action (pbd::controllers::settings::clicks_with_actions): watching, L3 and R3
		// take control; a taken fighter's L3 or R3 is that action, as in any fight.
		word clicks = 0;
	};
	constexpr std::uint32_t previous_round_target(std::uint32_t round, bool fighting) noexcept
	{
		return fighting && round > 1 ? round - 1 : std::max(round, 1u);
	}
	constexpr bool seek_finished(std::uint32_t target_match, std::uint32_t target_frame, std::uint32_t match,
		bool in_match, std::uint32_t frame) noexcept
	{
		return target_match && (match > target_match || (match == target_match && (!in_match || frame >= target_frame)));
	}
	constexpr bool round_forward_finished(std::uint32_t target_match, std::uint32_t target_round,
		std::uint32_t match, bool in_match, std::uint32_t round, bool fighting) noexcept
	{
		return target_match && target_round && (match > target_match || (match == target_match && (!in_match || (fighting && round >= target_round))));
	}
	// Viewer expiry depends on completed simulation boundaries, including a boundary held by a menu.
	// A paused boundary below the deadline never expires by wall time; the exact deadline always owns the prompt.
	constexpr bool takeover_finished(std::uint32_t index, std::uint32_t deadline, bool fighting,
		std::uint32_t last_fight, std::uint32_t start_index) noexcept
	{
		return index >= deadline || (!fighting && last_fight >= start_index && index >= last_fight && index - last_fight >= 8);
	}
	// The latch invokes this before interpreting a host word, including every paused/slowed poll.
	constexpr bool takeover_prompt_due(pbd::takeover_phase state, std::uint32_t index, std::uint32_t deadline,
		bool fighting, std::uint32_t last_fight, std::uint32_t start_index) noexcept
	{
		return state == pbd::takeover_phase::controlling && takeover_finished(index, deadline, fighting, last_fight, start_index);
	}
	inline constexpr std::size_t max_menu_rows = 8;
	// The Options menu's rows. Watching: the replay menu (Resume, either side's controls, speed, rounds, display), then
	// the pause menu's two leaving rows. In control: Resume, Try Again and Return to Replay once the attempt runs, Display and
	// the same two leaving rows.
	enum class option : std::uint8_t
	{
		resume,
		take_left,
		take_right,
		speed,
		rounds,
		display,
		retry,
		return_replay,
		launcher,
		quit
	};
	struct option_rows
	{
		std::array<option, max_menu_rows> rows{};
		std::uint32_t count = 0;
		constexpr void add(option row) noexcept
		{
			if (count < rows.size())
				rows[count++] = row;
		}
	};
	constexpr option_rows options_for(page menu, pbd::takeover_phase takeover) noexcept
	{
		option_rows o;
		if (menu == page::playback)
		{
			o.add(option::resume);
			o.add(option::take_left);
			o.add(option::take_right);
			o.add(option::speed);
			o.add(option::rounds);
			o.add(option::display);
		}
		else if (menu == page::takeover_options)
		{
			o.add(option::resume);
			if (takeover == pbd::takeover_phase::controlling)
			{
				o.add(option::retry);
				o.add(option::return_replay);
			}
			o.add(option::display);
		}
		else
			return o;
		o.add(option::launcher);
		o.add(option::quit);
		return o;
	}
	// Display: each side's command history (Player and Opponent Command History), then the frame data panels.
	enum class display_row : std::uint32_t
	{
		p1_history,
		p2_history,
		frame_data,
		count
	};
	inline constexpr std::uint32_t speed_rows = 5;
	constexpr std::uint32_t row_count(page menu, const context& c) noexcept
	{
		switch (menu)
		{
		case page::playback:
		case page::takeover_options: return options_for(menu, c.takeover).count;
		case page::speed: return speed_rows;
		case page::rounds: return std::min(c.round_count, 255u);
		case page::display: return static_cast<std::uint32_t>(display_row::count);
		default: return 0;
		}
	}
	constexpr std::uint32_t round_list_focus(const context& c) noexcept
	{
		return c.round_count ? std::min(c.round_focus, c.round_count - 1) : 0u;
	}

	// The replay buttons while watching: Circle pauses, Triangle opens the round list, Left / Right rewind and fast
	// forward (held: repeated), L1 / R1 go to the previous / next round, L3 / R3 take Player 1 / Player 2 at once, Options
	// (Start) opens the menu. L2 / R2 change the speed while playing, or step one frame backward / forward while paused.
	// Cross stays free.
} // namespace pbd::replay_viewer
