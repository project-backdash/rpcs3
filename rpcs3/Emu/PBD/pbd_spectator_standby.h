#pragma once

#include "pbd_env.h"
#include "pbd_json.h"

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>

// A standby watcher. While its player waits in line the launcher starts a watcher with this PC's own start state and no
// set. The watcher boots, proves its clean character select at the first boundary, says so in spectator-standby.json and
// holds there. The launcher binds it by writing the set's spectator-session.json (pbd-spectator-session-1) into the same
// channel, the file a watcher started for that set reads at launch, and the watch goes on from that boundary
// (docs/design/spectator-wire.md, "Standby and bind").
namespace pbd::spectator_standby
{
	inline constexpr std::string_view schema = "pbd-spectator-standby-1";
	// Written once the clean start is proved; written instead of a watch when the standby ends unbound or its bind is
	// refused.
	inline constexpr std::string_view ready_file = "spectator-standby.json";
	inline constexpr std::string_view end_file = "spectator-standby-end.json";

	// PBD_SPECTATOR_STANDBY=1, beside PBD_SPECTATOR_CHANNEL and without PBD_MATCH_SEED.
	inline bool requested()
	{
		const char* value = pbd::env::find("PBD_SPECTATOR_STANDBY");
		return value && std::string_view(value) == "1";
	}

	// The input policy the standby runs with. A set whose manifest names another one is refused at the bind
	// (spectator_input_policy_mismatch), so the launcher compares before it binds.
	struct policy
	{
		bool start_skip = true, exit_gate = true, keyed_draws = false;
		std::uint32_t battle_clock_version = 2, menu_hash_mask = 0;
	};

	inline constexpr const char* keyed_draws_refusal = "spectator_keyed_draws_mismatch";
	inline constexpr const char* input_policy_refusal = "spectator_input_policy_mismatch";

	// Why a set's manifest cannot be watched by a process running `actual`, or null. Keyed draws are named on their own,
	// since the launcher shows a sentence for them. A manifest without a policy (an older source) carries none to compare.
	template <typename Manifest>
	const char* policy_refusal(const policy& actual, const Manifest& manifest)
	{
		if (!manifest.policy_present)
			return nullptr;
		if (manifest.keyed_draws != actual.keyed_draws)
			return keyed_draws_refusal;
		if (manifest.start_skip != actual.start_skip || manifest.exit_gate != actual.exit_gate ||
			manifest.battle_clock_version != actual.battle_clock_version || manifest.menu_hash_mask != actual.menu_hash_mask)
			return input_policy_refusal;
		return nullptr;
	}

	struct ready
	{
		std::uint64_t native_generation = 0;
		std::uint32_t owner = 0;
		std::string checkpoint, emulator, recipe;
		policy input;
	};

	inline std::string ready_text(const ready& value)
	{
		const auto flag = [](bool on)
		{
			return on ? "true" : "false";
		};
		std::string text = "{\"schema\":" + pbd::json::quoted(schema) + ",\"type\":\"ready\",\"nativeGeneration\":\"" +
		                   std::to_string(value.native_generation) + "\",\"owner\":" + std::to_string(value.owner) +
		                   ",\"origin\":0,\"scene\":3,\"subscene\":44,\"checkpointSha256\":" + pbd::json::quoted(value.checkpoint) +
		                   ",\"emulatorSha256\":" + pbd::json::quoted(value.emulator) + ",\"recipeSha256\":" +
		                   pbd::json::quoted(value.recipe) + ",\"inputPolicy\":{\"sessionStartSkip\":" + flag(value.input.start_skip) +
		                   ",\"exitGate\":" + flag(value.input.exit_gate) + ",\"keyedDraws\":" + flag(value.input.keyed_draws) +
		                   ",\"battleClockVersion\":" + std::to_string(value.input.battle_clock_version) +
		                   ",\"menuHashMask\":" + std::to_string(value.input.menu_hash_mask) + "}}\n";
		return text;
	}

	inline std::string end_text(std::string_view reason, std::uint64_t waited_ms)
	{
		return "{\"schema\":" + pbd::json::quoted(schema) + ",\"type\":\"ended\",\"reason\":" + pbd::json::quoted(reason) +
		       ",\"waitedMs\":" + std::to_string(waited_ms) + "}\n";
	}

	// Whether the standby polls again after a read of the bound manifest found none (`error`, from read_manifest). A manifest
	// not there yet is polled for until the bind. A read the system denied (read_failed) is polled again for up to a second,
	// as the set channel's commands are: a file moved or linked into place on Windows can keep its writer's handle for a
	// moment. Anything else is a manifest the standby cannot take.
	class bind_wait
	{
		std::optional<std::chrono::steady_clock::time_point> m_denied;

	public:
		bool again(std::string_view error, std::chrono::steady_clock::time_point now)
		{
			if (error != "read_failed")
			{
				m_denied.reset();
				return error == "unavailable";
			}
			if (!m_denied)
				m_denied = now;
			return now - *m_denied < std::chrono::seconds(1);
		}
	};

	// Writes the whole text beside `file` and moves it into place, so the launcher never reads half of it. An existing
	// file is never replaced.
	inline bool publish(const std::filesystem::path& file, std::string_view text)
	{
		std::error_code ec;
		if (std::filesystem::exists(file, ec) || ec)
			return false;
		auto temporary = file;
		temporary.replace_filename("." + file.filename().string() + ".tmp");
		{
			std::ofstream out(temporary, std::ios::binary | std::ios::trunc);
			out.write(text.data(), static_cast<std::streamsize>(text.size()));
			if (!out.flush())
				return false;
		}
		std::filesystem::rename(temporary, file, ec);
		if (!ec)
			return true;
		std::filesystem::remove(temporary, ec);
		return false;
	}
} // namespace pbd::spectator_standby
