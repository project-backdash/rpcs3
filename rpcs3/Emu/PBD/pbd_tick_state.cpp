#include "stdafx.h"
#include "../Cell/pbd_rollback_session_boundary.h"

bool pbd::rollback_session::tick_dispatch::read_tick_counters(tick_sample& tick, bool soft_mode)
{
	tick.read = true;
	tick.rtt_us = data.peer.rtt.last_us;
	tick.rollback_depth = data.correction.at == index ? data.correction.depth : 0;
	for (u32 i = 0; i < battle_counter_addresses.size(); i++)
		tick.read = read_word(battle_counter_addresses[i], tick.battle_counters[i]) && tick.read;
	if (!tick.read)
	{
		fail("tick_witness_read_failed");
		return true;
	}
	// The battle-side consumer (witness[1]) keeps counting through an input-side counter reset (0x104cce40
	// restarts at 0 while 0x1008ebe4 keeps stepping), so in
	// round-transition mode it is compared with the unoffset expectation instead of the re-anchored counter.
	if (!data.round_lifecycle && tick.battle_counters[1] != row.before_counter)
	{
		if (!soft_mode)
		{
			fail("tick_witness_counter_mismatch");
			return true;
		}
		soft_fail("tick_witness_counter_mismatch"); // Battle-side consumer frozen or offset: recorded.
	}

	return false;
}

void pbd::rollback_session::tick_dispatch::hash_tick_combat(tick_sample& tick, bool relaxed_scope)
{
	tick.combat_read = true;
	for (u32 region = 0; region < combat_regions.size(); region++)
	{
		std::array<u8, 0x11a0> bytes{};
		const auto [address, size] = combat_regions[region];
		tick.combat_read = vm::try_access(address, bytes.data(), size, false) && tick.combat_read;
		u64 hash = 0xcbf29ce484222325ull;
		for (u32 i = 0; i < size; i++)
			hash = (hash ^ (combat_pointer_excluded(region, i) ? u8{0} : bytes[i])) * 0x100000001b3ull;
		// Non-fight boundaries (round lifecycle): the player structs of region 0 carry sound handles, emitter
		// records and intro animation cursors that a rollback cannot keep identical; the full hash is reported
		// as audio_bookkeeping and the enforced hash is the constant 0 until the fight resumes.
		if (region == 0 && relaxed_scope)
		{
			tick.audio_bookkeeping = hash;
			hash = 0;
		}
		tick.combat[region] = hash;
		if (data.peer.enabled)
		{
			if (data.session.region_ring.empty())
				data.session.region_ring.resize(128);
			auto& slot = data.session.region_ring[index % 128];
			if (region == 0)
			{
				slot.tag = index + 1;
				std::memcpy(slot.r0.data(), bytes.data(), std::min<usz>(size, slot.r0.size()));
			}
			else if (region == 1)
				std::memcpy(slot.r1.data(), bytes.data(), std::min<usz>(size, slot.r1.size()));
			else
				std::memcpy(slot.r2.data(), bytes.data(), std::min<usz>(size, slot.r2.size()));
		}
	}
}

void pbd::rollback_session::tick_dispatch::hash_tick_generator(tick_sample& tick, bool relaxed_scope)
{
	{
		// Owner-thread generator state (TLS through this PPU's r13) and the dependent countdown.
		tick.generator_base = static_cast<u32>(ppu.gpr[13]);
		be_t<u32> seed{};
		std::array<u8, 0x8c> tail{};
		tick.generator_read = vm::try_access(tick.generator_base - 0x7000, &seed, sizeof(seed), false) &&
		                      vm::try_access(tick.generator_base + 0x10000 - 0x6f6c, tail.data(), tail.size(), false);
		tick.generator_seed = seed;
		u64 digest = 0xcbf29ce484222325ull;
		std::array<u8, 4> seed_bytes{};
		std::memcpy(seed_bytes.data(), &seed, seed_bytes.size()); // be_t storage: guest byte order
		for (u8 b : seed_bytes)
			digest = (digest ^ b) * 0x100000001b3ull;
		if (!relaxed_scope)
			for (u8 b : tail)
				digest = (digest ^ b) * 0x100000001b3ull; // TLS tail: fight boundaries only
		tick.generator_digest = digest;
		if (data.session.enabled && data.match_at(index))
		{
			// Combat hash v3: the libc generator of the owner thread joins the enforced
			// confirmed-state hash at match ticks: seed word + flag + 3 padding bytes + last word + the 32-word table (140 bytes), WITHOUT the libc
			// noise word after the table (tail offsets 0x88..0x8c), so a generator divergence between the peers ends the session at
			// once (bundle + tape) instead of surfacing later in a region. The flow-object LCGs are inside region 1 already.
			u64 g = 0x84222325cbf29ce4ull;
			for (u8 b : seed_bytes)
				g = (g ^ b) * 0x100000001b3ull;
			for (usz i = 0; i < 0x88 && i < tail.size(); i++)
				g = (g ^ tail[i]) * 0x100000001b3ull;
			tick.combat[1] ^= g;
		}
		be_t<u16> countdown{};
		tick.countdown_read = vm::try_access(0x10090316, &countdown, sizeof(countdown), false);
		tick.countdown = countdown;
		tick.battle_time = data.battle_clock.enabled ? data.battle_clock.anchor + (static_cast<u64>(index) + 1) * data.battle_clock.freq / 60 : 0;
		tick.battle_reads = data.battle_clock.reads;
		tick.battle_clock_version = data.battle_clock.version;
		tick.battle_frame_samples = data.battle_clock.frame_samples;
		tick.monitor_read = read_word(0x100538bc, tick.monitor_state) && read_word(0x100538c0, tick.monitor_start) &&
		                    read_word(0x100538c4, tick.monitor_end) && read_word(0x100538c8, tick.monitor_index) &&
		                    read_dword(0x1074f9f8, tick.frame_start);
		tick.frame_start_adapted = data.correction.frame_start_adapted;
		hash_tick_effects(tick);
	}
}

