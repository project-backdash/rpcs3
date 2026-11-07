#include "pbd_test.h"
#include "Emu/PBD/pbd_custom_wire.h"
#include "Emu/PBD/pbd_peer_message.h"
#include "Emu/PBD/pbd_set_channel_model.h"
#include "Emu/PBD/pbd_custom_files.h"
#include "Emu/PBD/pbd_custom_save.h"
#include "Emu/PBD/pbd_custom_photo.h"

namespace
{
	using namespace pbd::custom_wire;
	using pbd::test::check;
	std::string base64(std::span<const u8> value)
	{
		constexpr char digits[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
		std::string out;
		for (std::size_t at = 0; at < value.size(); at += 3)
		{
			const u32 word = u32{value[at]} << 16 | u32{value[at + 1]} << 8 | value[at + 2];
			for (const u32 shift : {18u, 12u, 6u, 0u})
				out += digits[(word >> shift) & 63];
		}
		return out;
	}
	std::vector<u8> table(u32 row = 77, u32 slot = 3)
	{
		std::vector<u8> out(block_bytes, 255);
		const auto look = pack({row, {50, 61, 66, 67}, {-1, 3, 11}});
		std::copy(look.begin(), look.end(), out.begin() + (slot - 1) * look_bytes);
		return out;
	}
} // namespace

PBD_TEST(PbdCustomOnline, TablesAreCanonicalBoundedWholeBlocks)
{
	const auto good = table();
	check(read_table(base64(good)) == good);
	for (const auto& bad : std::array<std::string, 5>{"", base64(good) + "=", base64(good) + "\n", base64(good).substr(1), std::string(160, '/')})
		check(!read_table(bad));
	auto invalid = good;
	invalid[2 * look_bytes + 1] = 0;
	invalid[2 * look_bytes + 2] = 0;
	check(!read_table(base64(invalid)));
	std::vector<u8> maximum;
	for (u32 block = 0; block < 35; ++block)
		maximum.insert(maximum.end(), good.begin(), good.end());
	check(read_table(base64(maximum)) == maximum);
	maximum.insert(maximum.end(), good.begin(), good.end());
	check(!read_table(base64(maximum)));
}

PBD_TEST(PbdCustomOnline, SlotResolutionIsSideLocalAndUnambiguous)
{
	const auto left = table(), right = table(78, 4);
	check(resolve({3, left}, 77));
	check(!resolve({0, left}, 77) && !resolve({11, left}, 77));
	check(!resolve({3, right}, 77) && !resolve({3, left}, 78));
	auto duplicate = left;
	duplicate.insert(duplicate.end(), left.begin(), left.end());
	check(!resolve({3, duplicate}, 77));
	const auto cards = choices(left, 31);
	check(cards[2] && cards[2]->row == 77 && cards[2]->items[0] == 61);
	check(!choices(left, 5)[2]);
	const auto capsule = json::object({{"selection", json::object({{"leftRow", json::number(77)}, {"rightRow", json::number(78)}})},
		{"players", json::array({json::object({{"look", drawn_json(*resolve({3, left}, 77))}}), json::object({})})}});
	check(capsule_matches_tables(capsule, {left, right}));
	check(!capsule_matches_tables(capsule, {right, left}));
}

PBD_TEST(PbdCustomOnline, BothPeersBuildIdenticalCardsFromTheAgreedBytes)
{
	const auto first = read_table(base64(table()));
	const auto second = read_table(base64(table()));
	check(first && second);
	const auto a = resolve({3, *first}, 77), b = resolve({3, *second}, 77);
	check(a == b);
	std::array<u8, blocks_size> saved{};
	check(record_blocks(saved, *unpack(a->value)) == record_blocks(saved, *unpack(b->value)));
	auto unknown = a->value;
	unknown[1] = 0x7f;
	unknown[2] = 0xff;
	check(unpack(unknown));
	check(record_blocks(saved, *unpack(unknown)) == record_blocks(saved, look{77}));
}

PBD_TEST(PbdCustomOnline, ManifestSelectReadsTablesWithoutASecondLocalSwitch)
{
	using namespace pbd::set_channel;
	capabilities supported{"build", "compat", {77}, {23}};
	const auto value = json::object({{"memberId", json::string("one")}, {"name", json::string("One")},
		{"lookTable", json::string(base64(table()))}});
	fighter selected;
	check(parse_fighter(&value, selected, supported, true));
	check(selected.custom_fields && selected.look_table == table() && !selected.look);
	auto invalid = value;
	invalid.fields.emplace_back("look", drawn_json({3, pack(look{77})}));
	check(parse_fighter(&invalid, selected, supported, true));
	check(selected.look_table.empty() && !selected.look);
}

PBD_TEST(PbdCustomOnline, LocalSwitchRequiresExactlyOneAndAbsentOnlineFieldsStayAbsent)
{
	const char* current = pbd::env::find("PBD_CUSTOM_LOOKS");
	const std::string previous = current ? current : "";
	const auto put = [](const char* value)
	{
#ifdef _WIN32
		_putenv_s("PBD_CUSTOM_LOOKS", value);
#else
		if (*value)
			setenv("PBD_CUSTOM_LOOKS", value, 1);
		else
			unsetenv("PBD_CUSTOM_LOOKS");
#endif
	};
	struct restore
	{
		decltype(put)& write;
		const std::string& value;
		~restore()
		{
			write(value.c_str());
		}
	} guard{put, previous};
	for (const char* value : {"", "0", "true", "01", "1 "})
	{
		put(value);
		check(!enabled());
	}
	put("1");
	check(enabled());
	using namespace pbd::set_channel;
	capabilities supported{"build", "compat", {77}, {23}};
	const auto input = json::object({{"memberId", json::string("one")}, {"name", json::string("One")}});
	fighter selected;
	check(parse_fighter(&input, selected, supported, true));
	check(!selected.custom_fields && !selected.look && selected.look_table.empty());
}

PBD_TEST(PbdCustomOnline, OptionalSegmentSlotsRoundTripAndDefaultToTheOriginalWire)
{
	using namespace pbd::peer_message;
	input_message original;
	original.sync.session = 42;
	original.segment = {4, 60, 2, 40, 3};
	std::array<u8, max_input_bytes> baseline{}, custom{}, again{};
	const auto ordinary = encode_input(original, baseline);
	check(ordinary == 204 && detail::load<u32>(baseline, input_head::version) == 12);
	original.segment.look_slot = 3;
	original.segment.prev_look_slot = 7;
	const auto size = encode_input(original, custom);
	check(size == ordinary + 8 && detail::load<u32>(custom, input_head::version) == 13);
	input_message decoded;
	check(read_input({custom.data(), size}, 1, 42, decoded) == input_check::valid && decoded == original);
	original.segment.look_slot = original.segment.prev_look_slot = 0;
	check(encode_input(original, again) == ordinary && again == baseline);
	check(read_input({baseline.data(), ordinary}, 1, 42, decoded) == input_check::valid);
	check(decoded.segment.look_slot == 0 && decoded.segment.prev_look_slot == 0);
}

PBD_TEST(PbdCustomFiles, CustomizeUsesTheLaunchersPackedLookAndExactResultKeys)
{
	const std::string request = R"({"schema":"pbd-customize-request-1","id":"01234567-89ab-cdef-0123-456789abcdef","characterId":"lili","slot":3,"filled":[3,4],"bytes":"4d003d004300420032ff030b","pictureDir":"C:\\customize-photo"})";
	const auto parsed = pbd::custom_files::parse_customize(request);
	check(parsed && parsed->slot == 3 && parsed->filled[2] && parsed->filled[3]);
	check(parsed->start.row == 77 && parsed->start.items[0] == 61);
	const auto text = pbd::custom_files::result(*parsed, "saved", 5, parsed->start, true);
	const auto result = pbd::json::detail::json_parser(text).parse();
	check(result.value && result.value->fields.size() == 6);
	check(result.value->find("slot")->text == "5" && result.value->find("bytes")->text == "4d003d004300420032ff030b");
	check(result.value->find("picture")->text == "photo.png");
	check(pbd::custom_files::result(*parsed, "cancelled").find("bytes") == std::string::npos);
	check(pbd::custom_files::result(*parsed, "saved", 5, look{0}).find("unavailable") != std::string::npos);
	for (const auto pair : {std::pair{"[3,4]", "[4,3]"}, {"[3,4]", "[3,3]"}, {"\"slot\":3", "\"slot\":11"},
			 {"lili", "nina_williams"}, {"4d003d", "4d0000"}, {"C:\\\\customize-photo", "relative"}})
	{
		auto invalid = request;
		const auto at = invalid.find(pair.first);
		check(at != std::string::npos);
		invalid.replace(at, std::string_view(pair.first).size(), pair.second);
		check(!pbd::custom_files::parse_customize(invalid));
	}
	check(!pbd::custom_files::parse_customize(request + std::string(8192, ' ')));
}

PBD_TEST(PbdCustomFiles, ReplacingAResultIsAtomicAndBounded)
{
	const auto path = std::filesystem::temp_directory_path() / "pbd-customize-result-native-test.json";
	check(pbd::custom_files::commit_result(path, "old"));
	check(pbd::custom_files::commit_result(path, "new"));
	check(pbd::custom_files::read(path, 8) == "new");
	check(!pbd::custom_files::commit_result(path, std::string(8193, 'x')));
	check(pbd::custom_files::read(path, 8) == "new");
	std::error_code error;
	std::filesystem::remove(path, error);
}

PBD_TEST(PbdCustomFiles, PracticeLabelsStayLocalAndCannotCreateATableLook)
{
	const auto path = std::filesystem::temp_directory_path() / "pbd-practice-looks-native-test.json";
	const auto write = [&](const std::string& text)
	{
		std::ofstream file(path, std::ios::binary | std::ios::trunc);
		file << text;
		file.close();
		check(file);
	};
	write(R"({"schema":"pbd-practice-looks-1","table":")" + base64(table()) +
		  R"(","labels":[{"row":77,"slot":3,"name":"Rose"},{"row":77,"slot":4,"name":"Missing"}]})");
	const auto read = pbd::custom_files::read_practice(path);
	check(read.table == table() && read.labels.size() == 1);
	check(read.labels[0].name == "Rose" && read.labels[0].picture.empty());
	write(R"({"schema":"pbd-practice-looks-1","table":"malformed","labels":[{"row":77,"slot":3,"name":"Rose"}]})");
	const auto invalid = pbd::custom_files::read_practice(path);
	check(invalid.table.empty() && invalid.labels.empty());
	std::error_code error;
	std::filesystem::remove(path, error);
}

