#pragma once

#include "pbd_controller_mapper.h"

#include <array>
#include <cstdint>
#include <map>
#include <mutex>
#include <set>
#include <span>
#include <string>
#include <string_view>

// Offline play's two local players, each on a controller of its own, as Tekken 8's local Versus keeps them. A controller
// without a player joins the first free player with Start (claim, from the controller backend), and that press only joins.
// From then on the player is locked to that controller and other controllers reach nothing, until it is unplugged, which
// leaves the player free for the next Start. Select on a player's controller asks which player it is (to change sides) or
// opens its Button setup; with Start held it is the hold that leaves Versus and asks nothing. While a controller is asked
// only it answers, and none of its buttons reach the game until it has let go of them all after the answer. Each opening of
// the VS mode frees both players again (reset). The backend supplies each poll (pbd_controller_backend.cpp).
namespace pbd::controllers::seats
{
	inline constexpr unsigned players = 2;

	struct pad
	{
		std::string id, name;
		std::uint32_t buttons = 0; // logical bits (pbd_controller_mapper.h bit())
	};

	// The question on screen: which player `device` is. Left/Right choose the player; Down reaches Button setup.
	struct prompt
	{
		bool open = false;
		std::string device, name;
		unsigned player = 0;
		bool setup_focused = false;
		bool operator==(const prompt&) const = default;
	};

	struct view
	{
		prompt question;
		// Each player's controller, by id and by name (empty without one).
		std::array<std::string, players> ids{}, names{};
		std::uint64_t revision = 0;
	};

	enum class outcome
	{
		none,
		seated,
		setup
	};

	class table
	{
		std::array<std::string, players> m_seat{};
		std::map<std::string, std::string, std::less<>> m_names;
		std::map<std::string, std::uint32_t, std::less<>> m_last;
		std::set<std::string, std::less<>> m_release;
		prompt m_prompt;
		std::uint64_t m_revision = 1;
		// The controller that answered last, and the player it chose (outcome::seated) or its Button setup (outcome::setup).
		std::string m_answered;
		unsigned m_answered_player = 0;

		int seat_of(std::string_view id) const
		{
			for (unsigned i = 0; i < players; ++i)
				if (!m_seat[i].empty() && m_seat[i] == id)
					return static_cast<int>(i);
			return -1;
		}

		void ask(const std::string& id, unsigned seat)
		{
			m_prompt = {true, id, m_names[id], seat, false};
			++m_revision;
		}

		// The asked controller gives nothing to the game until every button it holds now is up.
		void close()
		{
			m_release.insert(m_prompt.device);
			m_prompt = {};
			++m_revision;
		}

		// `id` becomes `player`; the controller there before takes `id`'s old player, so the two trade sides.
		void seat(unsigned player, const std::string& id)
		{
			const unsigned other = 1 - player;
			if (m_seat[player] == id)
				return;
			if (m_seat[other] == id)
				m_seat[other] = m_seat[player];
			m_seat[player] = id;
			++m_revision;
		}

		outcome answer(std::uint32_t pressed)
		{
			if (pressed & (bit(circle) | bit(select) | bit(start)))
			{
				close();
				return outcome::none;
			}
			if (pressed & (bit(up) | bit(down)))
			{
				const bool focus = (pressed & bit(down)) != 0;
				if (focus != m_prompt.setup_focused)
				{
					m_prompt.setup_focused = focus;
					++m_revision;
				}
			}
			if (!m_prompt.setup_focused && (pressed & (bit(left) | bit(right))))
			{
				const unsigned player = (pressed & bit(right)) ? 1u : 0u;
				if (player != m_prompt.player)
				{
					m_prompt.player = player;
					++m_revision;
				}
			}
			if (!(pressed & bit(cross)))
				return outcome::none;
			m_answered = m_prompt.device;
			m_answered_player = m_prompt.player;
			if (m_prompt.setup_focused)
			{
				close();
				return outcome::setup;
			}
			seat(m_prompt.player, m_prompt.device);
			close();
			return outcome::seated;
		}

