#include "stdafx.h"
#include "pbd_rank_screens.h"
#include "pbd_t5dr_memory.h"
#include "pbd_env.h"
#include "../Cell/PPUThread.h"
#include "../Cell/pbd_rollback_session.h"
#include "Emu/System.h"
#include "Emu/Memory/vm.h"

#include <atomic>

LOG_CHANNEL(pbd_rank_log, "PBD Rank screens");

namespace pbd::rank_screens
{
	namespace
	{
		switches current;
		// The armed values and held ranks in one word, so the hooks read one consistent set: the stakes in bytes 0 and 1, the
		// promoted ranks plus one (0 for none) in bytes 2 and 3, the held ranks in bytes 4 and 5.
		std::atomic<u64> packed{0};
		// Touched only by the hooks, on the game's main thread.
		end_log match_ends;

		template <typename T>
		bool read(u32 address, T& out)
		{
			be_t<T> value{};
			if (!vm::try_access(address, &value, sizeof(value), false))
				return false;
			out = value;
			return true;
		}

		// The calls and the nops the hooks sit on, and the phase-9 request after the second, as this executable holds them.
		bool pinned()
		{
			constexpr std::array<std::pair<u32, u32>, 5> expected{{{0x14274, 0x4828e839}, {after_battle_setup, 0x60000000},
				{0x4353c, 0x4825ef4d}, {after_match_bookkeeping, 0x60000000}, {0x435fc, 0x38600009}}};
			for (const auto& [address, opcode] : expected)
			{
				u32 held = 0;
				if (!read(address, held) || held != opcode)
					return false;
			}
			return true;
		}

		bool write(const pbd::rank_stakes::guest_write& at)
		{
			if (at.bytes == 1)
			{
				u8 byte = static_cast<u8>(at.value);
				return vm::try_access(at.address, &byte, 1, true);
			}
			be_t<u32> word = at.value;
			return vm::try_access(at.address, &word, sizeof(word), true);
		}

		std::array<u32, 2> held_ranks(u64 word)
		{
			return {static_cast<u32>((word >> 32) & 0xff), static_cast<u32>((word >> 40) & 0xff)};
		}

		// Read the guest's screen and result fields after applying a diagnostic override.
		void report(u32 address, const pbd::rank_stakes::write_plan& plan, bool written)
		{
			u32 mode = 0, phase = 0, pending = 0, last_winner = 0;
			std::array<u32, 2> stake{}, change{};
			std::array<u8, 3> winner{};
			std::array<u8, 2> rank{};
			std::array<std::array<be_t<s16>, 2>, 2> rounds{};
			const u32 actors = pbd::replay_memory::actors, stride = pbd::replay_memory::actor_stride;
			const bool read_all = read(pbd::t5dr::game_mode_address, mode) && read(0x1009322c, phase) && read(0x10093230, pending) &&
			                      read(pbd::game_profile::previous_winner, last_winner) &&
			                      vm::try_access(pbd::game_profile::winner_bytes, winner.data(), 3, false) &&
			                      read(pbd::game_profile::stake_address(0), stake[0]) && read(pbd::game_profile::stake_address(1), stake[1]) &&
			                      read(pbd::game_profile::change_address(0), change[0]) && read(pbd::game_profile::change_address(1), change[1]) &&
			                      vm::try_access(pbd::game_profile::rank_address(0), &rank[0], 1, false) &&
			                      vm::try_access(pbd::game_profile::rank_address(1), &rank[1], 1, false) &&
			                      vm::try_access(actors + 0x90, rounds[0].data(), sizeof(rounds[0]), false) &&
			                      vm::try_access(actors + stride + 0x90, rounds[1].data(), sizeof(rounds[1]), false);
			pbd_rank_log.warning("Rank screens test at 0x%x%s: %u write(s)%s; mode %u, phase 0x1009322c=%u, pending 0x10093230=%u, "
								 "stake %u/%u, change %u/%u, rank %u/%u, winner 0x1008ebd5=%u/%u/%u, 0x1008f08c=%u, rounds %d/%d, markers %d/%d%s",
				address, g_pbd_replay_frame.load(std::memory_order_relaxed) ? " (re-executed)" : "", plan.count,
				written ? "" : " (a write failed)", mode, phase, pending, stake[0], stake[1], change[0], change[1], rank[0], rank[1],
				winner[0], winner[1], winner[2], last_winner, s16{rounds[0][0]}, s16{rounds[1][0]}, s16{rounds[0][1]}, s16{rounds[1][1]},
				read_all ? "" : " (some words unreadable)");
		}
	} // namespace

