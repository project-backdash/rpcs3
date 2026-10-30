#pragma once

#include "pbd_practice_frame_model.h"

namespace pbd::practice::frames
{
	// A stock commit at cursor c on frame t reaches cursor n on frame t + n - c. Advantage compares the move-data
	// readiness of both fighters at each contact; the queued reaction can commit up to two frames after contact.
	class calculator
	{
		struct motion
		{
			bool known = false;
			std::uint32_t move = 0;
			// The frame on which the move's cursor would have read 0.
			std::int64_t origin = 0;
		};
		struct waiting
		{
			bool active = false;
			tick at = 0;
		};
		static std::optional<std::int32_t> whiff_recovery(const calculated& out)
		{
			if (!out.ready || !out.last_active)
				return {};
			return static_cast<std::int32_t>(*out.last_active - (*out.ready + 1));
		}
		// A fighter's latest reading in absolute frames, from its latest attack, reaction, cancel or contact. close is the last
		// frame on which one of its escapes can still be committed; once it passes, the fighter took the plain branch.
		struct standing
		{
			bool valid = false, settled = false;
			std::int64_t close = 0;
			std::optional<std::int64_t> ready, alt;
			ready_state state = ready_state::stand;
			readiness_reading data;
		};
		static std::optional<std::int64_t> absolute(std::int64_t origin, std::optional<std::uint32_t> offset)
		{
			return offset ? std::optional<std::int64_t>(origin + *offset) : std::nullopt;
		}
		// The states in which the fighter decides between the plain branch and an escape.
		static bool chooses(ready_state state)
		{
			return state == ready_state::down || state == ready_state::stun;
		}
		// Primary advantage with neither fighter doing anything, and the alternative of the fighter with the choice, both as
		// the defender's free frame minus the attacker's.
		static void evaluate(calculated& out)
		{
			out.advantage.reset();
			out.alternative.reset();
			if (out.attacker_ready && out.defender_ready)
				out.advantage = static_cast<std::int32_t>(*out.defender_ready - *out.attacker_ready);
			out.defender_chooses = out.defender_ready && chooses(out.defender_state);
			if (out.defender_chooses)
			{
				if (out.defender_alt && out.attacker_ready)
					out.alternative = static_cast<std::int32_t>(*out.defender_alt - *out.attacker_ready);
			}
			else if (chooses(out.attacker_state) && out.attacker_alt && out.defender_ready)
				out.alternative = static_cast<std::int32_t>(*out.defender_ready - *out.attacker_alt);
		}
		static std::optional<std::int32_t> whiff_through_alternative(const calculated& out)
		{
			if (!out.alt || !out.last_active || !chooses(out.state))
				return {};
			return static_cast<std::int32_t>(*out.last_active - (*out.alt + 1));
		}
		void remember(unsigned side, std::int64_t origin, const std::optional<readiness_reading>& reading)
		{
			auto& mine = readings[side];
			mine = {};
			if (!reading)
				return;
			mine.valid = true;
			mine.data = *reading;
			mine.ready = origin + reading->free;
			mine.state = reading->state;
			// Only a fighter that is down or stunned has a choice. Elsewhere an escape's input (a sidestep or sway out of a
			// recovery) is a cancel like any other.
			if (!chooses(reading->state) || !reading->escape)
				return;
			mine.alt = absolute(origin, reading->escape);
			std::uint32_t last = 0;
			for (std::uint32_t index = 0; index < reading->route_count && index < reading->routes.size(); ++index)
				last = (std::max)(last, reading->routes[index].close);
			mine.close = origin + last;
		}
		// `side` took a branch of its latest reading: the plain one, or the escape that leaves it free on frame `realized`.
		void settle(unsigned side, branch taken, std::optional<std::int64_t> realized = {})
		{
			auto& mine = readings[side];
			mine.settled = true;
			if (taken == branch::alternative)
				mine.alt = realized;
			if (defends[side])
			{
				auto& out = latest[1 - side];
				if (taken == branch::alternative)
					out.defender_alt = realized;
				evaluate(out);
				if (out.defender_chooses)
					out.taken = taken;
				out.revision = ++revision;
			}
			if (drives[side])
			{
				auto& out = latest[side];
				if (taken == branch::alternative)
				{
					out.alt = realized;
					if (out.attacker_ready)
						out.attacker_alt = realized;
				}
				evaluate(out);
				out.whiff_alternative = out.whiff ? whiff_through_alternative(out) : std::nullopt;
				if (!out.defender_chooses && chooses(out.defender_ready ? out.attacker_state : out.state))
					out.taken = taken;
				out.revision = ++revision;
			}
		}
		// The side was free on `frame`, earlier than its reading said (it attacked out of a stance before that).
		void observed_free(unsigned side, std::int64_t frame)
		{
			readings[side].ready = frame;
			if (drives[side])
			{
				auto& out = latest[side];
				out.ready = frame;
				if (out.defender_ready)
					out.attacker_ready = frame;
				evaluate(out);
				if (out.whiff)
					out.whiff = whiff_recovery(out);
				out.revision = ++revision;
			}
			if (defends[side])
			{
				auto& out = latest[1 - side];
				out.defender_ready = frame;
				evaluate(out);
				out.revision = ++revision;
			}
		}
		std::array<standing, 2> readings{};
		// Whether latest[side]'s attacker frames follow readings[side], whether latest[1 - side]'s defender frames do, and
		// whether the side's latest commit was an attack.
		std::array<bool, 2> drives{}, defends{}, attacking{};
		std::array<motion, 2> motions{};
		// Indexed by attacker: a contact whose reaction has not started yet.
		std::array<waiting, 2> pending{};
		// Indexed by fighter: hit since its own last attack, so a wall reaction it enters belongs to the opponent's result.
		std::array<bool, 2> struck{};
		struct queued_cancel
		{
			std::uint32_t source = 0, target = 0, cursor = 1, revision = 0;
			tick commits = 0;
			readiness_reading data;
		};
		std::array<std::optional<queued_cancel>, 2> previews{};
		std::array<calculated, 2> shown{};
		std::uint32_t revision = 0;

