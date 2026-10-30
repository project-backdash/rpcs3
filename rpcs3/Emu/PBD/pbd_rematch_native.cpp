#include "stdafx.h"
#include "pbd_rematch_native.h"
#include "pbd_character_rule_native.h"
#include "pbd_character_rule.h"
#include "pbd_t5dr_memory.h"
#include "../Cell/pbd_practice_native.h"
#include "../Cell/PPUThread.h"
#include "Emu/System.h"
#include "Emu/Memory/vm.h"

namespace pbd::rematch
{
	namespace
	{
		bool word(u32 address, u32& out)
		{
			be_t<u32> value{};
			if (!vm::try_access(address, &value, sizeof(value), false))
				return false;
			out = value;
			return true;
		}
		bool read_options(options& out, bool initial_select = false)
		{
			u32 rounds = 0, time = 0;
			u8 configured_rounds = 0;
			std::array<be_t<u16>, 2> handicap{};
			// The first select precedes round init, which copies this configured byte into the applied match word.
			const bool rounds_read = initial_select ? vm::try_access(0x1008ebd4, &configured_rounds, sizeof(configured_rounds), false) :
				word(0x1008f0a0, rounds);
			if (!rounds_read || !word(0x1008f09c, time) ||
				!vm::try_access(0x1008ebf6, &handicap[0], 2, false) ||
				!vm::try_access(0x1008ebfa, &handicap[1], 2, false))
				return false;
			out = {initial_select ? configured_rounds : rounds, time, {u16{handicap[0]}, u16{handicap[1]}}};
			return valid(out);
		}
		bool matching_options(const options& expected, bool initial_select = false)
		{
			options actual;
			return read_options(actual, initial_select) && actual == expected;
		}
		bool matching_selection(const selection& expected, std::optional<u32> repick)
		{
			u32 left = 0, right = 0, stage = 0;
			return word(0x10093f50, left) && word(0x10093f54, right) && word(0x1008f088, stage) &&
			       (repick == 0 || left == expected.rows[0]) && (repick == 1 || right == expected.rows[1]) && stage == expected.stage;
		}
		// A load that goes straight to VS reaches its battle within this many boundaries of the previous game's end.
		constexpr u32 load_boundaries = 4096;
		// A repick also plays the stock select: the chooser's character countdown (a set admits at most 99 s), its zero press and the
		// handicap, and the stage the stage owner's PC steers through at most two lists.
		constexpr u32 repick_boundaries = load_boundaries + 99 * 60 + 2048;
		bool models_ready()
		{
			u32 left = 0, right = 0, pause = 1;
			return word(0x100908e4, left) && word(0x100911b4, right) && left && right &&
			       !(left & 3) && !(right & 3) && vm::check_addr(left, vm::page_readable, 0x20) &&
			       vm::check_addr(right, vm::page_readable, 0x20) && word(0x1008f0dc, pause) && !pause;
		}
	} // namespace

	bool capture_options(ppu_thread& owner, options& out, bool initial_select) noexcept
	{
		try
		{
			// The owner is held at this boundary. Reading its rules does not execute guest code, so pending scheduler work is safe.
			u32 mode = 0, scene = 0, subscene = 0;
			return get_current_cpu_thread() == &owner && owner.id == t5dr::main_ppu_id &&
			       owner.cia == t5dr::boundary_pc && owner.lr == t5dr::boundary_lr && owner.gpr[2] == t5dr::toc &&
			       pbd::practice::native::boundary_readable(owner) &&
			       (!initial_select || (word(t5dr::game_mode_address, mode) && mode == 4 &&
			           word(t5dr::scene_address, scene) && scene == 3 && word(t5dr::subscene_address, subscene) && subscene == 44)) &&
			       read_options(out, initial_select);
		}
		catch (...)
		{
			return false;
		}
	}

