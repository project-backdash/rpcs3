#pragma once

// Host wait state for start, input, exit-gate and scene barriers. The latch publishes elapsed time and Leave progress;
// the RSX overlay draws it using PBD_PEER_NAME (UTF-8, at most 32 characters, optionally empty).
//
// After leave_after, Circle must be released before a two-second hold can end the wait. These controller reads never enter
// guest input; lobby sets emit leave {"action":"peer_wait"} without a result or seat change, while other sessions end directly.

#include "../PBD/pbd_env.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <optional>
#include <string>
#include <string_view>

namespace pbd::session_wait
{
	// A shorter wait is ordinary lockstep pacing (a frame or two) and draws nothing.
	inline constexpr std::chrono::milliseconds show_after{500};
	inline constexpr std::uint32_t leave_after_seconds = 30;
	inline constexpr std::chrono::seconds leave_after{leave_after_seconds};
	inline constexpr std::chrono::milliseconds leave_hold{2000};
	inline constexpr std::size_t name_limit = 32;
	// Circle in the game's canonical 16-bit controller word (pbd_local_pad_word).
	inline constexpr std::uint16_t leave_button = 0x0020;

	// Loading and recovery share the same deliberate Leave action.
	enum class leave_kind : std::uint8_t
	{
		none,
		set,
		session
	};

	inline std::atomic<bool>& showing() noexcept
	{
		static std::atomic<bool> value{false};
		return value;
	}
	inline std::atomic<bool>& recovering() noexcept
	{
		static std::atomic<bool> value{false};
		return value;
	}
	// The match's standby waits for this PC's controller before the match starts (it holds the start until one is connected or
	// the wait times out): the overlay asks for one.
	inline std::atomic<bool>& controller_wanted() noexcept
	{
		static std::atomic<bool> value{false};
		return value;
	}

	// The shown wait's whole seconds and the Leave it offers once it has lasted leave_after. Both are written before showing() is
	// raised, so the modal's first frame already has them.
	inline std::atomic<std::uint32_t>& seconds() noexcept
	{
		static std::atomic<std::uint32_t> value{0};
		return value;
	}

	inline std::atomic<leave_kind>& leave() noexcept
	{
		static std::atomic<leave_kind> value{leave_kind::none};
		return value;
	}
	inline std::atomic<std::uint32_t>& leave_held_ms() noexcept
	{
		static std::atomic<std::uint32_t> value{0};
		return value;
	}

	inline bool leave_open(std::uint32_t waited_seconds, leave_kind kind) noexcept
	{
		return kind != leave_kind::none && waited_seconds >= leave_after_seconds;
	}

	// A hold starts only after Circle is released on or after the offer. Every release cancels the partial hold.
	struct leave_read
	{
		bool offered = false, chosen = false, released = false;
		std::optional<std::chrono::milliseconds> held_from;
		std::uint32_t held_ms = 0;
	};

	inline leave_read read_leave(std::chrono::milliseconds waited, leave_kind kind, std::uint16_t pad, leave_read previous) noexcept
	{
		if (kind == leave_kind::none || waited < leave_after)
			return {};
		leave_read out = previous;
		out.offered = true;
		out.chosen = false;
		out.held_ms = 0;
		if (!(pad & leave_button))
		{
			out.released = true;
			out.held_from.reset();
		}
		else if (out.released)
		{
			if (!out.held_from || waited < *out.held_from)
				out.held_from = waited;
			const auto held = waited - *out.held_from;
			out.held_ms = static_cast<std::uint32_t>(held >= leave_hold ? leave_hold.count() : held.count());
			out.chosen = held >= leave_hold;
		}
		return out;
	}

	// Appends one code point as UTF-8.
	inline void append_utf8(std::string& out, char32_t c)
	{
		if (c < 0x80)
			out += static_cast<char>(c);
		else if (c < 0x800)
		{
			out += static_cast<char>(0xc0 | (c >> 6));
			out += static_cast<char>(0x80 | (c & 0x3f));
		}
		else if (c < 0x10000)
		{
			out += static_cast<char>(0xe0 | (c >> 12));
			out += static_cast<char>(0x80 | ((c >> 6) & 0x3f));
			out += static_cast<char>(0x80 | (c & 0x3f));
		}
		else
		{
			out += static_cast<char>(0xf0 | (c >> 18));
			out += static_cast<char>(0x80 | ((c >> 12) & 0x3f));
			out += static_cast<char>(0x80 | ((c >> 6) & 0x3f));
			out += static_cast<char>(0x80 | (c & 0x3f));
		}
	}

	// Decodes strict UTF-8 (no overlong forms, surrogates or values past U+10FFFF); nothing when the text is not valid.
	inline std::optional<std::u32string> decode_utf8(std::string_view text)
	{
		std::u32string out;
		for (std::size_t at = 0; at < text.size();)
		{
			const auto lead = static_cast<unsigned char>(text[at]);
			const std::size_t length = lead < 0x80 ? 1 : (lead & 0xe0) == 0xc0 ? 2 :
			                                         (lead & 0xf0) == 0xe0     ? 3 :
			                                         (lead & 0xf8) == 0xf0     ? 4 :
			                                                                     0;
			if (!length || at + length > text.size())
				return std::nullopt;
			char32_t c = length == 1 ? lead : length == 2 ? (lead & 0x1f) :
			                              length == 3     ? (lead & 0x0f) :
			                                                (lead & 0x07);
			for (std::size_t k = 1; k < length; ++k)
			{
				const auto next = static_cast<unsigned char>(text[at + k]);
				if ((next & 0xc0) != 0x80)
					return std::nullopt;
				c = (c << 6) | (next & 0x3f);
			}
			constexpr char32_t smallest[] = {0, 0, 0x80, 0x800, 0x10000};
			if (c < smallest[length] || c > 0x10ffff || (c >= 0xd800 && c <= 0xdfff))
				return std::nullopt;
			out += c;
			at += length;
		}
		return out;
	}

