#pragma once

#include <cstdint>

// The libc rand() reseeds that give a stage's own objects the same random draws on both peers and in a replay. The
// stage creates those objects at moments that follow each host's loading speed, so every value derives from the
// session seed and the match number instead of from the generator state at that moment.
namespace pbd::stage_sync
{
	// On the exit frame of the last pre-battle screen, for the match that follows `matches_before` matches.
	inline constexpr std::uint32_t screen_seed(std::uint32_t session_seed, std::uint32_t matches_before) noexcept
	{
		return session_seed ^ ((matches_before + 1) * 0x85ebca6bu);
	}

	// Before the first mob controller of out-of-battle creation batch `batch` (numbered from 1) ahead of that match.
	inline constexpr std::uint32_t mob_batch_seed(
		std::uint32_t session_seed, std::uint32_t matches_before, std::uint32_t batch) noexcept
	{
		return session_seed ^ ((matches_before + 1) * 0xc2b2ae35u) ^ (batch * 0x27d4eb2fu);
	}

	// The mob creation batches counted outside the battle since the last match start. A stage creates its mobs on the
	// tick their data arrives, so the controllers created in one tick form one batch and share one reseed.
	class mob_batches
	{
	public:
		std::uint32_t count() const noexcept
		{
			return batches;
		}

		// The batch a controller created at `tick` opens, or 0 when that tick's batch is already open.
		std::uint32_t open(std::uint32_t tick) noexcept
		{
			if (tick == batch_tick)
				return 0;
			batch_tick = tick;
			return ++batches;
		}

		// Numbers restart at every match start, so a drift in them cannot outlive one match.
		void begin_match() noexcept
		{
			batches = 0;
		}

	private:
		std::uint32_t batches = 0;
		std::uint32_t batch_tick = ~std::uint32_t{0};
	};

	// The counts the reseeds are numbered by: the matches started, and the mob batches since the last match start.
	struct epoch
	{
		std::uint32_t match_seeds = 0;
		mob_batches mobs;

		// A fresh-game replay's epoch, set before it runs the recorded game's stage load: the recording's matches before
		// that game, and no mob batches, because the count the recorder saves already includes that load's batches.
		static epoch replay(std::uint32_t prior_matches) noexcept
		{
			return {prior_matches, {}};
		}

		// Whether a replay has counted the recording's mob batches by the match initializer, so its mobs drew the
		// recording's seeds.
		bool reached(std::uint32_t recorded_mob_batches) const noexcept
		{
			return mobs.count() == recorded_mob_batches;
		}
	};
} // namespace pbd::stage_sync
