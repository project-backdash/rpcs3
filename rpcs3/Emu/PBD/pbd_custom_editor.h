#pragma once

#include "pbd_custom_look.h"

#include <atomic>
#include <optional>

class ppu_thread;

namespace pbd::custom_editor
{
	using custom_look::u32;
	inline std::atomic<bool> save_blocked{false};
	inline std::atomic<std::uint64_t> save_generation{0};
	inline std::atomic<bool> trial_pad{false};
	inline std::atomic<std::uint16_t> trial_word{0};
	inline bool blocks_saves(std::uint64_t generation) noexcept
	{
		return save_blocked.load(std::memory_order_acquire) && save_generation.load(std::memory_order_acquire) == generation;
	}
	// A short item/category exercise; the stock controller interprets every press and changes the equipped record.
	constexpr std::uint16_t exercise_word(u32 frame) noexcept
	{
		if (frame >= 30 && frame < 32)
			return 0x40;
		if (frame >= 60 && frame < 62)
			return 0x2000;
		if (frame >= 90 && frame < 92)
			return 0x40;
		if ((frame >= 120 && frame < 122) || (frame >= 150 && frame < 152))
			return 0x20;
		return 0;
	}
	struct observation
	{
		bool verified = false, offline = false, parent_ready = false;
		u32 user = 0, mode = 0, scene = 0, pending = 0, selector = 0, next_selector = 0, shop = 0;
	};
	enum class action
	{
		wait,
		enter,
		open_items,
		capture,
		refuse,
	};

	// The editor is an isolated local boot. Its parent task must already belong to the stock select scene.
	inline bool admissible(const observation& value) noexcept
	{
		return value.verified && value.offline && value.user == 3 && value.mode == 4 && value.scene == 3 && value.pending == 44;
	}

	class flow
	{
		bool m_entered = false, m_open = false, m_finished = false;
		u32 m_frames = 0;

	public:
		void resume() noexcept
		{
			m_finished = false;
			m_frames = 0;
		}
		action step(const observation& value) noexcept
		{
			if (m_finished)
				return action::wait;
			if (!admissible(value) || ++m_frames > 3600)
			{
				m_finished = true;
				return action::refuse;
			}
			if (!m_entered)
			{
				if (!value.parent_ready || value.selector != 3 || value.next_selector != 3)
					return action::wait;
				m_entered = true;
				return action::enter;
			}
			// The stock controller may also finish when the select timer expires. Catch either exit before the next tick.
			if (value.next_selector == 7 || value.selector == 7 || (value.selector == 6 && (value.shop == 3 || value.shop == 6)))
			{
				m_finished = true;
				return m_open ? action::capture : action::refuse;
			}
			if (value.selector != 5 && value.selector != 6)
			{
				m_finished = true;
				return action::refuse;
			}
			if (value.selector == 6 && value.shop == 2)
			{
				if (m_open)
				{
					m_finished = true;
					return action::capture;
				}
				m_open = true;
				return action::open_items;
			}
			return action::wait;
		}
	};

	// A bounded direct-entry observation, separate from the launcher request/result protocol.
	bool trial_boundary(ppu_thread& ppu, u32 address) noexcept;
} // namespace pbd::custom_editor
