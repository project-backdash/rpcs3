#pragma once

#include <algorithm>
#include <string>
#include <string_view>
#include <array>
#include <cstdint>
#include <optional>

namespace pbd::practice::frames
{
	// Startup comes from the inclusive attack window at cursor 1. Block and hit advantage shown in Practice come from move
	// data at contact (pbd_practice_frame_calculator.h); the tracker's observed standing-neutral endpoints are logged beside them as a
	// cross-check, and its whiff recovery is what the panel shows for a move that never connected.
	using tick = std::uint32_t;
	enum class contact
	{
		unavailable,
		block,
		hit
	};
	struct event
	{
		tick counter = 0;
		std::uint32_t pc = 0, side = 0, move = 0, cursor = 0, queued = 0, state = 0, flags = 0;
		std::uint32_t window_start = 0, window_end = 0, target = 0;
	};
	// Allocation +0x44/+0x46 are a move's inclusive attack window and +0x24 its length: a move is an attack exactly when the
	// window exists. The hit level (+0x10) is no test, since jumps, dashes, sways, rolls and plain crouches carry one too.
	inline constexpr bool attack_window(std::uint32_t first, std::uint32_t last, std::uint32_t length) noexcept
	{
		return first && first <= last && last <= length;
	}
	// The thrower's half of a connected throw has hit level 0x0A00 in its low half and no attack window, so the level is
	// read directly whatever the window says.
	inline constexpr bool throw_hold(std::uint32_t level) noexcept
	{
		return (level & 0xffff) == 0x0a00;
	}

	// How a fighter stands on its first free frame (pbd::move_list::readiness): on its feet, crouching, in a stance of its
	// own moves, on the floor (from where it can roll or get up) or at the end of a stun it could break earlier.
	enum class ready_state : std::uint8_t
	{
		stand,
		crouch,
		stance,
		down,
		stun
	};
	// An input the move data offers to leave a state early (a stun break, a tech roll): the move it commits, the offset from
	// that move's own origin (its commit frame minus its commit cursor) of the frame on which the fighter is free, and the
	// last offset (of the move the escape leaves) on which that move can still be committed.
	struct escape_route
	{
		std::uint32_t target = 0, rest = 0, close = 0;
		bool operator==(const escape_route&) const = default;
	};
	// A move's readiness from move data, as cursor offsets of the move as committed: ready = origin + free, with origin the
	// commit frame minus the commit cursor.
	struct readiness_reading
	{
		std::uint32_t free = 0;
		ready_state state = ready_state::stand;
		// The fastest escape (origin + escape is its first free frame) and the move it starts with; routes has every escape.
		std::optional<std::uint32_t> escape;
		std::uint32_t escape_target = 0;
		std::array<escape_route, 4> routes{};
		std::uint32_t route_count = 0;
		// The moves the plain path enters after the committed one. They follow by themselves, so they are not cancels.
		std::array<std::uint32_t, 8> path{};
		std::uint32_t path_count = 0;
		// For a committed non-attack that is no reaction (a sway, a sidestep, a stance): the earlier offset of the frame on
		// which its own inputs free the fighter, in that stance.
		std::optional<std::uint32_t> own;
		bool operator==(const readiness_reading&) const = default;
	};
	// Which way a fighter with a choice went: not known yet, the plain branch (no input) or its alternative (the escape).
	enum class branch : std::uint8_t
	{
		none,
		primary,
		alternative
	};

	struct result
	{
		std::uint32_t move = 0, revision = 0;
		std::optional<std::uint32_t> startup;
		std::optional<std::int32_t> on_block, on_hit, on_whiff;
		contact outcome = contact::unavailable;
		std::optional<tick> impact, attacker_ready, defender_ready;
		std::optional<tick> whiff_last_active, whiff_ready;
		std::uint32_t contact_flags = 0, pending_reaction = 0, defender_state = 0;
	};

