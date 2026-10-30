#include "stdafx.h"
#include "../Cell/pbd_rollback_session_boundary.h"

// Sample both fighters after controls; advancing dummy frames teach the resting stance, and move changes enter the capture.
void pbd::rollback_session::boundary_call::practice_read_moves(u32 index)
{
	namespace movement = pbd::practice::movement;
	data.practice.dummy.rest.reset();
	u32 p1_link = 0, p2_link = 0;
	const bool paired = data.session.in_match && read_word(0x10090040 + 0x5c, p1_link) && p1_link == 0x10090910 &&
	                    read_word(0x10090910 + 0x5c, p2_link) && p2_link == 0x10090040;
	for (u32 side = 0; side < 2; ++side)
	{
		auto& sample = data.practice.movement.moves[side];
		sample.reset();
		if (!paired)
			continue;
		const u32 actor = side ? 0x10090910 : 0x10090040;
		u32 moveset = 0, table = 0, count = 0, entry = 0, other = 0;
		if (!read_word(actor + 0x50, moveset) || !moveset || !read_word(moveset + 0x1e0, table) || !read_word(moveset + 0x1e4, count) ||
			!read_word(actor + 0xc0, entry) || !read_word(actor + 0xc4, other))
			continue;
		const auto current = movement::move_index(table, count, entry);
		if (!current)
			continue;
		// A moveset allocation is reused across characters, so its table and count are part of the identity.
		sample = practice_move_sample{*current, movement::move_index(table, count, other).value_or(0xffffffffu),
			((u64{moveset} << 32) | table) ^ (u64{count} << 16)};
	}
	const auto& dummy = data.practice.movement.moves[1];
	if (dummy && dummy->identity != data.practice.movement.rest.identity)
	{
		data.practice.movement.rest.reset(dummy->identity);
		data.practice.dummy.caps = 0;
		data.practice.dummy.facing.caps = 0;
		data.practice.dummy.facing.proven = false;
	}
	read_dummy_facing(paired);
	learn_dummy_rest(index, dummy);
	if (!data.practice.inputs.active())
		return;
	for (u32 side = 0; side < 2; ++side)
	{
		const auto& sample = data.practice.movement.moves[side];
		const auto seen = sample ? std::optional<std::pair<u32, u32>>(std::pair<u32, u32>{sample->entry, sample->other}) : std::nullopt;
		if (seen == data.practice.movement.moves_logged[side])
			continue;
		data.practice.movement.moves_logged[side] = seen;
		const auto number = [](std::optional<u32> value)
		{
			return value && *value != 0xffffffffu ? std::to_string(*value) : std::string("null");
		};
		const std::string rest = side == 0 ? std::string() : fmt::format(",\"rest\":%s%s", data.practice.dummy.rest ? json_bool(*data.practice.dummy.rest) : "null", practice_facing_json());
		data.practice.inputs.append(fmt::format("{\"type\":\"move\",\"frame\":%u,\"boundary\":%u,\"port\":%u,\"move\":%s,\"move_c4\":%s%s}\n",
			data.practice.input_frame, index, side + 1, number(seen ? std::optional<u32>(seen->first) : std::nullopt),
			number(seen ? std::optional<u32>(seen->second) : std::nullopt), rest));
	}
}

void pbd::rollback_session::boundary_call::read_dummy_facing(bool paired)
{
	namespace movement = pbd::practice::movement;
	// The dummy's facing toward P1: its yaw against the direction to P1 on the floor plane. Positions are DR's world pair
	// (+0x760 / +0x768, the one its own recovery path uses); a value that is not a sane float leaves the facing unknown.
	data.practice.dummy.facing.verdict = movement::facing::unknown;
	data.practice.dummy.facing.error.reset();
	data.practice.dummy.facing.error16.reset();
	if (paired)
	{
		const auto coordinate = [&](u32 address) -> std::optional<float>
		{
			u32 bits = 0;
			if (!read_word(address, bits))
				return std::nullopt;
			const float value = std::bit_cast<float>(bits);
			return std::isfinite(value) && std::abs(value) <= 7000000.f ? std::optional<float>(value) : std::nullopt;
		};
		const auto x = coordinate(0x10090910 + 0x760), z = coordinate(0x10090910 + 0x768);
		const auto p1_x = coordinate(0x10090040 + 0x760), p1_z = coordinate(0x10090040 + 0x768);
		u32 yaw_bits = 0;
		be_t<s16> facing16{};
		if (x && z && p1_x && p1_z)
		{
			if (read_word(0x10090910 + 0x74, yaw_bits))
				data.practice.dummy.facing.error = movement::facing_error_degrees(std::bit_cast<float>(yaw_bits), *x, *z, *p1_x, *p1_z);
			if (vm::try_access(0x10090910 + 0x7a, &facing16, sizeof(facing16), false))
				data.practice.dummy.facing.error16 = movement::facing_error_degrees(movement::facing16_yaw(static_cast<s16>(facing16)), *x, *z, *p1_x, *p1_z);
		}
		if (data.practice.dummy.facing.caps < storage::settle_distrust_caps)
			data.practice.dummy.facing.verdict = movement::facing_verdict(data.practice.dummy.facing.error ?
																			  data.practice.dummy.facing.error :
																			  data.practice.dummy.facing.error16);
	}
}