PBD_TEST(PbdCustomFiles, SavePanelKeepsEditingAndConfirmsAnOccupiedDestinationOnce)
{
	using namespace pbd::custom_editor;
	save_panel panel;
	std::array<bool, 10> filled{};
	filled[4] = true;
	check(panel.begin(4, filled));
	panel.step(0);
	panel.photo_ready(true);
	panel.step(0x2000);
	check(panel.slot() == 5);
	panel.step(0);
	check(panel.step(0x40) == save_action::none && panel.overwrite());
	panel.step(0);
	panel.step(0x2000);
	panel.step(0);
	check(panel.step(0x40) == save_action::save);
	check(panel.begin(3, {}));
	panel.step(0);
	check(panel.step(0x20) == save_action::keep_editing);
}

PBD_TEST(PbdCustomFiles, PortraitHasOneFixedCropAndABoundedPixelCount)
{
	using namespace pbd::custom_photo;
	frame source{7, 8, 9, 1280, 720, true};
	source.rgba.resize(source.width * source.height * 4, 255);
	const auto cropped = portrait(source);
	check(cropped.valid && cropped.width == 320 && cropped.height == 480 && cropped.rgba.size() == 320 * 480 * 4);
	check(cropped.ticket == 7 && cropped.generation == 8 && cropped.flip == 9);
	check(std::all_of(cropped.rgba.begin(), cropped.rgba.end(), [](auto byte)
		{
			return byte == 255;
		}));
	source.valid = false;
	check(!portrait(source).valid);
}

