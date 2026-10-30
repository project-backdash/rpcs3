#pragma once

#include "pbd_env.h"
#include "pbd_json.h"
#include "pbd_held_presentation.h"
#include "pbd_spectator_standby.h"
#include "pbd_spectator_channel_model.h"
#include "../Cell/pbd_set_channel.h"
#include <charconv>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
#include <string_view>

namespace pbd::replay_standby
{
	inline constexpr std::uint32_t pbd_history_extent = 100000 + 1024;
	inline constexpr std::string_view pbd_schema = "pbd-replay-standby-bind-1";
	inline constexpr std::string_view pbd_bind_file = "replay-standby-bind.json";
	inline constexpr std::string_view pbd_bound_file = "replay-standby-bound.json";
	inline constexpr std::string_view pbd_presentation_file = "replay-standby-presentation.json";
	inline constexpr std::string_view pbd_presentation_schema = "pbd-replay-standby-presentation-1";
	inline constexpr std::string_view pbd_picture_file = "replay-standby-picture.json";
	inline constexpr std::string_view pbd_picture_schema = "pbd-replay-standby-picture-1";
	inline bool pbd_requested() noexcept
	{
		const char* value = pbd::env::find("PBD_REPLAY_STANDBY");
		return value && std::string_view(value) == "1";
	}
	inline bool pbd_shared() noexcept
	{
		const char* value = pbd::env::find("PBD_REPLAY_STANDBY_SHARED");
		return pbd_requested() && value && std::string_view(value) == "1";
	}
	struct pbd_binding
	{
		std::uint64_t generation = 0;
		std::uint32_t seed = 0;
		std::filesystem::path file;
		std::string checkpoint, emulator, recipe, replay;
	};
	struct pbd_read_result
	{
		std::optional<pbd_binding> value;
		const char* error = "replay_standby_bind_invalid";
	};
	inline std::string pbd_bound_text(const pbd_binding& value)
	{
		return "{\"schema\":" + pbd::json::quoted(pbd_schema) + ",\"type\":\"bound\",\"nativeGeneration\":\"" +
		       std::to_string(value.generation) + "\",\"seed\":" + std::to_string(value.seed) +
		       ",\"checkpointSha256\":" + pbd::json::quoted(value.checkpoint) +
		       ",\"emulatorSha256\":" + pbd::json::quoted(value.emulator) +
		       ",\"recipeSha256\":" + pbd::json::quoted(value.recipe) +
		       ",\"replaySha256\":" + pbd::json::quoted(value.replay) + "}\n";
	}
	inline std::string pbd_picture_text(const pbd_binding& value, const pbd::held_presentation::lifecycle& presentation)
	{
		if (!presentation.can_show() || !presentation.picture_us())
			return {};
		auto text = pbd_bound_text(value);
		text.replace(text.find(pbd_schema), pbd_schema.size(), pbd_picture_schema);
		text.replace(text.find("\"type\":\"bound\""), 14, "\"type\":\"picture-ready\"");
		text.resize(text.size() - 2);
		return text + ",\"startBoundary\":" + std::to_string(presentation.boundary()) +
		       ",\"pictureElapsedUs\":" + std::to_string(presentation.picture_us()) + "}\n";
	}
	inline std::string pbd_presentation_text(const pbd_binding& value, const pbd::held_presentation::lifecycle& presentation)
	{
		if (!presentation.ready())
			return {};
		auto text = pbd_bound_text(value);
		text.replace(text.find(pbd_schema), pbd_schema.size(), pbd_presentation_schema);
		text.replace(text.find("\"type\":\"bound\""), 14, "\"type\":\"presentation-ready\"");
		text.resize(text.size() - 2);
		return text + ",\"startBoundary\":" + std::to_string(presentation.boundary()) +
		       ",\"pictureElapsedUs\":" + std::to_string(presentation.picture_us()) +
		       ",\"audioElapsedUs\":" + std::to_string(presentation.audio_us()) +
		       ",\"outputGain\":" + std::to_string(presentation.gain()) + "}\n";
	}
	struct pbd_presentation_report
	{
		std::filesystem::path root;
		std::optional<pbd_binding> binding;
		bool published = false;
		bool picture_published = false;
	};
	// Written and polled by the input boundary's owner. Render/audio callbacks publish only atomic observations.
	inline pbd_presentation_report& pbd_report()
	{
		static pbd_presentation_report value;
		return value;
	}
	inline bool pbd_bind_presentation(const std::filesystem::path& root, const pbd_binding& binding,
		pbd::held_presentation::lifecycle& presentation = pbd::held_presentation::output())
	{
		if (!presentation.bound())
			return false;
		pbd_report() = {root, binding, false};
		pbd::spectator::presentation live;
		live.active = false;
		pbd::spectator::publish_presentation(binding.generation, live);
		return true;
	}
	inline bool pbd_publish_presentation()
	{
		auto& report = pbd_report();
		if (!report.binding || report.published)
			return true;
		// A visible picture is useful even when the output device has not acknowledged audio yet.
		if (!report.picture_published)
		{
			const auto picture = pbd_picture_text(*report.binding, pbd::held_presentation::output());
			if (!picture.empty())
			{
				report.picture_published = pbd::spectator_standby::publish(report.root / pbd_picture_file, picture);
				if (!report.picture_published)
					return false;
			}
		}
		const auto text = pbd_presentation_text(*report.binding, pbd::held_presentation::output());
		if (text.empty())
			return true;
		report.published = pbd::spectator_standby::publish(report.root / pbd_presentation_file, text);
		return report.published;
	}
	inline pbd_read_result pbd_parse(std::string_view text)
	{
		using namespace pbd::set_channel;
		if (text.empty() || text.size() > 8192)
			return {};
		auto parsed = detail::json_parser(text, 64).parse();
		if (!parsed.value || parsed.value->kind != json_kind::object || parsed.value->fields.size() != 8)
			return {};
		const auto& root = *parsed.value;
		if (!detail::is(root.find("schema"), pbd_schema))
			return {};
		pbd_binding result;
		std::uint64_t seed = 0;
		const auto* generation = root.find("nativeGeneration");
		const auto* file = root.find("replayFile");
		if (!generation || generation->kind != json_kind::string || !file || file->kind != json_kind::string ||
			file->text.empty() || file->text.size() > 4096 || !detail::valid_utf8(file->text) ||
			file->text.find('\0') != std::string::npos || !detail::number(root.find("seed"), 1, 0xffffffffu, seed))
			return {};
		result.seed = static_cast<std::uint32_t>(seed);
		const auto token = std::from_chars(generation->text.data(), generation->text.data() + generation->text.size(), result.generation);
		if (token.ec != std::errc{} || token.ptr != generation->text.data() + generation->text.size() || !result.generation)
			return {};
		const auto digest = [&](const char* name, std::string& out)
		{
			const auto* value = root.find(name);
			if (!value || value->kind != json_kind::string || value->text.size() != 64 ||
				value->text.find_first_not_of("0123456789abcdef") != std::string::npos)
				return false;
			out = value->text;
			return true;
		};
		if (!digest("checkpointSha256", result.checkpoint) || !digest("emulatorSha256", result.emulator) ||
			!digest("recipeSha256", result.recipe) || !digest("replaySha256", result.replay))
			return {};
		result.file = std::filesystem::u8path(file->text);
		if (!result.file.is_absolute() || result.file.extension() != ".pbdreplay")
			return {};
		return {std::move(result), ""};
	}
	inline pbd_read_result pbd_read(const std::filesystem::path& root)
	{
		const auto file = root / pbd_bind_file;
		std::error_code error;
		if (!std::filesystem::exists(file, error))
			return {{}, error ? "read_failed" : "unavailable"};
		const auto size = std::filesystem::file_size(file, error);
		if (error)
			return {{}, "read_failed"};
		if (!size || size > 8192)
			return {};
		std::ifstream stream(file, std::ios::binary);
		std::string text(static_cast<std::size_t>(size) + 1, '\0');
		stream.read(text.data(), static_cast<std::streamsize>(text.size()));
		if (stream.bad() || stream.gcount() != static_cast<std::streamsize>(size))
			return {{}, "read_failed"};
		text.resize(static_cast<std::size_t>(size));
		return pbd_parse(text);
	}
	template <typename Ready>
	const char* pbd_identity_refusal(const pbd_binding& value, const Ready& ready) noexcept
	{
		return value.generation != ready.native_generation ? "replay_standby_generation" :
		       value.checkpoint != ready.checkpoint        ? "replay_standby_checkpoint" :
		       value.emulator != ready.emulator            ? "replay_standby_emulator" :
		       value.recipe != ready.recipe                ? "replay_standby_recipe" :
		                                                     nullptr;
	}
} // namespace pbd::replay_standby
