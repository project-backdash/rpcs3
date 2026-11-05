#pragma once

#include <array>
#include <cstdint>

namespace pbd::custom_editor
{
	enum class save_action
	{
		none,
		save,
		keep_editing,
		discard,
	};

	// The owner supplies occupancy and photo completion. A decision carries no file path and performs no save itself.
	class save_panel
	{
		std::array<bool, 10> m_filled{};
		std::uint16_t m_held = 0;
		std::uint32_t m_slot = 1, m_row = 0;
		bool m_open = false, m_release = true, m_photo = false, m_overwrite = false, m_replace = false;

		save_action finish(save_action action)
		{
			m_open = false;
			return action;
		}

	public:
		bool begin(std::uint32_t slot, const std::array<bool, 10>& filled)
		{
			*this = {};
			if (slot < 1 || slot > filled.size())
				return false;
			m_slot = slot;
			m_filled = filled;
			m_open = true;
			return true;
		}
		void photo_ready(bool ready) noexcept
		{
			m_photo = ready;
			if (!ready)
				m_overwrite = m_replace = false;
		}
		bool open() const noexcept
		{
			return m_open;
		}
		bool can_save() const noexcept
		{
			return m_photo;
		}
		bool overwrite() const noexcept
		{
			return m_overwrite;
		}
		bool replace_selected() const noexcept
		{
			return m_replace;
		}
		std::uint32_t slot() const noexcept
		{
			return m_slot;
		}
		std::uint32_t row() const noexcept
		{
			return m_row;
		}

		save_action step(std::uint16_t raw)
		{
			constexpr std::uint16_t cross = 0x40, circle = 0x20, up = 0x1000, down = 0x4000, left = 0x8000, right = 0x2000;
			const auto pressed = static_cast<std::uint16_t>(raw & ~m_held);
			m_held = raw;
			if (!m_open)
				return save_action::none;
			// The press which left the shop cannot also accept or dismiss its save panel.
			if (m_release)
			{
				m_release = raw != 0;
				return save_action::none;
			}
			if (m_overwrite)
			{
				if (pressed & circle)
					m_overwrite = m_replace = false;
				else if (pressed & (left | right))
					m_replace = !m_replace;
				else if (pressed & cross)
				{
					if (m_replace && m_photo)
						return finish(save_action::save);
					m_overwrite = m_replace = false;
				}
				return save_action::none;
			}
			if (pressed & circle)
				return finish(save_action::keep_editing);
			if (pressed & up)
				m_row = (m_row + 2) % 3;
			else if (pressed & down)
				m_row = (m_row + 1) % 3;
			else if (!m_row && (pressed & left))
				m_slot = (m_slot + 8) % 10 + 1;
			else if (!m_row && (pressed & right))
				m_slot = m_slot % 10 + 1;
			else if (pressed & cross)
			{
				if (m_row == 1)
					return finish(save_action::keep_editing);
				if (m_row == 2)
					return finish(save_action::discard);
				if (m_photo)
				{
					if (!m_filled[m_slot - 1])
						return finish(save_action::save);
					m_overwrite = true;
					m_replace = false;
				}
			}
			return save_action::none;
		}
	};
} // namespace pbd::custom_editor
