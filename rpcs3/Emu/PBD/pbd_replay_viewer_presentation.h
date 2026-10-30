#pragma once

#include "pbd_replay_viewer_controller.h"

namespace pbd::replay_viewer
{
	// Compact bounded histories of the actual injected recorded/control words, updated once per boundary.
	struct input_run
	{
		word input = 0;
		std::uint32_t held = 0;
	};
	inline constexpr std::size_t history_rows = 16; // Matches the Practice history so both use one renderer.
	struct command_history
	{
		std::array<input_run, history_rows> rows{};
		std::uint32_t boundary = ~0u;
		void update(std::uint32_t index, word input) noexcept
		{
			if (index == boundary)
				return;
			if (index < boundary && boundary != ~0u)
				rows = {};
			boundary = index;
			if (rows[0].held && rows[0].input == input)
				++rows[0].held;
			else
			{
				for (std::size_t i = rows.size() - 1; i; --i)
					rows[i] = rows[i - 1];
				rows[0] = {input, 1};
			}
		}
	};
	struct snapshot
	{
		std::uint32_t enabled = 0, menu = 0, choice = 0, paused = 0, speed = 2, round = 0, match = 0, frame = 0, count = 0;
		pbd::takeover_phase takeover = pbd::takeover_phase::watching;
		std::uint32_t take_choice = 0, port = 0, frames_left = 0, flags = 0, fast = 0, bookmarked = 0;
		std::array<std::uint32_t, 255> rounds{};
		std::array<std::array<input_run, history_rows>, 2> history{};
		std::array<pbd::practice::feedback, 2> fighters{};
	};
	struct shared_presentation
	{
		std::atomic<std::uint32_t> enabled{0}, menu{0}, choice{0}, paused{0}, speed{2}, round{0}, match{0}, frame{0}, count{0};
		std::atomic<pbd::takeover_phase> takeover{pbd::takeover_phase::watching};
		std::atomic<std::uint32_t> take_choice{0}, port{0}, frames_left{0}, flags{0}, fast{0}, bookmarked{0};
		std::array<std::atomic<std::uint32_t>, 255> rounds{};
		std::array<std::array<std::atomic<std::uint32_t>, history_rows>, 2> words{}, holds{};
		mutable std::mutex fighters_mutex;
		std::array<pbd::practice::feedback, 2> fighters{};
		void store(const snapshot& s) noexcept
		{
			std::lock_guard lock(fighters_mutex);
			fighters = s.fighters;
			constexpr auto r = std::memory_order_relaxed;
			enabled.store(s.enabled, r);
			menu.store(s.menu, r);
			choice.store(s.choice, r);
			paused.store(s.paused, r);
			speed.store(s.speed, r);
			round.store(s.round, r);
			match.store(s.match, r);
			frame.store(s.frame, r);
			count.store(s.count, r);
			takeover.store(s.takeover, r);
			take_choice.store(s.take_choice, r);
			port.store(s.port, r);
			frames_left.store(s.frames_left, r);
			flags.store(s.flags, r);
			fast.store(s.fast, r);
			bookmarked.store(s.bookmarked, r);
			for (std::size_t i = 0; i < s.rounds.size(); ++i)
				rounds[i].store(s.rounds[i], r);
			for (std::size_t side = 0; side < 2; ++side)
				for (std::size_t i = 0; i < history_rows; ++i)
				{
					words[side][i].store(s.history[side][i].input, r);
					holds[side][i].store(s.history[side][i].held, r);
				}
		}
		snapshot load() const noexcept
		{
			std::lock_guard lock(fighters_mutex);
			constexpr auto r = std::memory_order_relaxed;
			snapshot s;
			s.fighters = fighters;
			s.enabled = enabled.load(r);
			s.menu = menu.load(r);
			s.choice = choice.load(r);
			s.paused = paused.load(r);
			s.speed = speed.load(r);
			s.round = round.load(r);
			s.match = match.load(r);
			s.frame = frame.load(r);
			s.count = std::min(count.load(r), 255u);
			s.takeover = takeover.load(r);
			s.take_choice = take_choice.load(r);
			s.port = port.load(r);
			s.frames_left = frames_left.load(r);
			s.flags = flags.load(r);
			s.fast = fast.load(r);
			s.bookmarked = bookmarked.load(r);
			for (std::size_t i = 0; i < s.rounds.size(); ++i)
				s.rounds[i] = rounds[i].load(r);
			for (std::size_t side = 0; side < 2; ++side)
				for (std::size_t i = 0; i < history_rows; ++i)
					s.history[side][i] = {static_cast<word>(words[side][i].load(r)), holds[side][i].load(r)};
			return s;
		}
	};
	inline shared_presentation presentation;

