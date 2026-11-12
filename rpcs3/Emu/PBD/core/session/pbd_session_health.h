#pragma once

#include <algorithm>
#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace pbd::session_health
{
	class activity
	{
		std::uint32_t published = 0;
		std::uint64_t advanced_ns = 0;
		bool seen = false;

	public:
		std::string_view observe(std::uint32_t through, std::uint64_t now, bool battle, bool paused, bool recovering) noexcept
		{
			if (!seen || through != published)
			{
				seen = true;
				published = through;
				advanced_ns = now;
			}
			if (recovering)
				return "waiting";
			if (paused)
			{
				advanced_ns = now;
				return "paused";
			}
			if (!battle)
				return "loading";
			// A callback can be sampled while executing normally or waiting for the next input. Its occupancy is not a fault.
			return now >= advanced_ns && now - advanced_ns >= 1'000'000'000 ? "waiting" : "running";
		}
	};

	// Quiet time counts only while the game is free to run. Paused emulation, a parked standby game and an open in-game
	// menu each hold it on purpose, and the clock restarts when the hold ends. A parked game stays held even when the
	// emulator was resumed under it, so its activation restarts the clock, not that resume.
	class quiet_clock
	{
		std::uint64_t m_from = 0;
		bool m_held = false;

	public:
		void observe(bool paused, bool parked, bool menu, std::uint64_t now) noexcept
		{
			if (paused || parked || menu)
				m_held = true;
			else if (m_held)
			{
				m_held = false;
				m_from = now;
			}
		}
		bool held() const noexcept
		{
			return m_held;
		}
		// The later of the last callback exit and the end of the last hold.
		std::uint64_t since(std::uint64_t last_callback) const noexcept
		{
			return std::max(last_callback, m_from);
		}
	};

	// The receiver ends a session whose other PC sent no valid input for this long (peer_silence_timeout).
	inline constexpr std::uint64_t peer_silence_us = 10'000'000;
	inline constexpr std::uint64_t stall_ns = 10'000'000'000;
	// While the renderer is inside a shader build or wait (pbd_shader_build.h) the game waits on it and is not stuck. No
	// quiet stretch outlasts this bound, however many builds it holds, so this PC ends the set before the other PC's 90 s
	// lease on its boundary call does.
	inline constexpr std::uint64_t building_stall_ns = 60'000'000'000;

	struct stall_facts
	{
		bool idle = false;          // the owner is outside the boundary call
		bool battle = false;        // the last published boundary was in a battle scene
		bool held = false;          // paused, parked or in the in-game menu (quiet_clock)
		bool boundary_seen = false; // the owner has left at least one boundary call
		std::uint64_t quiet_from_ns = 0, now_ns = 0;
		std::uint64_t peer_heard_us = 0, now_us = 0; // the other PC's last valid input; 0 when never heard or not watched
		bool renderer_building = false;              // the RSX thread is inside a shader build or wait
		std::uint64_t renderer_built_ns = 0;         // when its last build or wait ended, on the quiet_from_ns axis; 0 when none
	};

	// A game stopped in its own code: free to run, outside the boundary call and in battle, it published no boundary for
	// stall_ns while the other PC is still heard. Nothing inside the boundary call can time it out, because the peer and
	// hold timeouts run only there. Waits inside the call keep their own timeouts, a silent peer is the silence watch's to
	// end, and loading outside battle keeps the launcher's longer allowance. A renderer shader build holds off the stall_ns
	// bound while it lasts, and that bound restarts when it ends; building_stall_ns of quiet ends the game either way, so a
	// stream of builds cannot hide a freeze.
	inline bool stalled(const stall_facts& facts) noexcept
	{
		const bool heard = facts.peer_heard_us && (facts.now_us <= facts.peer_heard_us || facts.now_us - facts.peer_heard_us < peer_silence_us);
		if (!facts.idle || !facts.battle || facts.held || !facts.boundary_seen || !heard || facts.now_ns < facts.quiet_from_ns)
			return false;
		const std::uint64_t built = std::min(std::max(facts.quiet_from_ns, facts.renderer_built_ns), facts.now_ns);
		return facts.now_ns - facts.quiet_from_ns >= building_stall_ns || (!facts.renderer_building && facts.now_ns - built >= stall_ns);
	}

	inline constexpr std::size_t snapshot_bytes = 4096, header_bytes = 31;
	using page = std::array<char, snapshot_bytes>;
	struct progress
	{
		std::uint32_t game_number;
		std::array<std::uint32_t, 2> round_wins;
		bool operator==(const progress&) const = default;
	};
	inline bool valid(const progress& value) noexcept
	{
		return value.game_number && value.game_number <= 65535 && value.round_wins[0] < 5 && value.round_wins[1] < 5;
	}
	inline std::string progress_json(const std::optional<progress>& value)
	{
		if (!value || !valid(*value))
			return {};
		return ",\"progress\":{\"gameNumber\":" + std::to_string(value->game_number) +
		       ",\"roundWins\":{\"left\":" + std::to_string(value->round_wins[0]) +
		       ",\"right\":" + std::to_string(value->round_wins[1]) + "}}";
	}

	inline std::uint32_t checksum(std::string_view payload) noexcept
	{
		std::uint32_t result = 2166136261u;
		for (const unsigned char byte : payload)
			result = (result ^ byte) * 16777619u;
		return result;
	}

	// One allocated page is overwritten in place. The checksum lets an external reader reject a torn observation.
	inline std::optional<page> snapshot(std::string_view payload) noexcept
	{
		if (payload.empty() || payload.size() > snapshot_bytes - header_bytes)
			return {};
		page result;
		result.fill(' ');
		constexpr std::string_view magic = "PBD-HEALTH-1\n";
		std::copy(magic.begin(), magic.end(), result.begin());
		const auto hex = [&](std::size_t offset, std::uint32_t number)
		{
			constexpr char digits[] = "0123456789abcdef";
			for (unsigned digit = 0; digit < 8; ++digit)
				result[offset + digit] = digits[(number >> ((7 - digit) * 4)) & 15];
			result[offset + 8] = '\n';
		};
		hex(13, static_cast<std::uint32_t>(payload.size()));
		hex(22, checksum(payload));
		std::copy(payload.begin(), payload.end(), result.begin() + header_bytes);
		return result;
	}
} // namespace pbd::session_health
