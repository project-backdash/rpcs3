#pragma once

#include "../Cell/pbd_stage_names.h"
#include <array>
#include <cstdint>
#include <optional>
#include <string_view>

namespace pbd::practice::stage
{
	inline constexpr std::uint32_t random_row = static_cast<std::uint32_t>(pbd::stage_names::names.size());
	inline constexpr std::uint32_t row_count = random_row + 1;
	// The menu lists the Dark Resurrection stages (ids from 17) first and the original Tekken 5 stages after them, each
	// group in id order, then Random: entries of pbd::stage_names::names, which is sorted by id.
	inline constexpr auto rows = []
	{
		std::array<std::size_t, pbd::stage_names::names.size()> order{};
		std::size_t at = 0;
		for (int pass = 0; pass < 2; ++pass)
			for (std::size_t entry = 0; entry < order.size(); ++entry)
				if ((pbd::stage_names::names[entry].first >= 17) == (pass == 0))
					order[at++] = entry;
		return order;
	}();
	inline constexpr bool valid(std::uint32_t id) noexcept
	{
		return !pbd::stage_names::name(id).empty();
	}
	inline constexpr std::string_view row_name(std::uint32_t row) noexcept
	{
		return row < random_row ? pbd::stage_names::names[rows[row]].second : row == random_row ? "Random" :
		                                                                                          "";
	}
	inline constexpr std::optional<std::uint32_t> resolve(std::uint32_t row, std::uint32_t outside_word) noexcept
	{
		if (row >= row_count)
			return {};
		if (row == random_row)
			row = static_cast<std::uint32_t>((std::uint64_t{outside_word} * random_row) >> 32);
		return pbd::stage_names::names[rows[row]].first;
	}
	enum class progress : std::uint8_t
	{
		none,
		waiting,
		complete,
		invalid,
		timed_out
	};
	struct snapshot
	{
		std::uint64_t generation = 0;
		std::uint32_t boundary = 0, mode = 0, scene = 0, pending = 0, stage = 0, counter = 0;
		std::array<std::uint32_t, 2> rows{};
		bool paused = false, models_ready = false;
	};
	struct load_watch
	{
		std::uint64_t generation = 0;
		std::uint32_t boundary = 0, stage = 0, previous_counter = 0, advancing = 0;
		std::array<std::uint32_t, 2> rows{};
		bool active = false, left_arena = false, have_counter = false;
		void begin(std::uint64_t next_generation, std::uint32_t next_boundary, std::uint32_t next_stage,
			std::array<std::uint32_t, 2> next_rows) noexcept
		{
			*this = {};
			generation = next_generation;
			boundary = next_boundary;
			stage = next_stage;
			rows = next_rows;
			active = true;
		}
		progress finish(progress value) noexcept
		{
			active = false;
			return value;
		}
		progress step(const snapshot& now) noexcept
		{
			if (!active)
				return progress::none;
			if (now.generation != generation)
			{
				*this = {};
				return progress::none;
			}
			if (now.mode != 10)
				return finish(progress::invalid);
			if (now.boundary - boundary >= 1800)
				return finish(progress::timed_out);
			if (now.scene != 17 || now.pending != 44)
			{
				left_arena = true;
				have_counter = false;
				advancing = 0;
				return progress::waiting;
			}
			if (!left_arena)
				return progress::waiting;
			if (now.stage != stage || now.rows != rows)
				return finish(progress::invalid);
			if (now.paused || !now.models_ready)
			{
				have_counter = false;
				advancing = 0;
				return progress::waiting;
			}
			advancing = have_counter && now.counter - previous_counter == 1 ? advancing + 1 : 0;
			previous_counter = now.counter;
			have_counter = true;
			return advancing >= 3 ? finish(progress::complete) : progress::waiting;
		}
	};
} // namespace pbd::practice::stage