	std::string configure()
	{
		packed.store(0, std::memory_order_relaxed);
		match_ends.reset();
		if (auto error = read_switches(pbd::env::find, current); !error.empty())
			return error;
		if (current.ignored)
			pbd_rank_log.warning("Rank screens: %s is ignored outside offline play.", current.ignored);
		if (const auto& test = current.test)
			pbd_rank_log.warning("Rank screens test switch: stake %u/%u, promotion %d/%d in every VS game.", test->stake[0], test->stake[1],
				test->promote[0], test->promote[1]);
		else if (current.screens)
			pbd_rank_log.notice("Rank screens on: the game's own stake marks and PROMOTED TO follow the set's stakes.");
		return {};
	}

	const switches& configured() noexcept
	{
		return current;
	}

	void arm(const pbd::rank_stakes::game_values& values, const std::array<u32, 2>& held) noexcept
	{
		const auto rank = [](int value)
		{
			return static_cast<u64>(value < 0 ? 0 : (value + 1) & 0xff);
		};
		packed.store(static_cast<u64>(values.stake[0] & 0xff) | static_cast<u64>(values.stake[1] & 0xff) << 8 | rank(values.promote[0]) << 16 |
						 rank(values.promote[1]) << 24 | static_cast<u64>(pbd::game_profile::rank_byte(held[0])) << 32 |
						 static_cast<u64>(pbd::game_profile::rank_byte(held[1])) << 40,
			std::memory_order_release);
	}

	pbd::rank_stakes::game_values armed() noexcept
	{
		if (current.test)
			return *current.test;
		const u64 word = packed.load(std::memory_order_acquire);
		pbd::rank_stakes::game_values out;
		for (u32 side = 0; side < 2; ++side)
		{
			out.stake[side] = static_cast<u32>((word >> (8 * side)) & 0xff);
			out.promote[side] = static_cast<int>((word >> (16 + 8 * side)) & 0xff) - 1;
		}
		return out;
	}

	void event(ppu_thread& owner, u32 address) noexcept
	{
		if (!current.enabled() || (address != after_battle_setup && address != after_match_bookkeeping) ||
			owner.id != pbd::t5dr::main_ppu_id || get_current_cpu_thread() != &owner || owner.gpr[2] != pbd::t5dr::toc ||
			Emu.GetTitleID() != pbd::t5dr::title_id || Emu.GetExecutableHash() != pbd::t5dr::executable_hash)
			return;
		// VS only: the arcade modes keep the game's own stakes and promotions.
		u32 mode = 0;
		if (!read(pbd::t5dr::game_mode_address, mode) || mode != 4)
			return;
		if (!pinned())
		{
			static std::atomic<bool> reported{false};
			if (!reported.exchange(true))
				pbd_rank_log.error("Rank screens: the code at the hook points differs from this game's; nothing is written.");
			return;
		}
		const auto values = armed();
		pbd::rank_stakes::write_plan plan;
		std::optional<u32> winner;
		if (address == after_battle_setup)
		{
			plan = pbd::rank_stakes::stake_writes(values);
			match_ends.reset();
		}
		else
		{
			std::array<u8, 3> bytes{};
			if (vm::try_access(pbd::game_profile::winner_bytes, bytes.data(), 3, false))
				winner = pbd::rank_stakes::winner_side(bytes);
			// The test switch knows nothing of the records' ranks, so it writes the winner's only.
			std::optional<std::array<u32, 2>> held;
			if (!current.test)
				held = held_ranks(packed.load(std::memory_order_acquire));
			plan = pbd::rank_stakes::promotion_writes(values, winner, held);
		}
		bool written = true;
		for (u32 i = 0; i < plan.count; ++i)
			written = write(plan.writes[i]) && written;
		if (current.test)
			report(address, plan, written);
		else if (address == after_match_bookkeeping && plan.count && match_ends.due(winner, written))
			pbd_rank_log.notice("Rank screens: the match end names winner %s%s; %u write(s)%s.",
				winner ? (*winner ? "right" : "left") : "none", g_pbd_replay_frame.load(std::memory_order_relaxed) ? " (re-executed)" : "",
				plan.count, written ? "" : ", a write failed");
	}
} // namespace pbd::rank_screens
