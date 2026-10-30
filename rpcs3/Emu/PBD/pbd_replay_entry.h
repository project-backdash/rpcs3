#pragma once

#include <cstdint>

namespace pbd::replay_entry
{
	enum class refusal
	{
		none,
		direct_replay,
		boot_request,
	};

	template <typename Thread>
	struct context
	{
		Thread& owner;
		std::uint32_t address;
	};

	// Playback and boot change guest execution; recording only observes it. Its failure must stay local to the journal.
	template <typename Thread, typename Playback, typename Recording, typename Boot>
	refusal enter(context<Thread> value, Playback&& playback, Recording&& recording, Boot&& boot) noexcept
	{
		if (!playback(value.owner, value.address))
			return refusal::direct_replay;
		recording(value.owner, value.address);
		if (!boot(value.owner, value.address))
			return refusal::boot_request;
		return refusal::none;
	}
} // namespace pbd::replay_entry