PBD_TEST(PbdCustomFiles, CompactPicturesLoadForCustomAndDefaultCards)
{
	using namespace pbd::custom_photo;
	frame source{1, 1, 1, 320, 480, true};
	source.rgba.resize(320 * 480 * 4);
	u32 seed = 1;
	for (auto& byte : source.rgba)
	{
		seed = seed * 1664525u + 1013904223u;
		byte = seed >> 24;
	}
	const auto jpeg = encode_jpeg(source);
	check(jpeg.size() >= 4 && jpeg.size() <= 30 * 1024 && jpeg[0] == 0xff && jpeg[1] == 0xd8);
	const auto root = std::filesystem::temp_directory_path() / "pbd-compact-costume-native-test";
	std::filesystem::create_directories(root);
	for (const auto* name : {"lili-03.jpg", "costume-77.jpg"})
	{
		std::ofstream file(root / name, std::ios::binary | std::ios::trunc);
		file.write(reinterpret_cast<const char*>(jpeg.data()), jpeg.size());
		file.close();
		check(file);
	}
	check(pbd::custom_files::picture(root / "lili-03.png") == jpeg);
	const auto path = root / "practice-looks.json";
	{
		std::ofstream file(path, std::ios::binary | std::ios::trunc);
		file << "{\"schema\":\"pbd-practice-looks-1\",\"table\":\"" << base64(table()) << "\",\"labels\":[{\"row\":77,\"slot\":3,\"name\":\"Rose\",\"picture\":" << pbd::json::quoted((root / "lili-03.jpg").string()) << "}],\"costumePictures\":" << pbd::json::quoted(root.string()) << "}";
	}
	const auto read = pbd::custom_files::read_practice(path);
	check(read.labels.size() == 85 && read.labels[0].picture == jpeg && read.labels[78].picture == jpeg);
	std::error_code error;
	for (const auto* name : {"lili-03.jpg", "costume-77.jpg", "practice-looks.json"})
		std::filesystem::remove(root / name, error);
	std::filesystem::remove(root, error);
}

