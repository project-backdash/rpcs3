#pragma once

#include "../Cell/pbd_game_overlay.h"

namespace pbd::practice::match_prompt
{
	// Only local Practice may interrupt its menu with a bounded match offer or an automatic-match announcement.
	inline bool interrupts(const pbd::game_overlay::snapshot& panel, bool local_practice, bool peer) noexcept
	{
		if (!local_practice || peer || panel.practice || !panel.timer.present)
			return false;
		if (panel.kind == pbd::game_overlay::panel_kind::prompt)
			return panel.row_count == 2 && panel.timer.total_ms == 15000;
		if (panel.kind != pbd::game_overlay::panel_kind::challenger)
			return false;
		return (panel.row_count == 0 && panel.timer.total_ms == 1000) || (panel.row_count == 2 &&
																			 (panel.timer.total_ms == 15000 || panel.timer.total_ms == 20000 || panel.timer.total_ms == 24000));
	}

	inline bool expired(const pbd::game_overlay::snapshot& panel, std::uint64_t now_ms) noexcept
	{
		return panel.timer.present && now_ms >= panel.timer.deadline_ms;
	}
} // namespace pbd::practice::match_prompt
