#pragma once

#include "pbd_held_presentation.h"
#include <algorithm>
#include <atomic>
#include <cstdint>

// The volume that leaves the emulator. The master volume is applied here, after cellAudio's mix, by a gate that stays
// shut until play begins and then ramps toward the volume on every change, so neither the start of a game, nor a host
// mute, nor its end jumps. Only the host's output buffer is scaled: the game's ports, port levels and audio timing are
// the same as without it, so nothing the game can read changes.
namespace pbd::output_gain
{
	inline bool initially_visible(const char* host, const char* quiet) noexcept
	{
		return !(host && *host) && !(quiet && std::string_view(quiet) == "1");
	}
	// cellAudio always mixes at 48 kHz.
	inline constexpr std::uint32_t mix_rate = 48000;
	// A mute falls quickly, a restore or a louder step rises gently, and the first rise after the gate opens is a fade-in.
	inline constexpr std::uint32_t fall_ms = 20;
	inline constexpr std::uint32_t rise_ms = 250;
	inline constexpr std::uint32_t fade_in_ms = 1500;

	inline constexpr std::uint32_t frames_in(std::uint32_t ms)
	{
		return std::max<std::uint32_t>(1, mix_rate / 1000 * ms);
	}

	// The player's volume is clamped to 0..200: gain is squared up to 100 and linear above it.
	inline constexpr float level(int volume)
	{
		const float fraction = static_cast<float>(std::clamp(volume, 0, 200)) / 100.0f;
		return fraction <= 1.0f ? fraction * fraction : fraction;
	}

	class gate
	{
	public:
		explicit gate(pbd::held_presentation::lifecycle* presentation = nullptr, bool visible = true) noexcept
			: m_presentation(presentation), m_visible(visible) {}
		// A hide takes effect even if no audio block runs before the next show: the next block starts from silence.
		void visible(bool value) noexcept
		{
			if (!value)
				m_hides.fetch_add(1, std::memory_order_acq_rel);
			m_visible.store(value, std::memory_order_release);
		}
		// Lets the output rise; its first rise takes fade_ms. Only the first call counts: the gate never closes again.
		void open(std::uint32_t fade_ms) noexcept
		{
			std::uint32_t closed = 0;
			m_fade_ms.compare_exchange_strong(closed, std::max<std::uint32_t>(1, fade_ms), std::memory_order_acq_rel);
		}

		bool opened() const noexcept
		{
			return m_fade_ms.load(std::memory_order_acquire) != 0;
		}

		// The first frame shown. A run with the input hook opens the gate at its first live boundary instead (after the
		// start sync, and for a replay once its recorded game begins), so a game waiting for the other player, or a replay
		// loading behind the launcher, stays silent.
		void first_frame(bool input_hook) noexcept
		{
			if (!input_hook)
				open(fade_in_ms);
		}

		// Scales `frames` frames of `channels` interleaved samples toward `target`, one gain step per frame. Called by the
		// audio thread only. At a settled gain of 1 the samples are left exactly as mixed.
		void apply(float* samples, std::uint32_t channels, std::uint32_t frames, float target) noexcept
		{
			const std::uint32_t hides = m_hides.load(std::memory_order_acquire);
			const bool shut = !m_visible.load(std::memory_order_acquire) || (m_presentation && m_presentation->hold_audio());
			if (shut || hides != m_seen_hides)
			{
				m_gain = m_target = m_step = 0.0f;
				m_left = 0;
				m_risen = false;
				m_seen_hides = hides;
			}
			if (shut)
			{
				std::fill(samples, samples + channels * frames, 0.0f);
				return;
			}
			const std::uint32_t fade = m_fade_ms.load(std::memory_order_acquire);
			retarget(fade ? std::max(target, 0.0f) : 0.0f, fade);
			std::uint32_t frame = 0;
			for (; frame < frames && m_left; ++frame)
			{
				// Measured back from the target rather than summed, so rounding never builds up and the last frame lands
				// on the target exactly.
				m_gain = m_target - static_cast<float>(--m_left) * m_step;
				for (std::uint32_t channel = 0; channel < channels; ++channel)
					samples[frame * channels + channel] *= m_gain;
			}
			if (!m_left && m_gain > 0.0f)
				m_risen = true;
			if (m_presentation && frames && channels && fade && !m_left)
				m_presentation->audio(m_gain);
			if (frame == frames || m_gain == 1.0f)
				return;
			const std::uint32_t rest = (frames - frame) * channels;
			float* const from = samples + frame * channels;
			if (m_gain == 0.0f)
				std::fill(from, from + rest, 0.0f);
			else
				for (std::uint32_t i = 0; i < rest; ++i)
					from[i] *= m_gain;
		}

	private:
		pbd::held_presentation::lifecycle* const m_presentation;
		std::atomic<bool> m_visible;
		// Hides so far (any thread) and the count the audio thread has applied.
		std::atomic<std::uint32_t> m_hides{0};
		std::uint32_t m_seen_hides = 0;
		// 0 while shut; otherwise the length of the first rise.
		std::atomic<std::uint32_t> m_fade_ms{0};
		// Audio thread only: the gain of the last frame, where it is heading, its step per frame and the frames left.
		float m_gain = 0.0f, m_target = 0.0f, m_step = 0.0f;
		std::uint32_t m_left = 0;
		// The first rise after opening has reached a gain above 0.
		bool m_risen = false;

		// A new target starts a straight ramp from the current gain that ends on it exactly, after fall_ms going down and
		// rise_ms going up (the fade-in for the first rise).
		void retarget(float goal, std::uint32_t fade) noexcept
		{
			if (goal == m_target)
				return;
			m_target = goal;
			if (goal == m_gain)
			{
				m_left = 0;
				return;
			}
			m_left = frames_in(goal < m_gain ? fall_ms : m_risen ? rise_ms :
																   fade);
			m_step = (goal - m_gain) / static_cast<float>(m_left);
		}
	};

	// The emulator's one output. It lives as long as the process, so a game that boots again keeps it open.
	inline gate& output() noexcept
	{
		static gate value(&pbd::held_presentation::output(),
			initially_visible(pbd::env::find("PBD_GAME_HOST"), pbd::env::find("PBD_QUIET_WINDOW")));
		return value;
	}
} // namespace pbd::output_gain
