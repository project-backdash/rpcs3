#pragma once

#include <array>
#include <cmath>
#include <cstdint>
#include <deque>
#include <mutex>
#include <optional>

// The watcher's screen answers the mouse. The overlay publishes where it drew each control, in canvas units (1280x720 over
// the game viewport); the game window turns a left click on one into a click here, and the spectator channel's next pad read
// presses that control's button. A click is therefore the same press the controller makes: it reaches the launcher as a pad
// event and never the watched game's input.
namespace pbd::spectator::mouse
{
	enum class kind : std::uint8_t
	{
		button, // value: the control's pad bit (pbd::spectator_ui::pad)
		tab     // value: the chat tab's index; reached with L1/R1 presses
	};

	struct target
	{
		float x = 0, y = 0, width = 0, height = 0;
		kind what = kind::button;
		std::uint32_t value = 0;
		bool contains(float px, float py) const noexcept
		{
			return width > 0 && height > 0 && px >= x && py >= y && px < x + width && py < y + height;
		}
	};

	struct layout
	{
		std::uint32_t count = 0;
		std::array<target, 24> targets{};
		void add(const target& next) noexcept
		{
			if (count < targets.size())
				targets[count++] = next;
		}
	};

	struct click
	{
		kind what = kind::button;
		std::uint32_t value = 0;
	};

	struct shared_state
	{
		std::mutex mutex;
		layout current;
		std::deque<click> pending;
	};
	inline shared_state& state()
	{
		static shared_state value;
		return value;
	}

	// A screen without controls drops clicks not yet read, so none acts on whatever shows next.
	inline void publish(const layout& next)
	{
		auto& shared = state();
		std::lock_guard lock(shared.mutex);
		if (!next.count)
			shared.pending.clear();
		shared.current = next;
	}

	inline bool active()
	{
		auto& shared = state();
		std::lock_guard lock(shared.mutex);
		return shared.current.count != 0;
	}

	// The pointer at (x, y) in the game viewport (left, top, width, height), all in window pixels. True while it is over a
	// control; `press` then queues that control's click.
	inline bool pointer(float x, float y, float width, float height, bool press, float left = 0, float top = 0)
	{
		if (!std::isfinite(x) || !std::isfinite(y) || !std::isfinite(width) || !std::isfinite(height) || !std::isfinite(left) ||
			!std::isfinite(top) || width <= 0 || height <= 0)
			return false;
		const float px = (x - left) * 1280.f / width, py = (y - top) * 720.f / height;
		auto& shared = state();
		std::lock_guard lock(shared.mutex);
		for (std::uint32_t i = 0; i < shared.current.count; ++i)
		{
			const auto& control = shared.current.targets[i];
			if (!control.contains(px, py))
				continue;
			if (press && shared.pending.size() < 8)
				shared.pending.push_back({control.what, control.value});
			return true;
		}
		return false;
	}

	inline std::optional<click> take()
	{
		auto& shared = state();
		std::lock_guard lock(shared.mutex);
		if (shared.pending.empty())
			return std::nullopt;
		const click next = shared.pending.front();
		shared.pending.pop_front();
		return next;
	}
} // namespace pbd::spectator::mouse
