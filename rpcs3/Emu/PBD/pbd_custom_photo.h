#pragma once

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <filesystem>
#include <mutex>
#include <optional>
#include <span>
#include <vector>

namespace pbd::custom_photo
{
	using u8 = std::uint8_t;
	using u32 = std::uint32_t;
	using u64 = std::uint64_t;
	inline std::atomic<u64> input_serial{0};
	// A picture observes a neutral opening and abandons it as soon as either player's input changes it.
	class entry_pose
	{
		bool m_practice = false;
		u32 m_frames = 0;
		u64 m_input = 0;

	public:
		static constexpr u32 maximum_opening_boundaries = 600;
		void arm_practice() noexcept
		{
			m_practice = true;
			m_frames = 0;
			m_input = input_serial.load(std::memory_order_acquire);
		}
		void finish() noexcept
		{
			m_practice = false;
		}
		bool holding() const noexcept
		{
			return false;
		}
		bool observe(u32 mode, u32 scene, u32 phase, bool visible = true) noexcept
		{
			// Loading may finish while the host still hides the game; remember that opening until it is shown.
			if (mode == 10 && scene == 2)
				arm_practice();
			if (mode != 10 || scene == 3)
				finish();
			if (mode == 10 && m_practice && (input_serial.load(std::memory_order_acquire) != m_input || (visible && scene == 17 && ++m_frames > maximum_opening_boundaries)))
				finish();
			return visible && scene == 17 && ((mode != 10 && phase == 4) || (mode == 10 && phase == 17 && m_practice));
		}
	};
	struct frame
	{
		u64 ticket = 0, generation = 0, flip = 0;
		u32 width = 0, height = 0;
		bool valid = false;
		std::vector<u8> rgba;
	};
	// Keep a fixed portrait size across output resolutions; fighter openings use the player's side of the frame.
	inline frame portrait(const frame& source, bool player_one = false, bool player_two = false)
	{
		frame out{source.ticket, source.generation, source.flip, 320, 480};
		if (!source.valid || source.width < 2 || source.height < 2 || source.rgba.size() != u64{source.width} * source.height * 4)
			return out;
		out.valid = true;
		out.rgba.resize(out.width * out.height * 4);
		const u32 crop_height = source.height * 4 / 5;
		const u32 crop_width = std::min(source.width, crop_height * 2 / 3);
		const u32 centre = player_one ? source.width / 3 : player_two ? source.width * 2 / 3 :
		                                                                source.width / 2;
		const u32 left = std::min(source.width - crop_width, centre > crop_width / 2 ? centre - crop_width / 2 : 0);
		const u32 top = (source.height - crop_height) / 2;
		for (u32 y = 0; y < out.height; ++y)
			for (u32 x = 0; x < out.width; ++x)
			{
				const auto from = (u64{top + y * crop_height / out.height} * source.width + left + x * crop_width / out.width) * 4;
				std::copy_n(source.rgba.begin() + from, 4, out.rgba.begin() + (y * out.width + x) * 4);
			}
		return out;
	}

	// One explicit capture crosses from the editor's owner thread to the renderer. No guest memory or file path crosses it.
	class channel
	{
		std::mutex m_mutex;
		std::atomic<bool> m_pending{false};
		std::atomic<u64> m_latest_flip{0};
		u64 m_serial = 0, m_ticket = 0, m_generation = 0, m_after = 0;
		bool m_copied = false;
		std::optional<frame> m_result;

	public:
		u64 request(u64 generation, u64 after_flip)
		{
			std::lock_guard lock(m_mutex);
			if (m_ticket && generation == m_generation)
				return 0;
			m_ticket = ++m_serial;
			m_generation = generation;
			m_after = after_flip;
			m_copied = false;
			m_result.reset();
			m_pending.store(true, std::memory_order_release);
			return m_ticket;
		}
		u64 wants_frame(u64 generation, u64 flip)
		{
			m_latest_flip.store(flip, std::memory_order_release);
			if (!m_pending.load(std::memory_order_acquire))
				return 0;
			std::lock_guard lock(m_mutex);
			if (!m_ticket || m_copied || generation != m_generation || flip <= m_after)
				return 0;
			m_copied = true;
			return m_ticket;
		}
		u64 latest_flip() const noexcept
		{
			return m_latest_flip.load(std::memory_order_acquire);
		}
		void deliver(u64 ticket, u64 generation, u64 flip, std::span<const u8> pixels, u32 width, u32 height, bool bgra, bool fence_ok)
		{
			std::lock_guard lock(m_mutex);
			if (!ticket || ticket != m_ticket || generation != m_generation || flip <= m_after || m_result)
				return;
			frame value{ticket, generation, flip, width, height};
			const u64 bytes = u64{width} * height * 4;
			value.valid = fence_ok && width && height && width <= 4096 && height <= 4096 && pixels.size() == bytes;
			if (value.valid)
			{
				value.rgba.assign(pixels.begin(), pixels.end());
				for (std::size_t at = 0; at < value.rgba.size(); at += 4)
				{
					if (bgra)
						std::swap(value.rgba[at], value.rgba[at + 2]);
					value.rgba[at + 3] = 255;
				}
			}
			m_result = std::move(value);
		}
		std::optional<frame> take(u64 ticket)
		{
			std::lock_guard lock(m_mutex);
			if (!ticket || ticket != m_ticket || !m_result)
				return {};
			auto result = std::move(m_result);
			m_result.reset();
			m_ticket = 0;
			m_pending.store(false, std::memory_order_release);
			return result;
		}
		void cancel(u64 ticket)
		{
			std::lock_guard lock(m_mutex);
			if (ticket && ticket == m_ticket)
			{
				m_ticket = 0;
				m_pending.store(false, std::memory_order_release);
				m_result.reset();
			}
		}
	};
	inline channel pending;
	std::vector<u8> encode_png(const frame& value);
	std::vector<u8> encode_jpeg(const frame& value);
	bool save_async(frame value, u32 side, std::vector<std::filesystem::path> targets);
} // namespace pbd::custom_photo
