#include "pbd_test.h"

#include "Emu/Cell/pbd_update_script.h"
#include "Emu/PBD/pbd_spectator_channel_input.h"

#include <algorithm>
#include <array>
#include <filesystem>
#include <fstream>
#include <random>
#include <string>
#include <vector>

using pbd::test::check;

namespace
{
	using namespace pbd::spectator;
	using generator = std::mt19937;

	digest payload_hash(const std::uint8_t* bytes, std::size_t size)
	{
		digest out{};
		for (std::size_t i = 0; i < size; ++i)
			out[i % out.size()] = static_cast<std::uint8_t>((out[i % out.size()] * 33u) ^ bytes[i]);
		return out;
	}

	void put(std::vector<std::uint8_t>& bytes, std::size_t at, u64 value, unsigned count)
	{
		for (unsigned i = 0; i < count; ++i)
			bytes.at(at + i) = static_cast<std::uint8_t>(value >> (8 * i));
	}

	manifest identity()
	{
		manifest out;
		out.generation = 123;
		out.seed = 77;
		out.rounds = 3;
		out.stage = 17;
		out.left = 12;
		out.right = 32;
		out.binding.fill(1);
		out.build = "test-build";
		out.compat = "test-compat";
		out.title = "NPUB30009";
		out.emulator = std::string(64, 'a');
		out.checkpoint = std::string(64, 'b');
		out.recipe = std::string(64, 'c');
		out.applied = true;
		return out;
	}

	struct directories
	{
		std::filesystem::path parent = std::filesystem::canonical(std::filesystem::temp_directory_path());
		std::filesystem::path root;

		directories()
		{
			for (unsigned i = 0; i < 1024; ++i)
			{
				root = parent / ("pbd-packet-random-" + std::to_string(i));
				if (std::filesystem::create_directory(root))
					return;
			}
			pbd::test::fail("no unused test directory", __FILE__, __LINE__);
		}

		~directories()
		{
			const auto resolved = std::filesystem::canonical(root);
			if (resolved.parent_path() == parent && resolved.filename().string().starts_with("pbd-packet-random-"))
			{
				std::error_code error;
				std::filesystem::remove_all(resolved, error);
			}
		}

		std::filesystem::path make(unsigned index) const
		{
			const auto path = root / std::to_string(index);
			check(std::filesystem::create_directory(path));
			return path;
		}
	};

	void write(const std::filesystem::path& path, const void* bytes, std::size_t size)
	{
		std::ofstream out(path, std::ios::binary);
		out.write(static_cast<const char*>(bytes), static_cast<std::streamsize>(size));
		out.close();
		check(out, "the generated input is complete before the production parser reads it");
	}

	void begin(local_channel& channel, const std::filesystem::path& path)
	{
		check(channel.prepare(9, pbd::t5dr::main_ppu_id, 0, 0x8000004d, identity().checkpoint, identity().emulator));
		const std::string control =
			R"({"schema":"pbd-spectator-control-1","generation":"123","serial":1,"bindingDigest":")" +
			hex(identity().binding) + R"(","type":"begin_game","gameNumber":1,"originBoundary":0,)"
									  R"("originCounter":2147483725,"kind":"clean_character_select","stageId":17,)"
									  R"("leftCharacterRow":12,"rightCharacterRow":32,"roundsPerGame":3})";
		write(path / "spectator-control-1.json", control.data(), control.size());
		check(channel.poll_control() && channel.game_active(), "the packet reaches an active production consumer");
	}

	std::vector<std::uint8_t> packet(generator& random)
	{
		std::vector<std::uint8_t> bytes(header_bytes + applied_frame_bytes);
		constexpr std::array<std::uint8_t, 8> magic{'P', 'B', 'D', 'S', 'P', 'C', '1', 0};
		std::copy(magic.begin(), magic.end(), bytes.begin());
		put(bytes, 8, 123, 8);
		put(bytes, 16, 1, 4);
		put(bytes, 24, 1, 2);
		put(bytes, 26, 1, 1);
		put(bytes, 28, 1, 4);
		const auto binding = identity().binding;
		std::copy(binding.begin(), binding.end(), bytes.begin() + 32);
		put(bytes, header_bytes, random() & 0xffff, 2);
		put(bytes, header_bytes + 8, random() & 0xffff, 2);
		put(bytes, header_bytes + 20, 1, 4);
		put(bytes, header_bytes + 24, 0x8000004d, 4);
		put(bytes, header_bytes + 28, 17, 4);
		put(bytes, header_bytes + 32, 7, 4);
		const auto hash = payload_hash(bytes.data() + header_bytes, applied_frame_bytes);
		std::copy(hash.begin(), hash.end(), bytes.begin() + 64);
		return bytes;
	}
} // namespace

PBD_TEST(PbdSpectatorRandom, TruncatedGeneratedPacketsNeverExposeAFrame)
{
	directories folders;
	generator random(0x50424439);
	for (unsigned i = 0; i < 64; ++i)
	{
		const auto path = folders.make(i);
		local_channel channel(path, identity());
		begin(channel, path);
		const auto bytes = packet(random);
		const auto length = i == 0 ? 0 : random() % bytes.size();
		write(path / "spectator-input-1.bin", bytes.data(), length);
		check(!channel.poll_input(payload_hash), "a truncated production packet is refused");
		check(!channel.reserve() && channel.consumed() == 0, "refusal publishes no partial frame");
	}
}

PBD_TEST(PbdSpectatorRandom, MutatedPacketsCannotExposeOutOfRangeSyncFacts)
{
	directories folders;
	generator random(0x5042443a);
	for (unsigned i = 0; i < 64; ++i)
	{
		const auto path = folders.make(i);
		local_channel channel(path, identity());
		begin(channel, path);
		auto bytes = packet(random);
		if (i)
		{
			const auto at = random() % bytes.size();
			bytes[at] ^= static_cast<std::uint8_t>(1u << (random() % 8));
			const auto hash = payload_hash(bytes.data() + header_bytes, applied_frame_bytes);
			std::copy(hash.begin(), hash.end(), bytes.begin() + 64);
		}
		write(path / "spectator-input-1.bin", bytes.data(), bytes.size());
		if (channel.poll_input(payload_hash))
		{
			const auto accepted = channel.reserve();
			check(accepted && accepted->boundary <= maximum_boundary && accepted->segment <= 255);
			check(accepted->scene <= 32 && accepted->mask <= 7 && accepted->gate_hold <= 1);
		}
		else
			check(!channel.reserve() && channel.consumed() == 0);
	}
}
