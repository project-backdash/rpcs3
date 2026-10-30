#include "stdafx.h"
#include "pbd_replay_restore.h"
#include "pbd_t5dr_memory.h"
#include "Emu/Cell/PPUThread.h"
#include "Emu/Cell/pbd_replay_game_file.h"

#include <algorithm>

namespace pbd::replay_restore
{
	namespace
	{
		using pbd::replay_game_file::field;
		using pbd::replay_game_file::hex;
		using pbd::replay_game_file::integer;
		using pbd::replay_game_file::kind;
	} // namespace

	bool round_styles_bound(u32 ordinal) noexcept
	{
		try
		{
			for (u32 side = 0; side < 2; ++side)
			{
				const u32 actor = side ? 0x10090910 : 0x10090040;
				be_t<u16> character{}, style{};
				if (!vm::try_access(actor + 0x40, &character, sizeof(character), false) ||
					!vm::try_access(actor + 0x3e, &style, sizeof(style), false))
					return false;
				if (u16{character} != 0x12)
					continue;
				// 0x59448 selects Mokujin's generated style. The stock transition at 0x425f0 writes it to +0x3e and calls
				// 0x70a44 before the kind 6 initializer; the kind 4 and 5 initializers at 0x159f0 alone cannot bind another
				// round's move and model resources.
				u8 expected = 0;
				if ((ordinal <= 8 && !vm::try_access(0x10093470 + 9 * side + ordinal, &expected, 1, false)) ||
					u16{style} != expected)
					return false;
			}
			return true;
		}
		catch (...)
		{
			return false;
		}
	}

	generators::generators(ppu_thread& owner)
	{
		const u32 tls = static_cast<u32>(owner.gpr[13]);
		std::array<be_t<u32>, 624> again{};
		if (owner.gpr[13] > 0xffffffffu || tls < 0x7000 || tls > 0xffff6000u ||
			word(pbd::t5dr::toc - 0x7028) != 0x10053578 || word(pbd::t5dr::toc - 0x702c) != 0x10095168 ||
			!vm::try_access(tls - 0x7000, libc.data(), 4, false) ||
			!vm::try_access(tls + 0x9094, libc.data() + 4, 136, false))
			throw std::runtime_error("round_libc_context");
		flow = {word(0x1008f0c4), word(0x1008f0d0)};
		mt_index = word(0x10053578);
		if (mt_index > 624 || !vm::try_access(0x10095168, mt.data(), sizeof(mt), false) ||
			!vm::try_access(0x10095168, again.data(), sizeof(again), false) || mt != again ||
			mt_index != word(0x10053578) ||
			!vm::try_access(0x1061d47c, network.data(), sizeof(network), false) ||
			!vm::try_access(0x10093470, styles.data(), sizeof(styles), false) ||
			!vm::try_access(0x100934a0, eligibility.data(), sizeof(eligibility), false))
			throw std::runtime_error("round_generator_context");
	}

	generators::generators(const pbd::json::json_value& value)
	{
		libc = hex<140>(field(value, "sharedLibc140Hex"));
		const auto& lcg = field(value, "flowLcg");
		const auto& twister = field(value, "mt");
		const auto& words = field(twister, "words");
		if (lcg.kind != kind::array || lcg.items.size() != 2 || words.kind != kind::array || words.items.size() != 624)
			throw std::invalid_argument("generator_extent");
		for (usz i = 0; i < 2; ++i)
			flow[i] = integer(lcg.items[i]);
		for (usz i = 0; i < 624; ++i)
			mt[i] = integer(words.items[i]);
		mt_index = integer(twister, "index", 624);
		const auto& net = field(value, "network");
		network = {integer(net, "matchSeedLcg"), integer(net, "state"), integer(net, "count")};
		const auto& moku = field(value, "mokujin");
		styles = hex<18>(field(moku, "stylesHex"));
		eligibility = hex<8>(field(moku, "eligibilityHex"));
	}

	bool generators::apply(ppu_thread& owner) const
	{
		const u32 tls = static_cast<u32>(owner.gpr[13]);
		if (owner.gpr[13] > 0xffffffffu || tls < 0x7000 || tls > 0xffff6000u ||
			word(pbd::t5dr::toc - 0x7028) != 0x10053578 || word(pbd::t5dr::toc - 0x702c) != 0x10095168)
			return false;
		// Only each generator's fixed extent is written, never a wider image of guest memory. The style table is never
		// written: the stock producer generates it from the context before it runs.
		std::array<u8, 4> seed{};
		std::array<u8, 136> tail{};
		std::copy_n(libc.begin(), 4, seed.begin());
		std::copy_n(libc.begin() + 4, 136, tail.begin());
		return store(tls - 0x7000, seed) && store(tls + 0x9094, tail) &&
		       store_word(0x1008f0c4, flow[0]) && store_word(0x1008f0d0, flow[1]) &&
		       store(0x10095168, mt) && store_word(0x10053578, mt_index) && store(0x1061d47c, network) &&
		       store(0x100934a0, eligibility);
	}

	bool generators::style_readback() const
	{
		std::array<u8, 18> observed{};
		return vm::try_access(0x10093470, observed.data(), sizeof(observed), false) && observed == styles;
	}
} // namespace pbd::replay_restore
