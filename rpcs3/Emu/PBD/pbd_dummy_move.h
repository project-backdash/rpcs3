#pragma once

#include <algorithm>
#include <array>
#include <cstdint>
#include <optional>
#include <utility>
#include <vector>

namespace pbd::practice
{
	// Count and After apply to every impact, including each hit of a string.
	enum class contact_counted : std::uint8_t
	{
		blocks,
		hits,
		either
	};
	inline constexpr std::uint32_t min_reaction_times = 1, max_reaction_times = 5;
	inline constexpr bool counts(contact_counted counted, bool hit) noexcept
	{
		return counted == contact_counted::either || (counted == contact_counted::hits) == hit;
	}
} // namespace pbd::practice

namespace pbd::practice::dummy_move
{
	using u32 = std::uint32_t;
	enum class source
	{
		off,
		recording,
		moves
	};
	enum class contact
	{
		guard,
		hit
	};
	struct reaction
	{
		source kind = source::off;
		u32 slot = 0;
		std::vector<u32> ids;
		bool operator==(const reaction&) const = default;
	};
	struct settings
	{
		reaction after_guard, after_hit;
		bool operator==(const settings&) const = default;
		bool enabled() const
		{
			return after_guard.kind != source::off || after_hit.kind != source::off;
		}
	};
	inline thread_local settings current_settings;

	// A due reaction waits until the dummy can act.
	struct gate
	{
		contact_counted kind = contact_counted::either;
		u32 times = min_reaction_times;
		bool operator==(const gate&) const = default;
	};
	inline thread_local gate current_gate;
	// A series ends on the first frame the game says the dummy can act again. Without that reading it ends this long after
	// its last contact: a juggle's longest wait for its next hit, longer than any standing block or hit stun.
	inline constexpr u32 series_gap_frames = 40;
	// Contacts since the last boundary. Repeated callbacks for one frame cannot count the same impact twice.
	// The batch also keeps the first frame the dummy can act again after the latest contact, for the reactions that leave
	// the dummy to the game's Dummy Action until then (a movement input or a recorded Dummy Action; the moves use the
	// scheduler below). The game gives that frame in the move the hit or block put the dummy in: when the move's end
	// command hands over to standing (alias 0x8001), standing's own first-frame commands run inside that command's window,
	// so from the window's first cursor the dummy takes any input, while the move itself plays on to the window's last
	// cursor (pbd::move_list::readiness). Only then is the dummy back on its standing move, which after a block is many
	// frames later.
	struct contact_batch
	{
		std::optional<u32> previous;
		u32 hits = 0, blocks = 0;
		// The first frame the dummy can act on after the latest contact, and the frame that reading was taken on.
		std::optional<u32> earliest, read;
		void applied(u32 counter, bool guard)
		{
			if (previous == counter)
				return;
			previous = counter;
			++(guard ? blocks : hits);
			earliest.reset();
			read.reset();
		}
		// The game's reading on frame `counter`: the dummy's cursor in its move and the first cursor of that move's standing
		// end command window. A reading from the contact's own frame, or from before it, belongs to the move the contact
		// interrupted.
		void terminal(u32 counter, u32 cursor, u32 first)
		{
			if (!previous || counter <= *previous || cursor > 32767 || first > 32767)
				return;
			earliest = cursor < first ? counter + (first - cursor) : counter;
			read = counter;
		}
		// Whether the word chosen at the boundary after frame `counter`, which plays on frame counter + 1, reaches a frame
		// the dummy can act on. Only a reading of the frame that ran last counts: since an older one the dummy may have gone
		// into a move that does not end standing (a knockdown, a throw).
		bool free_next(u32 counter) const
		{
			return read == counter && earliest && counter + 1 >= *earliest;
		}
		std::array<u32, 2> take()
		{
			return {std::exchange(hits, 0), std::exchange(blocks, 0)};
		}
	};

