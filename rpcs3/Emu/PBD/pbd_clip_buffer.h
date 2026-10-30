#pragma once

#include <algorithm>
#include <cstdint>
#include <deque>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace pbd::clips
{
	template <typename Exists>
	std::optional<std::string> available_filename(const std::string& base, Exists exists)
	{
		for (unsigned copy = 1; copy < 1000; ++copy)
		{
			auto name = base + (copy == 1 ? "" : " (" + std::to_string(copy) + ")") + ".mp4";
			if (!exists(name) && !exists(name + ".part"))
				return name;
		}
		return {};
	}
	struct packet
	{
		std::vector<std::uint8_t> bytes;
		std::int64_t time_us = 0;
		std::int64_t duration_us = 0;
		bool audio = false;
		bool keyframe = false;
	};
	using packet_ptr = std::shared_ptr<const packet>;

	// Owned by the encoder thread. Saving shares encoded bytes; it never encodes them again.
	class buffer
	{
		std::deque<packet_ptr> m_packets;
		std::size_t m_bytes = 0;
		std::int64_t m_span_us = 30'000'000;
		static constexpr std::size_t byte_limit = 384 * 1024 * 1024;
	public:
		void clear() { m_packets.clear(); m_bytes = 0; }
		void seconds(unsigned value) { m_span_us = std::clamp(value, 15u, 300u) * 1'000'000ll; }
		std::size_t bytes() const { return m_bytes; }
		void push(packet_ptr value)
		{
			m_bytes += value->bytes.size();
			m_packets.push_back(std::move(value));
			const auto oldest = m_packets.back()->time_us - m_span_us - 1'000'000;
			while (!m_packets.empty() && (m_packets.front()->time_us < oldest || m_bytes > byte_limit))
			{
				m_bytes -= m_packets.front()->bytes.size();
				m_packets.pop_front();
			}
		}
		std::vector<packet_ptr> snapshot() const
		{
			if (m_packets.empty()) return {};
			const auto cut = m_packets.back()->time_us - m_span_us;
			std::int64_t start = -1;
			for (const auto& value : m_packets)
				if (!value->audio && value->keyframe)
				{
					if (start < 0 || value->time_us <= cut) start = value->time_us;
					if (value->time_us > cut) break;
				}
			if (start < 0) return {};
			std::vector<packet_ptr> result;
			for (const auto& value : m_packets)
				if (value->time_us >= start) result.push_back(value);
			std::stable_sort(result.begin(), result.end(), [](const auto& a, const auto& b)
			{
				if (a->time_us != b->time_us) return a->time_us < b->time_us;
				return a->audio < b->audio;
			});
			return result;
		}
	};

	enum class action { none, save, record };
	// A button only clips use (the touchpad, an Xbox Share button): a tap saves, a one-second hold starts or stops
	// recording. A button the game also reads (Create/Share is the game's Select): only a half-second hold saves, as
	// the consoles' own capture button does, so a tap stays a plain Select.
	struct gesture
	{
		bool armed = false, down = false, held = false;
		std::uint64_t since = 0;
		action update(bool pressed, std::uint64_t now_ms, bool shared_with_game = false)
		{
			if (!armed) { armed = !pressed; return action::none; }
			if (pressed && !down) { down = true; held = false; since = now_ms; }
			const std::uint64_t hold_ms = shared_with_game ? 500 : 1000;
			if (pressed && !held && now_ms >= since && now_ms - since >= hold_ms)
			{
				held = true;
				return shared_with_game ? action::save : action::record;
			}
			if (!pressed && down)
			{
				down = false;
				return held || shared_with_game ? action::none : action::save;
			}
			return action::none;
		}
	};

	// What follows the date in a clip's file name. The launcher writes it from fixed words and the game's character names, but
	// it arrives through a file, so only ASCII letters, digits and the marks in those names ("JACK-5", "Roger Jr.") are kept.
	// Anything else, path separators and control characters among them, becomes one space; the result never starts with a dot,
	// never ends with a space or a dot (Windows drops a trailing one) and is at most 80 characters.
	inline std::string file_label(std::string_view text)
	{
		constexpr std::size_t limit = 80;
		std::string out;
		for (const char c : text)
		{
			const bool kept = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' || c == '.';
			if (kept && (c != '.' || !out.empty()))
				out += c;
			else if (!out.empty() && out.back() != ' ')
				out += ' ';
			if (out.size() >= limit)
				break;
		}
		while (!out.empty() && (out.back() == ' ' || out.back() == '.'))
			out.pop_back();
		return out;
	}
}
