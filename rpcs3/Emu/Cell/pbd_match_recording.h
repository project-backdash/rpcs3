#pragma once

#include <array>
#include <cstdint>
#include <optional>

// Exact replay offsets come from verified native phase observations. Host frame counts and intro-duration estimates cannot
// identify a restorable round-call boundary, so an unknown or skipped phase closes publication instead of guessing an offset.
namespace pbd::match_recording
{
	using u32 = std::uint32_t;
	inline constexpr u32 max_ticks = 216000;
	inline constexpr u32 max_rounds = 255;

	enum class phase : std::uint8_t
	{
		loading,
		vs,
		character_intro,
		round_intro,
		fight,
		round_result,
		match_end
	};
	enum class winner : std::uint8_t
	{
		none,
		left,
		right,
		draw
	};
	enum class kind : std::uint8_t
	{
		clean_start,
		vs,
		intro,
		round_start,
		round_fight,
		round_end,
		match_end
	};
	enum class refusal : std::uint8_t
	{
		none,
		clean_load_missing,
		not_active,
		phase_unverified,
		boundary_order,
		boundary_limit,
		presentation_order,
		round_order,
		round_limit,
		result_unverified,
		score_invalid,
		outcome_invalid,
	};

	struct classified_phase
	{
		phase value = phase::loading;
		bool verified = false;
	};

	// The game's dispatcher copies pending 0x10093230 to current 0x1009322c before its switch. Reading pending at the
	// pre-tick latch identifies the update about to execute; current and the completed-update witness are one update late.
	inline classified_phase classify(u32 scene, u32 pending, bool round_closed, bool match_decided = false) noexcept
	{
		if (scene == 8)
			return {phase::vs, true};
		if (scene != 16 && scene != 17)
			return {phase::loading, true};
		switch (pending)
		{
		case 0:
		case 1:
		case 2:
		case 3:
			// Scene 16 reloads between rounds. Its preparation does not repeat the first character intro.
			return {round_closed && scene == 16 ? phase::round_result : phase::character_intro, true};
		case 4: return {phase::round_intro, true};
		case 5: return {phase::fight, true};
		case 6:
		case 7:
		case 8:
		case 9:
		case 10:
		case 11:
		case 12: return {phase::round_result, true};
		case 37:
			// Stock result handlers set pending 37 before scene 16 reloads the next round, or before scene 17 exits a decided
			// match. Both belong to the verified closed round; the terminal path additionally requires a committed score.
			return round_closed && (scene == 16 || (scene == 17 && match_decided)) ?
			           classified_phase{phase::round_result, true} :
			           classified_phase{};
		default: return {};
		}
	}

	struct event
	{
		kind type{};
		u32 match = 0, round = 0, boundary = 0;
		u32 counter = 0, score_left = 0, score_right = 0;
		winner outcome = winner::none;
	};
	struct events
	{
		std::array<event, 3> values{};
		u32 count = 0;
		refusal refused = refusal::none;
	};
	struct observation
	{
		// Boundary is the exact first-dispatch pre-tick point. The caller classifies phase using verified native fields/hooks.
		// A previous-tick fight witness alone cannot classify round_intro or the first future fight dispatch.
		u32 boundary = 0;
		phase native_phase = phase::loading;
		bool phase_verified = false;
		bool reexecution = false;
		bool result_verified = false;
		bool draw_verified = false;
		u32 score_left = 0, score_right = 0;
		winner outcome = winner::none;
	};

	class lifecycle
	{
		u32 m_match = 0, m_origin = 0, m_last_boundary = 0, m_round = 0, m_last_fight = 0;
		u32 m_score_left = 0, m_score_right = 0, m_rounds_to_win = 0;
		phase m_phase = phase::loading;
		bool m_observed = false, m_active = false, m_complete = false, m_vs = false, m_intro = false;
		bool m_round_open = false, m_round_fighting = false;
		refusal m_refusal = refusal::none;

		events reject(refusal reason) noexcept
		{
			m_active = false;
			m_refusal = reason;
			events output{};
			output.refused = reason;
			return output;
		}
		void emit(events& output, kind type, u32 boundary, winner outcome = winner::none) const noexcept
		{
			output.values[output.count++] = {type, m_match, m_round, boundary, 0, m_score_left, m_score_right, outcome};
		}

		refusal observe_vs(events& output, const observation& o) noexcept
		{
			if (m_intro || m_round_open)
				return refusal::presentation_order;
			if (!m_vs)
			{
				m_vs = true;
				emit(output, kind::vs, o.boundary);
			}
			return refusal::none;
		}

		refusal observe_character_intro(events& output, const observation& o) noexcept
		{
			if (!m_vs || m_round)
				return refusal::presentation_order;
			if (!m_intro)
			{
				m_intro = true;
				emit(output, kind::intro, o.boundary);
			}
			return refusal::none;
		}

		refusal observe_round_intro(events& output, const observation& o) noexcept
		{
			if (!m_intro)
				return refusal::presentation_order;
			if (m_phase != phase::round_intro)
			{
				if (m_round_open || (m_phase != phase::character_intro && m_phase != phase::round_result))
					return refusal::round_order;
				if (m_round >= max_rounds)
					return refusal::round_limit;
				++m_round;
				m_round_open = true;
				m_round_fighting = false;
				emit(output, kind::round_start, o.boundary);
			}
			return refusal::none;
		}

