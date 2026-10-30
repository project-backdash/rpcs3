#pragma once

#include "pbd_env.h"
#include "Emu/RSX/pbd_frame_tags.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <string_view>

// Host presentation only. A held game never changes guest sound, timing or the player's volume.
namespace pbd::held_presentation
{
	enum class purpose
	{
		none,
		replay,
		match,
		local,
		conflict
	};

	inline purpose requested(const char* replay, const char* match, const char* set = nullptr, const char* preselect = nullptr) noexcept
	{
		const bool watch = replay && std::string_view(replay) == "1";
		const bool fighter = (match && *match) || (set && *set && preselect && std::string_view(preselect) == "1");
		return watch && fighter ? purpose::conflict : watch ? purpose::replay :
		                                          fighter   ? purpose::match :
		                                                      purpose::none;
	}

	class lifecycle
	{
		enum class phase : unsigned
		{
			disabled,
			holding,
			loading,
			playing,
			ended
		};
		const purpose m_purpose;
		std::atomic<phase> m_phase;
		std::atomic<std::uint64_t> m_picture{0}, m_audio{0}, m_boundary{0};
		std::atomic<float> m_gain{0}, m_mixed_gain{0};
		std::atomic<bool> m_mixed{false};
		std::atomic<std::uint64_t> m_swaps{0};
		std::atomic<bool> m_parked{false};
		std::atomic<bool> m_wait_vs{true};
		const std::chrono::steady_clock::time_point m_origin = std::chrono::steady_clock::now();

		std::uint64_t elapsed() const noexcept
		{
			return 1 + static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(
						   std::chrono::steady_clock::now() - m_origin)
							   .count());
		}

	public:
		explicit lifecycle(purpose held) noexcept : m_purpose(held), m_phase(held == purpose::none ? phase::disabled : phase::holding) {}
		explicit lifecycle(bool standby = false) noexcept : lifecycle(standby ? purpose::replay : purpose::none) {}
		bool active() const noexcept
		{
			return m_phase.load(std::memory_order_acquire) != phase::disabled;
		}
		void park(bool value) noexcept
		{
			m_parked.store(value, std::memory_order_release);
		}
		// Called after all guest and rendering threads have joined, before a fresh checkpoint load.
		void rearm() noexcept
		{
			m_picture = 0;
			m_audio = 0;
			m_boundary = 0;
			m_mixed = false;
			m_parked = false;
			m_wait_vs = true;
			m_phase.store(m_purpose == purpose::none ? phase::disabled : phase::holding, std::memory_order_release);
		}
		bool parked() const noexcept
		{
			return m_parked.load(std::memory_order_acquire);
		}
		bool can_show() const noexcept
		{
			const auto value = m_phase.load(std::memory_order_acquire);
			return value == phase::disabled || value == phase::playing;
		}
		bool bound(purpose admitted = purpose::replay, bool wait_for_vs = true) noexcept
		{
			if ((admitted != purpose::replay && admitted != purpose::match && admitted != purpose::local) || admitted != m_purpose)
				return false;
			auto expected = phase::holding;
			if (m_phase.load(std::memory_order_acquire) != expected)
				return false;
			// Only the admitting owner writes this, before publishing the loading phase to rendering.
			m_wait_vs.store(wait_for_vs, std::memory_order_release);
			return m_phase.compare_exchange_strong(expected, phase::loading, std::memory_order_acq_rel);
		}
		// Fixed picks conceal the checkpoint select; a lobby with in-game picks must show it.
		bool bound_match(bool fixed_picks) noexcept
		{
			return bound(purpose::match, fixed_picks);
		}
		void play(std::uint64_t boundary) noexcept
		{
			// A matched game's boundary 0 still carries the checkpoint's character-select image.
			if (m_purpose == purpose::match && m_wait_vs.load(std::memory_order_acquire))
				return;
			open(boundary);
		}
		bool play_image(const pbd::frame_presentation::frame_tag_queue::record& token, std::uint64_t epoch, bool image) noexcept
		{
			if (m_purpose != purpose::match || !m_wait_vs.load(std::memory_order_acquire) || !image || !token.live || !token.verified() || token.session_epoch != epoch ||
				(token.scene != 8 && token.scene != 12))
				return false;
			return open(token.boundary);
		}

