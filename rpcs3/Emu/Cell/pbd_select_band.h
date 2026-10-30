#pragma once

#include "pbd_select_timer.h"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <string>
#include <string_view>
#include <utility>

// The update latch composes the selection countdown band from the native view at each fresh local sample; the RSX only draws it.
// Both peers use the same boundary and view, while pbd::select_timer alone controls the input at zero.
namespace pbd::select_band
{
	using u32 = std::uint32_t;
	using u64 = std::uint64_t;

	// The stage picker's controls in the stock stage select: it opens on a spinning roulette (stopping it is the random pick), Up
	// opens the stage list, and the countdown's zero press stops the roulette or takes the highlighted stage. {cross} is drawn as
	// the pad's Cross glyph.
	inline constexpr std::string_view picker_hint = "{cross} stops the roulette \xC2\xB7 Up opens the stage list \xC2\xB7 at zero it stops by itself";
	inline constexpr std::string_view right_side_rule = "Right side picks in Tekken 5 DR";
	// A game the lobby started with a random stage (the loser's Rematch with random stage): the stage countdown starts at zero, so
	// the roulette stops by itself for both players and nobody picks.
	inline constexpr std::string_view random_stage_title = "Random stage";
	inline constexpr std::string_view random_stage_detail = "The roulette stops by itself";
	// A game the loser started with Change character: only the loser picks a fighter, the winner keeps theirs, and the last game's
	// stage is taken again for both players.
	inline constexpr std::string_view kept_fighter_title = "Your fighter stays";
	inline constexpr std::string_view same_stage_title = "Same stage";
	inline constexpr std::string_view same_stage_detail = "The last game's stage stays";
	inline constexpr std::string_view unnamed_opponent = "Your opponent";
	inline constexpr std::size_t name_limit = 16;

	// The opponent's name as the band writes it into its sentences. The set manifest already admits only 1 to 16 letters, digits,
	// spaces, '_', '.' and '-'; the band still keeps printable ASCII only, drops surrounding spaces and keeps at most name_limit
	// characters, so a name of spaces or an unexpected byte can never reach the glyph run. An empty result reads "Your opponent".
	inline std::string display_name(std::string_view raw)
	{
		std::string kept;
		for (const char c : raw)
			if (c >= 0x20 && c <= 0x7e)
				kept += c;
		const auto first = kept.find_first_not_of(' ');
		if (first == std::string::npos)
			return std::string(unnamed_opponent);
		kept = kept.substr(first, kept.find_last_not_of(' ') - first + 1);
		if (kept.size() > name_limit)
			kept.resize(name_limit);
		while (kept.back() == ' ')
			kept.pop_back(); // a cut can end on a space; the first character is not one
		return kept;
	}

	struct snapshot
	{
		u64 generation = 0; // the emulation that published it; another emulation draws nothing
		bool visible = false;
		std::string title;           // "Choose your fighter", "Pick the stage", "<opponent> picks the stage", "Random stage",
		                             // "Your fighter stays" or "Same stage"
		u32 seconds = 0;             // whole seconds left in the countdown
		u32 frames = 0, total = 0;   // boundaries left and the countdown's length: the draining bar under the band
		std::string detail;          // "<opponent> is choosing" / "<opponent> is ready", the other player's fighter, or the stage rule
		bool detail_waiting = false; // the opponent is still choosing: drawn in the secondary ink
		std::string hint;            // the stage picker's controls with glyph tokens; empty for everyone else
		bool operator==(const snapshot&) const = default;
	};

	// The band for one sample. It shows while a countdown runs and the game shows its VS select: during the character state for
	// both players, then while the stage owner's record is in the stage state. Everywhere else (loading, the VS screen, battle,
	// between games, a refused or finished selection) it is hidden.
	inline snapshot compose(const pbd::select_timer::band& countdown, const pbd::select_timer::view& now, u32 local_role, std::string_view opponent)
	{
		using namespace pbd::select_timer;
		snapshot out;
		if (!now.verified || !now.vs_mode || now.scene != scene_select || local_role > 1 || (countdown.chooser && *countdown.chooser > 1))
			return out;
		const std::string name = display_name(opponent);
		if (countdown.current == phase::character && countdown.chooser)
		{
			// Change character: both screens count the chooser's countdown.
			const u32 chooser = *countdown.chooser;
			if (chooser == local_role)
			{
				out.title = "Choose your fighter";
				out.detail = name + " keeps the same fighter";
			}
			else
			{
				out.title = std::string(kept_fighter_title);
				out.detail_waiting = countdown.choosing[chooser];
				out.detail = name + (out.detail_waiting ? " is choosing a new fighter" : " is ready");
			}
		}
		else if (countdown.current == phase::character)
		{
			out.title = "Choose your fighter";
			out.detail_waiting = countdown.choosing[local_role ^ 1u];
			out.detail = name + (out.detail_waiting ? " is choosing" : " is ready");
		}
		else if (countdown.current == phase::stage && now.stage_open)
		{
			if (countdown.chooser)
			{
				out.title = std::string(same_stage_title);
				out.detail = std::string(same_stage_detail);
			}
			else if (countdown.random_stage)
			{
				out.title = std::string(random_stage_title);
				out.detail = std::string(random_stage_detail);
			}
			else if (countdown.stage_owner == local_role)
			{
				out.title = "Pick the stage";
				out.hint = std::string(picker_hint);
			}
			else
			{
				out.title = name + " picks the stage";
				if (countdown.stage_owner == 1)
					out.detail = std::string(right_side_rule);
			}
		}
		else
			return out;
		out.visible = true;
		out.seconds = countdown.seconds;
		out.frames = countdown.frames;
		out.total = countdown.total;
		return out;
	}

	// One published band for the renderer. The revision moves only when a drawn value changes, so the overlay rebuilds its draw
	// list at most once per changed boundary and never between two equal ones.
	inline std::mutex published_mutex;
	inline snapshot published;
	inline std::atomic<u64> published_revision{0};

	inline void publish(u64 generation, snapshot value)
	{
		value.generation = generation;
		std::lock_guard lock(published_mutex);
		if (published == value)
			return;
		published = std::move(value);
		published_revision.fetch_add(1, std::memory_order_release);
	}
	inline void hide(u64 generation)
	{
		publish(generation, snapshot{});
	}
	inline u64 revision()
	{
		return published_revision.load(std::memory_order_acquire);
	}
	inline snapshot load()
	{
		std::lock_guard lock(published_mutex);
		return published;
	}
} // namespace pbd::select_band
