#include "stdafx.h"
#include "../Cell/pbd_rollback_session_boundary.h"
#include "pbd_practice_lesson_channel.h"
#include "pbd_dummy_move_runtime.h"

// The contact check reads the Practice object's Counter Hit option at +0x30; only replace a valid Off/On value.
void pbd::rollback_session::boundary_call::practice_counter_hit_write(bool on)
{
	u32 singleton = 0, mode = 0, current = umax;
	if (!read_word(0x10052a50, singleton) || singleton != 0x1008ebf4 || !read_word(pbd::t5dr::game_mode_address, mode) || mode != 10 ||
		!read_word(singleton + 0x30, current) || current > 1 || (current == 1) == on)
		return;
	be_t<u32> value = on ? 1u : 0u;
	vm::try_access(singleton + 0x30, &value, sizeof(value), true);
}

// Guard and Counter Hit use applied contacts and a host RNG independent of game and playback random numbers.
void pbd::rollback_session::boundary_call::practice_guard_frame(const std::array<u32, 2>& contacts, bool hit)
{
	if (!data.practice.dummy.enabled)
		return;
	if (data.practice.frame_advanced &&
		(data.practice.reactions.guard.option != pbd::practice::pbd_guard_option::game || data.practice.reactions.counter_hit.random))
	{
		const auto floor = pbd::practice::native::read_wakeup();
		const pbd::practice::pbd_guard_frame now{contacts[0] != 0 || hit, contacts[1] != 0,
			floor ? std::optional<bool>(floor->standing) : std::nullopt};
		const auto draw = [&]
		{
			return pbd::practice::dummy_move::random(data.practice.reactions.guard_rng);
		};
		data.practice.reactions.guard.frame(now, draw);
		data.practice.reactions.counter_hit.frame(now, draw);
	}
	// At every boundary, so a value the game wrote while paused (the saved options' restore) is put right before the next frame.
	if (data.practice.reactions.counter_hit.random && data.practice.reactions.counter_hit.value)
		practice_counter_hit_write(*data.practice.reactions.counter_hit.value);
}

void pbd::rollback_session::boundary_call::practice_getup_guard()
{
	u32 singleton = 0, action = umax;
	if (data.practice.dummy.state == dummy_state::recording || data.practice.dummy.state == dummy_state::ready ||
		!read_word(0x10052a50, singleton) || singleton != 0x1008ebf4 || !read_word(singleton + 0x6c, action))
		action = umax;
	else
		action = pbd::practice::pbd_guard_action(data.practice.native.controlled ? data.practice.native.action[0] : action,
			data.practice.reactions.guard);
	pbd::practice::native::getup_guard(action);
}

// The Practice object's +0x6C action and +0x70 saved Freestyle value must agree while this route drives the dummy.
u32 pbd::rollback_session::boundary_call::practice_dummy_route(u32 index)
{
	if (!data.practice.dummy.enabled)
		return umax;
	// A rejected clock reader stops input ownership, but the independent route guards can still restore our settings.
	const bool driven = data.practice.native.pause_valid &&
	                    ((data.practice.reactions.wakeup_control.armed && data.practice.reactions.wakeup_control.engaged) || data.practice.reactions.throw_escape_control.owns() || pbd::practice::dummy_move::controls_dummy() || pbd::practice::lesson::controls_dummy() || data.practice.dummy.state == dummy_state::recording || data.practice.dummy.state == dummy_state::playing ||
							data.practice.dummy.state == dummy_state::ready);
	const bool getup = data.practice.dummy.state == dummy_state::playing && !data.practice.dummy.preview &&
	                   data.practice.movement.pause.active;
	u32 singleton = 0, mode = 0, page = 0;
	u32 p1_link = 0, p2_link = 0;
	be_t<u16> p1_port{}, p2_port{};
	std::array<be_t<u32>, 2> actions{};
	const bool native = read_word(0x10052a50, singleton) && singleton == 0x1008ebf4 &&
	                    read_word(pbd::t5dr::game_mode_address, mode) && mode == 10 &&
	                    read_word(singleton, page) &&
	                    read_word(0x10090040 + 0x5c, p1_link) && p1_link == 0x10090910 &&
	                    read_word(0x10090910 + 0x5c, p2_link) && p2_link == 0x10090040 &&
	                    vm::try_access(0x10090040 + 0x3c, &p1_port, sizeof(p1_port), false) && p1_port == 0u &&
	                    vm::try_access(0x10090910 + 0x3c, &p2_port, sizeof(p2_port), false) && p2_port == 1u &&
	                    vm::try_access(singleton + 0x6c, actions.data(), sizeof(actions), false);
	// Standing in place of the player's Guard All while Guard After First Hit or Random Guard lifts its guard
	// (pbd::practice::pbd_guard_action): only in the fight and while nothing else drives the dummy. With the menu open or the
	// game paused the player's own action is back in place, so the menu shows that one and its changes commit over it.
	const u32 chosen = data.practice.native.controlled && actions[0] == data.practice.native.written[0] ? data.practice.native.action[0] : u32{actions[0]};
	const bool lifted = !driven && native && page == 0 && actions[0] <= 10u && actions[1] <= 10u && data.practice.native.pause_valid &&
	                    !data.practice.native.paused && !data.practice.menu.open && data.practice.view.fight_live &&
	                    pbd::practice::pbd_guard_action(chosen, data.practice.reactions.guard) != chosen;
	const bool controlled = driven || lifted;
	if (!native)
	{
		data.practice.native.controlled = false;
		data.practice.native.getup = false;
		if (!controlled)
			return umax;
	}
	else if (!controlled || page != 0 || actions[0] > 10u || actions[1] > 10u)
	{
		if (const auto result = release_dummy_route(index, singleton, page, actions, controlled))
			return *result;
	}
	else
	{
		if (const auto result = apply_dummy_route(index, singleton, actions, driven, getup, lifted))
			return *result;
	}
	data.practice.dummy.state = dummy_state::idle;
	if (pbd::practice::lesson::controls_dummy())
		pbd::practice::lesson::current.finish("not_ready");
	data.practice.dummy.preview = false;
	data.practice.dummy.once = false;
	// Offense would start its movement again at the next boundary: it stays off until SELECT+Circle or the menu
	// turns it back on.
	if (data.practice.training_mode == pbd::practice::training_mode::offense)
		data.practice.offense.movement_off = true;
	data.practice.view.toast = "Choose Freestyle in the game's practice mode to control the dummy";
	data.practice.menu.toast_until = session_clock::now() + std::chrono::seconds(4);
	practice_dummy_log(fmt::format("{\"type\":\"dummy_native_route_refused\",\"boundary\":%u,\"mode\":%u,\"page\":%u}\n", index, mode, page));
	return umax;
}

