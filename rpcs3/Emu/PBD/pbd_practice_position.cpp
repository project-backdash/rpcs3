#include "stdafx.h"
#include "pbd_practice_position.h"
#include "pbd_practice_random.h"

namespace pbd::practice
{
	using namespace pbd::game_lifecycle;

	std::optional<placement_token> random_destination(const pbd::practice::walls::boundary& arena,
		const practice_reset& command, std::array<u32, 2> actor_addresses)
	{
		const auto pair = pbd::practice::random_position(arena, command.random_seed,
			{750.f, 2000.f});
		if (!pair)
			return {};
		placement_token out;
		out.lifetime = command.lifetime;
		out.actors = actor_addresses;
		out.positions = {facing_placement(pair->actors[0], pair->forward),
			facing_placement(pair->actors[1], {-pair->forward.x, -pair->forward.z})};
		return out;
	}
} // namespace pbd::practice
