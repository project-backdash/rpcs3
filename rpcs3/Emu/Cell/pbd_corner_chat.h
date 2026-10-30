#pragma once

#include "../PBD/pbd_env.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <deque>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

// Chat is a separate host drawing surface. It never owns a pad, parks a guest thread or changes a game clock.
namespace pbd::corner_chat
{
	using u64 = std::uint64_t;
	inline constexpr std::size_t max_lines = 5, max_seen = 256, max_batch = 20;
	inline constexpr u64 hold_ms = 8000, fade_ms = 2000;
	struct message
	{
		std::string id, author, text;
	};
	// text is the whole line ("author: message"); author and body are its parts, so the overlay can colour the name.
	struct line
	{
		std::string text;
		float opacity = 0;
		std::string author, body;
	};
	struct snapshot
	{
		u64 generation = 0, revision = 0;
		std::size_t count = 0;
		std::array<line, max_lines> lines{};
	};

	inline u64 now_ms() noexcept
	{
		return static_cast<u64>(std::chrono::duration_cast<std::chrono::milliseconds>(
			std::chrono::steady_clock::now().time_since_epoch())
				.count());
	}

	// Count complete Unicode scalars so a long message cannot split a UTF-8 codepoint or grow the corner into a menu.
	inline bool display_text(std::string_view value, std::string& out, std::size_t limit)
	{
		out.clear();
		std::size_t scalars = 0;
		for (std::size_t i = 0; i < value.size();)
		{
			const std::size_t first = i;
			const auto byte = static_cast<unsigned char>(value[i++]);
			std::uint32_t cp = byte;
			unsigned continuation = 0;
			if (byte >= 0xc2 && byte <= 0xdf)
			{
				cp &= 0x1f;
				continuation = 1;
			}
			else if (byte >= 0xe0 && byte <= 0xef)
			{
				cp &= 0x0f;
				continuation = 2;
			}
			else if (byte >= 0xf0 && byte <= 0xf4)
			{
				cp &= 7;
				continuation = 3;
			}
			else if (byte >= 0x80)
				return false;
			const unsigned extra = continuation;
			if (i + continuation > value.size())
				return false;
			while (continuation--)
			{
				const auto next = static_cast<unsigned char>(value[i++]);
				if ((next & 0xc0) != 0x80)
					return false;
				cp = (cp << 6) | (next & 0x3f);
			}
			if ((extra == 1 && cp < 0x80) || (extra == 2 && cp < 0x800) || (extra == 3 && cp < 0x10000) ||
				cp > 0x10ffff || (cp >= 0xd800 && cp <= 0xdfff) || cp < 0x20 || cp == 0x7f)
				return false;
			if (scalars++ < limit)
				out.append(value.substr(first, i - first));
		}
		if (scalars > limit)
			out += "...";
		return true;
	}

	class state
	{
		struct stored
		{
			std::string text;
			u64 received = 0;
			std::string author, body;
		};
		u64 epoch = 0;
		u64 revision = 0;
		bool visible = false;
		std::deque<std::string> seen;
		std::deque<stored> lines;

	public:
		void clear()
		{
			epoch = 0;
			++revision;
			visible = false;
			seen.clear();
			lines.clear();
		}
		bool apply(u64 generation, bool show, const std::vector<message>& additions, u64 now)
		{
			if (!generation || additions.size() > max_batch)
				return false;
			std::vector<message> admitted;
			admitted.reserve(additions.size());
			for (const auto& item : additions)
			{
				if (item.id.empty() || item.id.size() > 64 || item.author.size() > 16 || item.text.size() > 1120)
					return false;
				for (const unsigned char c : item.id)
					if (c < 0x21 || c > 0x7e)
						return false;
				message clean{item.id, {}, {}};
				if (!display_text(item.author, clean.author, 16) || !display_text(item.text, clean.text, 120))
					return false;
				admitted.push_back(std::move(clean));
			}
			if (epoch != generation)
			{
				clear();
				epoch = generation;
			}
			++revision;
			visible = show;
			if (!show)
			{
				lines.clear();
				return true;
			}
			for (const auto& item : admitted)
			{
				if (std::find(seen.begin(), seen.end(), item.id) != seen.end())
					continue;
				seen.push_back(item.id);
				if (seen.size() > max_seen)
					seen.pop_front();
				if (item.text.empty())
					continue;
				lines.push_back({item.author.empty() ? item.text : item.author + ": " + item.text, now, item.author, item.text});
				if (lines.size() > max_lines)
					lines.pop_front();
			}
			return true;
		}
		snapshot load(u64 now) const
		{
			snapshot out;
			out.generation = epoch;
			out.revision = revision;
			if (!visible)
				return out;
			for (const auto& item : lines)
			{
				const u64 age = now >= item.received ? now - item.received : 0;
				if (age >= hold_ms + fade_ms)
					continue;
				const float opacity = age <= hold_ms ? 1.f : static_cast<float>(hold_ms + fade_ms - age) / static_cast<float>(fade_ms);
				out.lines[out.count++] = {item.text, opacity, item.author, item.body};
			}
			return out;
		}
	};

	inline std::mutex surface_mutex;
	inline state surface;
	inline bool publish(u64 generation, bool visible, const std::vector<message>& additions)
	{
		std::lock_guard lock(surface_mutex);
		return surface.apply(generation, visible, additions, now_ms());
	}
	inline snapshot load()
	{
		std::lock_guard lock(surface_mutex);
		return surface.load(now_ms());
	}
	// Keep hide publication ordered against compilation: after a hide is acknowledged, an older copied surface cannot compile.
	template <typename Compile>
	inline auto compile_if_current(const snapshot& expected, Compile&& compile) -> decltype(compile())
	{
		std::lock_guard lock(surface_mutex);
		const auto current = surface.load(now_ms());
		if (!current.count || current.generation != expected.generation || current.revision != expected.revision)
			return {};
		return compile();
	}
	inline void clear()
	{
		std::lock_guard lock(surface_mutex);
		surface.clear();
	}
	inline bool requested() noexcept
	{
		const auto* channel = pbd::env::find("PBD_SET_CHANNEL");
		return channel && *channel;
	}
} // namespace pbd::corner_chat