		struct move_commit
		{
			unsigned side;
			std::uint32_t move, cursor;
			std::int64_t at, origin;
			std::optional<readiness_reading> reading;
			std::optional<std::int64_t> ready, alt;
			ready_state state;
		};

		bool choose_reading(const move_commit& c, bool attack, bool after_attack)
		{
			// The choice of the side's latest reading: one of its escapes committed in time (its free frame counts from this
			// commit), or the plain branch once that time is over.
			bool chose = false;
			if (const auto& mine = readings[c.side]; mine.valid && !mine.settled && mine.alt)
			{
				if (c.at <= mine.close)
					for (std::uint32_t index = 0; index < mine.data.route_count && index < mine.data.routes.size(); ++index)
						if (mine.data.routes[index].target == c.move)
						{
							settle(c.side, branch::alternative, c.origin + mine.data.routes[index].rest);
							chose = true;
							break;
						}
				if (!chose && c.at > mine.close)
					settle(c.side, branch::primary);
			}
			// An attack out of a stance before the frame the move data gave: the stance let the fighter act on the frame before.
			if (const auto& mine = readings[c.side]; attack && !after_attack && mine.valid && mine.state == ready_state::stance && mine.ready && c.at <= *mine.ready)
				observed_free(c.side, c.at - 1);
			return chose;
		}

		bool apply_reaction(const move_commit& c, bool reaction)
		{
			bool reacted = false;
			if (auto& wait = pending[1 - c.side]; wait.active)
			{
				wait.active = false;
				auto& out = latest[1 - c.side];
				if (reaction)
				{
					reacted = true;
					out.reaction = c.move;
					out.attacker_ready = out.ready;
					out.attacker_alt = out.alt;
					out.attacker_state = out.state;
					out.defender_ready = c.ready;
					out.defender_alt = c.alt;
					out.defender_state = c.state;
					out.taken = branch::none;
					evaluate(out);
					out.revision = ++revision;
					remember(c.side, c.origin, c.reading);
					defends[c.side] = true;
					drives[c.side] = false;
				}
			}
			return reacted;
		}

		void apply_wall(unsigned side)
		{
			auto& out = latest[1 - side];
			if (out.move && out.contacted && (out.label != outcome_label::wall || out.advantage))
			{
				out.label = outcome_label::wall;
				out.advantage.reset();
				out.defender_ready.reset();
				out.revision = ++revision;
			}
		}

		void start_attack(const move_commit& c, std::optional<std::uint32_t> last_active, std::optional<std::uint32_t> startup)
		{
			auto& own = latest[c.side];
			pending[c.side] = {};
			struck[c.side] = false;
			// An attack entered before the previous one can act again is the same string or multi-hit move (a follow-up
			// input or a hit the move hands over to by itself): it keeps the first hit's startup and shows the latest
			// hit's result.
			const bool continues = own.move && own.ready && c.at < *own.ready;
			const auto first = continues ? own.startup : startup;
			own = {c.move, 0, ++revision};
			own.startup = first;
			own.ready = c.ready;
			own.alt = c.alt;
			own.state = c.state;
			if (last_active && *last_active >= c.cursor)
				own.last_active = c.origin + *last_active;
			remember(c.side, c.origin, c.reading);
			drives[c.side] = true;
			// The new result no longer follows the opponent's reading, whose choice belonged to the result replaced.
			defends[c.side] = defends[1 - c.side] = false;
		}

