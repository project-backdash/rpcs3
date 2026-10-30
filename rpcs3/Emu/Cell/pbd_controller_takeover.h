#pragma once

// An unassigned pad can become Player 1 in Practice, replays and spectating, using its handler's default mapping.
// The pad thread rebinds under the same pad lock as cellPadGetData, keeping the port connected and each sample whole.
// Peer sessions are disabled; the caller excludes virtual ports and pads assigned to other ports.

#include "../PBD/pbd_env.h"

#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>
#include <vector>

namespace pbd::controller_takeover
{
	// PBD_CONTROLLER_TAKEOVER=0 disables takeover in the modes that offer it.
	inline bool enabled()
	{
		static const bool value = []
		{
			const auto set = [](const char* name)
			{
				const char* text = pbd::env::find(name);
				return text && *text;
			};
			const auto is = [](const char* name, std::string_view expected)
			{
				const char* text = pbd::env::find(name);
				return text && std::string_view(text) == expected;
			};
			if (is("PBD_CONTROLLER_TAKEOVER", "0") || set("PBD_PEER_ROLE") || set("PBD_SET_CHANNEL"))
				return false;
			return is("PBD_PRACTICE_REWIND", "1") || set("PBD_SESSION_REPLAY") || set("PBD_REPLAY_V2_GAME") || set("PBD_SPECTATOR_CHANNEL");
		}();
		return value;
	}

	// Handler number and its device name together identify a pad.
	struct device_id
	{
		std::uint32_t handler = 0;
		std::string device;

		friend bool operator==(const device_id& a, const device_id& b)
		{
			return a.handler == b.handler && a.device == b.device;
		}
		friend bool operator!=(const device_id& a, const device_id& b)
		{
			return !(a == b);
		}
		friend bool operator<(const device_id& a, const device_id& b)
		{
			return std::tie(a.handler, a.device) < std::tie(b.handler, b.device);
		}
	};

	// Candidate readings use the handler's button threshold; stick movement cannot take over.
	struct reading
	{
		device_id id;
		bool pressed = false;
	};

	// Confirm a held press only after Player 1 has been quiet since before it began.
	// Raw and mapped Player 1 activity reject duplicate handler listings; another takeover needs a release and cooldown.
	inline constexpr std::uint64_t confirm_ms = 40, quiet_ms = 300, cooldown_ms = 1000;

	class detector
	{
	public:
		// player1_active includes buttons and sticks from all configured ports; others contains unassigned candidates.
		std::optional<device_id> observe(std::uint64_t now_ms, bool player1_active, const std::vector<reading>& others)
		{
			// Player 1 counts as active when watching starts, so a press already under way then never takes over.
			if (player1_active || !m_started)
				m_player1_active_ms = now_ms;
			m_started = true;
			std::map<device_id, pad_state> next;
			std::optional<device_id> chosen;
			for (const auto& pad : others)
			{
				// A pad that left the list is forgotten; listed again, it must be seen released before it can take over.
				const auto found = m_pads.find(pad.id);
				pad_state state = found == m_pads.end() ? pad_state{} : found->second;
				if (!pad.pressed)
				{
					state.released_seen = true;
					state.pressing = false;
				}
				else if (state.released_seen && !state.pressing)
				{
					state.pressing = true;
					state.pressed_since = now_ms;
				}
				if (!chosen && state.pressing && !player1_active && now_ms >= m_cooldown_until && now_ms - state.pressed_since >= confirm_ms &&
					m_player1_active_ms + quiet_ms <= state.pressed_since)
					chosen = pad.id;
				next.emplace(pad.id, state);
			}
			m_pads = std::move(next);
			if (chosen)
			{
				m_cooldown_until = now_ms + cooldown_ms;
				for (auto& entry : m_pads)
					entry.second = {};
				m_player1_active_ms = now_ms;
			}
			return chosen;
		}

	private:
		struct pad_state
		{
			bool released_seen = false;
			bool pressing = false;
			std::uint64_t pressed_since = 0;
		};

		std::map<device_id, pad_state> m_pads;
		std::uint64_t m_player1_active_ms = 0, m_cooldown_until = 0;
		bool m_started = false;
	};

	// Withhold buttons and centre sticks until the takeover press is released, so it cannot also act in the game.
	// A silent handler gets first_poll_ms after rebinding to report that press; device enumeration time is excluded.
	inline constexpr std::uint64_t first_poll_ms = 300;

	class release_hold
	{
	public:
		void start(std::uint64_t now_ms) noexcept
		{
			m_active = true;
			m_seen = false;
			m_since_ms = now_ms;
		}

		// True while this poll's input must stay out of the game.
		bool observe(std::uint64_t now_ms, bool any_pressed) noexcept
		{
			if (!m_active)
				return false;
			if (any_pressed)
			{
				m_seen = true;
				return true;
			}
			if (m_seen || now_ms - m_since_ms >= first_poll_ms)
				m_active = false;
			return m_active;
		}

		bool active() const noexcept
		{
			return m_active;
		}

	private:
		bool m_active = false, m_seen = false;
		std::uint64_t m_since_ms = 0;
	};

	// The notice a takeover shows: "Controller: <name>" for notice_ms. Practice shows it as its own toast; replays and
	// spectating draw it in the in-game menu's overlay.
	inline constexpr std::uint64_t notice_ms = 2500;

	struct notice
	{
		std::string text;
		std::uint64_t until_ms = 0;
		std::uint64_t serial = 0;
	};

	inline std::uint64_t steady_ms() noexcept
	{
		return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count());
	}

	namespace detail
	{
		struct shared_notice
		{
			std::mutex mutex;
			notice current;
		};

		inline shared_notice& state()
		{
			static shared_notice value;
			return value;
		}
	} // namespace detail

	inline std::string notice_text(const std::string& device)
	{
		return "Controller: " + device;
	}

	// The same toast in the caller's own words (offline play's players, pbd_player_seats.h).
	inline void announce_text(std::string text)
	{
		auto& shared = detail::state();
		std::lock_guard lock(shared.mutex);
		shared.current.text = std::move(text);
		shared.current.until_ms = steady_ms() + notice_ms;
		++shared.current.serial;
	}

	inline void announce(const std::string& device)
	{
		announce_text(notice_text(device));
	}

	inline notice current()
	{
		auto& shared = detail::state();
		std::lock_guard lock(shared.mutex);
		return shared.current;
	}

	// The notice's text once per takeover: `seen` holds the serial the caller showed last.
	inline std::optional<std::string> take_new(std::uint64_t& seen)
	{
		const auto value = current();
		if (value.serial == seen)
			return std::nullopt;
		seen = value.serial;
		if (value.text.empty() || steady_ms() >= value.until_ms)
			return std::nullopt;
		return value.text;
	}
} // namespace pbd::controller_takeover
