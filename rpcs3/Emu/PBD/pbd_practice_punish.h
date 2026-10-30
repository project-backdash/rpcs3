#pragma once

// Punishment practice: once the player blocks one of the dummy's attacks that leaves the dummy at punishable or worse,
// the player's next attack to connect says whether it punished. It did when it landed before the dummy's first free frame,
// the frame from which a guarding dummy blocks it (an i12 attack started on the player's first free frame lands on the
// frame before that one at -12). A throw has no contact frame to judge and ends the gap without a verdict. Both fighters'
// frames come from the frame data calculator, so the reading takes host values only and changes nothing in the game.

#include "pbd_practice_frames.h"

#include <cstdint>
#include <optional>
#include <string>

namespace pbd::practice::punish
{
	// The fastest attacks in DR start on frame 10, so -10 on block is the first gap a player can punish.
	inline constexpr std::int32_t punishable = -10;
	// A gap the player has not attacked into is over this many frames after the dummy could act again; a hit after that is
	// a new situation, not a late punish.
	inline constexpr std::int64_t expiry_frames = 60;
	// How long the verdict stays on screen, as long as the movement streak pop-up.
	inline constexpr std::uint32_t shown_frames = 120;

	enum class verdict : std::uint8_t
	{
		punished,
		late
	};
	struct result
	{
		verdict outcome = verdict::punished;
		// The dummy's attack on block, and the startup of the player's attack when the frame data has it.
		std::int32_t advantage = 0;
		std::optional<std::uint32_t> startup;
		// Punished: the frames left before the dummy could block. Late: how many frames after that the attack landed.
		std::uint32_t frames = 0;
		// The late attack was blocked, rather than landing on a dummy that does not guard (for the log; the screen shows it).
		bool blocked = false;
	};

	// The pop-up's text, a title and a detail after a tab: "PUNISHED" over "i10 on -12, 2 frames to spare"; "TOO LATE" over
	// "i10 on -12, 1 frame late" for a fast enough attack pressed late; "TOO SLOW" over "i15 can't punish -12: use i12 or
	// faster" for an attack that could not have made it.
	inline std::string text(const result& value)
	{
		const auto count = [](std::uint32_t frames, const char* one, const char* many)
		{
			return std::to_string(frames) + (frames == 1 ? one : many);
		};
		const std::string gap = frames::signed_frames(value.advantage);
		const std::string attack = value.startup ? "i" + std::to_string(*value.startup) : std::string("Your attack");
		if (value.outcome == verdict::punished)
			return "PUNISHED\t" + attack + " on " + gap + ", " + (value.frames ? count(value.frames, " frame to spare", " frames to spare") : std::string("with no frame to spare"));
		const auto needed = static_cast<std::uint32_t>(-static_cast<std::int64_t>(value.advantage));
		if (value.startup && *value.startup > needed)
			return "TOO SLOW\t" + attack + " can't punish " + gap + ": use i" + std::to_string(needed) + " or faster";
		return "TOO LATE\t" + attack + " on " + gap + ", " + count(value.frames, " frame late", " frames late");
	}

	// Follows fighter 0 (the player) and fighter 1 (the dummy) from boundary to boundary. A contact is known by its move and
	// the frame it was applied on, so each blocked attack is judged once, and a contact from before a restore or a restart
	// never counts again.
	class watch
	{
		struct contact_id
		{
			std::uint32_t move = 0;
			std::int64_t impact = 0;
			bool operator==(const contact_id&) const = default;
		};
		struct window
		{
			contact_id blocked;
			std::int32_t advantage = 0;
			std::int64_t dummy_ready = 0;
			// The player's result when the gap opened, when that was a throw, and whether an attack of the player's has started
			// since: a throw shown after either is the player's answer to the gap.
			std::optional<std::uint32_t> old_throw;
			bool attacked = false;
		};
		std::optional<window> m_open;
		// The dummy's contact last opened or set aside, so a result that stays on screen is not taken up again.
		std::optional<contact_id> m_seen;

		// A connected throw: the calculator marks it contacted without a contact frame.
		static bool thrown(const frames::calculated& player) noexcept
		{
			return player.move && player.contacted && !player.impact;
		}
		void close() noexcept
		{
			if (m_open)
				m_seen = m_open->blocked;
			m_open.reset();
		}

	public:
		void reset() noexcept
		{
			m_open.reset();
			m_seen.reset();
		}
		bool waiting() const noexcept
		{
			return m_open.has_value();
		}
		// One boundary: both fighters' latest results (pbd::practice::frames::calculator) and the frame counter now. Returns
		// the verdict on the boundary that first shows the player's attack connecting.
		std::optional<result> boundary(const frames::calculated& player, const frames::calculated& dummy, std::int64_t now) noexcept
		{
			// The dummy's latest contact. A blocked attack opens a gap once the player's block reaction gives the advantage;
			// while it is the same contact, a cancel that changes the dummy's free frame moves the gap with it. A contact that
			// is not a punishable block (a safe attack, a hit on the player) ends any gap before it.
			if (dummy.move && dummy.contacted && dummy.impact)
			{
				const contact_id id{dummy.move, *dummy.impact};
				const bool open_here = m_open && m_open->blocked == id;
				if (!(m_seen && *m_seen == id) || open_here)
				{
					const bool block = dummy.outcome == frames::contact::block;
					if (block && dummy.advantage && dummy.attacker_ready && dummy.defender_ready)
					{
						if (*dummy.advantage <= punishable)
						{
							if (!open_here)
								m_open = window{id, 0, 0, thrown(player) ? std::optional<std::uint32_t>(player.move) : std::nullopt};
							m_open->advantage = *dummy.advantage;
							m_open->dummy_ready = *dummy.attacker_ready;
						}
						else
						{
							m_open.reset();
							m_seen = id;
						}
					}
					else if (!block)
					{
						m_open.reset();
						m_seen = id;
					}
				}
			}
			if (!m_open)
				return std::nullopt;
			const window gap = *m_open;
			// A counter that went back belongs to a restore; one far past the gap leaves nothing to judge.
			if (now < gap.blocked.impact)
			{
				close();
				return std::nullopt;
			}
			// A throw has no contact frame to judge (the game shows it caught only once the thrower's hold starts), so a throw
			// the player answers with ends the gap without a verdict.
			if (player.move && !player.contacted)
				m_open->attacked = true;
			else if (thrown(player) && (gap.attacked || gap.old_throw != player.move))
			{
				close();
				return std::nullopt;
			}
			if (player.move && player.contacted && player.impact && *player.impact > gap.blocked.impact &&
				*player.impact <= gap.dummy_ready + expiry_frames)
			{
				close();
				const bool hit = player.outcome == frames::contact::hit, blocked = player.outcome == frames::contact::block;
				const bool in_time = *player.impact < gap.dummy_ready;
				// An attack the game blocked while the readings say the dummy could not guard yet, or a contact of no known
				// kind, gives no verdict rather than a wrong one.
				if ((!hit && !blocked) || (in_time && blocked))
					return std::nullopt;
				result out;
				out.advantage = gap.advantage;
				out.startup = player.startup;
				out.blocked = blocked;
				out.outcome = in_time ? verdict::punished : verdict::late;
				out.frames = static_cast<std::uint32_t>(in_time ? gap.dummy_ready - 1 - *player.impact : *player.impact - gap.dummy_ready + 1);
				return out;
			}
			if (now > gap.dummy_ready + expiry_frames)
				close();
			return std::nullopt;
		}
	};
} // namespace pbd::practice::punish