PBD_TEST(PbdCustomFiles, FirstUsePicturesKeepTheExactSlotAndCostumeIdentity)
{
	const auto path = std::filesystem::temp_directory_path() / "pbd-picture-looks-native-test.json";
	{
		std::ofstream file(path);
		file << R"({"schema":"pbd-custom-pictures-1","looks":[{"slot":3,"bytes":"0d0004ffffffffffff02ffff"},{"slot":11,"bytes":"0d0004ffffffffffff02ffff"},{"slot":4,"bytes":"0d0004ffffffffffff02ffff"}]})";
	}
	const auto read = pbd::custom_files::read_picture_looks(path);
	check(read.size() == 2 && read[0].slot == 3 && read[1].slot == 4);
	check(read[0].value[0] == 13 && read[0].value[2] == 4 && read[0].value[9] == 2);
	{
		std::ofstream file(path);
		file << R"({"schema":"pbd-custom-pictures-1","looks":[],"captureDefaults":true})";
	}
	const auto defaults = pbd::custom_files::read_picture_looks(path);
	check(defaults.size() == 84);
	check(defaults[13].slot == 0 && defaults[13].value == pbd::custom_wire::pack(pbd::custom_look::look{13}));
	{
		std::ofstream file(path);
		file << R"({"schema":"pbd-custom-pictures-2","looks":[]})";
	}
	check(pbd::custom_files::read_picture_looks(path).empty());
	std::error_code error;
	std::filesystem::remove(path, error);
}
