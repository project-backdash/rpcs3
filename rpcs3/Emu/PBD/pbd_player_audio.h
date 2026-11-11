#pragma once

#include "pbd_json_reader.h"

namespace pbd::player_audio
{
	// The existing launch settings carry the player's mute independently of the configured volume.
	inline bool startup_muted(std::string_view settings)
	{
		if (settings.size() > 16384)
			return false;
		const auto parsed = pbd::json::detail::json_parser(settings, 512).parse();
		const auto* audio = parsed.value ? parsed.value->find("audio") : nullptr;
		const auto* muted = audio ? audio->find("muted") : nullptr;
		return muted && muted->kind == pbd::json::json_kind::boolean && muted->truth;
	}

	inline int audible_volume(int chosen, bool player_muted, bool host_muted)
	{
		return player_muted || host_muted ? 0 : std::clamp(chosen, 0, 200);
	}
} // namespace pbd::player_audio
