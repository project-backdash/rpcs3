#include "stdafx.h"
#include "pbd_hidden_selection.h"
#include "../PBD/pbd_character_rule_native.h"
#include "../PBD/pbd_t5dr_memory.h"
#include "PPUAnalyser.h"
#include "lv2/sys_sync.h"
#include "Emu/IdManager.h"
#include "Emu/System.h"
#include "Emu/Memory/vm.h"
#include "Crypto/sha256.h"

namespace pbd::hidden_selection
{
	namespace
	{
		bool bytes(u32 address, void* out, std::size_t size)
		{
			return vm::try_access(address, out, size, false);
		}
		u32 be32(const std::uint8_t* value)
		{
			return (u32{value[0]} << 24) | (u32{value[1]} << 16) | (u32{value[2]} << 8) | value[3];
		}
		u16 be16(const std::uint8_t* value)
		{
			return static_cast<u16>((u16{value[0]} << 8) | value[1]);
		}
		std::optional<u32> word(u32 address)
		{
			std::array<std::uint8_t, 4> data{};
			if (!bytes(address, data.data(), data.size()))
				return {};
			return be32(data.data());
		}
		template <std::size_t Size>
		std::string table_hash(const std::array<std::uint8_t, Size>& value)
		{
			std::array<std::uint8_t, 32> hash{};
			if (mbedtls_sha256_ret(value.data(), value.size(), hash.data(), 0) != 0)
				return "hash_read_failed";
			constexpr char hex[] = "0123456789abcdef";
			std::string actual;
			for (const auto byte : hash)
			{
				actual += hex[byte >> 4];
				actual += hex[byte & 15];
			}
			return actual;
		}
	} // namespace
	bool pinned_binary()
	{
		const auto* main = g_fxo->try_get<main_ppu_module<lv2_obj>>();
		if (Emu.GetTitleID() != pbd::t5dr::title_id || !main || !main->applied_patches.empty() ||
			!std::equal(pinned_loader_sha1.begin(), pinned_loader_sha1.end(), main->sha1))
			return false;
		if (word(pbd::t5dr::toc - 0x7280) != 0x10093f58u || word(pbd::t5dr::toc - 0x727c) != 0x10094048u ||
			word(pbd::t5dr::toc - 0x72c4) != 0x10052f04u || word(pbd::t5dr::toc - 0x60cc) != 0x1000687du ||
			word(pbd::t5dr::toc - 0x60e4) != 0x441fd0u || word(pbd::t5dr::toc - 0x60e0) != pbd::t5dr::game_mode_address ||
			word(pbd::t5dr::toc - 0x72c0) != 0x100934a0u)
			return false;
		// The loader digest binds all LOAD data to the pinned ELF. Live opcodes additionally reject a patched handler.
		constexpr std::pair<u32, u32> guards[]{
			{0x5a694, 0x80628d80}, {0x5a69c, 0x80628d84}, {0x5b3e8, 0x48055909},
			{0xaf670, 0x4bfff799}, {0xaf67c, 0x88090002}, {0xaf680, 0x901e0000},
			{0xaf684, 0x4bfffe4d}, {0xaf68c, 0x419effdc}, {0xaf514, 0x38600022},
			{0xaf51c, 0x2f800018}, {0xaf520, 0x2f000023}, {0xaf524, 0x419e0010}, {0xaf528, 0x419a000c},
			{0x59000, 0x81228d40}, {0x59004, 0xe8090000}, {0x59008, 0x7c001c36}, {0x5900c, 0x780307e0},
			{0xb00c4, 0xa3e30000}, {0xb0380, 0x4bfa8ef1}, {0xb03dc, 0x93890008},
			{0xb0990, 0x809f0048}, {0xb099c, 0x4bfaa1a9}, {0xb09ac, 0x809f0048}, {0xb09b8, 0x4bfaa18d},
			{0xb13a4, 0x7bab1764}, {0xb1400, 0x80090008}, {0xb1424, 0x93a90028},
			{0xb1694, 0x38000003}, {0xb16a0, 0x981f0021}, {0xb1968, 0x8009000c}};
		for (const auto& [address, expected] : guards)
		{
			const auto actual = word(address);
			if (!actual || *actual != expected)
				return false;
		}
		return true;
	}
	std::optional<tables> read_native_tables(std::string* refusal)
	{
		if (refusal)
			refusal->clear();
		const auto reject = [&](std::string reason) -> std::optional<tables>
		{
			if (refusal)
				*refusal = std::move(reason);
			return {};
		};
		const auto describe = [](const std::optional<u32>& value)
		{
			return value ? std::to_string(*value) : std::string("unreadable");
		};
		std::array<std::uint8_t, 36 * 6> grid{};
		std::array<std::uint8_t, 85 * 0x24> rows{};
		std::array<std::uint8_t, 25> buttons{};
		std::array<std::uint8_t, 8> availability{};
		const auto grid_pointer = word(0x441fd8), grid_count = word(0x441fdc), grid_mode = word(0x10094048 + 0xf8);
		if (grid_pointer != 0x10006694u)
			return reject("grid_pointer@0x441fd8=" + describe(grid_pointer));
		if (grid_count != 36u)
			return reject("grid_count@0x441fdc=" + describe(grid_count));
		if (grid_mode != 1u)
			return reject("grid_mode@0x10094140=" + describe(grid_mode));
		if (!bytes(0x10006694, grid.data(), grid.size()))
			return reject("grid_unreadable@0x10006694");
		if (!bytes(0x4403c0, rows.data(), rows.size()))
			return reject("rows_unreadable@0x4403c0");
		if (!bytes(0x1000687d, buttons.data(), buttons.size()))
			return reject("buttons_unreadable@0x1000687d");
		if (!bytes(0x100934a0, availability.data(), availability.size()))
			return reject("availability_unreadable@0x100934a0");
		const auto grid_sha = table_hash(grid), rows_sha = table_hash(rows);
		if (grid_sha != "c6ba0c030b8ae53525a0765c09fb74ac8fcbff8755ee3a7fd785f2e159395a80")
			return reject("grid_sha256=" + grid_sha);
		if (rows_sha != "6f72c11a06d8aa28efd552326d8db9b07fe957100cb39f2825dd222445c1a180")
			return reject("rows_sha256=" + rows_sha);
		constexpr std::array<std::uint8_t, 25> stock_buttons{0, 0, 1, 1, 255, 0, 0, 1, 1, 2, 0, 2, 1, 1, 255, 0, 2, 1, 3, 255, 0, 3, 1, 4, 2};
		if (buttons != stock_buttons)
			return reject("buttons_sha256=" + table_hash(buttons));
		tables value;
		value.buttons = buttons;
		value.availability = (std::uint64_t{be32(availability.data())} << 32) | be32(availability.data() + 4);
		if (!supported_availability(value.availability))
			return reject("availability@0x100934a0=" + std::to_string(value.availability));
		for (std::size_t i = 0; i < value.grid.size(); ++i)
		{
			value.grid[i].character = be16(grid.data() + i * 6);
			std::copy_n(grid.begin() + static_cast<std::ptrdiff_t>(i * 6 + 2), 4, value.grid[i].neighbor.begin());
		}
		for (std::size_t i = 0; i < value.characters.size(); ++i)
			value.characters[i] = {be16(rows.data() + i * 0x24), be16(rows.data() + i * 0x24 + 2)};
		for (u32 character = 0; character < value.costumes.size(); ++character)
		{
			std::array<std::uint8_t, 12> metadata{};
			if (!bytes(0x10052f04 + 12 * character, metadata.data(), metadata.size()))
				return reject("costume_metadata_unreadable:character=" + std::to_string(character));
			const auto pointer = be32(metadata.data());
			const auto count = be16(metadata.data() + 6);
			const bool available = (value.availability & (std::uint64_t{1} << character)) != 0;
			if ((!count && available) || count > 5 || pointer < 0x10000000u || pointer > 0x10ffffffu || pointer % 4)
				return reject("costume_metadata_invalid:character=" + std::to_string(character) + ",pointer=" + std::to_string(pointer) + ",count=" + std::to_string(count));
			auto& costumes = value.costumes[character].rows;
			for (u32 i = 0; i < count; ++i)
			{
				const auto row_id = word(pointer + 4 * i);
				if (!row_id || *row_id >= value.characters.size() || value.characters[*row_id].character != character)
					return reject("costume_row_invalid:character=" + std::to_string(character) + ",variant=" + std::to_string(i) + ",row=" + describe(row_id));
				costumes.push_back(*row_id);
			}
		}
		value.ban_jinpachi = pbd::character_rule::banned(static_cast<u64>(Emu.GetEmulationIdentifier()));
		value.verified = true;
		return value;
	}
	std::optional<observation> read_native_observation(u32 role)
	{
		if (role > 1)
			return {};
		const auto scene = word(pbd::t5dr::scene_address), subscene = word(pbd::t5dr::subscene_address),
				   mode = word(pbd::t5dr::game_mode_address), grid_mode = word(0x10094048 + 0xf8);
		if (!scene || !subscene || !mode || !grid_mode)
			return {};
		observation value;
		value.scene = *scene;
		value.subscene = *subscene;
		value.mode = *mode;
		value.grid_mode = *grid_mode;
		if (value.scene == 3)
		{
			std::array<std::uint8_t, 0x78> player{};
			if (!bytes(0x10093f58 + role * 0x78, player.data(), player.size()))
				return {};
			value.state = player[0x20];
			value.pad_role = player[0x23];
			value.cursor = be32(player.data());
			value.base_character = be32(player.data() + 8);
			value.committed_row = be32(player.data() + 0x48);
			value.handicap = be32(player.data() + 0x68);
			if (value.state == 13)
			{
				std::array<std::uint8_t, 0x1c0> work{};
				if (!bytes(0x10094048, work.data(), work.size()))
					return {};
				value.stage_mode = be32(work.data() + 0x104);
				value.stage_cursor = be32(work.data() + 0xfc);
				value.stage_owner = be32(work.data() + 0xf0);
				const auto fresh_count = be32(work.data() + 0x110), old_count = be32(work.data() + 0x114);
				if (fresh_count > 21 || old_count > 21)
					return {};
				for (u32 i = 0; i < fresh_count; ++i)
					value.new_stages.push_back(be32(work.data() + 0x118 + 4 * i));
				for (u32 i = 0; i < old_count; ++i)
					value.old_stages.push_back(be32(work.data() + 0x16c + 4 * i));
				for (const auto stage : value.new_stages)
					if (stage > 0x25)
						return {};
				for (const auto stage : value.old_stages)
					if (stage > 0x25)
						return {};
			}
		}
		if (value.scene == 17)
		{
			const auto left = word(0x10093f50), right = word(0x10093f54), stage = word(0x1008f088), rounds = word(0x1008f0a0);
			const auto p1 = word(0x10091ab0), p2 = word(0x10091ab4), time = word(0x1008f09c);
			if (!left || !right || !stage || !rounds || p1 != 0x10090040u || p2 != 0x10090910u || !time || *time > 4)
				return {};
			value.applied_left = *left;
			value.applied_right = *right;
			value.applied_stage = *stage;
			value.applied_rounds = *rounds;
			value.applied_verified = true;
		}
		value.verified = true;
		return value;
	}
} // namespace pbd::hidden_selection
