#pragma once

// Replay rewind: which in-memory checkpoints a watched replay keeps, which one a seek or a round jump restores, and which
// one gives way when the memory budget is reached. Standard library only, so these choices are testable; the latch owns the
// guest pages, the journals and the restore (pbd_rollback_session_rewind.cpp).
//
// A checkpoint is the state before the inputs of its boundary. Each keeps the guest's static pages and an undo journal: the
// earlier contents of every other page the game first wrote after it, up to the next checkpoint. Restoring checkpoint k
// applies the journals from k to the newest with the earliest copy of each page winning, then k's static pages, so every
// page holds its value at k again. Removing a checkpoint folds its journal into the one before it (a page that one already
// holds keeps the earlier copy), so thinning the list never breaks a restore to any checkpoint that is kept.
//
// Nothing here is written to a file: the checkpoints live only while the replay is watched.

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <span>
#include <unordered_set>

namespace pbd::replay_rewind
{
	using u16 = std::uint16_t;
	using u32 = std::uint32_t;
	using u64 = std::uint64_t;
	using s32 = std::int32_t;

	// One checkpoint per second of fight; each round's first fight boundary is always kept (pinned).
	inline constexpr u32 spacing = 60;
	// Rewind and fast forward move ten seconds per press; a held press repeats.
	inline constexpr u32 step_frames = 600;
	inline constexpr u32 repeat_delay_ms = 400, repeat_interval_ms = 200;
	// Physical boundaries keep increasing across a round reconstruction; the displayed frame follows the recorded input.
	constexpr u32 recorded_frame(u32 boundary, u32 anchor, u32 input_base, u32 first_battle_input) noexcept
	{
		if (anchor == ~0u || boundary < anchor)
			return 0;
		const u64 input = static_cast<u64>(input_base) + boundary - anchor;
		return input > first_battle_input ? static_cast<u32>(std::min<u64>(input - first_battle_input, UINT32_MAX)) : 0;
	}
	// About 4 MiB per checkpoint (2.4 MiB static pages plus a second of first writes): 512 MiB keeps two minutes at full
	// density before the list starts to thin out.
	inline constexpr u64 default_budget = 512ull << 20;
	inline constexpr u64 min_budget_mib = 64, max_budget_mib = 4096;

	struct point
	{
		u32 index = 0, match = 0, frame = 0, round = 0;
		bool pinned = false;
		u64 bytes = 0;
	};

	// Only the first preimage of each page is used by a restore. A later journal containing no new pages stays compressed.
	struct journal_coverage
	{
		std::unordered_set<u32> seen;
		bool include(std::span<const u32> pages)
		{
			bool needed = false;
			for (const u32 page : pages)
				needed = seen.insert(page).second || needed;
			return needed;
		}
	};

	// Whether boundary `index` takes a checkpoint. Only while the recording plays both sides (`watching`) and only on a fight
	// boundary: between rounds the render thread drives the game's shared frame object, which a restore must not rewind under
	// it. A round's first fight boundary is always taken; otherwise one `spacing` after the newest. After a restore the
	// newest checkpoint can lie ahead of `index` until the replay passes it again.
	constexpr bool capture_due(bool watching, bool fighting, bool round_start, bool have_newest, u32 newest_index,
		u32 index) noexcept
	{
		if (!watching || !fighting)
			return false;
		if (!have_newest)
			return true;
		if (index <= newest_index)
			return false;
		return round_start || index - newest_index >= spacing;
	}

	// The checkpoint a seek to `frame` of `match` restores: the newest one at or before that frame; for a frame before the
	// first checkpoint of the match, the first one (the earliest moment that can still be shown).
	inline std::optional<std::size_t> restore_choice(std::span<const point> points, u32 match, u32 frame, bool exact = false) noexcept
	{
		std::optional<std::size_t> best, first;
		for (std::size_t i = 0; i < points.size(); ++i)
		{
			if (points[i].match != match)
				continue;
			if (!first)
				first = i;
			if (points[i].frame <= frame)
				best = i;
		}
		return best ? best : exact ? std::nullopt :
		                             first;
	}

	// The pinned checkpoint at the start of `round` of `match`, if it is still held.
	inline std::optional<std::size_t> round_choice(std::span<const point> points, u32 match, u32 round) noexcept
	{
		for (std::size_t i = 0; i < points.size(); ++i)
			if (points[i].match == match && points[i].round == round && points[i].pinned)
				return i;
		return std::nullopt;
	}

	// The checkpoint to remove while `total` exceeds `budget`. Never the first (the earliest moment) or the newest (its
	// journal is still being written). Unpinned ones go first, the one whose neighbours lie closest together, so the kept
	// moments stay evenly spread; the oldest wins a tie. Pinned round starts go only when nothing else is left.
	inline std::optional<std::size_t> eviction_choice(std::span<const point> points, u64 total, u64 budget) noexcept
	{
		if (total <= budget || points.size() < 3)
			return std::nullopt;
		for (const bool pinned : {false, true})
		{
			std::optional<std::size_t> best;
			u32 best_span = std::numeric_limits<u32>::max();
			for (std::size_t i = 1; i + 1 < points.size(); ++i)
			{
				if (points[i].pinned != pinned)
					continue;
				const u32 span = points[i + 1].index - points[i - 1].index;
				if (span < best_span)
				{
					best_span = span;
					best = i;
				}
			}
			if (best)
				return best;
		}
		return std::nullopt;
	}

	// A seek step of `delta` frames from `base` (the pending target while a seek is still running, so repeated presses add
	// up and the newest target wins), clamped to the match: never before its first frame, never past `last`.
	constexpr u32 step_target(u32 base, s32 delta, u32 last) noexcept
	{
		const auto target = static_cast<std::int64_t>(base) + delta;
		if (target <= 0)
			return 0;
		return static_cast<u64>(target) >= last ? last : static_cast<u32>(target);
	}

	// PBD_REPLAY_REWIND_MIB: the checkpoint memory budget in MiB; absent, zero or out of range keeps the default.
	constexpr u64 budget_from_mib(u64 mib) noexcept
	{
		return mib >= min_budget_mib && mib <= max_budget_mib ? mib << 20 : default_budget;
	}

	// Hold-to-repeat for a set of buttons (rewind / fast forward). A fresh press acts at once; holding that one button repeats
	// it after repeat_delay_ms and then every repeat_interval_ms. Pressing a second button of the set, or releasing, stops
	// the repeat. Without a clock (now_ms == 0) only fresh presses act.
	struct repeat
	{
		u16 button = 0;
		u64 next_ms = 0;

		u16 poll(u16 held, u16 pressed, u16 buttons, u64 now_ms) noexcept
		{
			const u32 fresh = pressed & buttons;
			if (fresh)
			{
				button = static_cast<u16>(fresh & (~fresh + 1u));
				next_ms = now_ms ? now_ms + repeat_delay_ms : 0;
				return button;
			}
			if (!button || (held & buttons) != button)
			{
				button = 0;
				return 0;
			}
			if (!now_ms || !next_ms || now_ms < next_ms)
				return 0;
			next_ms = now_ms + repeat_interval_ms;
			return button;
		}
		void clear() noexcept
		{
			button = 0;
			next_ms = 0;
		}
	};
} // namespace pbd::replay_rewind
