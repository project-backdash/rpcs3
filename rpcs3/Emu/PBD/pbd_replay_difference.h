#pragma once

// Compare recorded replay frames and the terminal score with native playback.

#include <array>
#include <cstdint>

namespace pbd::replay_difference
{
	enum class kind : std::uint8_t
	{
		none,
		coordinate, // the frame's segment, scene, counter or hash mask
		hash,       // one of the combat hashes the mask covers
	};

	struct difference
	{
		kind what = kind::none;
		std::uint32_t region = 0; // the differing combat hash, for kind::hash

		explicit operator bool() const noexcept
		{
			return what != kind::none;
		}
	};

	// The first way a played frame differs from its recorded facts: its coordinates first, then each combat hash the
	// mask covers, in region order. A template, so this header needs no sync-facts type.
	template <typename Facts>
	difference find(const Facts& expected, std::uint32_t segment, std::uint32_t scene, std::uint32_t counter,
		std::uint32_t mask, const std::array<std::uint64_t, 3>& combat) noexcept
	{
		if (expected.segment != segment || expected.scene != scene || expected.counter != counter ||
			expected.mask != mask)
			return {kind::coordinate};
		for (std::uint32_t region = 0; region < 3; ++region)
			if ((mask & (1u << region)) && expected.combat[region] != combat[region])
				return {kind::hash, region};
		return {};
	}

	// The log event and refusal for the first difference from the recording.
	struct log_name
	{
		const char* event;
		const char* refusal;
	};
	// The final update has no following recorded input whose pre-state could check its result.
	inline const char* terminal_refusal(const std::array<std::uint32_t, 2>& expected,
		const std::array<std::uint32_t, 2>& observed, bool readable) noexcept
	{
		if (!readable)
			return "native_replay_final_score_unavailable";
		return observed == expected ? nullptr : "native_replay_final_score_mismatch";
	}

	inline log_name name_of(const difference& found) noexcept
	{
		if (found.what == kind::coordinate)
			return {"sync_coordinate_mismatch", "native_replay_coordinate_mismatch"};
		return {"sync_hash_mismatch", "native_replay_peer_sync_hash_mismatch"};
	}

} // namespace pbd::replay_difference