std::optional<u32> pbd::rollback_session::boundary_call::release_dummy_route(u32 index, u32 singleton, u32 page, std::array<be_t<u32>, 2>& actions, bool controlled)
{
	bool known = true;
	if (data.practice.native.controlled)
	{
		// Restore only fields still holding what the route wrote. A choice made in DR's menu wins.
		const auto written = data.practice.native.written;
		bool changed = false;
		for (u32 field = 0; field < 2; ++field)
			if (actions[field] == written[field])
			{
				actions[field] = data.practice.native.action[field];
				changed = true;
			}
		std::array<be_t<u32>, 2> observed{};
		const bool restored_action = !changed || (vm::try_access(singleton + 0x6c, actions.data(), sizeof(actions), true) &&
													 vm::try_access(singleton + 0x6c, observed.data(), sizeof(observed), false) && observed == actions);
		data.practice.native.controlled = false;
		data.practice.native.getup = false;
		practice_dummy_log(fmt::format("{\"type\":\"dummy_native_route\",\"boundary\":%u,\"controlled\":false,\"action\":%u,\"freestyle\":%u}\n", index, u32{actions[0]}, u32{actions[1]}));
		if (!restored_action)
		{
			known = false;
			data.practice.view.toast = "Couldn't restore the dummy's setting\tRestart Practice to fix it";
			data.practice.menu.toast_until = session_clock::now() + std::chrono::seconds(4);
			practice_dummy_log(fmt::format("{\"type\":\"dummy_native_route_refused\",\"boundary\":%u,\"reason\":\"restore_readback\"}\n", index));
		}
	}
	if (!controlled)
		return known && page == 0 && actions[0] <= 10u ? u32{actions[0]} : static_cast<u32>(umax);
	return std::nullopt;
}

std::optional<u32> pbd::rollback_session::boundary_call::apply_dummy_route(u32 index, u32 singleton, std::array<be_t<u32>, 2>& actions, bool driven, bool getup, bool lifted)
{
	if (!data.practice.native.controlled)
		data.practice.native.action = {u32{actions[0]}, u32{actions[1]}};
	else
	{
		const auto written = data.practice.native.written;
		for (u32 field = 0; field < 2; ++field)
			if (actions[field] != written[field])
				data.practice.native.action[field] = actions[field];
	}
	// DR's action 10 reads the same held/rising table populated by the ordinary input latch; the other actions
	// bypass it. Keep the saved Freestyle value consistent so native pause/resume cannot silently stop a recording.
	const bool was_controlled = data.practice.native.controlled, was_getup = data.practice.native.getup;
	data.practice.native.getup = getup;
	std::array<u32, 2> target{10, 10};
	if (lifted)
		target = {pbd::practice::pbd_standing_action, pbd::practice::pbd_standing_action};
	else if (const auto tech = data.practice.reactions.wakeup_control.native_action())
		target = {*tech, *tech};
	else if (getup && !data.practice.reactions.wakeup_control.engaged && !data.practice.reactions.throw_escape_control.owns())
		target = {pbd::practice::movement::getup_action(pbd::practice::pbd_guard_action(data.practice.native.action[0], data.practice.reactions.guard)),
			pbd::practice::movement::getup_action(pbd::practice::pbd_guard_action(data.practice.native.action[1], data.practice.reactions.guard))};
	actions = {target[0], target[1]};
	std::array<be_t<u32>, 2> observed{};
	if (vm::try_access(singleton + 0x6c, actions.data(), sizeof(actions), true) &&
		vm::try_access(singleton + 0x6c, observed.data(), sizeof(observed), false) &&
		observed == actions)
	{
		if (!was_controlled || was_getup != getup || data.practice.native.written != target)
			practice_dummy_log(fmt::format("{\"type\":\"dummy_native_route\",\"boundary\":%u,\"controlled\":true,\"getup\":%s,\"action\":%u,\"freestyle\":%u}\n",
				index, json_bool(getup), target[0], target[1]));
		data.practice.native.controlled = true;
		data.practice.native.written = target;
		return target[0];
	}
	data.practice.native.getup = was_getup;
	// A lifted guard is never worth stopping anything of the player's for: the next boundary tries it again.
	if (!driven)
		return umax;
	return std::nullopt;
}