	inline bool valid(const reaction& value)
	{
		if (value.kind == source::off)
			return value.ids.empty() && value.slot == 0;
		if (value.kind == source::recording)
			return value.ids.empty() && value.slot < 8;
		if (value.kind != source::moves || value.slot || value.ids.empty() || value.ids.size() > 64)
			return false;
		auto ordered = value.ids;
		std::sort(ordered.begin(), ordered.end());
		return ordered.back() < 4096 && std::adjacent_find(ordered.begin(), ordered.end()) == ordered.end();
	}
	struct variant_info
	{
		u32 count = 0, execution = 0;
	};
	struct choice
	{
		source kind = source::off;
		u32 id = 0, variant = 0, slot = 0;
		bool operator==(const choice&) const = default;
	};
	inline u32 random(u32& state)
	{
		u32 value = state ? state : 0x6d2b79f5;
		value ^= value << 13;
		value ^= value >> 17;
		value ^= value << 5;
		return state = value;
	}
	// Randomness belongs entirely to this local scheduler. The game's chooser and its random state are never used.
	template <class Lookup>
	std::optional<choice> choose(const reaction& selected, u32& state, Lookup lookup)
	{
		if (!valid(selected) || selected.kind == source::off)
			return {};
		if (selected.kind == source::recording)
			return choice{source::recording, 0, 0, selected.slot};
		std::vector<std::pair<u32, variant_info>> available;
		for (const auto id : selected.ids)
		{
			const std::optional<variant_info> move = lookup(id);
			// Reject the whole selection if the character catalog changed or one saved row is unavailable.
			if (!move || !move->count || move->count > 64 || !move->execution ||
				((move->execution == 2 || move->execution == 4) && move->count < 2))
				return {};
			available.emplace_back(id, *move);
		}
		const auto& [id, move] = available[random(state) % available.size()];
		const u32 variant = move.execution == 2 || move.execution == 4 ? 1 : random(state) % move.count;
		return choice{source::moves, id, variant};
	}
	class scheduler
	{
		std::optional<u32> previous_contact;
		// The contacts before the dummy recovers, the kind that made its reaction due and its first actionable frame.
		bool open = false, hit = false, due = false;
		u32 last = 0;
		std::optional<u32> actionable;
		void finish()
		{
			if (due)
				seen = 0;
			open = hit = due = false;
			actionable.reset();
		}

	public:
		u32 rng = 0x6d2b79f5;
		// Counted hits and blocks since the last reaction.
		u32 seen = 0;
		std::optional<choice> pending;
		void reset()
		{
			previous_contact.reset();
			pending.reset();
			actionable.reset();
			open = hit = due = false;
			seen = 0;
		}
		bool series_open() const
		{
			return open;
		}
		// The game reads the dummy able to act again from frame `earliest`. A reading from before the last contact belongs
		// to the stun that contact ended, not to the one it started.
		void recovered(u32 earliest)
		{
			if (open && earliest > last)
				actionable = earliest;
		}
		// Ends the recovery wait; a completed reaction starts the count again.
		void advance(u32 counter, const gate&)
		{
			if (open && (actionable ? counter >= *actionable : counter - last > series_gap_frames))
				finish();
		}
		// A contact the game applied on frame `counter`. Later contacts extend the recovery wait; they cannot cancel a
		// reaction already due, even when their kind is excluded by Count.
		template <class Lookup>
		bool applied_contact(u32 counter, contact kind, const settings& configured, Lookup lookup, const gate& when = {})
		{
			if (previous_contact == counter)
				return false;
			previous_contact = counter;
			advance(counter, when);
			open = true;
			last = counter;
			actionable.reset();
			if (counts(when.kind, kind == contact::hit))
			{
				if (!due)
					++seen;
				due = due || seen >= std::clamp(when.times, min_reaction_times, max_reaction_times);
				if (due)
					hit = kind == contact::hit;
			}
			pending.reset();
			if (due)
				pending = choose(hit ? configured.after_hit : configured.after_guard, rng, lookup);
			return pending.has_value();
		}
	};
} // namespace pbd::practice::dummy_move