	struct pointer_rectangle
	{
		std::int32_t x = 0, y = 0, width = 0, height = 0;
		std::uint32_t row = pointer_none;
	};
	struct pointer_position
	{
		float x = -1, y = -1;
	};
	inline pointer_position pointer_coordinates(float x, float y, const pointer_rectangle& viewport) noexcept
	{
		if (viewport.width <= 0 || viewport.height <= 0 || x < viewport.x || y < viewport.y ||
			x >= viewport.x + viewport.width || y >= viewport.y + viewport.height)
			return {};
		return {(x - viewport.x) * 1280.f / viewport.width, (y - viewport.y) * 720.f / viewport.height};
	}
	// Only drawn host UI rectangles cross RSX -> Qt. Only one pending click crosses Qt -> the simulation owner.
	struct pointer_surface
	{
		std::mutex guard;
		std::array<pointer_rectangle, 12> rectangles{};
		std::uint32_t count = 0;
		std::uint64_t identity = 0;
		std::atomic<std::uint64_t> pending{0};
		void publish(const snapshot& view, const std::array<pointer_rectangle, 12>& areas, std::uint32_t size)
		{
			std::lock_guard lock(guard);
			rectangles = areas;
			count = view.enabled ? std::min<std::uint32_t>(size, 12) : 0;
			identity = (std::uint64_t(view.match) << 32) | (std::uint64_t(view.menu) << 24) | (std::uint64_t(view.takeover) << 16);
		}
		std::uint32_t hit(float x, float y)
		{
			std::lock_guard lock(guard);
			return find(x, y);
		}
		bool press(float x, float y)
		{
			std::lock_guard lock(guard);
			const auto target = find(x, y);
			const auto row = target == pointer_none ? pointer_discard : target;
			std::uint64_t empty = 0;
			return pending.compare_exchange_strong(empty, identity | (row + 1u), std::memory_order_release, std::memory_order_relaxed);
		}
		bool back()
		{
			std::lock_guard lock(guard);
			if (!(identity & 0x00ff0000ull) && !(identity & 0xff000000ull))
				return false;
			std::uint64_t empty = 0;
			return pending.compare_exchange_strong(empty, identity | (pointer_back + 1u), std::memory_order_release, std::memory_order_relaxed);
		}
		std::uint32_t take(page menu, pbd::takeover_phase takeover, std::uint32_t match) noexcept
		{
			const auto packet = pending.exchange(0, std::memory_order_acquire);
			const auto current = (std::uint64_t(match) << 32) | (std::uint64_t(menu) << 24) | (std::uint64_t(takeover) << 16);
			if (!packet)
				return pointer_none;
			return (packet & ~std::uint64_t(0xffff)) == current ? static_cast<std::uint32_t>(packet & 0xffff) - 1u : pointer_discard;
		}
		void clear()
		{
			pending.store(0, std::memory_order_relaxed);
			std::lock_guard lock(guard);
			count = 0;
		}

	private:
		std::uint32_t find(float x, float y) const
		{
			for (std::uint32_t i = 0; i < count; ++i)
			{
				const auto& r = rectangles[i];
				if (x >= r.x && y >= r.y && x < r.x + r.width && y < r.y + r.height)
					return r.row;
			}
			return pointer_none;
		}
	};
	inline pointer_surface pointer_ui;
	inline const char* speed_text(std::uint32_t speed)
	{
		constexpr const char* text[] = {"1/4x", "1/2x", "1x", "2x", "4x"};
		return speed < 5 ? text[speed] : "1x";
	}

} // namespace pbd::replay_viewer
