#pragma once

#include "pbd_rank_stakes.h"
#include "../pbd_session_mode.h"

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

class ppu_thread;

// PBD_RANK_SCREENS=1 enables the game's stake banners and promotion screen in VS games.
// Offline diagnostics can override each game's values with PBD_TEST_RANK_STAKE and PBD_TEST_RANK_PROMOTE.
namespace pbd::rank_screens
{
	using u32 = std::uint32_t;

	// The nop after each call: the battle's own setup (bl 0x2a2aac at 0x14274, on every battle init) has returned, and the
	// end-of-match bookkeeping (bl 0x2a2488 at 0x4353c) has returned before phase 9 is requested at 0x435fc.
	inline constexpr u32 after_battle_setup = 0x14278, after_match_bookkeeping = 0x43540;

	struct switches
	{
		bool screens = false;                              // PBD_RANK_SCREENS=1
		std::optional<pbd::rank_stakes::game_values> test; // the test switch's values, in offline play only
		const char* ignored = nullptr;                     // a test switch set outside offline play, which nothing reads
		bool enabled() const noexcept
		{
			return screens || test.has_value();
		}
	};

	// The test switch's names, and whether each names promotions. A replay refuses to start while either is set
	// (pbd::replay_switches::refused).
	inline constexpr std::array<std::pair<const char*, bool>, 2> test_switch{{{"PBD_TEST_RANK_STAKE", false}, {"PBD_TEST_RANK_PROMOTE", true}}};

	// Refuse malformed offline overrides. Ignore them in sessions, where forced values would differ from peers or recordings.
	template <typename Find>
	std::string read_switches(Find&& find, switches& out)
	{
		out = {};
		const char* screens = find("PBD_RANK_SCREENS");
		out.screens = screens && std::string_view(screens) == "1";
		const bool offline = pbd::session_mode::offline_play_requested(find("PBD_OFFLINE_PLAY"), find("PBD_SESSION"));
		pbd::rank_stakes::game_values test;
		bool tested = false;
		for (const auto& [name, promotion] : test_switch)
		{
			const char* text = find(name);
			if (!text || !*text)
				continue;
			if (!offline)
			{
				out.ignored = name;
				continue;
			}
			if (!pbd::rank_stakes::read_test_values(text, promotion, test))
				return std::string(name) + ": expected left:<n> and/or right:<n>, comma separated";
			tested = true;
		}
		if (tested)
			out.test = test;
		return {};
	}

	// Reads the switches once for this process (read_switches).
	std::string configure();
	const switches& configured() noexcept;

	// Players arm agreed values when applying a game command; viewers arm recorded values at values_may_change boundaries.
	// `held` contains the ranks currently written by the profile writer.
	void arm(const pbd::rank_stakes::game_values& values, const std::array<u32, 2>& held) noexcept;
	// The values the hooks write: the test switch's when it is set.
	pbd::rank_stakes::game_values armed() noexcept;

	// Phase 8 repeats the hook every frame. Log changes of winner or write outcome, suppressing winnerless successful writes.
	class end_log
	{
	public:
		void reset() noexcept
		{
			logged = 0;
		}
		bool due(std::optional<u32> winner, bool written) noexcept
		{
			const u32 outcome = (winner && *winner < 2 ? *winner + 1 : 0) | (written ? 0 : 4);
			if (!outcome || outcome == logged)
				return false;
			logged = outcome;
			return true;
		}

	private:
		u32 logged = 0; // the winner plus one (0 for none), and 4 when a write failed
	};

	// The hook at either site, on the game's main thread, in a fresh or a re-executed frame alike: the game's own resets run
	// there too, so its writes are made again wherever the game's are.
	void event(ppu_thread& owner, u32 address) noexcept;
} // namespace pbd::rank_screens
