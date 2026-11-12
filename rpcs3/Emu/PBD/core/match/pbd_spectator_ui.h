#pragma once

#include "pbd_set_channel.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

// The launcher supplies each watch_ui snapshot through pbd_spectator_channel_model.h / pbd_spectator_channel_controls.h;
// the watcher's game controls fight-only visibility. Host controller presses return as immutable
// pad event files, without reaching the game's neutral virtual ports or changing what either fighter executes.
namespace pbd::spectator_ui
{
	using u16 = std::uint16_t;
	using u32 = std::uint32_t;
	using u64 = std::uint64_t;
	using json = pbd::set_channel::json_value;

	// The game's canonical 16-bit controller word (digital1 << 8 | digital2), as the select timer and the in-game menu read it.
	namespace pad
	{
		inline constexpr u16 l2 = 0x0001, r2 = 0x0002, l1 = 0x0004, r1 = 0x0008, triangle = 0x0010, circle = 0x0020, cross = 0x0040, square = 0x0080;
		inline constexpr u16 select = 0x0100, l3 = 0x0200, r3 = 0x0400, start = 0x0800, up = 0x1000, right = 0x2000, down = 0x4000, left = 0x8000;

		// The button a prompt's glyphs name ("{circle}"); of a pair ("{l1}/{r1}") the last, so a click on Tab moves forward.
		inline constexpr u16 of(std::string_view keys) noexcept
		{
			const auto open = keys.rfind('{'), close = keys.rfind('}');
			if (open == std::string_view::npos || close == std::string_view::npos || close < open)
				return 0;
			const auto name = keys.substr(open + 1, close - open - 1);
			constexpr std::array<std::pair<std::string_view, u16>, 9> names{{{"circle", circle}, {"cross", cross}, {"square", square},
				{"triangle", triangle}, {"start", start}, {"l1", l1}, {"r1", r1}, {"l2", l2}, {"r2", r2}}};
			for (const auto& [glyph, bit] : names)
				if (glyph == name)
					return bit;
			return 0;
		}
	} // namespace pad

	// watch_ui limits, in bytes of UTF-8 (counts of entries where named so).
	inline constexpr std::size_t max_tabs = 4, max_lines = 8, max_profile_lines = 4;
	inline constexpr std::size_t tab_bytes = 16, to_bytes = 48, author_bytes = 16, line_bytes = 160;
	inline constexpr std::size_t name_bytes = 16, profile_line_bytes = 48, preview_id_bytes = 64, language_bytes = 16, region_bytes = 32;
	inline constexpr u32 max_unread = 99, max_first_to = 10;
	// Preview lifetime starts when the watcher UI first becomes visible, fading over the last fade_ms.
	inline constexpr u64 preview_ms = 6000, fade_ms = 1000;

	struct line
	{
		std::string author, text;
		bool system = false; // a lobby line (a result, someone joined): drawn without an author, in the secondary ink
		bool operator==(const line&) const = default;
	};

	struct chat_panel
	{
		std::vector<std::string> tabs; // "All", "Lobby", "Direct"; L1/R1 change them (the launcher redraws the panel)
		u32 tab = 0;
		std::string to;          // "To: Lobby"
		std::vector<line> lines; // the newest last
		bool operator==(const chat_panel&) const = default;
	};

	struct preview
	{
		std::string id, author, text;
		bool operator==(const preview&) const = default;
	};

	struct profile
	{
		std::string name;
		std::vector<std::string> lines; // "EN · N. America", "Tonight: 3 sets, 2 won", "Last played: Bryan"
		bool operator==(const profile&) const = default;
	};

	struct origin
	{
		std::string language, region;
		bool operator==(const origin&) const = default;
	};

	// The launcher's watch_ui control: the watcher's whole screen. Each one replaces the previous one; revision only grows.
	struct state
	{
		u64 revision = 0;
		bool bar = false; // the button bar (shown from round 1, hidden in intros and at a K.O.)
		u32 first_to = 0; // the set's length for the score line and the VS cards; 0 draws no score line
		u32 unread = 0;   // chat lines the watcher has not seen: the badge on Chat
		std::optional<preview> new_line;
		std::optional<chat_panel> chat;                 // open over the match while present
		std::optional<std::array<profile, 2>> profiles; // left, right
		std::optional<std::array<origin, 2>> cards;     // the VS cards' language and region, left and right
		bool operator==(const state&) const = default;
	};

