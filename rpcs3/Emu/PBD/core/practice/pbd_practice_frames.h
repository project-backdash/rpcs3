#pragma once

#include "pbd_practice_frame_calculator.h"

namespace pbd::practice::frames
{
	// Events come from the stock transition, terminal command and applied-contact instructions. No elapsed wall time,
	// learned resting pose or animation duration is an action-ready observation.
	class tracker
	{
		struct episode
		{
			bool active = false, reaction_seen = false;
			std::array<std::uint32_t, 2> moves{};
			tick impact = 0;
		};
		std::array<episode, 2> episodes{};
		std::array<std::uint32_t, 2> current{};
		std::array<bool, 2> attacking{};
		std::array<bool, 2> rejected{};
		struct whiff_episode
		{
			bool active = false;
			tick started = 0;
			std::uint32_t last_active = 0;
		};
		std::array<whiff_episode, 2> whiffs{};
		std::optional<tick> clock;
		std::uint32_t revision = 0;

		void invalidate(unsigned attacker)
		{
			rejected[attacker] = true;
			whiffs[attacker] = {};
			episodes[attacker] = {};
			auto& out = latest[attacker];
			out.on_block.reset();
			out.on_hit.reset();
			out.on_whiff.reset();
			out.whiff_last_active.reset();
			out.whiff_ready.reset();
			out.attacker_ready.reset();
			out.defender_ready.reset();
			out.impact.reset();
			out.outcome = contact::unavailable;
			out.revision = ++revision;
		}

	public:
		std::array<result, 2> latest{};
		void reset()
		{
			episodes = {};
			current = {};
			attacking = {};
			rejected = {};
			whiffs = {};
			latest = {};
			clock.reset();
			for (auto& out : latest)
				out.revision = ++revision;
		}
		void boundary(tick frame)
		{
			if (clock && frame != *clock && frame != *clock + 1)
				reset();
			clock = frame;
			for (unsigned side = 0; side < 2; ++side)
				if (episodes[side].active && frame - episodes[side].impact > 600)
					invalidate(side);
		}
		void commit(unsigned side, std::uint32_t move, std::optional<std::uint32_t> attack_startup, bool attack = false,
			std::optional<std::uint32_t> fixed_last_active = {})
		{
			if (side > 1 || !clock)
				return;
			for (unsigned attacker = 0; attacker < 2; ++attacker)
			{
				auto& episode = episodes[attacker];
				if (!episode.active)
					continue;
				const auto& ready = side == attacker ? latest[attacker].attacker_ready : latest[attacker].defender_ready;
				if (side != attacker && !episode.reaction_seen && *clock - episode.impact <= 1 &&
					!attack_startup && !attack && move != current[side] && (!episode.moves[side] || episode.moves[side] == move))
				{
					episode.moves[side] = move;
					episode.reaction_seen = true;
				}
				else if (!ready)
					invalidate(attacker);
			}
			current[side] = move;
			whiffs[side] = {};
			attacking[side] = attack_startup.has_value() || attack;
			if (attack_startup || attack)
			{
				invalidate(side);
				rejected[side] = false;
				latest[side] = {move, ++revision, attack_startup};
				if (attack_startup && fixed_last_active && *fixed_last_active >= *attack_startup && *fixed_last_active <= 32767)
					whiffs[side] = {true, *clock, *fixed_last_active};
			}
		}
		void applied_contact(unsigned attacker, std::uint32_t reaction, contact kind)
		{
			if (attacker > 1 || !clock)
				return;
			whiffs[attacker] = {};
			auto& out = latest[attacker];
			// Several contacts before recovery, trades and an unrecognized source cannot share one advantage number.
			if (rejected[attacker] || episodes[attacker].active || episodes[1 - attacker].active || !out.startup ||
				out.move != current[attacker] || attacking[1 - attacker] || kind == contact::unavailable)
			{
				invalidate(attacker);
				invalidate(1 - attacker);
				return;
			}
			out.on_block.reset();
			out.on_hit.reset();
			out.on_whiff.reset();
			out.whiff_last_active.reset();
			out.whiff_ready.reset();
			out.attacker_ready.reset();
			out.defender_ready.reset();
			out.outcome = kind;
			out.impact = *clock;
			out.revision = ++revision;
			auto& episode = episodes[attacker];
			episode = {true, false, current, *clock};
			episode.moves[1 - attacker] = reaction;
		}
		void actionable(unsigned side, std::uint32_t move, std::optional<std::uint32_t> cursor = {})
		{
			if (side > 1 || !clock)
				return;
			auto& whiff = whiffs[side];
			if (whiff.active && current[side] == move && cursor && *cursor >= whiff.last_active &&
				*clock - whiff.started == *cursor - 1 && *clock - whiff.started < 600)
			{
				auto& out = latest[side];
				// The qualified standing command window accepts the next action for the following stock update.
				// Count from the final active frame through that first free frame; an idle animation's end is irrelevant.
				out.whiff_last_active = whiff.started + whiff.last_active - 1;
				out.whiff_ready = *clock + 1;
				out.on_whiff = -static_cast<std::int32_t>(*out.whiff_ready - *out.whiff_last_active);
				out.revision = ++revision;
				whiff.active = false;
			}
			for (unsigned attacker = 0; attacker < 2; ++attacker)
			{
				auto& episode = episodes[attacker];
				if (!episode.active || !episode.reaction_seen || *clock <= episode.impact || episode.moves[side] != move)
					continue;
				auto& out = latest[attacker];
				auto& ready = side == attacker ? out.attacker_ready : out.defender_ready;
				if (!ready)
				{
					ready = *clock;
					out.revision = ++revision;
				}
				if (!out.attacker_ready || !out.defender_ready)
					continue;
				const auto advantage = static_cast<std::int32_t>(static_cast<std::int64_t>(*out.defender_ready) - *out.attacker_ready);
				if (out.outcome == contact::block)
					out.on_block = advantage;
				else if (out.outcome == contact::hit)
					out.on_hit = advantage;
				out.revision = ++revision;
				episode.active = false;
			}
		}
		void reject_recovery(unsigned side, std::uint32_t move)
		{
			if (side > 1)
				return;
			if (current[side] == move)
			{
				whiffs[side] = {};
				auto& out = latest[side];
				if (out.on_whiff)
				{
					out.on_whiff.reset();
					out.whiff_last_active.reset();
					out.whiff_ready.reset();
					out.revision = ++revision;
				}
			}
			for (unsigned attacker = 0; attacker < 2; ++attacker)
				if (episodes[attacker].active && episodes[attacker].moves[side] == move)
					invalidate(attacker);
		}
	};
} // namespace pbd::practice::frames
