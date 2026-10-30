#pragma once

// Practice's in-game move list: the dummy's moves as the game's own command list orders them, opened over the Practice
// menu to choose the moves a Defense Dummy Action slot or Offense's Action After a Hit or Block plays. Each move is on or
// off; a Defense slot keeps one move, while Offense can pick from several at random. Up/Down move between moves (category
// titles are passed over), L1/R1 jump to the previous or next category, Left/Right a page, Cross turns the highlighted move
// on or off, and Circle closes the list with the moves that are on.

#include <algorithm>
#include <cstdint>
#include <iterator>
#include <string>
#include <utility>
#include <vector>

namespace pbd::practice::move_list_view
{
	using u32 = std::uint32_t;

	// What the list chooses for: a Defense slot (0..7) or Offense's reaction.
	inline constexpr u32 offense_target = 8;
	// Rows shown at once, and the most moves one choice holds (pbd::practice::dummy_move::valid).
	inline constexpr u32 visible_rows = 14, max_chosen = 64;

	struct row
	{
		u32 id = 0;
		std::string name, command;
		bool header = false, selectable = false;
		// The command as the overlay draws it, in arrow and button glyph tokens (pbd::move_list::notation_glyphs); empty for
		// a category title.
		std::string glyphs;
	};

	struct state
	{
		bool open = false;
		u32 target = 0;
		std::vector<row> rows;
		// The ids that are on, ascending.
		std::vector<u32> chosen;
		// The highlighted row (never a category title while a move exists) and the first row shown.
		u32 focus = 0, first = 0;

		bool on(u32 id) const
		{
			return std::binary_search(chosen.begin(), chosen.end(), id);
		}
		bool single_choice() const
		{
			return target < offense_target;
		}
		bool focusable(u32 at) const
		{
			return at < rows.size() && !rows[at].header;
		}
		// Opens on the first move that is on, or the first move.
		void open_on(u32 for_target, std::vector<row> list, std::vector<u32> current)
		{
			*this = {};
			open = true;
			target = for_target;
			rows = std::move(list);
			std::sort(current.begin(), current.end());
			current.erase(std::unique(current.begin(), current.end()), current.end());
			for (const auto id : current)
				if ((!single_choice() || chosen.empty()) &&
					std::any_of(rows.begin(), rows.end(), [&](const row& each) { return !each.header && each.id == id; }))
					chosen.push_back(id);
			focus = rows.empty() ? 0 : static_cast<u32>(rows.size());
			for (u32 at = 0; at < rows.size() && focus == rows.size(); ++at)
				if (focusable(at) && (chosen.empty() || on(rows[at].id)))
					focus = at;
			if (focus == rows.size())
				focus = 0;
			scroll();
		}
		// Up/Down: the next move either way, wrapping and passing over category titles.
		void step(bool down)
		{
			const auto count = static_cast<u32>(rows.size());
			for (u32 tried = 0, at = focus; tried < count; ++tried)
			{
				at = down ? (at + 1) % count : (at + count - 1) % count;
				if (focusable(at))
				{
					focus = at;
					break;
				}
			}
			scroll();
		}
		// Left/Right: a page of moves, stopping at the ends.
		void page(bool down)
		{
			u32 at = focus;
			for (u32 moved = 0; moved < visible_rows;)
			{
				if (down ? at + 1 >= rows.size() : at == 0)
					break;
				at = down ? at + 1 : at - 1;
				if (focusable(at))
				{
					focus = at;
					++moved;
				}
			}
			scroll();
		}
		// L1/R1: the first move of the next category, or of this one (then the one before) going back. Past the last
		// category Down goes to the last move.
		void category(bool down)
		{
			std::vector<u32> starts;
			bool fresh = true;
			for (u32 at = 0; at < rows.size(); ++at)
			{
				if (rows[at].header)
					fresh = true;
				else if (std::exchange(fresh, false))
					starts.push_back(at);
			}
			if (starts.empty())
				return;
			if (down)
			{
				const auto next = std::upper_bound(starts.begin(), starts.end(), focus);
				if (next != starts.end())
					focus = *next;
				else
					for (u32 at = static_cast<u32>(rows.size()); at > 0; --at)
						if (focusable(at - 1))
						{
							focus = at - 1;
							break;
						}
			}
			else
			{
				const auto at = std::lower_bound(starts.begin(), starts.end(), focus);
				focus = at != starts.begin() ? *std::prev(at) : starts.front();
			}
			scroll();
		}
		// Cross: the highlighted move on or off. False when it cannot be (a move the dummy cannot perform, or the list full).
		bool toggle()
		{
			if (!focusable(focus))
				return false;
			const u32 id = rows[focus].id;
			const auto at = std::lower_bound(chosen.begin(), chosen.end(), id);
			if (at != chosen.end() && *at == id)
				chosen.erase(at);
			else if (rows[focus].selectable && single_choice())
				chosen.assign(1, id);
			else if (rows[focus].selectable && chosen.size() < max_chosen)
				chosen.insert(at, id);
			else
				return false;
			return true;
		}
		// Keeps the highlighted row in view, with its category title when that fits.
		void scroll()
		{
			const auto count = static_cast<u32>(rows.size());
			if (count <= visible_rows)
			{
				first = 0;
				return;
			}
			if (focus < first)
				first = focus;
			if (focus >= first + visible_rows)
				first = focus - visible_rows + 1;
			if (first && first == focus && rows[first - 1].header)
				--first;
			first = (std::min)(first, count - visible_rows);
		}
		// The name of the one move on, for a menu value.
		std::string chosen_name() const
		{
			if (chosen.size() != 1)
				return {};
			for (const auto& each : rows)
				if (!each.header && each.id == chosen[0])
					return each.name;
			return {};
		}
	};
} // namespace pbd::practice::move_list_view