	namespace detail
	{
		using pbd::set_channel::json_kind;
		inline bool text(const json* value, std::size_t min, std::size_t max, std::string& out)
		{
			if (!pbd::set_channel::detail::plain_text(value, min, max))
				return false;
			out = value->text;
			return true;
		}
		inline bool absent(const json* value)
		{
			return !value || value->kind == json_kind::null;
		}
		inline bool texts(const json* value, std::size_t count, std::size_t min, std::size_t max, std::vector<std::string>& out)
		{
			if (!value || value->kind != json_kind::array || value->items.size() > count)
				return false;
			out.clear();
			for (const auto& item : value->items)
			{
				std::string entry;
				if (!text(&item, min, max, entry))
					return false;
				out.push_back(std::move(entry));
			}
			return true;
		}
		inline bool chat(const json& value, chat_panel& out)
		{
			if (value.kind != json_kind::object)
				return false;
			if (const auto* tabs = value.find("tabs"); !absent(tabs) && !texts(tabs, max_tabs, 1, tab_bytes, out.tabs))
				return false;
			if (const auto* tab = value.find("tab"); !absent(tab) &&
													 (!pbd::set_channel::detail::number32(tab, 0, static_cast<u32>(max_tabs - 1), out.tab) || out.tab >= std::max<std::size_t>(out.tabs.size(), 1)))
				return false;
			if (const auto* to = value.find("to"); !absent(to) && !text(to, 0, to_bytes, out.to))
				return false;
			if (const auto* lines = value.find("lines"); !absent(lines))
			{
				if (lines->kind != json_kind::array || lines->items.size() > max_lines)
					return false;
				for (const auto& item : lines->items)
				{
					line entry;
					if (item.kind != json_kind::object || !text(item.find("text"), 1, line_bytes, entry.text))
						return false;
					if (const auto* author = item.find("author"); !absent(author) && !text(author, 0, author_bytes, entry.author))
						return false;
					if (const auto* system = item.find("system"); !absent(system) && !pbd::set_channel::detail::boolean(system, entry.system))
						return false;
					out.lines.push_back(std::move(entry));
				}
			}
			return true;
		}
		inline bool side_pair(const json* value, auto&& read_side)
		{
			if (!value || value->kind != json_kind::object)
				return false;
			const auto* left = value->find("left");
			const auto* right = value->find("right");
			return left && right && left->kind == json_kind::object && right->kind == json_kind::object && read_side(*left, 0) && read_side(*right, 1);
		}
	} // namespace detail

