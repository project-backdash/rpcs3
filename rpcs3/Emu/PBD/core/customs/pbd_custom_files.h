#pragma once

#include "pbd_custom_wire.h"
#include <filesystem>
#include <fstream>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#endif

namespace pbd::custom_files
{
	using namespace custom_wire;
	using wire_json = pbd::json::json_value;
	inline constexpr std::size_t picture_limit = 1024 * 1024;
	// A reader sees either the previous whole result or the new whole result, including on Windows.
	inline bool commit_result(const std::filesystem::path& file, std::string_view text)
	{
		if (file.empty() || !file.is_absolute() || text.size() > 8192)
			return false;
		auto temporary = file;
		temporary += ".tmp";
		{
			std::ofstream stream(temporary, std::ios::binary | std::ios::trunc);
			stream.write(text.data(), text.size());
			stream.flush();
			stream.close();
			if (!stream)
				return false;
		}
#ifdef _WIN32
		return MoveFileExW(temporary.c_str(), file.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != 0;
#else
		std::error_code error;
		std::filesystem::rename(temporary, file, error);
		return !error;
#endif
	}
	inline std::optional<std::string> read(const std::filesystem::path& path, std::size_t limit)
	{
		std::ifstream stream(path, std::ios::binary);
		if (!stream)
			return {};
		std::string text(limit + 1, '\0');
		stream.read(text.data(), text.size());
		text.resize(static_cast<std::size_t>(stream.gcount()));
		return !stream.bad() && text.size() <= limit ? std::optional(std::move(text)) : std::nullopt;
	}
	struct label
	{
		u32 row = 0, slot = 0;
		std::string name;
		std::vector<u8> picture;
		bool operator==(const label&) const = default;
	};
	struct practice
	{
		std::vector<u8> table;
		std::vector<label> labels;
	};
	inline std::vector<u8> picture(const std::filesystem::path& path)
	{
		if (!path.is_absolute())
			return {};
		auto preferred = path;
		preferred.replace_extension(".jpg");
		auto data = read(preferred, 30 * 1024);
		if (data && data->size() >= 4 && static_cast<u8>((*data)[0]) == 0xff && static_cast<u8>((*data)[1]) == 0xd8 &&
			static_cast<u8>((*data)[data->size() - 2]) == 0xff && static_cast<u8>(data->back()) == 0xd9)
			return {data->begin(), data->end()};
		auto legacy = path;
		legacy.replace_extension(".png");
		data = read(legacy, picture_limit);
		constexpr std::array<u8, 8> signature{137, 80, 78, 71, 13, 10, 26, 10};
		if (!data || data->size() < 24 || !std::equal(signature.begin(), signature.end(), reinterpret_cast<const u8*>(data->data())))
			return {};
		const auto word = [&](std::size_t at)
		{
			const auto* bytes = reinterpret_cast<const u8*>(data->data() + at);
			return u32{bytes[0]} << 24 | u32{bytes[1]} << 16 | u32{bytes[2]} << 8 | bytes[3];
		};
		if (!word(16) || !word(20) || word(16) > 4096 || word(20) > 4096)
			return {};
		return {data->begin(), data->end()};
	}
	inline practice read_practice(const std::filesystem::path& path)
	{
		practice out;
		const auto text = read(path, 256 * 1024);
		if (!text)
			return out;
		const auto parsed = pbd::json::detail::json_parser(*text, 8192, 256 * 1024, 16384).parse();
		if (!parsed.value || !pbd::json::detail::is(parsed.value->find("schema"), "pbd-practice-looks-1"))
			return out;
		out.table = table_field(parsed.value->find("table"));
		const auto* labels = parsed.value->find("labels");
		if (!labels || labels->kind != pbd::json::json_kind::array || labels->items.size() > 350)
			return out;
		for (const auto& entry : labels->items)
		{
			label value;
			if (!pbd::json::detail::number32(entry.find("row"), 0, 84, value.row) ||
				!pbd::json::detail::number32(entry.find("slot"), 1, 10, value.slot) || !resolve({value.slot, out.table}, value.row))
				continue;
			if (const auto* name = entry.find("name"); pbd::json::detail::plain_text(name, 1, 64) &&
													   std::count_if(name->text.begin(), name->text.end(), [](unsigned char byte)
														   {
															   return (byte & 0xc0) != 0x80;
														   }) <= 16)
				value.name = name->text;
			if (const auto* file = entry.find("picture"); file && file->kind == pbd::json::json_kind::string && file->text.size() <= 4096)
				value.picture = picture(std::filesystem::u8path(file->text));
			out.labels.push_back(std::move(value));
		}
		if (const auto* root = parsed.value->find("costumePictures"); root && root->kind == pbd::json::json_kind::string && root->text.size() <= 4096)
			for (u32 row = 0; row < 84; ++row)
			{
				const auto file = std::filesystem::u8path(root->text) / ("costume-" + std::string(row < 10 ? "0" : "") + std::to_string(row) + ".png");
				out.labels.push_back({row, 0, {}, picture(file)});
			}
		return out;
	}
	inline constexpr std::array<std::string_view, 35> character_ids{
		"paul_phoenix", "marshall_law", "lei_wulong", "king", "yoshimitsu", "nina_williams", "hwoarang",
		"ling_xiaoyu", "christie_monteiro", "jin_kazama", "julia_chang", "kuma", "bryan_fury", "heihachi_mishima",
		"kazuya_mishima", "lee_chaolan", "steve_fox", "craig_marduk", "mokujin", "jack_5", "roger_jr",
		"anna_williams", "wang_jinrei", "ganryu", "asuka_kazama", "bruce_irvin", "baek_doo_san", "devil_jin",
		"raven", "feng_wei", "armor_king", "lili", "sergei_dragunov", "eddy_gordo", "jinpachi_mishima"};
	inline std::vector<drawn> read_picture_looks(const std::filesystem::path& path)
	{
		std::vector<drawn> out;
		const auto text = read(path, 64 * 1024);
		if (!text)
			return out;
		const auto parsed = pbd::json::detail::json_parser(*text, 4096, 64 * 1024, 4096).parse();
		if (!parsed.value || !pbd::json::detail::is(parsed.value->find("schema"), "pbd-custom-pictures-1"))
			return out;
		const auto* looks = parsed.value->find("looks");
		if (!looks || looks->kind != pbd::json::json_kind::array || looks->items.size() > 350)
			return out;
		for (const auto& value : looks->items)
			for (u32 row = 0; row < 84; ++row)
				if (auto look = read_drawn(&value, row))
				{
					out.push_back(*look);
					break;
				}
		if (const auto* defaults = parsed.value->find("captureDefaults"); defaults && defaults->kind == pbd::json::json_kind::boolean && defaults->truth)
			for (u32 row = 0; row < 84; ++row)
				if (custom_look::normalise(custom_look::look{row}))
					out.push_back({0, pack(custom_look::look{row})});
		return out;
	}
	// Online tables establish the worn look; only this PC's matching slot may supply its local picture.
	template <typename Cards>
	void read_card_pictures(const std::filesystem::path& index, Cards& cards)
	{
		if (!index.is_absolute() || index.filename() != "picture-looks.json")
			return;
		const auto looks = read_picture_looks(index);
		const auto root = index.parent_path() / "photos";
		for (auto& card : cards)
		{
			card.picture.clear();
			const auto* row = custom_look::catalogue_row(card.look.row);
			if (!row)
				continue;
			const u32 slot = card.slot ? *card.slot + 1 : 0;
			if (slot && std::find(looks.begin(), looks.end(), drawn{slot, pack(card.look)}) == looks.end())
				continue;
			const auto number = slot ? slot : card.look.row;
			const auto name = slot ? std::string(character_ids[row->fighter]) : std::string("costume");
			card.picture = picture(root / (name + "-" + (number < 10 ? "0" : "") + std::to_string(number) + ".jpg"));
		}
	}
	struct customize
	{
		std::string id, character;
		u32 slot = 0;
		look start;
		std::array<bool, 10> filled{};
		std::filesystem::path picture_dir;
	};
	inline std::optional<customize> parse_customize(std::string_view text)
	{
		const auto parsed = pbd::json::detail::json_parser(text, 128, 8192, 4096).parse();
		if (!parsed.value)
			return {};
		const auto& value = *parsed.value;
		customize out;
		const auto *id = value.find("id"), *character = value.find("characterId"), *bytes = value.find("bytes"),
				   *filled = value.find("filled"), *directory = value.find("pictureDir");
		if (!pbd::json::detail::is(value.find("schema"), "pbd-customize-request-1") || value.fields.size() != 7 ||
			!id || id->kind != pbd::json::json_kind::string || id->text.size() != 36 ||
			!character || character->kind != pbd::json::json_kind::string ||
			!pbd::json::detail::number32(value.find("slot"), 1, 10, out.slot) ||
			!bytes || bytes->kind != pbd::json::json_kind::string || bytes->text.size() != 24 ||
			!filled || filled->kind != pbd::json::json_kind::array || filled->items.size() > 10 ||
			!directory || directory->kind != pbd::json::json_kind::string || directory->text.size() > 4096)
			return {};
		for (std::size_t at = 0; at < id->text.size(); ++at)
			if ((at == 8 || at == 13 || at == 18 || at == 23) ? id->text[at] != '-' : std::string_view("0123456789abcdef").find(id->text[at]) == std::string_view::npos)
				return {};
		std::optional<drawn> packed;
		const auto shape = wire_json::object({{"slot", wire_json::number(out.slot)}, {"bytes", *bytes}});
		for (u32 row = 0; row < 84 && !packed; ++row)
			packed = read_drawn(&shape, row);
		if (!packed)
			return {};
		out.start = *unpack(packed->value);
		const auto* row = catalogue_row(out.start.row);
		if (!row->dressable || row->fighter >= character_ids.size() || (character->text != character_ids[row->fighter] && !(out.start.row == 31 && character->text == "panda")) || !shop_blocks(out.start))
			return {};
		u32 previous = 0;
		for (const auto& item : filled->items)
		{
			u32 slot = 0;
			if (!pbd::json::detail::number32(&item, previous + 1, 10, slot))
				return {};
			out.filled[slot - 1] = true;
			previous = slot;
		}
		out.id = id->text;
		out.character = character->text;
		out.picture_dir = std::filesystem::u8path(directory->text);
		return out.picture_dir.is_absolute() ? std::optional(std::move(out)) : std::nullopt;
	}
	inline std::string result(const customize& request, std::string state, u32 slot = 0,
		const std::optional<look>& value = {}, bool photo = false)
	{
		auto out = wire_json::object({{"schema", wire_json::string("pbd-customize-result-1")}, {"id", wire_json::string(request.id)}, {"state", wire_json::string(state)}});
		if (state == "saved" && value && slot >= 1 && slot <= 10 && normalise(*value) &&
			catalogue_row(value->row)->fighter == catalogue_row(request.start.row)->fighter)
		{
			out.fields.emplace_back("slot", wire_json::number(slot));
			out.fields.emplace_back("bytes", wire_json::string(hex(pack(*value))));
			if (photo)
				out.fields.emplace_back("picture", wire_json::string("photo.png"));
		}
		else if (state == "saved")
			out.fields[2].second = wire_json::string("unavailable");
		return pbd::json::detail::serialize(out) + "\n";
	}
} // namespace pbd::custom_files