void pbd::rollback_session::boundary_call::learn_dummy_rest(u32 index, const std::optional<practice_move_sample>& dummy)
{
	namespace movement = pbd::practice::movement;
	// Neutral port input teaches only an unhurt stance. A game-driven dummy teaches after the bounded get-up wait,
	// so a knockdown held on one move entry cannot become a resting stance.
	if (data.practice.frame_advanced)
	{
		if (data.practice.dummy.hit)
		{
			data.practice.dummy.since_hit = 0;
			data.practice.dummy.recovering = true;
		}
		else if (data.practice.dummy.since_hit < movement::settle_cap_frames)
			++data.practice.dummy.since_hit;
	}
	const u32 driver = data.practice.dummy.driver;
	const bool game_driven = driver < movement::dummy_action_controller;
	const bool contact = data.practice.contact.watching && data.practice.contact.tracker.pending;
	const bool hurt = data.practice.dummy.hit || data.practice.movement.pause.active || data.practice.dummy.recovering || contact;
	const bool port_watched = driver == movement::dummy_action_controller &&
	                          ((data.practice.native.controlled && !data.practice.native.getup) || data.practice.contact.watching);
	const auto teach = game_driven ?
	                       (data.practice.dummy.since_hit >= movement::settle_cap_frames && !contact ? movement::teacher::game : movement::teacher::none) :
	                   port_watched && !hurt ? movement::teacher::port :
	                                           movement::teacher::none;
	if (data.practice.frame_advanced && (game_driven || port_watched) &&
		data.practice.movement.rest.observe(dummy ? std::optional<u32>(dummy->entry) : std::nullopt,
			game_driven || data.practice.history_words[1] == 0, teach))
		practice_dummy_log(fmt::format("{\"type\":\"dummy_rest_learned\",\"boundary\":%u,\"move\":%u,\"entries\":%u,\"game\":%s}\n",
			index, dummy->entry, static_cast<u32>(data.practice.movement.rest.count), json_bool(game_driven)));
	// Once the reading is in use, an unreadable sample is not rest. The fixed waits are for a character whose reading
	// does not work, not for a moment it misses: a thrown fighter, for one, may run moves outside its own table, and
	// ending a gate there would start the next action inside the throw. The gates' caps still bound the wait, and a
	// reading that stays unreadable caps the recovery gates until it is set aside. While the dummy is hurt a still entry
	// is not rest (movement::rest_learner::resting): lying on the floor is still too. Without a usable reading nothing
	// can show the dummy back at rest, so a hit leaves no recovery to wait for.
	if (data.practice.movement.rest.learned() && data.practice.dummy.caps < storage::settle_distrust_caps)
		data.practice.dummy.rest = dummy && data.practice.movement.rest.resting(dummy->entry, hurt);
	if (data.practice.dummy.rest != false)
		data.practice.dummy.recovering = false;
}

// The facing verdict and the yaw/16-bit facing errors toward P1, in degrees.
std::string pbd::rollback_session::boundary_call::practice_facing_json()
{
	const auto degrees = [](std::optional<float> value)
	{
		return value ? fmt::format("%.1f", static_cast<double>(*value)) : std::string("null");
	};
	const bool set_aside = data.practice.dummy.facing.caps >= storage::settle_distrust_caps;
	return fmt::format(",\"facing\":\"%s\",\"yaw_error\":%s,\"facing16_error\":%s",
		set_aside ? "set_aside" : pbd::practice::movement::facing_text(data.practice.dummy.facing.verdict),
		degrees(data.practice.dummy.facing.error), degrees(data.practice.dummy.facing.error16));
}