	private:
		bool open(std::uint64_t boundary) noexcept
		{
			if (m_phase.load(std::memory_order_acquire) != phase::loading)
				return false;
			m_boundary.store(boundary, std::memory_order_relaxed);
			auto expected = phase::loading;
			return m_phase.compare_exchange_strong(expected, phase::playing, std::memory_order_release);
		}

	public:
		// Called only after a real image's successful present to a visible window, never an overlay-only flip.
		void picture(bool visible) noexcept
		{
			if (visible && m_phase.load(std::memory_order_acquire) == phase::playing)
			{
				std::uint64_t empty = 0;
				m_picture.compare_exchange_strong(empty, elapsed(), std::memory_order_release);
			}
		}
		bool hold_audio() const noexcept
		{
			return active() && (m_phase.load(std::memory_order_acquire) != phase::playing ||
								   !m_picture.load(std::memory_order_acquire));
		}
		// The mixer and output-device callback acknowledge separately: queued or rejected sound is not ready.
		void audio(float gain) noexcept
		{
			if (!active() || hold_audio())
				return;
			m_mixed_gain.store(gain, std::memory_order_relaxed);
			m_mixed.store(true, std::memory_order_release);
		}
		bool awaiting_audio() const noexcept
		{
			return active() && !hold_audio() && m_mixed.load(std::memory_order_acquire) && !m_audio.load(std::memory_order_acquire);
		}
		void submitted(bool signal) noexcept
		{
			if (!awaiting_audio())
				return;
			const float gain = m_mixed_gain.load(std::memory_order_acquire);
			if (gain > 0.0f && !signal)
				return;
			m_gain.store(gain, std::memory_order_relaxed);
			std::uint64_t empty = 0;
			m_audio.compare_exchange_strong(empty, elapsed(), std::memory_order_release);
		}
		bool ready() const noexcept
		{
			return m_phase.load(std::memory_order_acquire) == phase::playing &&
			       m_picture.load(std::memory_order_acquire) && m_audio.load(std::memory_order_acquire);
		}
		void swapped() noexcept
		{
			if (active())
				m_swaps.fetch_add(1, std::memory_order_release);
		}
		std::uint64_t swaps() const noexcept
		{
			return m_swaps.load(std::memory_order_acquire);
		}
		std::uint64_t picture_us() const noexcept
		{
			return m_picture.load(std::memory_order_acquire);
		}
		std::uint64_t audio_us() const noexcept
		{
			return m_audio.load(std::memory_order_acquire);
		}
		std::uint64_t boundary() const noexcept
		{
			return m_boundary.load(std::memory_order_acquire);
		}
		float gain() const noexcept
		{
			return m_gain.load(std::memory_order_acquire);
		}
		void end() noexcept
		{
			if (active())
				m_phase.store(phase::ended, std::memory_order_release);
		}
	};

	inline lifecycle& output() noexcept
	{
		static lifecycle value([]
			{
				const auto existing = requested(pbd::env::find("PBD_REPLAY_STANDBY"), pbd::env::find("PBD_MATCH_STANDBY_CHANNEL"),
					pbd::env::find("PBD_SET_CHANNEL"), pbd::env::find("PBD_LOBBY_PRESELECT"));
				const char* local = pbd::env::find("PBD_LOCAL_STANDBY_CHANNEL");
				return local && *local ? (existing == purpose::none ? purpose::local : purpose::conflict) : existing;
			}());
		return value;
	}
} // namespace pbd::held_presentation