		refusal observe_fight(events& output, const observation& o) noexcept
		{
			if (!m_round_open || (m_phase != phase::round_intro && m_phase != phase::fight))
				return refusal::round_order;
			if (!m_round_fighting)
			{
				m_round_fighting = true;
				emit(output, kind::round_fight, o.boundary);
			}
			m_last_fight = o.boundary;
			return refusal::none;
		}

		refusal observe_round_result(events& output, const observation& o) noexcept
		{
			if (m_phase != phase::round_result)
			{
				if (!m_round_open || !m_round_fighting || m_phase != phase::fight)
					return refusal::round_order;
				if (!o.result_verified)
					return refusal::result_unverified;
				if (o.score_left < m_score_left || o.score_right < m_score_right ||
					o.score_left > m_rounds_to_win || o.score_right > m_rounds_to_win ||
					o.score_left - m_score_left > 1 || o.score_right - m_score_right > 1)
					return refusal::score_invalid;
				const u32 advanced = o.score_left - m_score_left + o.score_right - m_score_right;
				if (advanced != 1 && !o.draw_verified)
					return refusal::outcome_invalid;
				m_score_left = o.score_left;
				m_score_right = o.score_right;
				m_round_open = false;
				emit(output, kind::round_end, m_last_fight);
			}
			return refusal::none;
		}

		refusal observe_match_end(events& output, const observation& o) noexcept
		{
			if (!m_vs || !m_intro || !m_round || m_round_open || m_phase != phase::round_result)
				return refusal::round_order;
			if (!o.result_verified)
				return refusal::result_unverified;
			if (o.score_left != m_score_left || o.score_right != m_score_right)
				return refusal::score_invalid;
			if (o.outcome == winner::left  ? m_score_left != m_rounds_to_win || m_score_right == m_rounds_to_win :
				o.outcome == winner::right ? m_score_right != m_rounds_to_win || m_score_left == m_rounds_to_win :
											 o.outcome != winner::draw || m_score_left != m_rounds_to_win || m_score_right != m_rounds_to_win)
				return refusal::outcome_invalid;
			emit(output, kind::match_end, o.boundary, o.outcome);
			m_active = false;
			m_complete = true;
			return refusal::none;
		}

		refusal observe_phase(events& output, const observation& o) noexcept
		{
			switch (o.native_phase)
			{
			case phase::loading: return m_intro || m_round_open ? refusal::presentation_order : refusal::none;
			case phase::vs: return observe_vs(output, o);
			case phase::character_intro: return observe_character_intro(output, o);
			case phase::round_intro: return observe_round_intro(output, o);
			case phase::fight: return observe_fight(output, o);
			case phase::round_result: return observe_round_result(output, o);
			case phase::match_end: return observe_match_end(output, o);
			default: return refusal::phase_unverified;
			}
		}

	public:
		bool active() const noexcept
		{
			return m_active;
		}
		bool complete() const noexcept
		{
			return m_complete;
		}
		refusal refused() const noexcept
		{
			return m_refusal;
		}
		bool round_closed() const noexcept
		{
			return m_round && !m_round_open;
		}
		bool match_decided() const noexcept
		{
			return round_closed() && (m_score_left == m_rounds_to_win || m_score_right == m_rounds_to_win);
		}
		std::optional<std::array<u32, 2>> round_wins() const noexcept
		{
			if (!m_active || !m_round || match_decided())
				return {};
			return std::array{m_score_left, m_score_right};
		}
		u32 rounds_to_win() const noexcept
		{
			return m_rounds_to_win;
		}

		events begin(u32 match, u32 boundary, u32 counter, u32 rounds_to_win, bool clean_loaded) noexcept
		{
			*this = lifecycle{};
			if (!clean_loaded || !match || match > 65535 || rounds_to_win < 1 || rounds_to_win > 5)
				return reject(refusal::clean_load_missing);
			m_match = match;
			m_origin = boundary;
			m_rounds_to_win = rounds_to_win;
			m_active = true;
			events output{};
			emit(output, kind::clean_start, boundary);
			output.values[0].counter = counter;
			return output;
		}

		events observe(const observation& o) noexcept
		{
			if (o.reexecution)
				return {};
			if (!m_active)
				return reject(m_refusal == refusal::none ? refusal::not_active : m_refusal);
			if (!o.phase_verified)
				return reject(refusal::phase_unverified);
			if (o.boundary < m_origin || (m_observed && o.boundary <= m_last_boundary))
				return reject(refusal::boundary_order);
			if (o.boundary - m_origin >= max_ticks && !(o.native_phase == phase::match_end && o.boundary - m_origin == max_ticks))
				return reject(refusal::boundary_limit);
			events output{};
			if (const auto reason = observe_phase(output, o); reason != refusal::none)
				return reject(reason);
			m_phase = o.native_phase;
			m_last_boundary = o.boundary;
			m_observed = true;
			return output;
		}
	};
} // namespace pbd::match_recording