	// The name as shown: control characters and surrounding spaces removed, at most name_limit characters. Text that is not
	// valid UTF-8 gives an empty name, which shows the generic line.
	inline std::string clean_name(std::string_view raw)
	{
		const auto decoded = decode_utf8(raw);
		if (!decoded)
			return {};
		std::u32string kept;
		for (const char32_t c : *decoded)
			if (c >= 0x20 && !(c >= 0x7f && c <= 0x9f) && c != 0x2028 && c != 0x2029)
				kept += c;
		const auto first = kept.find_first_not_of(U' ');
		if (first == std::u32string::npos)
			return {};
		kept = kept.substr(first, kept.find_last_not_of(U' ') - first + 1);
		if (kept.size() > name_limit)
			kept.resize(name_limit);
		std::string out;
		for (const char32_t c : kept)
			append_utf8(out, c);
		return out;
	}

	// "0:12", "1:05", "12:00".
	inline std::string elapsed(std::uint32_t waited_seconds)
	{
		const std::uint32_t rest = waited_seconds % 60;
		return std::to_string(waited_seconds / 60) + (rest < 10 ? ":0" : ":") + std::to_string(rest);
	}

	// The wait's headline in the display face: WAITING FOR KAI (the name's printable ASCII upper case, which the face draws),
	// WAITING FOR YOUR OPPONENT without one, or RECONNECTING.
	inline std::string title(std::string_view name, bool reconnecting = recovering().load(std::memory_order_acquire))
	{
		if (reconnecting)
			return "RECONNECTING";
		std::string shown;
		for (const char c : name)
			if (c >= 0x20 && c <= 0x7e)
				shown += c >= 'a' && c <= 'z' ? static_cast<char>(c - 'a' + 'A') : c;
		const auto first = shown.find_first_not_of(' ');
		return "WAITING FOR " + (first == std::string::npos ? std::string("YOUR OPPONENT") : shown.substr(first));
	}

	// The controller notice: the headline, and the line under it (the standby holds the match's start for the controller).
	inline constexpr std::string_view controller_title = "CONNECT A CONTROLLER", controller_line = "The match waits for you.";

	// The way out once Leave opens, with {circle} drawn as the pad's Circle glyph. A lobby set's leave sends the peer_wait
	// reason, which the lobby ends with no result and never counts against the player who left.
	inline std::string_view leave_prompt(leave_kind kind) noexcept
	{
		return kind == leave_kind::set ? "{circle} Hold to leave \xC2\xB7 no penalty" : "{circle} Hold to leave";
	}

	// UTF-16 from the Windows environment to UTF-8; an unpaired surrogate makes the whole value invalid (empty).
	inline std::string utf16_to_utf8(std::u16string_view text)
	{
		std::string out;
		for (std::size_t at = 0; at < text.size(); ++at)
		{
			char32_t c = text[at];
			if (c >= 0xd800 && c <= 0xdbff)
			{
				if (at + 1 >= text.size() || text[at + 1] < 0xdc00 || text[at + 1] > 0xdfff)
					return {};
				c = 0x10000 + ((c - 0xd800) << 10) + (text[++at] - 0xdc00);
			}
			else if (c >= 0xdc00 && c <= 0xdfff)
				return {};
			append_utf8(out, c);
		}
		return out;
	}

	// PBD_PEER_NAME, read once. Windows keeps the environment in UTF-16, so it is read wide there: the narrow copy would
	// be in the system code page, not UTF-8.
	inline const std::string& peer_name()
	{
		static const std::string value = []
		{
#ifdef _WIN32
			std::string text;
			if (const wchar_t* raw = pbd::env::find_wide("PBD_PEER_NAME"))
			{
				static_assert(sizeof(wchar_t) == sizeof(char16_t));
				text = utf16_to_utf8(std::u16string_view(reinterpret_cast<const char16_t*>(raw)));
			}
			return clean_name(text);
#else
			const char* raw = pbd::env::find("PBD_PEER_NAME");
			return clean_name(raw ? raw : "");
#endif
		}();
		return value;
	}

	// The wait notices are needed in a two-PC session: one configured at launch (PBD_PEER_ROLE; the latch refuses one without
	// a role) or a game held ready for one (PBD_MATCH_STANDBY_CHANNEL), whose role arrives with the match bind after the
	// renderer has created its overlays.
	inline bool requested() noexcept
	{
		const auto set = [](const char* name)
		{
			const char* value = pbd::env::find(name);
			return value && *value;
		};
		return set("PBD_PEER_ROLE") || set("PBD_MATCH_STANDBY_CHANNEL");
	}
} // namespace pbd::session_wait
