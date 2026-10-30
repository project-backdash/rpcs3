#pragma once

#include <array>
#include <cstdint>

// Short taps. The game reads the controller once per frame, while a button on an arcade stick or a hitbox can go down and
// up again well inside one frame. Read as a level, such a tap is lost whenever both edges land between two reads. Every
// word here is the game's canonical 16-bit controller word (digital1 << 8 | digital2).
namespace pbd::input_taps
{
	// The readers of this PC's controller that each read it once per frame.
	enum class reader : unsigned
	{
		game,         // the game's own read of the controller's port (cellPadGetData)
		replay,       // replay controls and a fighter taken in a replay (pbd_replay_pad_words)
		continuation, // a fighter taken from a replay round (pbd_direct_match)
		practice,     // Practice's menu, shortcuts and dummy recording
		count
	};

	// A button pressed since a reader's previous read reads as held at its next read, so a tap that went down and up
	// between the two still counts for one frame. Nothing else changes: a press reads at the first read after it and a
	// release at the first read after it, so a press is never delayed, a release is never stretched and a button held
	// across reads reads as it is. Each reader keeps its own record, so one read never takes a tap from another.
	class tap_buffer
	{
	public:
		// A tap waits this long at most (six frames). A reader that has not read for longer (the game paused, or not yet
		// reading the pads) does not get an old tap afterwards.
		static constexpr std::uint64_t hold_us = 100'000;

		// The controller's word as the host sees it now.
		void sample(std::uint16_t word, std::uint64_t now_us) noexcept
		{
			const auto pressed = static_cast<std::uint16_t>(word & ~m_level);
			for (unsigned bit = 0; bit < m_pressed_us.size(); ++bit)
				if (pressed >> bit & 1)
					m_pressed_us[bit] = now_us;
			for (auto& taps : m_taps)
				taps |= pressed;
			m_level = word;
		}

		// What the reader's read would return now, without taking its taps.
		std::uint16_t peek(reader from, std::uint64_t now_us) const noexcept
		{
			std::uint16_t word = m_level;
			const std::uint16_t taps = m_taps[static_cast<unsigned>(from)];
			for (unsigned bit = 0; bit < m_pressed_us.size(); ++bit)
				if ((taps >> bit & 1) && now_us - m_pressed_us[bit] <= hold_us)
					word |= static_cast<std::uint16_t>(1u << bit);
			return word;
		}

		// The reader's read: the buttons held now and those pressed since its previous read.
		std::uint16_t read(reader from, std::uint64_t now_us) noexcept
		{
			const std::uint16_t word = peek(from, now_us);
			m_taps[static_cast<unsigned>(from)] = 0;
			return word;
		}

		std::uint16_t level() const noexcept
		{
			return m_level;
		}

	private:
		std::uint16_t m_level = 0;
		std::array<std::uint16_t, static_cast<unsigned>(reader::count)> m_taps{};
		std::array<std::uint64_t, 16> m_pressed_us{};
	};

	// The game's own input ring for one port. Its input thread writes each word it reads from the pad into the next of 16
	// slots and then counts the slot as written; the game's update reads only the newest slot. When the input thread writes
	// twice between two updates (the two threads wake on the same vertical blank in either order) the older word is never
	// read, and a tap that reached only that word would be lost. A ring_reader keeps the count and the newest word of its
	// previous read and adds every button pressed in the slots written since, by the same rule as tap_buffer.
	class ring_reader
	{
	public:
		// The newest slots the input thread has finished; the 16th back may be the one it is writing.
		static constexpr std::uint32_t readable = 15;

		// words: the port's ring, read after the count `written`; newest: the word this read reports as held (the newest
		// slot's, or the word the game's own read just returned).
		std::uint16_t read(const std::array<std::uint16_t, 16>& words, std::uint32_t written, std::uint16_t newest) noexcept
		{
			std::uint16_t pressed = 0;
			// A count that went back (the game restarted its input thread) or ran past the ring has no usable history.
			if (m_valid && written - m_written <= readable)
			{
				std::uint16_t previous = m_word;
				for (std::uint32_t slot = m_written; slot != written; ++slot)
				{
					pressed |= static_cast<std::uint16_t>(words[slot & 15] & ~previous);
					previous = words[slot & 15];
				}
			}
			m_valid = true;
			m_written = written;
			m_word = words[(written - 1) & 15];
			return static_cast<std::uint16_t>(newest | pressed);
		}

	private:
		std::uint32_t m_written = 0;
		std::uint16_t m_word = 0;
		bool m_valid = false;
	};
} // namespace pbd::input_taps
