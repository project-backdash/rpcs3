#pragma once

#include <array>
#include <atomic>
#include <cstdint>

#include "pbd_controller_mapper.h"

// What offline play's Versus (Emu/Cell/pbd_versus.h) and the controller backend tell each other between the game's reads.
namespace pbd::versus::input
{
	inline std::atomic<bool> active{false};

	inline pbd::controllers::state home_as_start(pbd::controllers::state value)
	{
		using namespace pbd::controllers;
		if (value.buttons & bit(home))
			value.buttons = (value.buttons & ~bit(home)) | bit(pbd::controllers::start);
		return value;
	}

	// Each local player's controller as its own buttons: the game word before the controller's layout, from every poll.
	inline std::array<std::atomic<std::uint16_t>, 2> words{};
	// The Versus character select is up. There a controller's buttons pick and confirm as themselves, as Tekken 8's do (its
	// layout gives them actions only in a game), and Select asks which player the controller is.
	inline std::atomic<bool> select{false};
	// A player's Controller Setup is open: that controller's Select removes a button there and does not ask which player it is.
	inline std::array<std::atomic<bool>, 2> panel{};
	// Counts each opening of the VS mode and each New Players choice. Each starts with both players free, as
	// Tekken 8's local Versus does; the selects between a set's games keep the players.
	inline std::atomic<std::uint32_t> opened{0};
} // namespace pbd::versus::input
