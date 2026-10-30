#pragma once

// Private state and helpers shared by the scenarios in spectator_channel_test.cpp.
namespace
{
	using namespace pbd::spectator;
	digest test_hash(const std::uint8_t* bytes, std::size_t size)
	{
		digest out{};
		for (std::size_t i = 0; i < size; ++i)
			out[i % out.size()] = static_cast<std::uint8_t>((out[i % out.size()] * 33u) ^ bytes[i]);
		return out;
	}
	void put(std::vector<std::uint8_t>& bytes, std::size_t at, u64 value, unsigned count)
	{
		for (unsigned i = 0; i < count; ++i)
			bytes[at + i] = static_cast<std::uint8_t>(value >> (8 * i));
	}
	// A fixed set binds stage 17 and rows 12/32 for every game; an in-game set binds none.
	manifest identity(bool in_game = false)
	{
		manifest m;
		m.generation = 123;
		m.seed = 77;
		m.rounds = 3;
		if (in_game)
			m.in_game = true;
		else
		{
			m.stage = 17;
			m.left = 12;
			m.right = 32;
		}
		m.binding.fill(1);
		m.build = "test-build";
		m.compat = "test-compat";
		m.title = "NPUB30009";
		m.emulator = std::string(64, 'a');
		m.checkpoint = std::string(64, 'b');
		m.recipe = std::string(64, 'c');
		m.applied = true;
		return m;
	}
	const std::string fixed_picks = "\"stageId\":17,\"leftCharacterRow\":12,\"rightCharacterRow\":32,";
	std::optional<std::optional<picks>> parse_picks(const std::string& text)
	{
		const auto parsed = pbd::set_channel::detail::json_parser(text).parse();
		check(parsed.value);
		std::optional<picks> out;
		if (!read_picks(*parsed.value, out))
			return {};
		return out;
	}
	struct port
	{
		bool injected = true, posted = true, stored_held_read = true, stored_rising_read = true;
		u32 injected_r3 = 0, stored_held = 0, stored_rising = 0;
		u64 raw = 0, effective = 0, held = 0, rising = 0;
	};
	struct row
	{
		bool entry_seen = true, completed = true, before_read = true, after_read = true;
		const char* failure = nullptr;
		const char* soft = nullptr;
		std::array<port, 2> ports{};
	};
	struct sample_document
	{
		std::filesystem::path parent = std::filesystem::canonical(std::filesystem::temp_directory_path());
		std::filesystem::path directory;
		local_channel input;
		u32 controls = 0;
		explicit sample_document(unsigned id, bool in_game = false, std::optional<manifest> selected = {})
			: directory(parent / ("pbd-spectator-model-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + "-" + std::to_string(id))),
			  input(make_directory(), selected.value_or(identity(in_game))) {}
		std::filesystem::path make_directory()
		{
			check(std::filesystem::create_directory(directory));
			return directory;
		}
		~sample_document()
		{
			const auto resolved = std::filesystem::canonical(directory);
			check(std::filesystem::equivalent(resolved.parent_path(), parent) && resolved.filename().string().starts_with("pbd-spectator-model-"));
			std::filesystem::remove_all(resolved);
		}
		void write(const std::string& name, const std::string& text)
		{
			std::ofstream out(directory / name, std::ios::binary);
			out.write(text.data(), text.size());
			out.close();
			check(out);
		}
		bool control(const std::string& tail, bool valid = true, const pbd::rematch::options* actual = nullptr)
		{
			++controls;
			write("spectator-control-" + std::to_string(controls) + ".json",
				"{\"schema\":\"pbd-spectator-control-1\",\"generation\":\"123\",\"bindingDigest\":\"" + hex(identity().binding) +
					"\",\"serial\":" + std::to_string(controls) + "," + tail + "}");
			const auto result = input.poll_control(actual);
			check(result == valid);
			return result;
		}
		void begin(u32 game = 1, u32 origin = 0, const native_round* round = nullptr, bool valid = true, const std::string& picks_fields = fixed_picks)
		{
			control("\"type\":\"begin_game\",\"gameNumber\":" + std::to_string(game) + ",\"originBoundary\":" + std::to_string(origin) +
						",\"originCounter\":2147483725,\"kind\":\"" + (game == 1 ? "clean_character_select" : "natural_vs") +
						"\"," + picks_fields + "\"roundsPerGame\":3" +
						(round ? ",\"nativeRound\":" + pbd::set_channel::detail::serialize(round->json()) : ""),
				valid);
		}
		bool game_picks(u32 game, u32 stage, u32 left, u32 right, bool valid = true)
		{
			return control("\"type\":\"game_picks\",\"gameNumber\":" + std::to_string(game) + ",\"stageId\":" + std::to_string(stage) +
							   ",\"leftCharacterRow\":" + std::to_string(left) + ",\"rightCharacterRow\":" + std::to_string(right),
				valid);
		}
		// The report with this serial, as the native consumer wrote it.
		std::string report(u32 serial)
		{
			std::ifstream in(directory / ("spectator-native-" + std::to_string(serial) + ".json"), std::ios::binary);
			check(in);
			return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
		}
		void prepare()
		{
			check(input.prepare(9, 0x01000000, 0, 0x8000004d, identity().checkpoint, identity().emulator));
		}
		void packet(u32 serial, u32 game, u32 first, u32 boundary, u32 count = 2, bool corrupt = false, u32 raw_left = 0x40)
		{
			std::vector<std::uint8_t> bytes(header_bytes + count * applied_frame_bytes);
			constexpr std::array<std::uint8_t, 8> magic{'P', 'B', 'D', 'S', 'P', 'C', '1', 0};
			std::copy(magic.begin(), magic.end(), bytes.begin());
			put(bytes, 8, 123, 8);
			put(bytes, 16, game, 4);
			put(bytes, 20, first, 4);
			put(bytes, 24, count, 2);
			put(bytes, 26, 1, 2);
			put(bytes, 28, serial, 4);
			const auto binding = identity().binding;
			std::copy(binding.begin(), binding.end(), bytes.begin() + 32);
			for (u32 i = 0; i < count; ++i)
			{
				const auto offset = header_bytes + i * applied_frame_bytes;
				put(bytes, offset, raw_left, 2);
				put(bytes, offset + 8, 0x20, 2);
				put(bytes, offset + 16, boundary + i, 4);
				put(bytes, offset + 20, 1, 4);
				put(bytes, offset + 24, 0x8000004d + boundary + i, 4);
				put(bytes, offset + 28, 17, 4);
				put(bytes, offset + 32, 7, 4);
				put(bytes, offset + 40, 11, 8);
				put(bytes, offset + 48, 22, 8);
				put(bytes, offset + 56, 33, 8);
				put(bytes, offset + 64, 0x40, 2);
				put(bytes, offset + 72, 0x20, 2);
			}
			auto hash = test_hash(bytes.data() + header_bytes, count * applied_frame_bytes);
			if (corrupt)
				hash[0] ^= 1;
			std::copy(hash.begin(), hash.end(), bytes.begin() + 64);
			std::ofstream out(directory / ("spectator-input-" + std::to_string(serial) + ".bin"), std::ios::binary);
			out.write(reinterpret_cast<const char*>(bytes.data()), bytes.size());
			out.close();
			check(out);
		}
		row completed()
		{
			row result;
			result.ports[0].injected_r3 = 0x40;
			result.ports[0].raw = result.ports[0].effective = 0x40;
			result.ports[1].injected_r3 = 0x20;
			result.ports[1].raw = result.ports[1].effective = 0x20;
			return result;
		}
		void play(u32 boundary, bool expose = false, picks applied = {17, 12, 32})
		{
			const auto frame = input.reserve();
			check(frame && frame->boundary == boundary);
			check(input.verify_state(boundary, 1, 17, 0x8000004d + boundary, 7, {11, 22, 33}));
			if (expose)
				check(input.expose(0x01000000, boundary, applied.stage, applied.left, applied.right, 3, true));
			check(input.commit(0x01000000, 0x12efc, boundary, completed(), {0x40, 0x20}));
		}
		// The intro readback of one game with these picks, which the channel must refuse.
		void refused_intro(u32 boundary, picks applied, std::string_view reason)
		{
			const auto frame = input.reserve();
			check(frame && frame->boundary == boundary);
			check(input.verify_state(boundary, 1, 17, 0x8000004d + boundary, 7, {11, 22, 33}));
			check(!input.expose(0x01000000, boundary, applied.stage, applied.left, applied.right, 3, true));
			check(input.reason() == reason && load_presentation(9).hidden && input.consumed() == 0);
		}
	};
} // namespace