	public:
		// Every connected controller's report this poll.
		outcome update(std::span<const pad> pads)
		{
			outcome result = outcome::none;
			std::set<std::string, std::less<>> present;
			for (const auto& p : pads)
			{
				present.insert(p.id);
				m_names[p.id] = p.name;
				const auto last = m_last.find(p.id);
				// A controller's first report is its starting state, never a press.
				const std::uint32_t before = last == m_last.end() ? p.buttons : last->second;
				m_last[p.id] = p.buttons;
				if (!p.buttons)
					m_release.erase(p.id);
				const std::uint32_t pressed = p.buttons & ~before;
				if (m_prompt.open && p.id == m_prompt.device)
				{
					if (pressed)
						result = answer(pressed);
				}
				else if (const int seat = seat_of(p.id); !m_prompt.open && seat >= 0 && (pressed & bit(select)) && !(p.buttons & bit(start)) && !m_release.contains(p.id))
					ask(p.id, static_cast<unsigned>(seat));
			}
			for (auto it = m_last.begin(); it != m_last.end();)
				it = present.contains(it->first) ? std::next(it) : m_last.erase(it);
			// An unplugged controller leaves its player free for the next Start.
			for (auto& seated : m_seat)
				if (!seated.empty() && !present.contains(seated))
				{
					seated.clear();
					++m_revision;
				}
			if (m_prompt.open && !present.contains(m_prompt.device))
			{
				m_prompt = {};
				++m_revision;
			}
			return result;
		}

		// `id`, a controller without a player, takes `player` while it has none. That press only joins: the controller gives the
		// game nothing until it has let go. True when the controller took the player.
		bool claim(unsigned player, const std::string& id)
		{
			if (player >= players || id.empty() || !m_seat[player].empty() || seat_of(id) >= 0 || m_release.contains(id))
				return false;
			m_seat[player] = id;
			m_release.insert(id);
			++m_revision;
			return true;
		}

		// Both players free, and nobody asked. A button held now is let go before it joins or reaches the game.
		void reset()
		{
			for (auto& seated : m_seat)
				seated.clear();
			m_prompt = {};
			for (const auto& [id, buttons] : m_last)
				if (buttons)
					m_release.insert(id);
			++m_revision;
		}

		const std::string& player(unsigned i) const
		{
			return m_seat[i];
		}
		bool seated(std::string_view id) const
		{
			return seat_of(id) >= 0;
		}

		// Input that stays out of the game: the asked controller, and one still holding a button from its answer.
		bool withheld(std::string_view id) const
		{
			return (m_prompt.open && m_prompt.device == id) || m_release.contains(id);
		}

		// The controller that answered last (outcome::seated, outcome::setup) and the player it chose.
		const std::string& answered() const
		{
			return m_answered;
		}
		unsigned answered_player() const
		{
			return m_answered_player;
		}

		view snapshot() const
		{
			view out;
			out.question = m_prompt;
			for (unsigned i = 0; i < players; ++i)
			{
				const auto name = m_names.find(m_seat[i]);
				out.ids[i] = m_seat[i];
				out.names[i] = m_seat[i].empty() ? std::string() : name == m_names.end() ? m_seat[i] :
				                                                                           name->second;
			}
			out.revision = m_revision;
			return out;
		}
	};

	// The table's view for the overlay to draw (pbd_settings_overlay.cpp).
	namespace detail
	{
		inline std::mutex mutex;
		inline view shown;
	} // namespace detail

	inline void publish(const view& value)
	{
		std::lock_guard lock(detail::mutex);
		detail::shown = value;
	}

	inline view read()
	{
		std::lock_guard lock(detail::mutex);
		return detail::shown;
	}
} // namespace pbd::controllers::seats
