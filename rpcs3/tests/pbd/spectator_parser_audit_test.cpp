#include "pbd_test.h"

#include "Emu/Cell/pbd_update_script.h"
#include "Emu/PBD/pbd_spectator_channel_input.h"

#include <fstream>
#include <vector>

namespace
{
	namespace spectator = pbd::spectator;
	using pbd::test::check;
	using bytes = std::vector<std::uint8_t>;

	spectator::manifest identity()
	{
		spectator::manifest m;
		m.generation = 123;
		m.seed = 23;
		m.stage = 17;
		m.left = 12;
		m.right = 32;
		m.rounds = 3;
		m.emulator = std::string(64, '1');
		m.checkpoint = std::string(64, '2');
		m.recipe = std::string(64, '3');
		m.title = "NPUB30009";
		return m;
	}

	bytes packet()
	{
		bytes out(160);
		constexpr std::array<std::uint8_t, 8> magic{'P', 'B', 'D', 'S', 'P', 'C', '1', 0};
		std::copy(magic.begin(), magic.end(), out.begin());
		out[8] = 123;
		out[16] = 1;
		out[24] = 1;
		out[28] = 1;
		out[116] = 1;
		out[120] = 23;
		out[124] = 7;
		return out;
	}

	// Header bounds are checked before the injected digest callback can accept the payload.
	spectator::digest zero_hash(const std::uint8_t*, std::size_t)
	{
		return {};
	}

	struct sample_document
	{
		std::filesystem::path parent = std::filesystem::canonical(std::filesystem::temp_directory_path());
		std::filesystem::path directory;
		spectator::local_channel input;

		sample_document()
			: directory(parent / ("pbd-spectator-parser-" +
									 std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()))),
			  input(make_directory(), identity())
		{
			const auto m = identity();
			check(input.prepare(9, 0x01000000, 0, 0x80000017, m.checkpoint, m.emulator));
			std::ofstream control(directory / "spectator-control-1.json", std::ios::binary);
			control << "{\"schema\":\"pbd-spectator-control-1\",\"generation\":\"123\",\"bindingDigest\":\""
					<< spectator::hex(m.binding) << "\",\"serial\":1,\"type\":\"begin_game\",\"gameNumber\":1,"
													"\"originBoundary\":0,\"originCounter\":2147483671,\"kind\":\"clean_character_select\","
													"\"stageId\":17,\"leftCharacterRow\":12,\"rightCharacterRow\":32,\"roundsPerGame\":3}";
			control.close();
			check(control && input.poll_control());
		}

		std::filesystem::path make_directory()
		{
			check(std::filesystem::create_directory(directory));
			check(std::filesystem::canonical(directory).parent_path() == parent);
			return directory;
		}

		~sample_document()
		{
			std::error_code error;
			std::filesystem::remove_all(directory, error);
		}

		void write(const bytes& wire, unsigned serial = 1)
		{
			std::ofstream out(directory / ("spectator-input-" + std::to_string(serial) + ".bin"), std::ios::binary);
			if (!wire.empty())
				out.write(reinterpret_cast<const char*>(wire.data()), static_cast<std::streamsize>(wire.size()));
			out.close();
			check(out);
		}
	};

	void refused(const bytes& wire)
	{
		sample_document f;
		f.write(wire);
		check(!f.input.poll_input(zero_hash));
		check(!f.input.healthy() && f.input.consumed() == 0 && !f.input.reserve());
	}
} // namespace

PBD_TEST(PbdSpectatorParserAudit, AValidChunkIsReceivedAndAGapNeverAdvancesReceipt)
{
	sample_document f;
	f.write(packet(), 2);
	check(f.input.poll_input(zero_hash) && !f.input.reserve());
	f.write(packet());
	check(f.input.poll_input(zero_hash));
	check(f.input.reserve() && f.input.consumed() == 0);
}

PBD_TEST(PbdSpectatorParserAudit, EveryTruncatedChunkAndAnOversizeChunkAreRefused)
{
	const auto valid = packet();
	for (std::size_t size = 0; size < valid.size(); size++)
		refused(bytes(valid.begin(), valid.begin() + size));
	auto oversized = valid;
	oversized.resize(spectator::maximum_packet_bytes + 1);
	refused(oversized);
	auto trailing = valid;
	trailing.push_back(0);
	refused(trailing);
}

PBD_TEST(PbdSpectatorParserAudit, WrongVersionBindingCountSerialAndReservedFieldsAreRefused)
{
	for (const auto offset : {6u, 8u, 16u, 20u, 24u, 26u, 27u, 28u, 32u, 64u, 98u})
	{
		auto bad = packet();
		bad[offset] = 0xff;
		refused(bad);
	}
}

PBD_TEST(PbdSpectatorParserAudit, NativeFrameCoordinateBoundsAreEnforced)
{
	// The packet's frame count is bounded separately from the source game's native boundary coordinate.
	for (const auto boundary : {spectator::maximum_frames, spectator::maximum_boundary})
	{
		sample_document f;
		auto valid = packet();
		for (unsigned byte = 0; byte < 4; byte++)
			valid[112 + byte] = static_cast<std::uint8_t>(boundary >> (8 * byte));
		f.write(valid);
		check(f.input.poll_input(zero_hash) && f.input.healthy());
		const auto received = f.input.reserve();
		check(received && received->boundary == boundary);
	}
	{
		auto bad = packet();
		std::fill_n(bad.begin() + 112, 4, 0xff); // UINT32_MAX is reserved; maximum_boundary is UINT32_MAX - 1.
		refused(bad);
	}
	for (const auto offset : {117u, 124u})
	{
		auto bad = packet();
		bad[offset] = offset == 124 ? 33 : 1;
		refused(bad);
	}
}

PBD_TEST(PbdSpectatorParserAudit, ReversedBoundariesInsideAChunkAreRefused)
{
	auto bad = packet();
	bad.resize(224);
	std::copy_n(bad.begin() + 96, 64, bad.begin() + 160);
	bad[24] = 2;
	bad[112] = 2;
	bad[176] = 1;
	refused(bad);
}