		void continue_move(const move_commit& c, bool reacted, bool chose)
		{
			auto& own = latest[c.side];
			if (reacted || chose)
				return;
			// The plain path's own next moves (a landing, a stance's loop) follow by themselves and are not cancels.
			if (const auto& mine = readings[c.side]; mine.valid)
				for (std::uint32_t index = 0; index < mine.data.path_count && index < mine.data.path.size(); ++index)
					if (mine.data.path[index] == c.move)
						return;
			// A move the attacker enters before its attack lets it act again, by an input (a cancel into a sway, a stance, a
			// backdash or sidestep) or by the recovery's own next step: the fighter can now act when the new move lets it, so
			// the shown advantage and whiff recovery follow it, and a late cancel gives a worse number. Moves from the first
			// free frame on are new actions and leave the result alone.
			if (!own.move || !own.ready || c.at >= *own.ready)
				return;
			own.ready = c.ready;
			own.alt = c.alt;
			own.state = c.state;
			remember(c.side, c.origin, c.reading);
			drives[c.side] = true;
			defends[c.side] = false;
			if (own.defender_ready)
			{
				own.attacker_ready = c.ready;
				own.attacker_alt = c.alt;
				own.attacker_state = c.state;
				evaluate(own);
				if (!own.defender_chooses)
					own.taken = branch::none;
			}
			if (own.whiff)
			{
				own.whiff = whiff_recovery(own);
				own.whiff_alternative = whiff_through_alternative(own);
				own.taken = branch::none;
			}
			own.revision = ++revision;
		}

	public:
		static constexpr tick reaction_delay = 2;
		std::array<calculated, 2> latest{};