	// What a contact led to when the result is not a plain advantage: Tekken's own words for it, from the game's move data.
	enum class outcome_label : std::uint8_t
	{
		none,
		launch,
		knockdown,
		juggle,
		wall,
		throw_hit
	};
	inline constexpr std::string_view label_text(outcome_label label) noexcept
	{
		switch (label)
		{
		case outcome_label::launch: return "Launch";
		case outcome_label::knockdown: return "KND";
		case outcome_label::juggle: return "Juggle";
		case outcome_label::wall: return "Wall";
		case outcome_label::throw_hit: return "Throw";
		default: return {};
		}
	}
	// Word +0x0C of a move record holds the move's state flags; the actor keeps a copy of its current move's at +0xCC. Every
	// reaction that takes a fighter off its feet has both down bits (0x6000): lying on the floor adds bit 0x4 (the dwDWN
	// poses, and a thrown fighter's half of a throw), while falling or flying through the air leaves it clear (the sDw and
	// Dw_AIR reactions, sweeps and trips). Bit 0x01000000 marks the wall reactions (the wDm moves a hit fighter branches
	// into when it reaches a wall), and nothing else in a bank.
	inline constexpr std::uint32_t down_flags = 0x6000, lying_flag = 0x4, wall_flag = 0x01000000;
	// Hit level word +0x10: 0x0A00 in its low half is the thrower's half of a connected throw (the "_n" moves of every throw,
	// throw escape and parry), whose reaction is the thrown fighter's "_y" move.
	inline constexpr std::uint32_t throw_level = 0x0a00;
	// The hit level the game's contact check reads: each move change copies the low half of the new move's word +0x10 to the
	// actor's +0x150 (0xF1D6C), and move property 0x8074 can replace it part-way through a move (0x4AE3C). The check (0x1822C)
	// blocks an attack whose level shares a bit with the defender's guard bits at +0x152: 0x10 for a standing guard, 0x08 for
	// a crouching one. In the move banks 0x0512 is on the jabs and high kicks, 0x0217 on the mid attacks (jump attacks and
	// get-up kicks among them), 0x010F on the low kicks, 0x041F on the crouching punches and 0x0707, which has no guard bit,
	// on the unblockables. The move data's requirement checks (0x39944) count 0x031F with the high and mid levels (0x3A040)
	// and 0x041F with the lows (0x3A39C), as a special mid and a special low; both carry both guard bits. Any other level
	// without a guard bit is one no guard can stop.
	inline constexpr std::uint32_t hit_level_guards = 0x18;
	inline constexpr std::string_view hit_level_text(std::uint32_t level) noexcept
	{
		switch (level & 0xffff)
		{
		case 0x0512: return "High";
		case 0x0217: return "Mid";
		case 0x010f: return "Low";
		case 0x031f: return "Special Mid";
		case 0x041f: return "Special Low";
		case 0x0707: return "Unblockable";
		case throw_level: return "Throw";
		default: return (level & 0xffff) && !(level & hit_level_guards) ? "Unblockable" : std::string_view{};
		}
	}
	// Halfwords +0x28 and +0x2A of a reaction that falls through the air are the cursors on which the fighter lands, the
	// second one less than the first (52 and 51 for sDw_UP_01_). The launch reactions (sDw_UP) keep a fighter up until
	// frame 47 or later; knockdowns, sweeps, trips and pop-ups land by frame 43, so 45 splits them. A reaction whose pair
	// is not a fall (a crumple, a slow collapse) is a knockdown.
	inline constexpr std::uint32_t launch_landing = 45;
	// What the move data shows at the moment of contact.
	struct contact_reading
	{
		// The attacker's current move's hit level and the defender's current move's state flags (before the reaction).
		std::uint32_t attacker_level = 0, defender_flags = 0;
		// The reaction the game queued for the defender: its state flags and landing pair, and whether it ends in a stance
		// the defender can act from (pbd::move_list::actionable). Empty when the reaction is not a move of either bank.
		std::optional<std::uint32_t> reaction_flags;
		std::uint32_t reaction_landing = 0, reaction_landed = 0;
		bool reaction_plain = false;
	};
	// The label a contact gets. A blocked contact and a reaction that ends standing or crouching have a plain number and
	// none. A throw is a throw however it ends; a hit on a fighter already in the air is a juggle, and one on a fighter
	// lying on the floor keeps them down. Otherwise the reaction decides: one that ends lying on the floor is a knockdown,
	// one through the air a launch or a knockdown by how long it stays up, and a wall reaction is Wall.
	inline outcome_label classify(contact kind, const contact_reading& at) noexcept
	{
		if (kind == contact::block)
			return outcome_label::none;
		if ((at.attacker_level & 0xffff) == throw_level)
			return outcome_label::throw_hit;
		if ((at.defender_flags & down_flags) == down_flags)
			return at.defender_flags & lying_flag ? outcome_label::knockdown : outcome_label::juggle;
		if (!at.reaction_flags)
			return outcome_label::none;
		if (*at.reaction_flags & wall_flag)
			return outcome_label::wall;
		if (at.reaction_plain || (*at.reaction_flags & down_flags) != down_flags)
			return outcome_label::none;
		if (*at.reaction_flags & lying_flag)
			return outcome_label::knockdown;
		const bool falls = at.reaction_landed + 1 == at.reaction_landing;
		return falls && at.reaction_landing >= launch_landing ? outcome_label::launch : outcome_label::knockdown;
	}