	bool native_load::arm(const load& value, const sealed_state& previous, bool draw, u64 runtime, u32 boundary, bool next_set)
	{
		if (active() || !runtime || validate(value, previous, draw, next_set) != error::none)
			return false;
		// The rows the next game keeps must be available; a repick's chooser picks under the select's own character rule.
		const auto kept = [&](u32 side)
		{
			return value.repick == side || character_rule::row_available(character_rule::rule::ban_jinpachi, value.selected.rows[side]);
		};
		if (character_rule::banned(runtime) && (!kept(0) || !kept(1)))
			return false;
		pending = target{value.selected, value.rules, value.repick};
		generation = runtime;
		first_boundary = boundary;
		requested = false;
		return true;
	}

	bool native_load::arm_start(const selection& selected, const options& rules, u64 runtime, u32 boundary)
	{
		if (active() || !runtime || boundary || !valid(selected) || !valid(rules))
			return false;
		if (character_rule::banned(runtime) &&
			(!character_rule::row_available(character_rule::rule::ban_jinpachi, selected.rows[0]) ||
			 !character_rule::row_available(character_rule::rule::ban_jinpachi, selected.rows[1])))
			return false;
		pending = target{selected, rules, {}, true};
		generation = runtime;
		first_boundary = boundary;
		requested = false;
		return true;
	}

	native_result native_load::advance(ppu_thread& owner, u32 boundary) noexcept
	{
		if (!pending)
			return {progress::complete};
		try
		{
			if (generation != static_cast<u64>(Emu.GetEmulationIdentifier()) || get_current_cpu_thread() != &owner ||
				owner.id != t5dr::main_ppu_id || owner.cia != t5dr::boundary_pc || owner.lr != t5dr::boundary_lr ||
				owner.gpr[2] != t5dr::toc || pbd::practice::native::in_native_call())
				return {progress::refused, "rematch_owner_changed"};
			if (boundary < first_boundary || boundary - first_boundary > (pending->repick ? repick_boundaries : load_boundaries))
				return {progress::refused, "rematch_load_boundary_exceeded"};
			u32 scene = 0, subscene = 0, mode = 0;
			if (!word(t5dr::scene_address, scene) || !word(t5dr::subscene_address, subscene) ||
				!word(t5dr::game_mode_address, mode) || mode != 4)
				return {progress::refused, "rematch_flow_unreadable"};
			if (requested)
			{
				if (scene != 16 && scene != 17)
					return {progress::loading};
				if (!matching_selection(pending->selected, pending->repick) || !matching_options(pending->rules) || !models_ready())
					return {progress::refused, "rematch_loaded_state_changed"};
				pending.reset();
				return {progress::complete};
			}
			// Let the natural result and cleanup finish, then replace the first selection input with the stock resource request.
			if (scene != 3)
				return {progress::waiting};
			if (subscene != 44 || !matching_options(pending->rules, pending->initial_select))
				return {progress::refused, "rematch_select_boundary_changed"};
			// A repick plays this select with input alone (the chooser's own, the steered kept fighter and stage), so the other PC
			// and every watcher replay it from the recorded words. Nothing is requested; the battle readback above checks it.
			if (pending->repick)
			{
				requested = true;
				return {progress::loading};
			}
			const pbd::replay_native::choices selected{pending->selected.rows[0], pending->selected.rows[1], pending->selected.stage};
			const char* refusal = "rematch_stock_resource_request_failed";
			// LV2 may mark this thread for preemption from another thread at any instant, here as the select's sound threads
			// start. Service pending scheduler work first, then hold what arrives later until the setters return, so the
			// request never depends on host timing: both PCs make it at this same boundary.
			if (!pbd::replay_native::service_owner(owner, generation, &refusal))
				return {progress::refused, refusal};
			const pbd::replay_native::owner_guard admission(owner);
			if (!pbd::replay_native::prepare_resources(owner, generation, selected, &refusal))
				return {progress::refused, refusal};
			if (!matching_selection(pending->selected, pending->repick) || !matching_options(pending->rules, pending->initial_select))
				return {progress::refused, "rematch_resource_readback_changed"};
			requested = true;
			return {progress::loading};
		}
		catch (...)
		{
			return {progress::refused, "rematch_native_exception"};
		}
	}
} // namespace pbd::rematch