		void reset()
		{
			motions = {};
			pending = {};
			struck = {};
			latest = {};
			previews = {};
			shown = {};
			for (auto& out : latest)
				out.revision = ++revision;
			readings = {};
			drives = {};
			defends = {};
			attacking = {};
		}
		void clear_preview(unsigned side)
		{
			if (side > 1 || !previews[side])
				return;
			previews[side].reset();
			latest[side].revision = ++revision;
		}
		// Only a move actually accepted into the game's queue is previewed. The committed readings stay intact until its
		// commit arrives, so a replaced queue, an interruption or a restart can discard the preview without restoring data.
		void preview_cancel(unsigned side, std::uint32_t source, std::uint32_t target, tick now, tick commits,
			std::uint32_t cursor, const std::optional<readiness_reading>& data)
		{
			if (side > 1)
				return;
			const auto& own = latest[side];
			if (!data || !target || target == source || !cursor || !motions[side].known || motions[side].move != source ||
				!drives[side] || struck[side] || !own.move || !own.ready || commits < now || static_cast<std::int64_t>(commits) >= *own.ready)
			{
				clear_preview(side);
				return;
			}
			const auto& prior = previews[side];
			if (prior && prior->source == source && prior->target == target && prior->commits == commits && prior->cursor == cursor && prior->data == *data)
				return;
			previews[side] = queued_cancel{source, target, cursor, ++revision, commits, *data};
		}
		void check_queue(unsigned side, std::uint32_t source, std::uint32_t target)
		{
			if (side < 2 && previews[side] && (previews[side]->source != source || previews[side]->target != target))
				clear_preview(side);
		}
		const std::array<calculated, 2>& displayed()
		{
			shown = latest;
			for (unsigned side = 0; side < 2; ++side)
				if (const auto& queued = previews[side])
				{
					auto projected = *this;
					projected.previews = {};
					projected.commit_reading(side, queued->target, queued->commits, queued->cursor, queued->data, false);
					shown[side] = projected.latest[side];
					shown[side].revision = (std::max)(latest[side].revision, queued->revision);
				}
			return shown;
		}
		// Whether a commit of `side` on frame `now` is the reaction to the opponent's latest contact.
		bool reacting(unsigned side, tick now) const
		{
			if (side > 1)
				return false;
			const auto& wait = pending[1 - side];
			return wait.active && now >= wait.at && now - wait.at <= reaction_delay;
		}
		// A fighter commits `move` at `cursor` on frame `now`; its move-data readiness gives ready = origin + free,
		// its state and escapes. An attack supplies its last active cursor and startup; `wall` marks a wall reaction.
		void commit_reading(unsigned side, std::uint32_t move, tick now, std::uint32_t cursor, const std::optional<readiness_reading>& data,
			bool attack, std::optional<std::uint32_t> last_active = {}, std::optional<std::uint32_t> startup = {}, bool wall = false)
		{
			if (side > 1)
				return;
			clear_preview(side);
			const std::int64_t at = now, origin = at - cursor;
			const bool reaction = reacting(side, now);
			// A non-attack the fighter entered by itself (a cancel into a sway, a sidestep or a stance) frees it where its own
			// inputs open; a reaction's inputs do not.
			std::optional<readiness_reading> reading = data;
			if (reading && !reaction && reading->own)
			{
				reading->free = *reading->own;
				reading->state = ready_state::stance;
			}
			motions[side] = {true, move, origin};
			const std::optional<std::int64_t> ready = reading ? std::optional<std::int64_t>(origin + reading->free) : std::nullopt;
			const auto alt = reading ? absolute(origin, reading->escape) : std::nullopt;
			const auto state = reading ? reading->state : ready_state::stand;
			const bool after_attack = attacking[side];
			attacking[side] = attack;
			const move_commit c{side, move, cursor, at, origin, reading, ready, alt, state};
			const bool chose = choose_reading(c, attack, after_attack);
			const bool reacted = apply_reaction(c, reaction);
			if (wall && struck[side])
				apply_wall(side);
			if (attack)
			{
				start_attack(c, last_active, startup);
				return;
			}
			continue_move(c, reacted, chose);
		}
		// `side` entered the thrower's half of a connected throw (a move of throw_level): the game enters it only once the
		// throw has caught, so the result is a throw whatever the contact reads.
		void throw_connected(unsigned side)
		{
			if (side > 1 || !latest[side].move)
				return;
			auto& own = latest[side];
			own.contacted = true;
			own.label = outcome_label::throw_hit;
			own.level = throw_level;
			own.whiff.reset();
			struck[1 - side] = true;
			own.revision = ++revision;
		}
		// The start of frame `now`: an attack whose active frames ended without a contact shows its whiff recovery, and a
		// reading whose escapes can no longer be committed took its plain branch.
		void boundary(tick now)
		{
			for (unsigned side = 0; side < 2; ++side)
				if (previews[side] && now > previews[side]->commits && now - previews[side]->commits > 1)
					clear_preview(side);
			for (auto& out : latest)
			{
				if (!out.move || out.contacted || out.whiff || !out.last_active || static_cast<std::int64_t>(now) <= *out.last_active)
					continue;
				out.whiff = whiff_recovery(out);
				out.whiff_alternative = whiff_through_alternative(out);
				if (out.whiff)
					out.revision = ++revision;
			}
			for (unsigned side = 0; side < 2; ++side)
				if (const auto& mine = readings[side]; mine.valid && !mine.settled && mine.alt && static_cast<std::int64_t>(now) > mine.close)
					settle(side, branch::primary);
		}
		// The game applied a contact of `attacker`'s move to the other fighter on frame `now`; `label` is what it led to
		// (classify) and `level` the hit level the contact check read (hit_level_text).
		void applied_contact(unsigned attacker, contact kind, tick now, outcome_label label = outcome_label::none, std::uint32_t level = 0)
		{
			if (attacker > 1)
				return;
			clear_preview(1 - attacker);
			auto& out = latest[attacker];
			const auto& own = motions[attacker];
			out.contacted = true;
			out.outcome = kind;
			out.impact = now;
			out.label = label;
			out.level = level;
			struck[1 - attacker] = true;
			out.reaction = 0;
			out.advantage.reset();
			out.attacker_ready.reset();
			out.defender_ready.reset();
			out.whiff.reset();
			out.revision = ++revision;
			pending[attacker] = {};
			out.attacker_alt.reset();
			out.defender_alt.reset();
			out.alternative.reset();
			out.whiff_alternative.reset();
			out.attacker_state = out.defender_state = ready_state::stand;
			out.defender_chooses = false;
			out.taken = branch::none;
			// The fighter it hit was interrupted: its own attack neither whiffed nor follows the reaction as a cancel.
			auto& interrupted = latest[1 - attacker];
			if (interrupted.move && !interrupted.contacted && !interrupted.whiff)
			{
				interrupted.ready.reset();
				interrupted.last_active.reset();
				interrupted.alt.reset();
			}
			drives[1 - attacker] = false;
			// The attacker must still be in the attack the panel shows; a contact from anything else has no number.
			if (kind == contact::unavailable || !own.known || own.move != out.move)
				return;
			pending[attacker] = {true, now};
		}
		// The attacker's move read again on the contact frame from its cursor then, now that the contact kind is known, so
		// recovery that depends on the outcome counts. It replaces the frames read when the move was committed; the move's
		// origin stays the one its commit gave.
		void reread(unsigned attacker, std::uint32_t move, const std::optional<readiness_reading>& reading)
		{
			if (attacker > 1 || !reading)
				return;
			auto& own = latest[attacker];
			if (!motions[attacker].known || motions[attacker].move != move || own.move != move)
				return;
			const std::int64_t origin = motions[attacker].origin;
			own.ready = origin + reading->free;
			own.alt = absolute(origin, reading->escape);
			own.state = reading->state;
			remember(attacker, origin, reading);
			drives[attacker] = true;
			own.revision = ++revision;
		}
	};
} // namespace pbd::practice::frames