	// Frame advantage of a fighter's latest attack, calculated the moment its contact starts the opponent's reaction.
	struct calculated
	{
		std::uint32_t move = 0, reaction = 0, revision = 0;
		// Whether the move has connected at all, and how its latest contact landed (unavailable: a juggle, a knockdown
		// state or an unread contact kind).
		bool contacted = false;
		contact outcome = contact::unavailable;
		// The frame the game applied that latest contact on.
		std::optional<std::int64_t> impact;
		std::optional<std::int32_t> advantage;
		// What the latest contact led to (classify); the panel shows it when there is no advantage. A wall reaction later in
		// the same reaction makes it Wall and clears the advantage.
		outcome_label label = outcome_label::none;
		// The hit level of the latest contact as the contact check read it (hit_level_text), or throw_level once a throw caught.
		std::uint32_t level = 0;
		std::optional<std::int64_t> attacker_ready, defender_ready;
		// The attack's first free frame as it stands now (a cancel moves it) and its last active frame; once the active
		// frames pass without a contact, whiff is the recovery from that last active frame to the first free one.
		std::optional<std::int64_t> ready, last_active;
		std::optional<std::int32_t> whiff;
		// The startup of the string's or multi-hit move's first hit.
		std::optional<std::uint32_t> startup;
		// Both fighters' readiness: how each stands once free, and the first free frame through the escape its move data
		// offers (a tech roll, a stun break) beside the plain one. alt and state are the attack's own, as ready is.
		std::optional<std::int64_t> attacker_alt, defender_alt, alt;
		ready_state attacker_state = ready_state::stand, defender_state = ready_state::stand, state = ready_state::stand;
		// The advantage through the alternative of the fighter with the choice: the defender's when it is stunned or down,
		// otherwise the attacker's when it is down. taken is the branch that fighter took; the primary advantage stays.
		std::optional<std::int32_t> alternative, whiff_alternative;
		bool defender_chooses = false;
		branch taken = branch::none;
	};

	// Allocation +44/+46 are the inclusive attack window, not animation length or recovery.
	inline std::optional<std::uint32_t> startup(std::uint32_t first, std::uint32_t last, std::uint32_t length, bool attack)
	{
		if (!attack || !first || first > last || last > length || length > 32767)
			return {};
		return first;
	}

	// The Frame Advantage value: the signed plain number, the word for how the fighter with the choice stands, and the
	// alternative in brackets: "+16 Crumple (-14)", "+15 Down (+9)", "-5 FC", "+0". It never runs past
	// advantage_text_limit characters (the word goes first, then the alternative). marked_at and marked_size cover the
	// number of the branch that was taken, drawn in the highlight colour; logged is the text with "*" after that number.
	struct advantage_display
	{
		std::string text, logged;
		std::size_t marked_at = 0, marked_size = 0;
	};
	inline constexpr std::size_t advantage_text_limit = 18;
	inline std::string signed_frames(std::int32_t value)
	{
		return (value >= 0 ? "+" : "") + std::to_string(value);
	}
	inline advantage_display advantage_text(std::int32_t primary, std::string_view tag, std::optional<std::int32_t> alternative, branch taken)
	{
		const auto compose = [&](bool with_tag, bool with_alternative)
		{
			advantage_display out;
			out.text = signed_frames(primary);
			if (taken == branch::primary)
				out.marked_size = out.text.size();
			if (with_tag && !tag.empty())
			{
				out.text += ' ';
				out.text += tag;
			}
			if (with_alternative && alternative)
			{
				out.text += " (";
				const std::size_t at = out.text.size();
				out.text += signed_frames(*alternative);
				if (taken == branch::alternative)
				{
					out.marked_at = at;
					out.marked_size = out.text.size() - at;
				}
				out.text += ')';
			}
			out.logged = out.text;
			if (out.marked_size)
				out.logged.insert(out.marked_at + out.marked_size, "*");
			return out;
		};
		auto out = compose(true, true);
		if (out.text.size() > advantage_text_limit)
			out = compose(false, true);
		if (out.text.size() > advantage_text_limit)
			out = compose(false, false);
		return out;
	}
	// The word for a state. The defender's is shown when it has the choice (Crumple: the alternative is its fastest stun
	// break; KND: its tech); the attacker's otherwise (Down: the alternative is its own tech; FC; Stance, until the data
	// gives stances their names).
	inline std::string_view readiness_tag(ready_state state, bool defender) noexcept
	{
		switch (state)
		{
		case ready_state::crouch: return defender ? std::string_view{} : std::string_view{"FC"};
		case ready_state::stance: return defender ? std::string_view{} : std::string_view{"Stance"};
		case ready_state::down: return defender ? "KND" : "Down";
		case ready_state::stun: return defender ? "Crumple" : "Down";
		default: return {};
		}
	}
	// A contact's result. A `label` for what the contact led to (a launch, a knockdown, a juggle) replaces the word only;
	// both numbers stay.
	inline advantage_display advantage_text(const calculated& out, std::string_view label = {})
	{
		if (!out.advantage)
			return {};
		const std::string_view tag = !label.empty()       ? label :
		                             out.defender_chooses ? readiness_tag(out.defender_state, true) :
		                                                    readiness_tag(out.attacker_state, false);
		return advantage_text(*out.advantage, tag, out.alternative, out.taken);
	}
	// A whiff's recovery relative to the opponent, with the attack's own alternative (a tech after a landing) beside it.
	inline advantage_display whiff_text(const calculated& out)
	{
		if (!out.whiff)
			return {};
		return advantage_text(*out.whiff, readiness_tag(out.state, false), out.whiff_alternative, out.taken);
	}

} // namespace pbd::practice::frames