	// One watch_ui control: {"revision", "bar", "firstTo", "unread"} and the optional "preview", "chat", "profiles" and "cards"
	// (absent or null: none). The revision must be higher than the last one applied. Any malformed field refuses the whole control.
	inline pbd::set_channel::result<state> parse(const json& root, u64 previous_revision)
	{
		using pbd::set_channel::detail::boolean;
		using pbd::set_channel::detail::number;
		using pbd::set_channel::detail::number32;
		state out;
		if (root.kind != pbd::set_channel::json_kind::object || !number(root.find("revision"), previous_revision + 1, pbd::set_channel::max_safe_integer, out.revision) ||
			!boolean(root.find("bar"), out.bar) || !number32(root.find("firstTo"), 0, max_first_to, out.first_to) ||
			!number32(root.find("unread"), 0, max_unread, out.unread))
			return {{}, "watch_ui_invalid"};
		if (const auto* item = root.find("preview"); !detail::absent(item))
		{
			preview shown;
			if (item->kind != pbd::set_channel::json_kind::object || !pbd::set_channel::detail::identifier(item->find("id")) ||
				item->find("id")->text.size() > preview_id_bytes || !detail::text(item->find("text"), 1, line_bytes, shown.text))
				return {{}, "watch_ui_invalid"};
			shown.id = item->find("id")->text;
			if (const auto* author = item->find("author"); !detail::absent(author) && !detail::text(author, 0, author_bytes, shown.author))
				return {{}, "watch_ui_invalid"};
			out.new_line = std::move(shown);
		}
		if (const auto* item = root.find("chat"); !detail::absent(item))
		{
			chat_panel panel;
			if (!detail::chat(*item, panel))
				return {{}, "watch_ui_invalid"};
			out.chat = std::move(panel);
		}
		if (const auto* item = root.find("profiles"); !detail::absent(item))
		{
			std::array<profile, 2> pair{};
			if (!detail::side_pair(item, [&](const json& side, std::size_t index)
					{
						return detail::text(side.find("name"), 1, name_bytes, pair[index].name) &&
				               (detail::absent(side.find("lines")) || detail::texts(side.find("lines"), max_profile_lines, 0, profile_line_bytes, pair[index].lines));
					}))
				return {{}, "watch_ui_invalid"};
			out.profiles = std::move(pair);
		}
		if (const auto* item = root.find("cards"); !detail::absent(item))
		{
			std::array<origin, 2> pair{};
			if (!detail::side_pair(item, [&](const json& side, std::size_t index)
					{
						return detail::text(side.find("language"), 0, language_bytes, pair[index].language) &&
				               detail::text(side.find("region"), 0, region_bytes, pair[index].region);
					}))
				return {{}, "watch_ui_invalid"};
			out.cards = std::move(pair);
		}
		return {std::move(out), {}};
	}

	// Whether the watcher's screen may show at a boundary of the watched game: a fight is on screen (pending phase 5 of a battle
	// scene), the game is exposed (its first intro was verified), and the watch is healthy. The intros (phases 0..4, the character
	// and round calls), a K.O. and the round results (6..12, 37) and the scene-16 reload between rounds show nothing, as a fighting
	// game's own spectator view does; so the bar first appears when round 1 starts.
	inline bool fight_on_screen(u32 scene, u32 pending) noexcept
	{
		return (scene == 16 || scene == 17) && pending == 5;
	}

	// Start requests local fast playback; it never enters either virtual game port.
	enum class button : std::uint8_t
	{
		circle,
		square,
		r2,
		triangle,
		l1,
		r1,
		cross,
		start
	};

	inline std::string_view button_name(button which)
	{
		switch (which)
		{
		case button::circle: return "circle";
		case button::square: return "square";
		case button::r2: return "r2";
		case button::triangle: return "triangle";
		case button::l1: return "l1";
		case button::r1: return "r1";
		case button::cross: return "cross";
		case button::start: return "start";
		}
		return "circle";
	}

	struct press
	{
		button which = button::circle;
		bool chat_open = false; // a chat panel was open (Circle then ends the chat instead of watching)
		bool operator==(const press&) const = default;
	};

	// Only new presses count. Circle and Start also work while the match screen is hidden, so intros cannot block stop or
	// catch-up. The first read and reads claimed by the settings menu only follow held buttons; releasing that menu cannot
	// accidentally stop watching or skip. Other buttons continue to belong to the visible chat and profile controls.
	class pad_reader
	{
		u16 m_previous = 0;
		bool m_primed = false;

	public:
		// Presses come back in a fixed order; Circle precedes Start when both are pressed together.
		// `menu`: the in-game menu holds the controller (pbd::settings_menu::claimed); the read reports nothing and moves nothing.
		std::vector<press> read(u16 word, const state* ui, bool shown, bool menu = false)
		{
			std::vector<press> out;
			if (!m_primed || menu)
			{
				m_primed = true;
				m_previous = word;
				return out;
			}
			const u16 down = static_cast<u16>(word & ~m_previous);
			m_previous = word;
			const bool chat = ui && ui->chat;
			if (!shown)
			{
				if (down & pad::circle)
					out.push_back({button::circle, chat});
				if (down & pad::start)
					out.push_back({button::start, chat});
				return out;
			}
			static constexpr std::array<std::pair<u16, button>, 7> order{{{pad::circle, button::circle}, {pad::start, button::start}, {pad::square, button::square}, {pad::r2, button::r2},
				{pad::triangle, button::triangle}, {pad::l1, button::l1}, {pad::r1, button::r1}}};
			for (const auto& [bit, which] : order)
				if (down & bit)
					out.push_back({which, chat});
			return out;
		}
	};

