#pragma once

#include "../Cell/pbd_hidden_selection.h"
#include "pbd_custom_look.h"

#include <array>
#include <optional>
#include <span>
#include <string>
#include <vector>

// The local costume row produces stock confirm buttons. It owns no game cursor, costume, random generator or save.
namespace pbd::costume_select
{
	using u16 = std::uint16_t;
	using u32 = std::uint32_t;
	inline constexpr u16 cross = 0x40, square = 0x80, circle = 0x20, triangle = 0x10;
	inline constexpr u16 left = 0x8000, right = 0x2000, l1 = 0x04, r1 = 0x08, start = 0x800;
	inline constexpr u16 reserved = cross | square | circle | triangle | l1 | r1 | start;
	inline constexpr u32 custom_slots = 10;

	// Practice moves one physical pad from the player to the dummy and then the stage. A confirmation belongs only to
	// the choice that emitted it; every later read is neutral until that physical press has been released.
	class confirm_release
	{
		bool m_waiting = false;

	public:
		void consume() noexcept
		{
			m_waiting = true;
		}
		bool holds(u16 raw) noexcept
		{
			if (!m_waiting)
				return false;
			m_waiting = raw != 0;
			return true;
		}
	};

	struct card
	{
		custom_look::look look;
		u16 confirm = 0;
		std::optional<u32> slot;
		std::string display_name;
		std::vector<std::uint8_t> picture;
		bool operator==(const card&) const = default;
	};

	// Defaults come from the verified stock list, including every third, fourth and fifth costume. Custom slots follow
	// in slot order; malformed looks and looks for another fighter are omitted before any confirm can be produced.
	inline std::vector<card> cards(const hidden_selection::tables& tables, u32 fighter,
		std::span<const std::optional<custom_look::look>> customs = {})
	{
		std::vector<card> out;
		if (fighter >= tables.costumes.size())
			return out;
		for (const auto row : tables.costumes[fighter].rows)
			if (const auto plan = hidden_selection::resolve(tables, row))
				out.push_back({custom_look::look{row}, plan->confirm, {}});
		if (out.empty())
			return out;
		for (u32 slot = 0; slot < std::min<std::size_t>(customs.size(), custom_slots); ++slot)
		{
			if (!customs[slot])
				continue;
			const auto normal = custom_look::normalise(*customs[slot]);
			const auto plan = hidden_selection::resolve(tables, customs[slot]->row);
			if (normal && plan && plan->character == fighter && custom_look::catalogue_row(normal->row)->dressable)
				out.push_back({*normal, plan->confirm, slot});
		}
		return out;
	}

	struct result
	{
		// No value outside verified Practice or Versus character selection. Zero holds the whole pad neutral.
		std::optional<u16> word;
		std::optional<card> picked;
		bool controller_setup = false;
	};

	class row
	{
		struct remembered
		{
			u32 row = 0;
			std::optional<u32> slot;
		};
		std::array<std::optional<remembered>, 35> m_last{};
		std::vector<card> m_cards;
		std::optional<card> m_committed;
		u32 m_fighter = ~u32{0}, m_focus = 0;
		u16 m_held = 0;
		bool m_open = false, m_release = false;

		result choose()
		{
			const auto picked = m_cards[m_focus];
			m_last[m_fighter] = remembered{picked.look.row, picked.slot};
			m_committed = picked;
			m_open = false;
			m_release = true;
			return {picked.confirm, picked, false};
		}

	public:
		bool open() const noexcept
		{
			return m_open;
		}
		u32 focus() const noexcept
		{
			return m_focus;
		}
		const std::vector<card>& choices() const noexcept
		{
			return m_cards;
		}

		result step(const hidden_selection::tables& tables, const hidden_selection::observation& seen, u16 raw,
			std::span<const std::optional<custom_look::look>> customs = {}, bool expired = false)
		{
			const u16 pressed = raw & ~m_held;
			m_held = raw;
			if (!seen.verified || (seen.mode != 10 && seen.mode != 4) || seen.scene != 3 || seen.subscene != 44 || seen.grid_mode != 1 || seen.state != 3)
			{
				m_open = m_release = false;
				m_cards.clear();
				m_committed.reset();
				m_fighter = ~u32{0};
				return {};
			}
			if (seen.base_character != m_fighter)
				m_committed.reset();
			// A timeout may wait for a confirmation already in flight. Every retry keeps the same highlighted costume.
			if (expired && m_committed)
				return {m_committed->confirm, m_committed, false};
			if (m_release && !expired)
			{
				m_release = raw != 0;
				return {u16{0}, {}, false};
			}
			if (!m_open || seen.base_character != m_fighter)
			{
				m_open = false;
				m_fighter = seen.base_character;
				m_cards = cards(tables, m_fighter, customs);
				m_focus = 0;
			}
			if (m_cards.empty())
				return {static_cast<u16>(raw & ~reserved), {}, false};
			// A shared countdown chooses the highlighted card even if Cross was already held. Closed rows choose Default 01.
			if (expired)
				return choose();
			if (!m_open)
			{
				if (pressed & triangle)
				{
					m_release = true;
					return {u16{0}, {}, true};
				}
				if (pressed & cross)
					return choose();
				if (pressed & square)
				{
					m_committed.reset();
					m_open = true;
					if (const auto& last = m_last[m_fighter])
						for (u32 i = 0; i < m_cards.size(); ++i)
							if (m_cards[i].look.row == last->row && m_cards[i].slot == last->slot)
								m_focus = i;
					return {u16{0}, {}, false};
				}
				return {static_cast<u16>(raw & ~reserved), {}, false};
			}
			if (pressed & circle)
			{
				m_committed.reset();
				m_open = false;
				m_release = true;
			}
			else if (pressed & cross)
				return choose();
			else if (pressed & left)
				m_focus = (m_focus + static_cast<u32>(m_cards.size()) - 1) % static_cast<u32>(m_cards.size());
			else if (pressed & right)
				m_focus = (m_focus + 1) % static_cast<u32>(m_cards.size());
			else if (pressed & l1)
				m_focus = 0;
			else if (pressed & r1)
			{
				for (u32 i = 0; i < m_cards.size(); ++i)
					if (m_cards[i].slot)
					{
						m_focus = i;
						break;
					}
			}
			return {u16{0}, {}, false};
		}
	};
} // namespace pbd::costume_select
