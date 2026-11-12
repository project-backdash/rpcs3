#pragma once

// Input Delay Frames: the player's own fighter runs each word a set number of frames after it was read, as an online match
// with that much input delay plays. Only the words Practice hands the game wait; the game still reads one word a frame, so
// its speed and flow are its own, and what it runs (and a rewind replays) is the delayed word.

#include <array>
#include <cstdint>

namespace pbd::practice
{
	inline constexpr std::uint32_t max_input_delay = 6;

	class input_delay
	{
	public:
		// Return the word read `frames` frames earlier, or neutral until the delay fills.
		// Changing delay discards queued words and starts the new delay with neutral output.
		std::uint16_t next(std::uint16_t word, std::uint32_t frames) noexcept
		{
			frames = frames < max_input_delay ? frames : max_input_delay;
			if (frames != m_frames)
			{
				clear();
				m_frames = frames;
			}
			if (!frames)
				return word;
			const std::uint16_t out = m_filled >= frames ? m_words[(m_at + m_words.size() - frames) % m_words.size()] : std::uint16_t{0};
			m_words[m_at] = word;
			m_at = (m_at + 1) % static_cast<std::uint32_t>(m_words.size());
			if (m_filled < frames)
				++m_filled;
			return out;
		}
		// One word of the player's at the game's read: delayed while the fight runs; anywhere else (a pause, one of the game's
		// own screens) it passes at once and the line empties, so nothing pressed there arrives in the fight.
		std::uint16_t frame(std::uint16_t word, std::uint32_t frames, bool fight) noexcept
		{
			if (fight)
				return next(word, frames);
			clear();
			return word;
		}
		// Empties the line: the fight's next words are neutral until the delay has passed again.
		void clear() noexcept
		{
			m_words = {};
			m_at = 0;
			m_filled = 0;
		}

	private:
		std::array<std::uint16_t, max_input_delay> m_words{};
		std::uint32_t m_at = 0, m_filled = 0, m_frames = 0;
	};
} // namespace pbd::practice