	// What the watcher's overlay draws, composed by the spectator channel from the launcher's watch_ui, its presentation control
	// and the watched game's own phase.
	struct view
	{
		u64 generation = 0;
		bool present = false; // a watch_ui arrived: this screen replaces the older status band
		bool shown = false;   // a fight is on screen, exposed, healthy and not a broadcast
		bool bar = false;
		u32 game = 0, first_to = 0, left_score = 0, right_score = 0;
		std::string left_name, right_name;
		u32 unread = 0;
		std::optional<preview> new_line;
		u64 new_line_since_ms = 0; // host steady milliseconds when this preview first showed (0 until it has)
		std::optional<chat_panel> chat;
		std::optional<std::array<profile, 2>> profiles;
		bool operator==(const view&) const = default;
	};

	// "Game 2 · Kai 1 – 0 Rook · first to 2". Empty without a set length or a game.
	inline std::string score_line(const view& shown)
	{
		if (!shown.first_to || !shown.game)
			return {};
		const std::string dot = " \xC2\xB7 ";
		std::string scores = std::to_string(shown.left_score) + " \xE2\x80\x93 " + std::to_string(shown.right_score);
		if (!shown.left_name.empty())
			scores = shown.left_name + " " + scores;
		if (!shown.right_name.empty())
			scores += " " + shown.right_name;
		return "Game " + std::to_string(shown.game) + dot + scores + dot + "first to " + std::to_string(shown.first_to);
	}

	// One group of the button bar: the glyph tokens, the label, and a count drawn beside it (0: none).
	struct bar_item
	{
		std::string keys, label;
		u32 badge = 0;
		bool operator==(const bar_item&) const = default;
	};

	// The bar for the screen as it is: the open chat's controls, the profiles' way back, or the watcher's own buttons.
	inline std::vector<bar_item> bar_items(const view& shown)
	{
		if (shown.chat)
		{
			std::vector<bar_item> out{{"{circle}", "End Chat", 0}};
			if (shown.chat->tabs.size() > 1)
				out.push_back({"{l1}/{r1}", "Tab", 0});
			return out;
		}
		if (shown.profiles)
			return {{"{r2}", "Close Profiles", 0}, {"{square}", "Chat", shown.unread}, {"{circle}", "Stop Watching", 0}};
		return {{"{circle}", "Stop Watching", 0}, {"{square}", "Chat", shown.unread}, {"{r2}", "Profiles", 0}};
	}

	// The preview's opacity at host time now_ms: full, then a fade over the last fade_ms, then gone.
	inline float preview_opacity(u64 since_ms, u64 now_ms) noexcept
	{
		const u64 age = now_ms > since_ms ? now_ms - since_ms : 0;
		if (age >= preview_ms)
			return 0.f;
		if (age + fade_ms <= preview_ms)
			return 1.f;
		return static_cast<float>(preview_ms - age) / static_cast<float>(fade_ms);
	}

	inline u64 steady_ms() noexcept
	{
		return static_cast<u64>(std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count());
	}

	// One published view for the renderer. The revision moves only when a drawn value changes.
	inline std::mutex published_mutex;
	inline view published;
	inline std::atomic<u64> published_revision{0};

	inline void publish(u64 generation, view value)
	{
		value.generation = generation;
		std::lock_guard lock(published_mutex);
		if (published == value)
			return;
		published = std::move(value);
		published_revision.fetch_add(1, std::memory_order_release);
	}
	inline u64 revision()
	{
		return published_revision.load(std::memory_order_acquire);
	}
	// The view of emulation `generation`; another emulation's view is never drawn.
	inline view load(u64 generation)
	{
		std::lock_guard lock(published_mutex);
		return published.generation == generation ? published : view{};
	}
} // namespace pbd::spectator_ui