void pbd::rollback_session::tick_dispatch::hash_tick_effects(tick_sample& tick)
{
	tick.effect_read = data.checkpoint.effect_hash == 1 || (data.checkpoint.effect_hash < 0 && !data.peer.enabled);
	for (u32 region = 0; tick.effect_read && region < effect_candidate_regions.size(); region++)
	{
		u64 hash = 0xcbf29ce484222325ull;
		std::array<u8, 4096> page{};
		const u32 base = effect_candidate_regions[region];
		for (u32 offset = 0; offset < 0x10000; offset += 4096)
		{
			tick.effect_read = vm::try_access(base + offset, page.data(), page.size(), false) && tick.effect_read;
			u64 page_hash = 0xcbf29ce484222325ull;
			for (u8 b : page)
			{
				hash = (hash ^ b) * 0x100000001b3ull;
				page_hash = (page_hash ^ b) * 0x100000001b3ull;
			}
			tick.effect_pages[region * 16 + offset / 4096] = page_hash;
		}
		tick.effect[region] = hash;
	}
}

bool pbd::rollback_session::tick_dispatch::verify_tick_phase(tick_sample& tick, bool pbd_round_cut, bool soft_mode)
{
	if (pbd_round_cut)
	{
		// Initialization replaces the prior round rather than executing one of its battle updates.
		// Recorded coordinates and hashes still validate this entry below the host witness checks.
		tick.checked = false;
		tick.one_tick = false;
		tick.fight = false;
		tick.fight_count = index > data.start_index ? data.tick_at(index - 1).fight_count : 0;
		initial_state().battle_counter = tick.battle_counters[1] - tick.fight_count;
	}
	else if (index > data.start_index)
	{
		const auto& previous = data.tick_at(index - 1);
		tick.checked = true;
		const bool step = tick.battle_counters[1] == previous.battle_counters[1] + 1;
		tick.one_tick = previous.read && (previous.battle_counters[4] == 0 || data.session.enabled) &&
		                tick.battle_counters[0] == previous.battle_counters[0] + 1 && step &&
		                tick.battle_counters[2] == previous.battle_counters[2] - 1 && tick.battle_counters[3] == previous.battle_counters[3] + 1;
		if (data.round_lifecycle)
		{
			// Phase law: a FIGHT boundary is exactly one battle tick;
			// a NON-FIGHT boundary (K.O. sequence, round end, round start) freezes the battle-side consumer with the
			// pause flag clear while frame counters may step or restart and the round timer may reset.
			tick.fight = step;
			tick.fight_count = previous.fight_count + (step ? 1 : 0);
			// Session mode: a new match initialises the battle-side consumer on a MENU frame (for example 7200 -> 0 in the
			// transition before match 2); the law's base is re-anchored there. Inside matches the contract is unchanged.
			const bool rebase = data.session.enabled && !step && !data.match_at(index) && tick.battle_counters[1] != previous.battle_counters[1];
			if (rebase)
			{
				initial_state().battle_counter = tick.battle_counters[1] - tick.fight_count;
				pbd_session_log.warning("Session: battle-side consumer %u -> %u on menu boundary %u (fight law re-anchored)", previous.battle_counters[1], tick.battle_counters[1], index);
			}
			if (step && !tick.one_tick)
			{
				fail("tick_witness_not_exactly_one_tick");
				return true;
			}
			// Session mode: the pause flag may be set on non-fight boundaries (the pause menu); counted, not a failure.
			if (!step && !rebase && (tick.battle_counters[1] != previous.battle_counters[1] || (tick.battle_counters[4] != 0 && !data.session.enabled)))
			{
				fail("tick_witness_law");
				return true;
			}
			if (tick.battle_counters[1] != initial_state().battle_counter + tick.fight_count)
			{
				fail("tick_witness_fight_count");
				return true;
			}
			if (step)
				data.last_fight_index = index;
		}
		else if (!tick.one_tick)
		{
			if (!soft_mode)
			{
				fail("tick_witness_not_exactly_one_tick");
				return true;
			}
			soft_fail("tick_witness_not_exactly_one_tick");
		}
	}
	else if (data.round_lifecycle)
	{
		tick.fight = false;
		tick.fight_count = 0;
		data.last_fight_index = index; // The savestate is in battle: the first boundary counts as fight for the grace rule.
		if (tick.battle_counters[1] != initial_state().battle_counter)
		{
			fail("tick_witness_fight_count");
			return true;
		}
	}

	return false;
}
